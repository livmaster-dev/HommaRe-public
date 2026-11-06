# llvm-mingw.cmake - cross-compile the Windows build from macOS or Linux with
# llvm-mingw (https://github.com/mstorsjo/llvm-mingw): clang, lld and the
# MinGW-w64 UCRT runtime.
#
#   cmake --preset windows-cross -DLLVM_MINGW_ROOT=/path/to/llvm-mingw
#
# LLVM_MINGW_ROOT may also come from the environment. RECOMP_WINDOWS_ARCH
# picks x86_64 (the default) or aarch64.
set(CMAKE_SYSTEM_NAME Windows)
if(NOT RECOMP_WINDOWS_ARCH)
  set(RECOMP_WINDOWS_ARCH x86_64)
endif()
set(CMAKE_SYSTEM_PROCESSOR ${RECOMP_WINDOWS_ARCH})
if(NOT LLVM_MINGW_ROOT)
  set(LLVM_MINGW_ROOT $ENV{LLVM_MINGW_ROOT})
endif()
if(NOT LLVM_MINGW_ROOT)
  message(FATAL_ERROR "llvm-mingw.cmake: set LLVM_MINGW_ROOT to the llvm-mingw directory")
endif()
# Cached, so the reconfigure a build runs by itself finds the toolchain again
# without the environment, and handed on to try_compile's projects.
set(LLVM_MINGW_ROOT ${LLVM_MINGW_ROOT} CACHE PATH "The llvm-mingw directory")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW_ROOT)
set(_triple ${RECOMP_WINDOWS_ARCH}-w64-mingw32)
set(CMAKE_C_COMPILER ${LLVM_MINGW_ROOT}/bin/${_triple}-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW_ROOT}/bin/${_triple}-clang++)
set(CMAKE_RC_COMPILER ${LLVM_MINGW_ROOT}/bin/${_triple}-windres)
set(CMAKE_AR ${LLVM_MINGW_ROOT}/bin/llvm-ar)
set(CMAKE_RANLIB ${LLVM_MINGW_ROOT}/bin/llvm-ranlib)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW_ROOT}/${_triple})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Keep compiler/runtime libraries static, in a game module's DLL too, so the
# app needs no libc++ or libunwind DLL beside it. FFmpeg uses explicit import
# libraries and ships as replaceable DLLs beside the app when RECOMP_VIDEO is
# enabled; SDL3.dll is the one shared library of a multi-game app.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static")
