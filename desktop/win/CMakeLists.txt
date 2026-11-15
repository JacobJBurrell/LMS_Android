# Native Windows 11 ARM64 build of the LEGO Marvel Super Heroes recompilation: lb.exe.
# Standalone project; normally driven by desktop/build_win.ps1:
#   cmake -G Ninja -S desktop/win -B desktop/build/win \
#         -DCMAKE_TOOLCHAIN_FILE=<abs>/desktop/win-arm64-toolchain.cmake
#   ninja -C desktop/build/win lb_runtime   # runtime only (no gen/ needed)
#   ninja -C desktop/build/win -j4           # everything -> lb.exe
# Flags mirror desktop/win_flags.sh exactly; see desktop/WINDOWS_ARM64.md.
cmake_minimum_required(VERSION 3.20)
project(lb_win C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "desktop/win is the Windows build; use the root Makefile on Linux.")
endif()

get_filename_component(LB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." REALPATH)
set(LB_SDL2_DIR "${LB_ROOT}/desktop/build/win-sdl2" CACHE PATH
    "Existing SDL2 build directory holding SDL2.lib (import library) and SDL2.dll")
set(LB_EXTRA_LIBS "" CACHE STRING "Extra libraries for lb.exe (;-list, e.g. dbghelp;shlwapi;winmm)")
set(LB_EXTRA_LINK_OPTIONS "" CACHE STRING "Extra link options for lb.exe (;-list, e.g. LINKER:/FORCE:UNRESOLVED)")

# The per-target options below are the whole story: drop CMake's per-config
# defaults (-O3 -DNDEBUG / -O0 -g ...) so nothing is added behind our back.
# The Linux build does not define NDEBUG either.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (per-config flags are cleared)" FORCE)
endif()
foreach(_lang C CXX)
  foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_${_lang}_FLAGS_${_cfg} "")
  endforeach()
endforeach()

# ------------------------------------------------------------------ flags
set(LB_DEFINES GAME_MARVEL _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE NOMINMAX
               WIN32_LEAN_AND_MEAN SDL_MAIN_HANDLED
               _USE_MATH_DEFINES)  # UCRT math.h only defines M_PI/M_LN2 etc. with this (glibc always does)
# The xwin CRT/SDK headers come from the toolchain's CMAKE_<LANG>_FLAGS_INIT as
# -idirafter (not -isystem: MSVC's arm_neon.h would shadow clang's), so they are
# searched after everything below and after clang's own resource headers.
# Order matters: the POSIX shim headers must shadow anything else.
set(LB_INCLUDES "${LB_ROOT}/runtime/win/include" "${LB_ROOT}/runtime" "${LB_ROOT}/gen"
                "${LB_ROOT}/android/app/jni/sdlinc")
set(LB_COMMON_FLAGS -fno-strict-aliasing -fwrapv)
set(LB_GEN_CFLAGS -std=gnu11 -O1 -g0 -w)
set(LB_RT_CFLAGS -std=gnu11 -O2 -g -Wall -Wno-unused-function)
set(LB_RT_CXXFLAGS -std=c++17 -O2 -g -Wno-everything)

# ------------------------------------------------------------------ gen/ (lifted code)
# gen/ is written by Stage 1 and may be empty or partial; CONFIGURE_DEPENDS
# re-globs on every build so new files are picked up without a manual re-configure.
file(GLOB LB_GEN_SRC CONFIGURE_DEPENDS "${LB_ROOT}/gen/*.c")
list(LENGTH LB_GEN_SRC LB_GEN_COUNT)
if(LB_GEN_COUNT EQUAL 0)
  # An OBJECT library needs at least one source; keep the configure working.
  set(_ph "${CMAKE_CURRENT_BINARY_DIR}/gen_placeholder.c")
  if(NOT EXISTS "${_ph}")
    file(WRITE "${_ph}" "/* gen/ is empty: placeholder so lb_gen has a source. */\ntypedef int lb_gen_placeholder;\n")
  endif()
  set(LB_GEN_SRC "${_ph}")
  message(WARNING "gen/ has no .c files yet: lb_gen is a placeholder and lb.exe will not link.")
else()
  message(STATUS "gen/: ${LB_GEN_COUNT} generated source files")
endif()

add_library(lb_gen OBJECT ${LB_GEN_SRC})
target_compile_definitions(lb_gen PRIVATE ${LB_DEFINES})
target_include_directories(lb_gen PRIVATE ${LB_INCLUDES})
target_compile_options(lb_gen PRIVATE ${LB_COMMON_FLAGS} ${LB_GEN_CFLAGS})

# ------------------------------------------------------------------ runtime
file(GLOB LB_RT_C CONFIGURE_DEPENDS "${LB_ROOT}/runtime/*.c" "${LB_ROOT}/runtime/win/*.c")
file(GLOB LB_RT_CXX CONFIGURE_DEPENDS "${LB_ROOT}/runtime/*.cpp" "${LB_ROOT}/runtime/win/*.cpp")

# OBJECT (not STATIC) so every runtime object reaches the link, as in the Makefile.
add_library(lb_runtime OBJECT ${LB_RT_C} ${LB_RT_CXX})
target_compile_definitions(lb_runtime PRIVATE ${LB_DEFINES})
target_include_directories(lb_runtime PRIVATE ${LB_INCLUDES})
target_compile_options(lb_runtime PRIVATE
  ${LB_COMMON_FLAGS}
  $<$<COMPILE_LANGUAGE:C>:${LB_RT_CFLAGS}>
  $<$<COMPILE_LANGUAGE:CXX>:${LB_RT_CXXFLAGS}>)

# ------------------------------------------------------------------ SDL2 (prebuilt, not rebuilt here)
set(LB_SDL2_LIB "${LB_SDL2_DIR}/SDL2.lib")
set(LB_SDL2_DLL "${LB_SDL2_DIR}/SDL2.dll")
if(NOT EXISTS "${LB_SDL2_LIB}" OR NOT EXISTS "${LB_SDL2_DLL}")
  message(WARNING "SDL2.lib/SDL2.dll not found in ${LB_SDL2_DIR} yet; lb_runtime still builds, "
                  "lb.exe needs them (re-run the build once the SDL2 build has finished).")
endif()
add_library(lb_sdl2 SHARED IMPORTED)
set_target_properties(lb_sdl2 PROPERTIES
  IMPORTED_IMPLIB "${LB_SDL2_LIB}"
  IMPORTED_LOCATION "${LB_SDL2_DLL}")

# ------------------------------------------------------------------ lb.exe
option(LB_CONSOLE "Console subsystem (log to the console) instead of a windowed app" OFF)
# Icon extracted by build_win.ps1 from your own LEGOMARVEL.exe (game asset: build dir only).
set(LB_ICON "${LB_ROOT}/desktop/build/win-icon/lb.ico" CACHE FILEPATH "Icon to embed in lb.exe")

add_executable(lb $<TARGET_OBJECTS:lb_gen> $<TARGET_OBJECTS:lb_runtime>)
set_target_properties(lb PROPERTIES LINKER_LANGUAGE CXX OUTPUT_NAME lb)
target_link_libraries(lb PRIVATE lb_sdl2 d3d9 ${LB_EXTRA_LIBS})
if(LB_CONSOLE)
  # CMake adds -Xlinker /subsystem:console for a non-WIN32 executable.
  message(STATUS "lb.exe: console subsystem")
else()
  # Windowed app (no console window); keep main() as the entry point. With no console,
  # the runtime logs to <save_dir>\log.txt (runtime/win/win_platform.c).
  set_target_properties(lb PROPERTIES WIN32_EXECUTABLE ON)
  target_link_options(lb PRIVATE "LINKER:/ENTRY:mainCRTStartup")
  message(STATUS "lb.exe: windows (GUI) subsystem")
endif()
if(EXISTS "${LB_ICON}")
  enable_language(RC)
  file(TO_CMAKE_PATH "${LB_ICON}" LB_ICON_RC)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/lb.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/lb.rc" @ONLY)
  target_sources(lb PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/lb.rc")
  message(STATUS "lb.exe: embedding icon ${LB_ICON}")
else()
  message(STATUS "lb.exe: no icon (${LB_ICON} not found; build_win.ps1 extracts it from LEGOMARVEL.exe)")
endif()
target_link_options(lb PRIVATE
  "LINKER:/STACK:8388608"      # 8 MB main stack, like Linux (Windows default is 1 MB)
  "LINKER:/DEBUG"              # lb.pdb for the runtime (gen/ is -g0)
  ${LB_EXTRA_LINK_OPTIONS})
add_custom_command(TARGET lb POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LB_SDL2_DLL}" "$<TARGET_FILE_DIR:lb>"
  COMMENT "Copying SDL2.dll next to lb.exe")
