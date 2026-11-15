/* Windows-only platform pieces: crash report, native HWND for D3D9, and
 * stubs for hooks that only exist in the patched dxvk-native build. */
#define POSIX_COMPAT_IMPL
#include "posix_compat.h"
#include <windows.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

extern uint8_t *MEM;
void rt_dump_guest(void);
void *platform_window(void);

/* ---------------------------------------------------------------- HWND */
/* dxvk-native takes the SDL_Window* as its HWND; the system d3d9.dll needs
 * the real window handle. */
void *platform_hwnd(void) {
    SDL_Window *w = (SDL_Window *)platform_window();
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (w && SDL_GetWindowWMInfo(w, &info) && info.subsystem == SDL_SYSWM_WINDOWS) return info.info.win.window;
    return NULL;
}

/* ---------------------------------------------------------------- app defaults */
#define APP_NAME "LEGO Marvel Super Heroes (ARM 64)"

static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp; *q; q++) {
        if ((*q == '\\' || *q == '/') && q > tmp && q[-1] != ':') {
            char ch = *q;
            *q = 0;
            CreateDirectoryA(tmp, NULL);
            *q = ch;
        }
    }
    CreateDirectoryA(tmp, NULL);
}

static int ini_path(char *ini, size_t sz) {
    DWORD n = GetModuleFileNameA(NULL, ini, (DWORD)sz);
    if (!n || n >= sz) return 0;
    char *slash = strrchr(ini, '\\');
    if (!slash || (size_t)(slash - ini) + 16 >= sz) return 0;
    strcpy(slash + 1, "lb_paths.ini");
    return GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
}

int win_ini_get(const char *section, const char *key, char *out, size_t out_sz) {
    char ini[MAX_PATH], raw[1024];
    if (!out_sz || !ini_path(ini, sizeof ini)) return 0;
    if (!GetPrivateProfileStringA(section, key, "", raw, sizeof raw, ini) || !raw[0]) return 0;
    ExpandEnvironmentStringsA(raw, out, (DWORD)out_sz);
    return out[0] != 0;
}

void win_default_paths(char *game, size_t game_sz, char *save, size_t save_sz) {
    char ini[MAX_PATH];
    if (!ini_path(ini, sizeof ini)) return;
    char raw[1024];
    if (GetPrivateProfileStringA("paths", "game_dir", "", raw, sizeof raw, ini) && raw[0])
        ExpandEnvironmentStringsA(raw, game, (DWORD)game_sz);
    if (GetPrivateProfileStringA("paths", "save_dir", "", raw, sizeof raw, ini) && raw[0])
        ExpandEnvironmentStringsA(raw, save, (DWORD)save_sz);
    else
        ExpandEnvironmentStringsA("%LOCALAPPDATA%\\" APP_NAME "\\save", save, (DWORD)save_sz);
}

void win_setup_log(const char *save_dir) {
    /* A console build, or a GUI build started with redirected stderr, keeps its stderr. */
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    mkdirs(save_dir);
    char path[1100];
    snprintf(path, sizeof path, "%s\\log.txt", save_dir);
    if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 4096);
}

void win_fatal_box(const char *title, const char *msg) {
    MessageBoxA(NULL, msg, title, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

/* The patched dxvk shows this on its HUD ("portstatus"); nothing to show it on here. */
void dxvk_port_status(const char *text) { (void)text; }

/* ---------------------------------------------------------------- crash */
static void print_pc(int i, DWORD64 pc) {
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)pc, &mod) && mod) {
        GetModuleFileNameA(mod, name, sizeof name);
        const char *b = strrchr(name, '\\');
        fprintf(stderr, "  #%02d %s+0x%llx\n", i, b ? b + 1 : name,
                (unsigned long long)(pc - (DWORD64)(uintptr_t)mod));
    } else {
        fprintf(stderr, "  #%02d 0x%llx\n", i, (unsigned long long)pc);
    }
}

static LONG WINAPI on_crash(PEXCEPTION_POINTERS ep) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    fprintf(stderr, "CRASH: exception 0x%08lx at %p (thread %lu)", er->ExceptionCode, er->ExceptionAddress,
            GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        uintptr_t a = (uintptr_t)er->ExceptionInformation[1];
        fprintf(stderr, ", %s %p", er->ExceptionInformation[0] == 1 ? "writing" : "reading", (void *)a);
        if (MEM && a >= (uintptr_t)MEM && a < (uintptr_t)MEM + (1ull << 32))
            fprintf(stderr, " (guest 0x%08llx)", (unsigned long long)(a - (uintptr_t)MEM));
    }
    fputc('\n', stderr);
#if defined(_M_ARM64) || defined(__aarch64__)
    CONTEXT ctx = *ep->ContextRecord;
    fprintf(stderr, "  pc %016llx lr %016llx sp %016llx fp %016llx\n", ctx.Pc, ctx.Lr, ctx.Sp, ctx.Fp);
    for (int r = 0; r < 29; r += 4) {
        fprintf(stderr, "  x%-2d", r);
        for (int k = r; k < r + 4 && k < 29; k++) fprintf(stderr, " %016llx", ctx.X[k]);
        fputc('\n', stderr);
    }
    fprintf(stderr, "  guest mem base %p\n", (void *)MEM);
    if (MEM) rt_dump_guest();
    fprintf(stderr, "backtrace:\n");
    for (int i = 0; i < 48 && ctx.Pc; i++) {
        print_pc(i, ctx.Pc);
        DWORD64 base = 0;
        /* Caller frames hold return addresses; after a call to a noreturn function at
         * the end of a function that address belongs to the next function, so look up
         * the call instruction itself. */
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(i ? ctx.Pc - 4 : ctx.Pc, &base, NULL);
        if (!fe) { ctx.Pc = ctx.Lr; ctx.Lr = 0; continue; } /* leaf function */
        PVOID hd = NULL;
        DWORD64 est = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Pc, fe, &ctx, &hd, &est, NULL);
    }
#endif
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Room to run the crash handler after a stack overflow (sigaltstack
 * equivalent). Per thread: rt_altstack() calls it for every guest thread. */
void win_thread_stack_guarantee(void) {
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
}

#include <timeapi.h>
#pragma comment(lib, "winmm")

void win_install_crash_handler(void) {
    SetUnhandledExceptionFilter(on_crash);
    win_thread_stack_guarantee();
    /* Timed waits (SleepConditionVariableSRW, WaitForSingleObject(h, 1) loops in the
     * guest) follow the system tick: ask for 1 ms rather than rely on SDL doing it. */
    timeBeginPeriod(1);
}
