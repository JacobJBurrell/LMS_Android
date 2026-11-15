# Shared compiler flags for the native Windows ARM64 build (sourced by the
# desktop Windows scripts). Toolchain: LLVM in $LLVM, MSVC CRT + SDK from
# xwin in $XWIN. See desktop/WINDOWS_ARM64.md.
_UP=${USERPROFILE//\\//}                 # %USERPROFILE% with forward slashes
LLVM=${LLVM:-${LLVM_ROOT:-$_UP/tools/llvm}}
XWIN=${XWIN:-${XWIN_ROOT:-$_UP/tools/xwin}}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CC="$LLVM/bin/clang.exe"
CXX="$LLVM/bin/clang++.exe"
TARGET="--target=aarch64-pc-windows-msvc"
SYSINC="-idirafter $XWIN/crt/include -idirafter $XWIN/sdk/include/ucrt -idirafter $XWIN/sdk/include/um -idirafter $XWIN/sdk/include/shared"
DEFS="-DGAME_MARVEL -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DSDL_MAIN_HANDLED -D_USE_MATH_DEFINES"
INCS="-I$ROOT/runtime/win/include -I$ROOT/runtime -I$ROOT/gen -I$ROOT/android/app/jni/sdlinc"
COMMON="$TARGET $DEFS $INCS $SYSINC -fno-strict-aliasing -fwrapv"
RT_CFLAGS="$COMMON -std=gnu11 -O2 -g -Wall -Wno-unused-function"
RT_CXXFLAGS="$COMMON -std=c++17 -O2 -g -Wno-everything"
GEN_CFLAGS="$COMMON -std=gnu11 -O1 -g0 -w"
LIBPATHS="-Xlinker /libpath:$XWIN/crt/lib/aarch64 -Xlinker /libpath:$XWIN/sdk/lib/um/aarch64 -Xlinker /libpath:$XWIN/sdk/lib/ucrt/aarch64"
