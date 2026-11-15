# Native Windows 11 ARM64 desktop target: porting outline

Target: `aarch64-pc-windows-msvc`, built with clang (GNU-style driver, not clang-cl, so
`-std=gnu11`, `__attribute__`, `__thread` and `__builtin_*` keep working) and linked with
lld-link against the MSVC/UCRT libraries and Windows SDK 10.0.26100.

The generated code (`gen/*.c`) is portable as-is. It only uses `x86.h` and `lift_prelude.h`
(`<stdint.h>`, `<string.h>`, `<math.h>`, `<setjmp.h>`). All porting work is in `runtime/`
plus the build files.

## 0. Toolchain

No Visual Studio is needed. The build uses a standalone toolchain, which also sidesteps VS
installer problems such as MSI error 1714:

- **LLVM** (tested with 23.1.5, the official `aarch64-pc-windows-msvc` release, extracted
  to `%USERPROFILE%\tools\llvm`): `clang.exe`, `lld-link.exe`, `llvm-readobj.exe`...
  The `.tar.zst` uses a long zstd window that Windows' `tar` cannot decode; unpack it with
  Python `zstandard` (`max_window_size=2**31`), or use the `.tar.xz`.
- **MSVC CRT + Windows SDK for aarch64**, fetched with
  [xwin](https://github.com/Jake-Shadle/xwin) (tested with 0.10.0):
  `xwin --accept-license --arch aarch64 splat --output %USERPROFILE%\tools\xwin`.
  Directories are named `aarch64`, not `arm64`, and `d3d9.h` is in `sdk/include/shared`.
- **CMake 3.20+ and Ninja** on PATH.

Other locations can be given with the `LLVM_ROOT` / `XWIN_ROOT` environment variables, or
with `-DLLVM_ROOT=` / `-DXWIN_ROOT=` (CMake) and `LLVM=` / `XWIN=` (`win_flags.sh`).

Working flags. `-imsvc` is clang-cl only. Use `-idirafter`, not `-isystem`: with `-isystem`,
MSVC's `arm_neon.h` in `crt/include` shadows clang's and fails to link (undefined `neon_*`).
For CMake, `desktop/win-arm64-toolchain.cmake` sets all of this, with `CMAKE_AR` = llvm-ar
(the GNU-style driver makes CMake use ar syntax).

```
clang --target=aarch64-pc-windows-msvc -fuse-ld=lld \
  -idirafter X/crt/include -idirafter X/sdk/include/ucrt \
  -idirafter X/sdk/include/um -idirafter X/sdk/include/shared \
  ... -Xlinker /libpath:X/crt/lib/aarch64 -Xlinker /libpath:X/sdk/lib/um/aarch64 \
  -Xlinker /libpath:X/sdk/lib/ucrt/aarch64 -ld3d9
```

A smoke test built with these flags runs natively (machine `0xAA64`) and confirms:
- 4 GiB `VirtualAlloc` reserve+commit, plus `PAGE_NOACCESS` on the null page
- decommit/recommit reads back zero
- `__thread` works
- `setjmp`/`longjmp` across 50 frames works (UCRT's unwinding longjmp, with clang's default unwind info)
- `Direct3DCreate9` from the system `d3d9.dll` sees the GPU (tested on an NVIDIA GPU,
  vendor `0x10de`)

Note: with an NVIDIA vendor ID, the game takes its prebuilt-shader path (about 1,700 shaders
created up front) instead of compiling with D3DCompiler at runtime (see `gpu_vendor_choice` in
d3d9_bridge.cpp). That's fast on a desktop GPU; `LB_GPU_VENDOR` overrides it.

Resources: the README puts each generated file at 1-2 GB of RAM to compile, so on a 16 GB
machine use `-Jobs 2`. Windows Defender real-time scanning of the freshly generated files
slows Stage 1 and the compile noticeably; an exclusion for the repo folder (Windows Security >
Virus & threat protection > Exclusions) helps.

## 1. Strategy

Don't scatter `#ifdef _WIN32` through the shims, and don't include `<windows.h>` from the
guest-facing shim files: `kernel32.c` defines its own `ERROR_*`, `WAIT_TIMEOUT` and
`INVALID_HANDLE`, and the shims use guest Win32 names. Instead, add:

- `runtime/win/posix_compat.h`: force-included (`-include`) for runtime C/C++ on `_WIN32`.
  It declares POSIX-named wrappers (`pthread_*`, `usleep`, `clock_gettime`, `opendir`...)
  and pulls in only `<io.h>`, `<direct.h>`, `<sys/stat.h>` and `<process.h>`, never `<windows.h>`.
- `runtime/win/posix_compat.c` (POSIX wrappers, guest memory and lazy commit) and
  `runtime/win/win_platform.c` (crash handler, stack guarantee): the only files that include
  `<windows.h>`.
- Empty shim headers in `runtime/win/include/` for `unistd.h`, `pthread.h`, `dirent.h`,
  `strings.h`, `sys/time.h`, `sys/mman.h`, `sched.h` and `fnmatch.h` that just include
  `posix_compat.h`. That way the existing `#include` lines compile unchanged.

## 2. Call-by-call mapping

### Guest address space (`main.c`, `heap.c`): most important

| POSIX | Where | Win32 replacement |
|---|---|---|
| `mmap(NULL, 4 GiB, RW, PRIVATE\|ANON\|NORESERVE)` | main.c:393 | `VirtualAlloc(NULL, 1ull<<32, MEM_RESERVE\|MEM_COMMIT, PAGE_READWRITE)`. Commit is charged against the 44 GB commit limit but stays demand-zero, so it isn't resident. If commit is tight: `MEM_RESERVE` only, plus a vectored exception handler that commits 64 KiB on first touch. |
| `mprotect(MEM, 0x10000, PROT_NONE)` | main.c:396 | `VirtualProtect(..., PAGE_NOACCESS, &old)` |
| `madvise(MADV_DONTNEED)` in `gvfree` | heap.c:199 | `VirtualFree(p, n, MEM_DECOMMIT)` then `VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE)`. `gvalloc` relies on freed pages reading back as zero, which `MEM_RESET` does **not** guarantee. |

### Threads and sync (`kernel32.c`, `heap.c`, `dispatch.c`, `user32.c`, `xaudio2.c`, `misc.c`, `shader_status.c`, `marvel_shims.c`)

| POSIX | Win32 |
|---|---|
| `pthread_mutex_t` + `PTHREAD_MUTEX_INITIALIZER`, lock/unlock/trylock | `SRWLOCK` (`SRWLOCK_INIT` is static-initializable) |
| `PTHREAD_MUTEX_RECURSIVE` (critical sections in kernel32.c:363, xaudio2.c:769) | struct `{SRWLOCK l; DWORD owner; int depth;}` so one `pthread_mutex_t` type covers both kinds |
| `pthread_cond_wait` / `pthread_cond_timedwait` (absolute `CLOCK_REALTIME` deadline) / `pthread_cond_broadcast` | `CONDITION_VARIABLE` + `SleepConditionVariableSRW` (convert the deadline to relative ms; return `ETIMEDOUT` on `ERROR_TIMEOUT`) + `WakeAllConditionVariable` |
| `pthread_rwlock_rdlock` / `wrlock` / `unlock` (marvel_shims.c:375) | `SRWLOCK` shared/exclusive. Unlock must know which mode it holds, so keep a thread-local flag. |
| `pthread_create` / `detach` / `attr_setdetachstate`, `pthread_exit` | `_beginthreadex` (+ `CloseHandle` for detached threads), `_endthreadex`. Pass a stack size: Windows defaults to 1 MB where glibc gives 8 MB. |
| `sched_yield`, `usleep` | `SwitchToThread`, `Sleep`. The xaudio2 mixer uses `usleep(1000)` pacing and plain `Sleep` granularity is 15.6 ms, so use `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` or `timeBeginPeriod(1)`, otherwise audio will crackle. |
| `__thread` (setjmp.c) | works as-is with clang on MSVC targets |

### Time

| POSIX | Win32 |
|---|---|
| `clock_gettime(CLOCK_MONOTONIC)` (dispatch.c, kernel32.c, misc.c, shader_status.c, xaudio2.c, marvel_shims.c) | `QueryPerformanceCounter` / `QueryPerformanceFrequency` |
| `clock_gettime(CLOCK_REALTIME)`, `gettimeofday` | `GetSystemTimePreciseAsFileTime` minus the 1601-to-1970 offset |
| `gmtime_r(&t, &tm)` | `gmtime_s(&tm, &t)` (argument order is swapped) |
| `timegm` | `_mkgmtime` |

### Files and paths (`kernel32.c`, `main.c`, `misc.c`, `marvel_shims.c`, `touch.cpp`)

| POSIX | Win32 / UCRT |
|---|---|
| `open(path, fl, mode)` | `_open(path, fl \| **_O_BINARY**, _S_IREAD\|_S_IWRITE)`. Without `_O_BINARY`, UCRT opens in text mode and corrupts every `.DAT` read. |
| `read`, `write`, `close` | `_read`, `_write`, `_close` |
| `lseek` with `off_t` | `_lseeki64` with `int64_t`. `off_t` is 32-bit on Windows, so `(off_t)hi << 32` (kernel32.c:675) breaks. |
| `pread(fd, buf, n, off)` (kernel32.c:675, 1524) | `ReadFile` with an `OVERLAPPED` offset on `(HANDLE)_get_osfhandle(fd)`. Don't emulate it with seek+read: it must not move the file pointer and must be thread-safe. |
| `ftruncate` | `_chsize_s` |
| `stat` / `fstat` / `struct stat` | `_stat64` / `_fstat64` (64-bit size); define `S_ISDIR` and `S_ISREG` |
| `mkdir(p, mode)`, `rmdir`, `unlink`, `access`, `chdir` | `_mkdir(p)`, `_rmdir`, `_unlink`, `_access` (`F_OK=0`, `R_OK=4`), `_chdir` |
| `rename` (MoveFile semantics) | UCRT `rename` fails if the target exists, which POSIX allows. Use `MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)`: touch.cpp:508 and `MoveFileA`. |
| `opendir` / `readdir` / `closedir`, `struct dirent` | small wrapper over `FindFirstFileA` / `FindNextFileA` (~40 lines) |
| `fnmatch(pat, name, FNM_CASEFOLD)` (FindFirstFile shim, kernel32.c:816) | `PathMatchSpecA` (shlwapi) or a 20-line `*`/`?` matcher |
| `strcasecmp`, `strncasecmp` | `_stricmp`, `_strnicmp` |
| `setenv(k, v, 1)` | `_putenv_s(k, v)` |
| `ssize_t`, `useconds_t` | typedef `intptr_t`, `uint32_t` |

`rt_host_path` / `resolve_ci` (kernel32.c:578) maps guest `C:\...` paths to `/`-separated host
paths and resolves case through a directory cache. Windows accepts `/` and NTFS is already
case-insensitive, so add a `_WIN32` fast path where `resolve_ci` is just a `_stat64`.
`g_game_dir` may now contain a drive letter (`D:\...`), so build it with `/` joins only.

### Crash handler (`main.c:196-286`)

The glibc and Android branches use `sigaction`, `siginfo_t`, `ucontext`, `backtrace`,
`dladdr`, `_Unwind_Backtrace`, `sigaltstack` and `gettid`. The Windows version lives in
`runtime/win/win_platform.c`:
- `SetUnhandledExceptionFilter` catches `EXCEPTION_ACCESS_VIOLATION` and friends. (The
  vectored handler in `posix_compat.c` is only for lazy commit of guest memory.)
- Registers come from `CONTEXT` (`X0`-`X28`, `Fp`, `Lr`, `Sp`, `Pc`).
- The backtrace walks frames with `RtlLookupFunctionEntry` + `RtlVirtualUnwind`, and
  `GetModuleHandleExA` gives module+offset. No DbgHelp is needed.
- `SetThreadStackGuarantee` replaces `sigaltstack`, and `GetCurrentThreadId` replaces `gettid`.

### setjmp / longjmp (`setjmp.c`, `lift_prelude.h`): risk to test first

Lifted code calls a host `setjmp` and `rt_longjmp` jumps back across lifted frames. On
ARM64 Windows, UCRT `longjmp` does a full SEH unwind (`RtlUnwindEx`). That works only if every
frame in between has `.pdata`/`.xdata`. Clang emits these by default, but this is the most
likely place for a mysterious crash. If it fails, swap in a tiny non-unwinding ARM64
setjmp/longjmp (save x19-x30, sp and d8-d15; ~30 lines of `.S`) for these two call sites only.

### Window / D3D9 (`d3d9_bridge.cpp`, `platform_sdl.c`, `touch.cpp`)

dxvk-native treats `HWND` as an `SDL_Window*`: `platform_window()` is passed straight to
`CreateDevice` and `hDeviceWindow` (d3d9_bridge.cpp:152 and 280) and is also cast back to
`SDL_Window*` for `SDL_GetWindowSize`.

Split it into `platform_sdl_window()` and `platform_hwnd()`, where `platform_hwnd()` is
`SDL_GetWindowWMInfo(...).info.win.window`. There are two backend options:

1. **Windows' own ARM64 `d3d9.dll`** (D3D9On12 on the GPU's D3D12 driver). Recommended
   first, since it means no dxvk build:
   - Compile the bridge against the SDK's `<d3d9.h>` instead of `-I$(DXVK)/include/native/*`.
   - Link `d3d9.lib`.
   - Drop `SDL_WINDOW_VULKAN`.
   - Stub `dxvk_port_status()` (marvel_shims.c:298), which only feeds the patched dxvk's
     `portstatus` HUD line.
   - Leave `d3d9_bcemu.cpp` off by default. It emulates BC formats for Adreno; desktop GPUs
     have them.
2. **dxvk (the Windows build, not native) for ARM64 over Vulkan.** Keeps parity with the
   phone build and its patches, but means building dxvk 1.9.2b with MSVC for ARM64 and
   re-applying the port's patches. Treat it as a follow-up if option 1 shows rendering
   differences.

### Desktop-only test hooks

`LB_KEYFIFO` (platform_sdl.c:133) opens a FIFO with `O_NONBLOCK` and `fdopen`. Keep it
Linux-only (`#if !defined(_WIN32)`) or reimplement it with a named pipe later.

### Win64 LLP64 notes

`long` is 32-bit on Windows, but runtime uses are safe (kernel32.c:106 `tv_nsec`, main.c:70
`ftell` of a 27 MB exe, msvcrt.c:125 `strtol`). The `%lx` prints in the Android-only crash
code don't matter.

## 3. Build (implemented)

The Linux `Makefile` / `desktop/build.sh` path is unchanged. Windows uses:

1. **SDL2** (once), built from `android/app/jni/SDL2` into `desktop/build/win-sdl2` as a
   shared ARM64 DLL. It needs `-DSDL_LIBC=ON`: without the CRT, clang's implicit
   `strlen`/`wcslen` calls are left unresolved.
   ```
   cmake -S android/app/jni/SDL2 -B desktop/build/win-sdl2 -G Ninja -DCMAKE_TOOLCHAIN_FILE=<absolute path to repo>/desktop/win-arm64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_LIBC=ON
   ninja -C desktop/build/win-sdl2 -j3
   ```
2. **The game:** `desktop/win/CMakeLists.txt` (a standalone project), driven by
   `desktop/build_win.ps1`. It writes to `desktop/build/win`, runs at Idle priority, and takes
   `-Jobs` (default 4), `-Target`, `-Fresh`, `-Reconfigure` and `-ConfigureOnly`.
   - `lb_gen`: an OBJECT library from `gen/*.c`, with `-O1 -g0 -w`.
   - `lb_runtime`: an OBJECT library from `runtime/*.c`, `runtime/win/*.c` and
     `runtime/*.cpp`. It can be built alone (`-Target lb_runtime`) before gen/ exists.
   - `lb.exe`: those plus SDL2 (imported from step 1) and `d3d9`, with `/STACK:8388608`,
     the windowed (GUI) subsystem by default (`-DLB_CONSOLE=ON` for a console build) and
     `/DEBUG` (lb.pdb). SDL2.dll is copied next to it.
     dbghelp, shlwapi and winmm are not needed.
   - Trial link with gen/ empty: the only unresolved symbols are generated ones, namely 29
     `f_<addr>_guest` functions and the `g_fn_table_*` / `g_fn_count_*` tables. No CRT, SDK
     or POSIX name is missing.
   - Don't build `lb` while Stage 1 is still writing gen/: CONFIGURE_DEPENDS would compile
     half-written files.
   - lb.exe needs the ARM64 MSVCP140/VCRUNTIME140 DLLs, which most Windows 11 ARM PCs
     already have; if not, install the ARM64 VC++ redistributable.
3. **Stage 1:** `tools/regen_parallel.ps1 -GameDir GAME` runs the exe chain and the DLL
   chain in parallel. `-ReuseExeDiscovery` keeps an existing `analysis/functions.json`
   (exe discovery took about 33 min on a Windows on ARM laptop, under x64 emulation).

## 4. Status / next

1. Done: toolchain, `runtime/win/` compat layer, HWND split, SDL2, CMake build, runtime
   trial link. An independent review of the compat layer found 11 issues, all fixed:
   - kernel I/O into lazily committed pages (now pre-committed)
   - pread restore race (now locked)
   - open files can be renamed/deleted (FILE_SHARE_DELETE)
   - C++ rename in touch.cpp
   - pre-1970 gmtime_r/timegm
   - st_ino
   - per-thread stack guarantee
   - usleep timer handle leak
   - madvise recommit failure
   - unwinder return-address lookup
   - 1 ms timer resolution

   A regression test passes in both modes, normal and `LB_LAZY_COMMIT=1` (which forces the
   on-demand commit path the game falls back to when the 4 GiB commit fails).
2. Done (2026-11-14): full build (`.\desktop\build_win.ps1 -Jobs 2`, about 10 min), giving
   `lb.exe`, 115.8 MB, ARM64.
   - The build needed `_USE_MATH_DEFINES` (gen/ uses `M_PI`/`M_LN2`).
3. Done: **first run renders.** Run with
   `lb.exe "<Steam library>\steamapps\common\LEGO Marvel Super Heroes" "<save folder>"`
   and `LB_LOG=1`. The D3D9 device, XAudio2 (48 kHz) and XInput all initialize, and the intro
   renders at about 300 fps uncapped.
   - The Steam install has no DirectX DLLs: copy the 32-bit `D3DX9_43.dll` and
     `D3DCompiler_43.dll` (from SysWOW64, SHA-1 `f5944df4` / `98be17e1`) into
     `<game>\redist\`. The real PC game never loads DLLs from that folder.
   - Windows-only bug fixed: `D3DADAPTER_IDENTIFIER9` is 1100 bytes on x86, because
     `d3d9types.h` uses `#pragma pack(4)` only there, but 1104 on ARM64. The real d3d9.dll
     overran the guest's stack buffer by 4 bytes and tripped its /GS cookie (guest
     `TerminateProcess(0xC0000409)`). The bridge now fills a host copy and copies 0x44C bytes.
   - New diagnostics:
     - The guest `TerminateProcess` and `UnhandledExceptionFilter` shims log the caller, the
       guest context and the EBP frame chain.
     - `LB_GPU_DESC` overrides the adapter description the game sees.
4. Done: **installable app.** The app is "LEGO Marvel Super Heroes (ARM 64)": search for it
   in Start, or double-click it.
   - lb.exe is a windowed (GUI-subsystem) app with entry point `mainCRTStartup`, so there
     is no console window. `-DLB_CONSOLE=ON` builds the old console variant.
   - It embeds the game's icon and a version block. `build_win.ps1` extracts the icon from
     your `GAME\LEGOMARVEL.exe` into `desktop\build\win-icon\lb.ico` (never committed);
     `-GameExe` and `-Python` override the defaults.
   - With no arguments, lb.exe reads `lb_paths.ini` next to itself. The section is `[paths]`,
     with keys `game_dir` and `save_dir`, and `%VARS%` are expanded. save_dir defaults to
     `%LOCALAPPDATA%\LEGO Marvel Super Heroes (ARM 64)\save`. Command-line arguments still
     override both.
   - With no console, the log goes to `<save_dir>\log.txt`. Redirected stderr is kept.
     `rt_fatal` also shows a message box.
   - Install: `.\desktop\install_win.ps1 [-GameDir ...] [-SaveDir ...]`. Without `-GameDir`
     it finds the game in your Steam libraries (`libraryfolders.vdf`). It needs no admin
     and writes nothing to the registry. It copies the exe (renamed) and SDL2.dll to
     `%LOCALAPPDATA%\Programs\LEGO Marvel Super Heroes (ARM 64)\`, writes lb_paths.ini, and
     adds a Start Menu shortcut, which `Get-StartApps` lists.
   - Uninstall: `.\desktop\uninstall_win.ps1` removes only that folder and the shortcut, and
     refuses while the game is running. Saves are kept.
   - Verified: launched from the shortcut, it reaches the title screen with no console
     process, and the CLI form still works.
5. Done: **x86 PC-game mods, made native.** The ARM64 app can't load the x86 mod DLLs that
   the PC game uses, so each mod got an ARM64 equivalent:
   - **dgVoodoo2:** ARM64X `MS/arm64x/D3D9.dll` from the dgVoodoo2 2.85+ zip.
     `install_win.ps1 -DgVoodooZip <zip> -DgVoodooConf <your dgVoodoo.conf>` puts it next to
     the exe. The exe imports `d3d9.dll`, so the app folder's copy loads in place of the system
     one; verified with the module list, and it renders through D3D12.
   - **ReShade:** replaced by a built-in pass (`runtime/win/postfx.cpp`): SMAA, then AMD CAS,
     then vibrance, run just before Present.
     - SMAA (`SMAA.fxh` and its lookup textures, MIT, Jimenez et al.) and CAS (AMD, MIT) are
       ported from the SweetFX copies in reshade-shaders, with their notices kept. Vibrance is
       new code.
     - Settings live in `lb_paths.ini [postfx]` (`enabled`, `smaa`, `smaa_edges`, `cas`,
       `cas_sharpness`, `cas_contrast`, `vibrance`, `vibrance_strength`). `LB_POSTFX=0/1`
       overrides `enabled`, and Scroll Lock toggles the pass when the game window has focus.
     - While the pass is enabled, the device is created without `D3DCREATE_PUREDEVICE`, so
       that state save/restore can use Get*.
   - **Borderless:** `[display] window=borderless` gives a DPI-aware, desktop-sized
     `SDL_WINDOW_FULLSCREEN_DESKTOP` window that doesn't minimize on focus loss.
     - Window, mode-list and back-buffer sizes now use `SDL_GetWindowSizeInPixels`. Before
       that, a 250% display reported 1536x960 points, and the game fell back to a 1152x720
       back buffer.
   - **Settings and saves:** the Windows build no longer moves `pcconfig.txt` aside on the
     first launch (that was for phones). Steam Cloud saves live in
     `<save>\steamcloud\<path with / turned into _>`; for example, Steam's
     `remote\savedgames\slot1\game1.legomarvelsuperheroessavegamedata` becomes
     `steamcloud\savedgames_slot1_game1.legomarvelsuperheroessavegamedata`.
   - **Own game copy:** the installer prefers `%LOCALAPPDATA%\LEGO Marvel Super Heroes (ARM 64)\game`,
     so tools that rewrite the `.DAT`s (CMM) never touch the Steam install.
6. Next: play-test (menus, input, audio, saves, a level), and look for any other structs
   written directly into guest memory with 8-byte members.
