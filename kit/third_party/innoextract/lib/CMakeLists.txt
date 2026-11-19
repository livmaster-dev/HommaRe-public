# innoextract 1.9 as a static library for the iOS launcher (recomp_innoextract.h),
# with the parts of Boost it needs compiled in. Configured by build-ios.sh,
# which fetches the sources and passes their folders:
#   INNOEXTRACT_SRC  innoextract-1.9 (patched)
#   BOOST_SRC        boost_1_89_0 (headers and libs/{filesystem,iostreams}/src)
#   LZMA_INCLUDE     XZ Utils' src/liblzma/api (liblzma itself is built apart)
# zlib, bzip2 and libiconv are the system's.
#
# With RECOMP_INNOEXTRACT_PROGRAM, innoextract's own command line instead
# (src/cli/main.cpp), for Android, whose launcher runs it as the Linux
# launcher runs the system's (build-android.sh). Android has no libbz2, so
# bzip2 is compiled in from BZIP2_SRC, and Boost.Program_Options with the rest
# of Boost (libs/program_options/src); LZMA_LIBRARY is the liblzma built apart.
cmake_minimum_required(VERSION 3.24)
project(recomp_innoextract LANGUAGES C CXX)
option(RECOMP_INNOEXTRACT_PROGRAM "Build innoextract's command line, not the library" OFF)

# C++20 for Boost.Filesystem's std::atomic_ref.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(src ${INNOEXTRACT_SRC}/src)
set(innoextract_sources
  cli/extract.cpp cli/gog.cpp cli/goggalaxy.cpp
  crypto/adler32.cpp crypto/arc4.cpp crypto/checksum.cpp crypto/crc32.cpp crypto/hasher.cpp
  crypto/md5.cpp crypto/sha1.cpp
  loader/exereader.cpp loader/offsets.cpp
  setup/component.cpp setup/data.cpp setup/delete.cpp setup/directory.cpp setup/expression.cpp
  setup/file.cpp setup/filename.cpp setup/header.cpp setup/icon.cpp setup/info.cpp setup/ini.cpp
  setup/item.cpp setup/language.cpp setup/message.cpp setup/permission.cpp setup/registry.cpp
  setup/run.cpp setup/task.cpp setup/type.cpp setup/version.cpp setup/windows.cpp
  stream/block.cpp stream/chunk.cpp stream/file.cpp stream/lzma.cpp stream/slice.cpp
  util/console.cpp util/encoding.cpp util/load.cpp util/log.cpp util/time.cpp)
list(TRANSFORM innoextract_sources PREPEND ${src}/)

# Boost.Filesystem and the zlib and bzip2 filters of Boost.Iostreams; the
# rest innoextract uses (Date_Time, System, Range, ...) is header-only.
set(boost_sources
  ${BOOST_SRC}/libs/filesystem/src/codecvt_error_category.cpp
  ${BOOST_SRC}/libs/filesystem/src/directory.cpp
  ${BOOST_SRC}/libs/filesystem/src/exception.cpp
  ${BOOST_SRC}/libs/filesystem/src/operations.cpp
  ${BOOST_SRC}/libs/filesystem/src/path.cpp
  ${BOOST_SRC}/libs/filesystem/src/path_traits.cpp
  ${BOOST_SRC}/libs/filesystem/src/portability.cpp
  ${BOOST_SRC}/libs/filesystem/src/unique_path.cpp
  ${BOOST_SRC}/libs/filesystem/src/utf8_codecvt_facet.cpp
  ${BOOST_SRC}/libs/iostreams/src/bzip2.cpp
  ${BOOST_SRC}/libs/iostreams/src/file_descriptor.cpp
  ${BOOST_SRC}/libs/iostreams/src/zlib.cpp)

if(RECOMP_INNOEXTRACT_PROGRAM)
  file(GLOB program_options_sources ${BOOST_SRC}/libs/program_options/src/*.cpp)
  list(FILTER program_options_sources EXCLUDE REGEX "winmain\\.cpp$")
  add_library(recomp_bz2 STATIC
    ${BZIP2_SRC}/blocksort.c ${BZIP2_SRC}/huffman.c ${BZIP2_SRC}/crctable.c ${BZIP2_SRC}/randtable.c
    ${BZIP2_SRC}/compress.c ${BZIP2_SRC}/decompress.c ${BZIP2_SRC}/bzlib.c)
  target_include_directories(recomp_bz2 PUBLIC ${BZIP2_SRC})
  target_compile_options(recomp_bz2 PRIVATE -O2 -w)
  add_executable(innoextract
    ${src}/cli/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/release.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/process.cpp
    ${innoextract_sources}
    ${boost_sources}
    ${program_options_sources})
  target_include_directories(innoextract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${src})
  target_include_directories(innoextract SYSTEM PRIVATE ${BOOST_SRC} ${LZMA_INCLUDE})
  # innoextract runs on one thread; the NDK's libc++ has no std::atomic_ref,
  # which Boost.Filesystem otherwise guards its few shared values with.
  target_compile_definitions(innoextract PRIVATE
    BOOST_ALL_NO_LIB BOOST_FILESYSTEM_STATIC_LINK=1 BOOST_IOSTREAMS_STATIC_LINK=1
    BOOST_PROGRAM_OPTIONS_STATIC_LINK=1 BOOST_FILESYSTEM_NO_DEPRECATED LZMA_API_STATIC
    BOOST_FILESYSTEM_SINGLE_THREADED)
  set_source_files_properties(${boost_sources} ${program_options_sources} PROPERTIES
    COMPILE_DEFINITIONS "BOOST_FILESYSTEM_SOURCE;BOOST_IOSTREAMS_SOURCE;BOOST_PROGRAM_OPTIONS_SOURCE")
  target_compile_options(innoextract PRIVATE -O2 -w)
  target_link_libraries(innoextract PRIVATE ${LZMA_LIBRARY} recomp_bz2 z)
  return()
endif()

add_library(recomp_innoextract STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/recomp_innoextract.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/release.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/process.cpp
  ${innoextract_sources}
  ${boost_sources})
target_include_directories(recomp_innoextract PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${src})
target_include_directories(recomp_innoextract SYSTEM PRIVATE ${BOOST_SRC} ${LZMA_INCLUDE})
target_compile_definitions(recomp_innoextract PRIVATE
  # Boost built into this library, without auto-linking or DLL markup.
  BOOST_ALL_NO_LIB BOOST_FILESYSTEM_STATIC_LINK=1 BOOST_IOSTREAMS_STATIC_LINK=1
  BOOST_FILESYSTEM_NO_DEPRECATED LZMA_API_STATIC)
# Boost's own sources build the library (its headers then define the symbols
# its users call through versioned names).
set_source_files_properties(${boost_sources} PROPERTIES
  COMPILE_DEFINITIONS "BOOST_FILESYSTEM_SOURCE;BOOST_IOSTREAMS_SOURCE")
target_compile_options(recomp_innoextract PRIVATE -O2 -w -fvisibility=hidden -fvisibility-inlines-hidden)
# Only the C entry point is visible.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/recomp_innoextract.cpp PROPERTIES
  COMPILE_OPTIONS "-fvisibility=default")
