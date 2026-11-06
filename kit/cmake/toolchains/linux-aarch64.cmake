# linux-aarch64.cmake - cross-compile the Linux build for 64-bit ARM
# (aarch64: ARM Chromebooks' Linux environment, Snapdragon X laptops, Asahi
# Linux, Raspberry Pi 5) from an x86-64 Linux host, with the host's clang and
# lld and a sysroot of the target's headers and libraries.
#
#   cmake --preset linux-arm64 -DRECOMP_LINUX_SYSROOT=/path/to/sysroot
#
# RECOMP_LINUX_SYSROOT may also come from the environment; tools/build.py
# --arch arm64 passes the one tools/linux_sysroot.py makes (a Debian 12
# image's /usr, so the binaries run on Debian 12 and anything newer; HommaRe
# needs glibc 2.35 and GCC 12's libstdc++, so Ubuntu 22.04 too). The x86-64
# host's own GNU ar and ranlib read any
# ELF's symbols, so no llvm-ar is needed; nothing is stripped.
#
# With qemu-user installed (qemu-aarch64 or qemu-aarch64-static), test
# binaries run through it: ctest uses CMAKE_CROSSCOMPILING_EMULATOR, and the
# sysroot holds the target's loader and libraries.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(_triple aarch64-linux-gnu)

if(NOT RECOMP_LINUX_SYSROOT)
  set(RECOMP_LINUX_SYSROOT $ENV{RECOMP_LINUX_SYSROOT})
endif()
if(NOT RECOMP_LINUX_SYSROOT OR NOT EXISTS ${RECOMP_LINUX_SYSROOT}/usr/include/${_triple})
  message(FATAL_ERROR "linux-aarch64.cmake: set RECOMP_LINUX_SYSROOT to an aarch64 sysroot "
                      "(tools/linux_sysroot.py makes one); got '${RECOMP_LINUX_SYSROOT}'")
endif()
# try_compile projects re-read this file without the cache: hand it on.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RECOMP_LINUX_SYSROOT)
set(ENV{RECOMP_LINUX_SYSROOT} ${RECOMP_LINUX_SYSROOT})

set(CMAKE_SYSROOT ${RECOMP_LINUX_SYSROOT})
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
# The host's ld knows only x86; lld links every architecture. Its aarch64
# default aligns segments to 64 KiB, so the binaries load on 4, 16 and 64 KiB
# page kernels alike.
foreach(kind EXE SHARED MODULE)
  set(CMAKE_${kind}_LINKER_FLAGS_INIT "-fuse-ld=lld")
endforeach()

set(CMAKE_FIND_ROOT_PATH ${RECOMP_LINUX_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# SDL's configure asks pkg-config for the X11, Wayland and audio headers: the
# sysroot's .pc files, with their paths moved into the sysroot. Tools such as
# wayland-scanner stay the host's (PROGRAM NEVER above).
set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR}
    "${RECOMP_LINUX_SYSROOT}/usr/lib/${_triple}/pkgconfig:${RECOMP_LINUX_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${RECOMP_LINUX_SYSROOT})

# QEMU_LD_PREFIX goes into the guest's environment as well: a test that runs
# itself again as a child (seh_tests, platform_tests) starts it through the
# kernel's binfmt_misc qemu, which finds the sysroot only from there.
find_program(RECOMP_QEMU_AARCH64 NAMES qemu-aarch64 qemu-aarch64-static)
if(RECOMP_QEMU_AARCH64)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${RECOMP_QEMU_AARCH64} -L ${RECOMP_LINUX_SYSROOT}
      -E QEMU_LD_PREFIX=${RECOMP_LINUX_SYSROOT})
endif()
