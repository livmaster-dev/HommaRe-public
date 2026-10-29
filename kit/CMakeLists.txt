cmake_minimum_required(VERSION 3.24)
# The Mac app runs on macOS 14 (Sonoma) and later, the oldest the SwiftUI
# launcher builds for (@Observable). Without it the compiler targets the SDK's
# own version. SDL, FFmpeg and the innoextract helper
# (third_party/innoextract/build.sh) follow it; iOS presets set their own.
# Must precede project().
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME AND NOT CMAKE_TOOLCHAIN_FILE
    AND NOT CMAKE_OSX_DEPLOYMENT_TARGET AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Oldest macOS the app runs on" FORCE)
endif()
project(PopulousRecomp LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()
# The SwiftUI launcher (host/launcher/apple/) on macOS and iOS, the same one
# drawn by the host on Linux (host/launcher/linux/); the canvas launcher
# everywhere else and with this off.
option(RECOMP_NATIVE_LAUNCHER "Build the native launcher (SwiftUI on Apple platforms, the host's on Linux)" ON)
# Several games in one app (Apple, Linux): each game also builds as a module,
# libgame.<id>.dylib (.so) with recomp_game_main for an entry, and the app the
# launcher runs in loads the module of the game the player picks
# (host/sdl/main.cpp run_game_module, tools/assemble_games.py). Modules and the
# app share one SDL, so SDL links as a shared library in this mode.
option(RECOMP_GAME_MODULES "Build the game as a loadable module and link SDL shared (Apple, Linux)" OFF)
if(RECOMP_GAME_MODULES AND NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "RECOMP_GAME_MODULES is for Apple and Linux builds")
endif()
if(APPLE AND RECOMP_NATIVE_LAUNCHER)
  if(IOS AND NOT CMAKE_GENERATOR STREQUAL "Xcode" AND NOT CMAKE_Swift_COMPILER_TARGET)
    set(CMAKE_Swift_COMPILER_TARGET "arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
  elseif(NOT IOS AND CMAKE_OSX_DEPLOYMENT_TARGET AND NOT CMAKE_Swift_COMPILER_TARGET)
    set(CMAKE_Swift_COMPILER_TARGET "${CMAKE_SYSTEM_PROCESSOR}-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
  # Swift 5 for every target. Xcode 16 otherwise gets CMake's default, 4,
  # after the targets' own -swift-version 5, and the launcher fails there.
  set(CMAKE_Swift_LANGUAGE_VERSION 5.0)
  enable_language(Swift)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
if(EMSCRIPTEN)
  # EM_ASM and EM_JS, in SDL and in the web host, need the GNU dialects.
  set(CMAKE_C_EXTENSIONS ON)
  set(CMAKE_CXX_EXTENSIONS ON)
  # WebAssembly's tail calls (return_call): the translation continues a guest
  # jump into another function with JUMP_FN, a call in tail position that a
  # native build makes a jump. Without them each one grows the browser's
  # stack, and a loop spread over two functions overflows it.
  add_compile_options(-mtail-call)
  add_link_options(-mtail-call)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(ANDROID OR (RECOMP_GAME_MODULES AND CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  # All static and object libraries feed the shared SDL application (Android)
  # or a game module (Linux; Apple's code is position independent already).
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
# $<LINK_LIBRARY:AS_NEEDED,lib>: a library recorded only when something
# links against it (mods/CMakeLists.txt: libatomic, which an optimised build
# rarely calls and a desktop may not have installed).
set(CMAKE_LINK_LIBRARY_USING_AS_NEEDED "LINKER:--push-state,--as-needed" "<LINK_ITEM>" "LINKER:--pop-state")
set(CMAKE_LINK_LIBRARY_USING_AS_NEEDED_SUPPORTED TRUE)

# Optimization is a per-target decision (cmake/Warnings.cmake). The
# configuration only decides whether -O0 is forced.
foreach(lang C CXX OBJCXX)
  set(CMAKE_${lang}_FLAGS_RELEASE "-g")
  set(CMAKE_${lang}_FLAGS_DEBUG "-g -O0")
  if(EMSCRIPTEN)
    # The web keeps function names (--profiling-funcs at link, host/CMakeLists.txt)
    # instead of DWARF: a translated game's debug sections are several times its
    # code, and the browser downloads and parses whatever the module carries.
    set(CMAKE_${lang}_FLAGS_RELEASE "")
  endif()
endforeach()

set(POP_ROOT ${CMAKE_SOURCE_DIR})
set(POP_TRANSLATE AUTO CACHE STRING "AUTO, ON, OFF or STUB: how targets that need generated code get it")
set_property(CACHE POP_TRANSLATE PROPERTY STRINGS AUTO ON OFF STUB)
# Where every output goes: the translation, the texture pack, the apps and the
# logs. A game outside the kit keeps them beside itself (tools/build.py passes
# <game-dir>/build); the kit's own builds use build/.
set(POP_BUILD_ROOT ${POP_ROOT}/build CACHE PATH "Root of build outputs (build/recomp, build/ios, ...)")
# A stub build (no game code) keeps its outputs apart from a real build so it
# can never overwrite the archive or the bundle a player runs.
if(ANDROID AND POP_TRANSLATE STREQUAL "STUB")
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/android-stub)
elseif(ANDROID)
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/android)
elseif(IOS AND POP_TRANSLATE STREQUAL "STUB" AND CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/ios-sim-stub)
elseif(IOS AND POP_TRANSLATE STREQUAL "STUB")
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/ios-stub)
# Cross builds (the web, Windows from macOS or Linux) keep theirs apart from
# the machine's own, which share the tree; the translation stays shared.
elseif(EMSCRIPTEN AND POP_TRANSLATE STREQUAL "STUB")
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/web-stub)
elseif(EMSCRIPTEN)
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/web)
elseif(WIN32 AND CMAKE_CROSSCOMPILING AND POP_TRANSLATE STREQUAL "STUB")
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/windows-stub)
elseif(WIN32 AND CMAKE_CROSSCOMPILING)
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/windows)
elseif(POP_TRANSLATE STREQUAL "STUB")
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/stub)
elseif(IOS AND CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/ios-sim)
elseif(IOS)
  set(POP_BUILD_DIR ${POP_BUILD_ROOT}/ios)
else()
  set(POP_BUILD_DIR ${POP_BUILD_ROOT})
endif()
set(POP_OUT ${POP_BUILD_DIR}/recomp)
# Static archives are compiler/platform-specific. Shared game outputs can hold
# both mobile and desktop builds; never let one preset overwrite another's libs.
set(POP_ARCHIVE_DIR ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${POP_OUT})
set(POP_TRACE_DIR "" CACHE PATH "Snapshot directory for pop_fixture_trace")

# tools/build.py passes its own interpreter; a bare configure finds one.
if(NOT Python3_EXECUTABLE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

# The game this build is for: a directory holding game.toml, rendered into
# generated/game_config.h (C macros) and generated/game_config.cmake (build
# names) at configure time; editing the toml reconfigures. The kit's own
# default is the stub game; real games live in their own repositories.
set(RECOMP_GAME_DIR ${POP_ROOT}/games/stub CACHE PATH "Directory holding the game.toml this build is for")
if(NOT IS_ABSOLUTE ${RECOMP_GAME_DIR})
  message(FATAL_ERROR "RECOMP_GAME_DIR must be absolute: ${RECOMP_GAME_DIR}")
endif()
if(NOT EXISTS ${RECOMP_GAME_DIR}/game.toml)
  message(FATAL_ERROR "No game config at ${RECOMP_GAME_DIR}/game.toml")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${RECOMP_GAME_DIR}/game.toml ${RECOMP_GAME_DIR}/globals.toml
  ${POP_ROOT}/tools/gen_game_config.py ${POP_ROOT}/tools/game_config.py)
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/gen_game_config.py
          --game-dir ${RECOMP_GAME_DIR}
          --header ${CMAKE_BINARY_DIR}/generated/game_config.h
          --cmake ${CMAKE_BINARY_DIR}/generated/game_config.cmake
  RESULT_VARIABLE RECOMP_GAME_CONFIG_RESULT)
if(NOT RECOMP_GAME_CONFIG_RESULT EQUAL 0)
  message(FATAL_ERROR "tools/gen_game_config.py failed for ${RECOMP_GAME_DIR}")
endif()
include(${CMAKE_BINARY_DIR}/generated/game_config.cmake)
if(NOT RECOMP_APP_NAME MATCHES "^[A-Za-z0-9_-]+$")
  message(FATAL_ERROR "Invalid app_name in ${RECOMP_GAME_DIR}/game.toml: ${RECOMP_APP_NAME}")
endif()
include_directories(${CMAKE_BINARY_DIR}/generated)
add_compile_definitions(GUEST_IMAGE_BASE=${RECOMP_IMAGE_BASE})
add_compile_definitions(GUEST_HEAP_BASE=${RECOMP_HEAP_BASE})
add_compile_definitions(GUEST_SIZE=${RECOMP_GUEST_SIZE})
add_compile_definitions(GUEST_HEAP_EXTRA_BASE=${RECOMP_HEAP_EXTRA_BASE})
add_compile_definitions(GUEST_HEAP_EXTRA_SIZE=${RECOMP_HEAP_EXTRA_SIZE})
# A build for players (tools/build.py --release): the kit's own performance
# overlay (counters, the frame-time graph, F11) is off and its settings row
# gone (mods/display_settings.cpp).
option(RECOMP_RELEASE "Build for players: no performance overlay" OFF)
if(RECOMP_RELEASE)
  add_compile_definitions(RECOMP_RELEASE=1)
endif()

if(IOS)
  set(RECOMP_IOS_TEAM "$ENV{RECOMP_IOS_TEAM}" CACHE STRING "Apple development team for automatic signing")
  # A device build for sideloading (tools/build.py --unsigned) is signed later,
  # by the player's tool.
  option(RECOMP_IOS_UNSIGNED "Build for devices without signing (an IPA for sideloading)" OFF)
  # The simulator runs unsigned builds (the ios-sim preset), so it needs no team.
  if(RECOMP_IOS_TEAM STREQUAL "" AND NOT POP_TRANSLATE STREQUAL "STUB"
     AND NOT CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator" AND NOT RECOMP_IOS_UNSIGNED)
    message(FATAL_ERROR "iOS builds need a signing team: set RECOMP_IOS_TEAM or pass tools/build.py --team; "
                        "`security find-identity -v -p codesigning` lists them")
  endif()
  # Every signed product, not only the app: a game module (recomp_game) and the
  # shared SDL it loads are dylibs Xcode signs too (RECOMP_GAME_MODULES).
  if(NOT RECOMP_IOS_TEAM STREQUAL "")
    set(CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${RECOMP_IOS_TEAM}")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic)
  endif()
endif()

# The version is the git description of the commit being built; a tree
# without git history gets "unknown". The release workflow passes it in.
find_package(Git QUIET)
set(POP_RECOMP_VERSION "unknown" CACHE STRING "Version string baked into the hosts")
if(POP_RECOMP_VERSION STREQUAL "unknown" AND GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                  WORKING_DIRECTORY ${POP_ROOT} OUTPUT_VARIABLE POP_GIT_DESCRIBE
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(POP_GIT_DESCRIBE)
    set(POP_RECOMP_VERSION ${POP_GIT_DESCRIBE})
  endif()
endif()
configure_file(${POP_ROOT}/host/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)


if(NOT IOS AND NOT ANDROID)
  enable_testing()
endif()
add_custom_target(check_binaries)

include(cmake/Warnings.cmake)
include(cmake/Plugins.cmake)
include(cmake/BuiltinMods.cmake)
include(cmake/Translate.cmake)
include(cmake/MacBundle.cmake)
include(cmake/IosBundle.cmake)
include(cmake/Dependencies.cmake)
add_subdirectory(platform)
add_subdirectory(runtime)
add_subdirectory(dx)
add_subdirectory(mods)
# The game's own mods (core plugins, examples, smoke probe), when it has any.
if(NOT IOS AND NOT ANDROID AND EXISTS ${RECOMP_GAME_DIR}/mods/CMakeLists.txt)
  add_subdirectory(${RECOMP_GAME_DIR}/mods ${CMAKE_BINARY_DIR}/game-mods)
endif()
add_subdirectory(host)
