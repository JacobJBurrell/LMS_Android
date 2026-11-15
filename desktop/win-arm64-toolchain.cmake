# CMake toolchain: native Windows ARM64 (aarch64-pc-windows-msvc) with a
# standalone LLVM and the MSVC CRT + Windows SDK fetched by xwin.
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=desktop/win-arm64-toolchain.cmake ...
# Locations: -DLLVM_ROOT=... -DXWIN_ROOT=..., or the LLVM_ROOT / XWIN_ROOT environment
# variables; the default is %USERPROFILE%\tools\llvm and %USERPROFILE%\tools\xwin.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)

foreach(_v LLVM XWIN)
  string(TOLOWER "${_v}" _sub)
  if(DEFINED ENV{${_v}_ROOT})
    file(TO_CMAKE_PATH "$ENV{${_v}_ROOT}" _default_${_v})
  else()
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}/tools/${_sub}" _default_${_v})
  endif()
endforeach()
set(LLVM_ROOT "${_default_LLVM}" CACHE PATH "Standalone LLVM (aarch64-pc-windows-msvc build)")
set(XWIN_ROOT "${_default_XWIN}" CACHE PATH "xwin splat output (crt/ and sdk/)")

set(CMAKE_C_COMPILER "${LLVM_ROOT}/bin/clang.exe")
set(CMAKE_CXX_COMPILER "${LLVM_ROOT}/bin/clang++.exe")
set(CMAKE_RC_COMPILER "${LLVM_ROOT}/bin/llvm-rc.exe")
set(CMAKE_LINKER "${LLVM_ROOT}/bin/lld-link.exe")
# GNU-style clang driver => CMake drives the archiver with ar syntax (qc), so llvm-ar, not llvm-lib
set(CMAKE_AR "${LLVM_ROOT}/bin/llvm-ar.exe")
set(CMAKE_RANLIB "${LLVM_ROOT}/bin/llvm-ranlib.exe")
set(CMAKE_C_COMPILER_TARGET aarch64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET aarch64-pc-windows-msvc)

set(_xwin_inc "-idirafter ${XWIN_ROOT}/crt/include -idirafter ${XWIN_ROOT}/sdk/include/ucrt -idirafter ${XWIN_ROOT}/sdk/include/um -idirafter ${XWIN_ROOT}/sdk/include/shared")
set(CMAKE_C_FLAGS_INIT "${_xwin_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_xwin_inc}")
set(CMAKE_RC_FLAGS_INIT "-I ${XWIN_ROOT}/crt/include -I ${XWIN_ROOT}/sdk/include/ucrt -I ${XWIN_ROOT}/sdk/include/um -I ${XWIN_ROOT}/sdk/include/shared")

set(_xwin_lib "-fuse-ld=lld -Xlinker /libpath:${XWIN_ROOT}/crt/lib/aarch64 -Xlinker /libpath:${XWIN_ROOT}/sdk/lib/um/aarch64 -Xlinker /libpath:${XWIN_ROOT}/sdk/lib/ucrt/aarch64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_xwin_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_lib}")

# xwin fetches only the release CRT (no msvcrtd.lib): always link /MD, including try-compiles.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_FIND_ROOT_PATH "${XWIN_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
