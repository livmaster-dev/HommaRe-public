# Each executable lists exactly the host sources its shell script listed, so
# which strong host_* callbacks a binary carries does not change.
set(HOST ${CMAKE_CURRENT_SOURCE_DIR})

# Portable host code shared by every host that boots the guest.
add_library(host_common OBJECT boot.cpp report_lock.cpp)
target_include_directories(host_common PUBLIC ${POP_ROOT} ${POP_ROOT}/runtime)
target_link_libraries(host_common PUBLIC recomp_runtime)
target_compile_options(host_common PRIVATE ${POP_WARN_HOST})
pop_optimize(host_common 1)

# The launcher's portable core (host/launcher/): finding, checking and
# importing the player's game. miniz reads and writes ZIPs.
add_library(recomp_miniz OBJECT ${POP_ROOT}/third_party/miniz/miniz.c)
target_include_directories(recomp_miniz PUBLIC ${POP_ROOT}/third_party/miniz)
set_target_properties(recomp_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
pop_optimize(recomp_miniz 2)
add_library(launcher_core OBJECT ${HOST}/launcher/launcher.cpp ${HOST}/launcher/launcher_canvas.cpp
                                 ${HOST}/launcher/launcher_ui.cpp)
target_include_directories(launcher_core PUBLIC ${POP_ROOT})
target_compile_options(launcher_core PRIVATE ${POP_WARN_STRICT})
set_target_properties(launcher_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
pop_optimize(launcher_core 1)

# Compiling Objective-C++ under ARC, as every script did; a no-op for C++.
if(APPLE)
  set(POP_OBJC_ARC $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
else()
  set(POP_OBJC_ARC "")
endif()

# The GPU interface, its test double, the Vulkan backend and (on Apple) Metal.
add_subdirectory(gpu)

if(NOT IOS AND NOT ANDROID)
# The platform boundary: no Objective-C++ and no Apple framework outside gpu/metal/.
add_test(NAME host_boundary_check
  COMMAND ${Python3_EXECUTABLE} -m unittest ${POP_ROOT}/tools/recomp/tests/test_host_boundary.py
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(host_boundary_check PROPERTIES LABELS nogame)

# The committed SPIR-V must match the GLSL sources; skips where glslc is absent.
add_test(NAME shader_drift_check
  COMMAND ${Python3_EXECUTABLE} -m unittest ${POP_ROOT}/tools/recomp/tests/test_shaders.py
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(shader_drift_check PROPERTIES LABELS nogame)

# Portable pieces built everywhere. compositor_tests needs a real device for
# its pixel half (label gpu); ui_layer_tests needs nothing.
add_executable(compositor_tests ${HOST}/compositor.cpp ${HOST}/tests/compositor_tests.cpp)
target_include_directories(compositor_tests PRIVATE ${POP_ROOT} ${HOST})
target_compile_options(compositor_tests PRIVATE ${POP_WARN_WERROR})
target_link_libraries(compositor_tests PRIVATE gpu_backend)
pop_optimize(compositor_tests 1)
pop_test_binary(compositor_tests)
add_test(NAME compositor_tests COMMAND compositor_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(compositor_tests PROPERTIES LABELS gpu)

add_executable(input_touch_tests ${HOST}/input_touch.cpp ${HOST}/tests/input_touch_tests.cpp)
target_include_directories(input_touch_tests PRIVATE ${POP_ROOT})
target_compile_options(input_touch_tests PRIVATE ${POP_WARN_WERROR})
pop_link_sdl(input_touch_tests)
pop_optimize(input_touch_tests 1)
pop_test_binary(input_touch_tests)
add_test(NAME input_touch_tests COMMAND input_touch_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(input_touch_tests PROPERTIES LABELS nogame)

add_executable(input_trackpad_tests ${HOST}/input_trackpad.cpp ${HOST}/tests/input_trackpad_tests.cpp)
target_include_directories(input_trackpad_tests PRIVATE ${POP_ROOT})
target_compile_options(input_trackpad_tests PRIVATE ${POP_WARN_WERROR})
pop_optimize(input_trackpad_tests 1)
pop_test_binary(input_trackpad_tests)
add_test(NAME input_trackpad_tests COMMAND input_trackpad_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(input_trackpad_tests PROPERTIES LABELS nogame)

add_executable(keypad_tests ${HOST}/keypad_layout.cpp ${HOST}/keypad_modifiers.cpp ${HOST}/tests/keypad_tests.cpp)
target_include_directories(keypad_tests PRIVATE ${POP_ROOT})
target_compile_options(keypad_tests PRIVATE ${POP_WARN_WERROR})
pop_link_sdl(keypad_tests)
pop_optimize(keypad_tests 1)
pop_test_binary(keypad_tests)
add_test(NAME keypad_tests COMMAND keypad_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(keypad_tests PROPERTIES LABELS nogame)

add_executable(controls_tests ${HOST}/controls/json.cpp ${HOST}/controls/layout.cpp
                              ${HOST}/controls/builtin_layouts.cpp ${HOST}/controls/layout_store.cpp
                              ${HOST}/controls/router.cpp ${HOST}/controls/vpad.cpp
                              ${HOST}/controls/binding.cpp ${HOST}/controls/overlay_view.cpp
                              ${HOST}/controls/raster.cpp ${HOST}/controls/overlay_paint.cpp
                              ${HOST}/controls/pad_art.cpp
                              ${HOST}/controls/haptics.cpp
                              ${HOST}/controls/editor.cpp
                              ${HOST}/controls/editor_actions.cpp
                              ${HOST}/keypad_layout.cpp
                              ${HOST}/keypad_modifiers.cpp ${HOST}/tests/controls_tests.cpp
                              ${HOST}/tests/keypad_legacy_oracle.cpp ${POP_ROOT}/mods/font6x8.cpp)
target_include_directories(controls_tests PRIVATE ${POP_ROOT})
target_compile_options(controls_tests PRIVATE ${POP_WARN_WERROR})
target_link_libraries(controls_tests PRIVATE recomp_platform)
pop_link_sdl(controls_tests)
pop_optimize(controls_tests 1)
pop_test_binary(controls_tests)
add_test(NAME controls_tests COMMAND controls_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(controls_tests PROPERTIES LABELS nogame)

# Opt-in physical device probe. Never run a motor test from unattended CTest.
add_executable(gamepad_device_probe ${HOST}/tests/gamepad_device_probe.cpp
                                   ${HOST}/controls/vpad.cpp ${HOST}/controls/haptics.cpp)
target_compile_options(gamepad_device_probe PRIVATE ${POP_WARN_WERROR})
pop_link_sdl(gamepad_device_probe)
pop_test_binary(gamepad_device_probe)

add_executable(launcher_tests ${HOST}/tests/launcher_tests.cpp ${POP_ROOT}/mods/font6x8.cpp)
target_include_directories(launcher_tests PRIVATE ${POP_ROOT} ${POP_ROOT}/runtime ${POP_ROOT}/third_party/lua
                                                  ${CMAKE_BINARY_DIR}/generated)
target_compile_options(launcher_tests PRIVATE ${POP_WARN_WERROR})
if(WIN32)
  # fopen and getenv, as the POSIX hosts use them.
  target_compile_definitions(launcher_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
target_link_libraries(launcher_tests PRIVATE launcher_core recomp_miniz recomp_platform)
pop_optimize(launcher_tests 1)
pop_test_binary(launcher_tests)
add_test(NAME launcher_tests COMMAND launcher_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(launcher_tests PROPERTIES LABELS nogame)

# The interface mods' bookkeeping over scratch folders; no game.
add_executable(ui_mods_tests ${HOST}/ui_mods.cpp ${HOST}/tests/ui_mods_tests.cpp $<TARGET_OBJECTS:recomp_miniz>)
target_include_directories(ui_mods_tests PRIVATE ${POP_ROOT} ${POP_ROOT}/third_party/miniz)
target_compile_options(ui_mods_tests PRIVATE ${POP_WARN_STRICT})
pop_optimize(ui_mods_tests 1)
pop_test_binary(ui_mods_tests)
add_test(NAME ui_mods_tests COMMAND ui_mods_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(ui_mods_tests PROPERTIES LABELS nogame)

add_executable(ui_layer_tests ${HOST}/ui_layer.cpp ${HOST}/tests/test_frame_builder.cpp ${HOST}/tests/ui_layer_tests.cpp)
target_compile_options(ui_layer_tests PRIVATE ${POP_WARN_WERROR})
pop_optimize(ui_layer_tests 1)
pop_test_binary(ui_layer_tests)
add_test(NAME ui_layer_tests COMMAND ui_layer_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(ui_layer_tests PROPERTIES LABELS nogame)
endif()

# The hosts and the full host suite build on every platform over gpu_backend.
if(APPLE)
  set(FW_FOUNDATION "-framework Foundation")
  set(FW_METAL "-framework Metal" "-framework MetalKit" "-framework CoreText"
               "-framework CoreVideo" "-framework CoreGraphics" "-framework QuartzCore")
  if(NOT (IOS AND CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")) # not in the Simulator SDK
    list(APPEND FW_METAL "-framework MetalFX") # spatial upscaling of the presented frame
  endif()
  if(NOT IOS)
    list(APPEND FW_METAL "-framework IOKit") # energy.mm: the power source
  endif()
else()
  set(FW_FOUNDATION "")
  set(FW_METAL "")
endif()

# pop_host(<target> [GEN] [UI_LAYER] [OPT <n>] SOURCES ... LIBS ... FRAMEWORKS ...):
# a host or a test binary over the runtime, shims, mods and Lua.
function(pop_host target)
  cmake_parse_arguments(ARG "GEN;UI_LAYER;SHARED" "OPT" "SOURCES;FRAMEWORKS;LIBS" ${ARGN})
  if((ANDROID AND target STREQUAL "recomp_app") OR ARG_SHARED)
    add_library(${target} SHARED ${ARG_SOURCES})
  else()
    add_executable(${target} ${ARG_SOURCES})
  endif()
  target_link_libraries(${target} PRIVATE ${ARG_LIBS} ${ARG_FRAMEWORKS})
  target_include_directories(${target} PRIVATE ${POP_ROOT} ${POP_ROOT}/runtime
                                               ${POP_ROOT}/host ${POP_ROOT}/third_party/lua
                                               ${CMAKE_BINARY_DIR}/generated)
  target_compile_options(${target} PRIVATE ${POP_WARN_HOST} ${POP_OBJC_ARC})
  if(ARG_UI_LAYER)
    target_compile_definitions(${target} PRIVATE POPM_PRESENT_HAS_UI_LAYER=1)
  endif()
  if(NOT ARG_OPT)
    set(ARG_OPT 1)
  endif()
  pop_optimize(${target} ${ARG_OPT})
  if(ARG_GEN)
    pop_link_gen(${target})
  endif()
endfunction()

# The mixer, its output device and the synth: portable, over SDL audio.
set(POP_AUDIO_SOURCES ${HOST}/audio/mixer.cpp ${HOST}/audio/sdl_sink.cpp ${HOST}/audio/dolby_sink.cpp
                      ${HOST}/audio/midi_synth.cpp
                      ${HOST}/audio_math.cpp ${HOST}/audio_capture.cpp)
set(POP_CORE_LIBS recomp_platform recomp_runtime recomp_snapshot recomp_dx recomp_mods lua host_common gpu_backend)
# The on-screen controls the presenter draws (controls_host.cpp is the app's own).
set(POP_CONTROLS_SOURCES ${HOST}/controls/json.cpp ${HOST}/controls/layout.cpp
                         ${HOST}/controls/builtin_layouts.cpp ${HOST}/controls/layout_store.cpp
                         ${HOST}/controls/router.cpp ${HOST}/controls/vpad.cpp
                         ${HOST}/controls/binding.cpp ${HOST}/controls/overlay_view.cpp
                         ${HOST}/controls/overlay.cpp ${HOST}/controls/raster.cpp
                          ${HOST}/controls/overlay_paint.cpp ${HOST}/controls/pad_art.cpp
                          ${HOST}/controls/haptics.cpp
                          ${HOST}/controls/editor.cpp ${HOST}/controls/editor_actions.cpp
                         ${HOST}/keypad_layout.cpp ${HOST}/keypad_modifiers.cpp)
set(POP_PRESENT_SOURCES ${HOST}/present_pixels.cpp ${HOST}/present_thread.cpp ${HOST}/compositor.cpp
                        ${HOST}/ui_layer.cpp ${HOST}/performance_overlay.cpp ${POP_CONTROLS_SOURCES}
                        ${HOST}/d3d_render.cpp
                        ${HOST}/gpu2d.cpp
                        ${HOST}/indexed_frame.cpp)
# The Direct3D 9 device's GPU renderer: the host entry points, and a backend
# per GPU API (host/gpu/d3d9_backend.h). Without one the shim draws on the CPU.
list(APPEND POP_PRESENT_SOURCES ${HOST}/gpu/d3d9_host.cpp)
if(RECOMP_D9_VULKAN)
  list(APPEND POP_PRESENT_SOURCES ${HOST}/gpu/vulkan/d3d9_vulkan.cpp)
  set_source_files_properties(${HOST}/gpu/d3d9_host.cpp PROPERTIES COMPILE_DEFINITIONS RECOMP_D9_VULKAN=1)
  set_source_files_properties(${HOST}/gpu/vulkan/d3d9_vulkan.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${POP_ROOT}/third_party/volk;${POP_ROOT}/third_party/vulkan-headers/include"
    COMPILE_DEFINITIONS "VK_NO_PROTOTYPES;RECOMP_D9_VULKAN=1")
  set(POP_D9_LIBS glslang::glslang glslang::glslang-default-resource-limits)
  list(APPEND POP_CORE_LIBS ${POP_D9_LIBS})
endif()
if(EMSCRIPTEN)
  list(APPEND POP_PRESENT_SOURCES ${HOST}/gpu/webgpu/d3d9_webgpu.cpp)
endif()
if(APPLE)
  list(APPEND POP_PRESENT_SOURCES ${HOST}/gpu/metal/d3d9_metal.mm ${HOST}/energy.mm)
  set_source_files_properties(${HOST}/gpu/metal/d3d9_metal.mm ${HOST}/energy.mm
                              PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

# The host's platform file: desktop or iPadOS (host/sdl/platform_ui.h), and
# the launcher's (host/launcher/launcher_sdl.h; iPadOS keeps its own in
# platform_ui_ios.mm).
set(POP_LAUNCHER_SOURCES $<TARGET_OBJECTS:launcher_core> $<TARGET_OBJECTS:recomp_miniz>
                         ${HOST}/launcher/launcher_config.cpp ${HOST}/launcher/launcher_sdl.cpp
                         ${HOST}/launcher/native_launcher_sdl.cpp)
# The SwiftUI launcher Apple builds show when the game repository has a
# launcher-games.json (host/launcher/native_launcher.h).
if(APPLE AND RECOMP_NATIVE_LAUNCHER)
  add_library(launcher_native STATIC ${HOST}/launcher/apple/LauncherBridge.swift
                                     ${HOST}/launcher/apple/LauncherCatalog.swift
                                     ${HOST}/launcher/apple/LauncherGog.swift
                                     ${HOST}/launcher/apple/LauncherGogView.swift
                                     ${HOST}/launcher/apple/LauncherInstaller.swift
                                     ${HOST}/launcher/apple/LauncherModel.swift
                                     ${HOST}/launcher/apple/LauncherSkin.swift
                                     ${HOST}/launcher/apple/LauncherSkinView.swift
                                     ${HOST}/launcher/apple/LauncherStrings.swift
                                     ${HOST}/launcher/apple/LauncherView.swift)
  set_target_properties(launcher_native PROPERTIES Swift_MODULE_NAME RecompLauncher Swift_LANGUAGE_VERSION 5)
  # RecompSettingsFile: the C settings-file API (mods/settings_file.h) as a
  # Swift module, through the module map beside the sources.
  if(CMAKE_GENERATOR STREQUAL "Xcode")
    set_target_properties(launcher_native PROPERTIES
      XCODE_ATTRIBUTE_SWIFT_INCLUDE_PATHS "${HOST}/launcher/apple/RecompSettings")
  else()
    target_compile_options(launcher_native PRIVATE "SHELL:-I ${HOST}/launcher/apple/RecompSettings")
  endif()
  # iOS installs from GOG offline installers with innoextract built in as a
  # library (an app there cannot run the Mac's helper): built once per build
  # tree and SDK by third_party/innoextract/build-ios.sh, which fetches pinned
  # sources. Without it the launcher tells the player to unpack on a Mac.
  if(IOS)
    option(RECOMP_IOS_INNOEXTRACT "Build innoextract into the iOS launcher (GOG installers)" ON)
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
      set(pop_inno_sdk iphonesimulator)
    else()
      set(pop_inno_sdk iphoneos)
    endif()
    set(pop_inno_dir ${POP_BUILD_ROOT}/innoextract-ios)
    set(pop_inno_lib ${pop_inno_dir}/${pop_inno_sdk}/librecomp_innoextract.a)
    if(RECOMP_IOS_INNOEXTRACT AND NOT EXISTS ${pop_inno_lib})
      message(STATUS "Building innoextract for ${pop_inno_sdk} (third_party/innoextract/build-ios.sh)")
      execute_process(COMMAND sh ${POP_ROOT}/third_party/innoextract/build-ios.sh ${pop_inno_dir} ${pop_inno_sdk}
                      RESULT_VARIABLE pop_inno_result)
      if(NOT pop_inno_result EQUAL 0)
        message(WARNING "innoextract for iOS did not build; the launcher cannot install from GOG installers")
      endif()
    endif()
    if(RECOMP_IOS_INNOEXTRACT AND EXISTS ${pop_inno_lib})
      target_link_libraries(launcher_native PUBLIC ${pop_inno_lib} z bz2 iconv)
      if(CMAKE_GENERATOR STREQUAL "Xcode")
        set_target_properties(launcher_native PROPERTIES
          XCODE_ATTRIBUTE_SWIFT_ACTIVE_COMPILATION_CONDITIONS "RECOMP_INNOEXTRACT_LIB")
      else()
        target_compile_options(launcher_native PRIVATE "SHELL:-D RECOMP_INNOEXTRACT_LIB")
      endif()
    endif()
  endif()
endif()
# The same launcher on Linux, drawn by the host itself (host/launcher/linux/):
# the canvas launcher's window with the game's own textures and TrueType
# text, installing from GOG installers with the system's innoextract. Its
# words are the SwiftUI launcher's, apple/launcher-strings.json, embedded.
# Android draws the same one, with the system's pickers for the installers
# and the game folders (launcher/linux/android_files.cpp) and innoextract
# from the APK (third_party/innoextract/build-android.sh).
if((CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID) AND RECOMP_NATIVE_LAUNCHER)
  set(pop_strings_cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/launcher_strings.cpp)
  add_custom_command(
    OUTPUT ${pop_strings_cpp}
    COMMAND ${CMAKE_COMMAND} -DOUT=${pop_strings_cpp} -DNAMESPACE=launcher::native
            "-DENTRIES=launcher_strings_json=${HOST}/launcher/apple/launcher-strings.json"
            -P ${POP_ROOT}/cmake/EmbedFiles.cmake
    DEPENDS ${POP_ROOT}/cmake/EmbedFiles.cmake ${HOST}/launcher/apple/launcher-strings.json
    COMMENT "Embedding the launcher's strings"
    VERBATIM)
  # Everything but the window, which the tests build too.
  set(POP_NATIVE_LAUNCHER_CORE ${HOST}/launcher/linux/config.cpp ${HOST}/launcher/linux/detect.cpp
                               ${HOST}/launcher/linux/installer.cpp ${HOST}/launcher/linux/skin.cpp
                               ${HOST}/launcher/linux/l10n.cpp ${HOST}/launcher/linux/text.cpp
                               ${HOST}/launcher/linux/layout.cpp ${pop_strings_cpp})
  add_library(launcher_native OBJECT ${POP_NATIVE_LAUNCHER_CORE} ${HOST}/launcher/linux/model.cpp
                                     ${HOST}/launcher/linux/view.cpp ${HOST}/launcher/linux/bridge.cpp)
  if(ANDROID)
    target_sources(launcher_native PRIVATE ${HOST}/launcher/linux/android_files.cpp)
  endif()
  target_include_directories(launcher_native PRIVATE ${POP_ROOT} ${POP_ROOT}/runtime ${CMAKE_BINARY_DIR}/generated)
  target_compile_options(launcher_native PRIVATE ${POP_WARN_STRICT})
  pop_link_sdl(launcher_native)
  pop_optimize(launcher_native 2)

  # Its portable part over fake installs and paks in a scratch folder: no
  # window, no game. The faces the app takes from the runtime (Open Sans),
  # embedded again for them. Desktop only: a device runs no CTest.
  if(NOT ANDROID)
  set(pop_test_fonts_cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/native_launcher_test_fonts.cpp)
  set(pop_fonts_dir ${POP_ROOT}/third_party/fonts/opensans)
  add_custom_command(
    OUTPUT ${pop_test_fonts_cpp}
    COMMAND ${CMAKE_COMMAND} -DOUT=${pop_test_fonts_cpp} -DNAMESPACE=gdi
            "-DENTRIES=bundled_sans_regular=${pop_fonts_dir}/OpenSans-Regular.ttf|bundled_sans_semibold=${pop_fonts_dir}/OpenSans-Semibold.ttf"
            -P ${POP_ROOT}/cmake/EmbedFiles.cmake
    DEPENDS ${POP_ROOT}/cmake/EmbedFiles.cmake ${pop_fonts_dir}/OpenSans-Regular.ttf
            ${pop_fonts_dir}/OpenSans-Semibold.ttf
    COMMENT "Embedding the bundled fonts for the launcher tests"
    VERBATIM)
  foreach(pop_test native_launcher_tests native_installer_tests native_skin_tests)
    add_executable(${pop_test} ${HOST}/tests/${pop_test}.cpp ${POP_NATIVE_LAUNCHER_CORE} ${pop_test_fonts_cpp}
                               ${HOST}/controls/json.cpp ${POP_ROOT}/mods/font6x8.cpp)
    target_include_directories(${pop_test} PRIVATE ${POP_ROOT} ${POP_ROOT}/runtime ${CMAKE_BINARY_DIR}/generated)
    target_compile_options(${pop_test} PRIVATE ${POP_WARN_STRICT})
    target_link_libraries(${pop_test} PRIVATE launcher_core recomp_miniz recomp_platform)
    pop_optimize(${pop_test} 1)
    pop_test_binary(${pop_test})
    add_test(NAME ${pop_test} COMMAND ${pop_test} WORKING_DIRECTORY ${POP_ROOT})
    set_tests_properties(${pop_test} PROPERTIES LABELS nogame)
  endforeach()
  # The model and its screens too, drawn on a canvas; SDL for the key codes.
  target_sources(native_launcher_tests PRIVATE ${HOST}/launcher/linux/model.cpp ${HOST}/launcher/linux/view.cpp)
  pop_link_sdl(native_launcher_tests)
  endif()
endif()
if(IOS)
  set(POP_PLATFORM_UI ${HOST}/sdl/platform_ui_ios.mm)
  list(APPEND FW_METAL "-framework UIKit" "-framework UniformTypeIdentifiers" "-framework CoreHaptics")
elseif(ANDROID)
  set(POP_PLATFORM_UI ${HOST}/sdl/platform_ui_desktop.cpp ${HOST}/launcher/launcher_platform_android.cpp)
else()
  set(POP_PLATFORM_UI ${HOST}/sdl/platform_ui_desktop.cpp ${HOST}/launcher/launcher_platform_desktop.cpp)
  if(APPLE) # the trackpad's phases and inertia, from AppKit (sdl/trackpad_mac.h)
    list(APPEND POP_PLATFORM_UI ${HOST}/sdl/trackpad_mac.mm)
    set_source_files_properties(${HOST}/sdl/trackpad_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    list(APPEND FW_METAL "-framework AppKit")
  endif()
endif()

if(TARGET recomp_gen)
  if(NOT IOS AND NOT ANDROID)
  pop_host(pop_headless GEN
    SOURCES ${HOST}/headless_main.cpp ${HOST}/page_overlay.cpp ${POP_AUDIO_SOURCES}
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS ${FW_FOUNDATION})
  pop_link_sdl(pop_headless)
  if(TARGET core_mods)
    add_dependencies(pop_headless core_mods)
  endif()

  # Test71: the same optimisation as the shipped app, so timings measured here are the app's.
  pop_host(pop_smoke GEN UI_LAYER OPT 2
    SOURCES ${HOST}/smoke_main.cpp ${HOST}/script.cpp ${HOST}/input.cpp ${HOST}/input_gate.cpp
            ${HOST}/input_touch.cpp ${HOST}/controls/vpad_host_api.cpp ${HOST}/sdl/keymap.cpp
            ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/audio_math.cpp
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL})
  pop_link_sdl(pop_smoke)
  # Each target is optional on its own: a game can ship core mods without a
  # smoke probe, and depending on one that does not exist fails generation.
  if(TARGET core_mods)
    add_dependencies(pop_smoke core_mods)
  endif()
  if(TARGET smoke_probe)
    add_dependencies(pop_smoke smoke_probe)
  endif()

  endif()

  set(POP_APP_SOURCES ${HOST}/sdl/main.cpp ${HOST}/sdl/keymap.cpp ${POP_PLATFORM_UI} ${POP_LAUNCHER_SOURCES}
            ${HOST}/controls/controls_host.cpp ${HOST}/controls/gamepad_sdl.cpp
            ${HOST}/controls/vpad_host_api.cpp
            ${HOST}/game_path.cpp ${HOST}/present.cpp ${HOST}/text_overrides.cpp ${HOST}/ui_mods.cpp
            ${HOST}/server_prompt.cpp
            ${HOST}/page_overlay.cpp
            ${POP_PRESENT_SOURCES}
            ${HOST}/input.cpp ${HOST}/input_gate.cpp ${HOST}/input_touch.cpp
            ${HOST}/input_trackpad.cpp ${POP_AUDIO_SOURCES})
  # The game's own screens drawn natively over it on iOS (sdl/platform_ui.h
  # platform_ui_native_screen): SwiftUI sources in the game repository's
  # native-screens/, one module, RecompScreens, linked into the app alone (a
  # game's module calls the app's copy). Swift is there whenever the native
  # launcher is.
  file(GLOB pop_screen_sources ${RECOMP_GAME_DIR}/native-screens/*.swift)
  if(IOS AND TARGET launcher_native AND pop_screen_sources)
    add_library(native_screens STATIC ${pop_screen_sources})
    set_target_properties(native_screens PROPERTIES Swift_MODULE_NAME RecompScreens Swift_LANGUAGE_VERSION 5)
  endif()
  function(pop_link_native_screens target)
    if(TARGET native_screens)
      target_link_libraries(${target} PRIVATE native_screens)
      target_compile_definitions(${target} PRIVATE RECOMP_NATIVE_SCREENS=1)
      set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
      if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
        set(sdk "${CMAKE_OSX_SYSROOT}")
        if(NOT IS_DIRECTORY "${sdk}")
          execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
        endif()
        target_link_options(${target} PRIVATE "-L${sdk}/usr/lib/swift" "-Wl,-rpath,/usr/lib/swift")
      endif()
    endif()
  endfunction()

  pop_host(recomp_app GEN UI_LAYER OPT 2
    SOURCES ${POP_APP_SOURCES}
    LIBS ${POP_CORE_LIBS}
    FRAMEWORKS ${FW_METAL})
  pop_link_sdl(recomp_app)
  pop_link_native_screens(recomp_app)
  set_target_properties(recomp_app PROPERTIES OUTPUT_NAME ${RECOMP_APP_NAME})
  if(RECOMP_GAME_MODULES)
    # The launcher's app also runs the other games' modules (sdl/main.cpp).
    target_compile_definitions(recomp_app PRIVATE RECOMP_GAME_MODULES=1)
  endif()
  if(TARGET launcher_native)
    target_link_libraries(recomp_app PRIVATE launcher_native)
    target_compile_definitions(recomp_app PRIVATE RECOMP_NATIVE_LAUNCHER=1)
    if(NOT APPLE)
      # host/launcher/linux/: it reads the window's events itself.
      target_compile_definitions(recomp_app PRIVATE RECOMP_NATIVE_LAUNCHER_SDL=1)
    endif()
    # The app links as C++; the Swift objects autolink swiftCore, SwiftUI and
    # the rest, which live in the SDK (and in the OS at run time).
    set_target_properties(recomp_app PROPERTIES LINKER_LANGUAGE CXX)
    if(APPLE AND NOT CMAKE_GENERATOR STREQUAL "Xcode")
      set(pop_sdk "${CMAKE_OSX_SYSROOT}")
      if(NOT IS_DIRECTORY "${pop_sdk}")
        execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE pop_sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
      endif()
      target_link_options(recomp_app PRIVATE "-L${pop_sdk}/usr/lib/swift" "-Wl,-rpath,/usr/lib/swift")
    endif()
  endif()
  if(ANDROID)
    # SDLActivity loads libmain.so from the APK's native library directory.
    set_target_properties(recomp_app PROPERTIES OUTPUT_NAME main)
    target_link_libraries(recomp_app PRIVATE log)
    # Core adaptations must be available before guest startup on mobile too.
    # Their manifests are staged into APK assets by tools/build.py.
    pop_builtin_mods(recomp_app)
  elseif(IOS)
    # No plugin loading on a stock device: the core mods are compiled in.
    pop_builtin_mods(recomp_app RESOURCES $<TARGET_BUNDLE_CONTENT_DIR:recomp_app>)
    pop_ios_bundle(recomp_app)
  elseif(EMSCRIPTEN)
    # The web: core mods compiled in, the host's resources packaged at
    # /app/resources (runtime/layout.cpp), threads for the game.
    pop_builtin_mods(recomp_app)
    set(web_resources ${POP_OUT}/web-resources)
    file(REMOVE_RECURSE ${web_resources})
    file(MAKE_DIRECTORY ${web_resources})
    file(COPY ${POP_ROOT}/tools/recomp/baseline/classic-modes.json DESTINATION ${web_resources})
    if(EXISTS ${POP_BUILD_ROOT}/recomp/symbols.json)
      file(COPY ${POP_BUILD_ROOT}/recomp/symbols.json DESTINATION ${web_resources})
    endif()
    if(EXISTS ${RECOMP_GAME_DIR}/mods/core)
      file(COPY ${RECOMP_GAME_DIR}/mods/core DESTINATION ${web_resources}/mods FILES_MATCHING PATTERN "*.toml"
           PATTERN "*.json" PATTERN "*.lua" PATTERN "*.png" PATTERN "*.bmp" PATTERN "*.txt")
    endif()
    # The player page (tools/web_launcher.py --web-build places it beside the launcher).
    set(APP ${RECOMP_APP_NAME})
    set(TITLE ${RECOMP_GAME_NAME})
    configure_file(${POP_ROOT}/web/player/index.html ${POP_OUT}/index.html @ONLY)
    target_link_options(recomp_app PRIVATE
      -pthread -sPTHREAD_POOL_SIZE=24 -sWASMFS
      -sINITIAL_MEMORY=1073741824 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4294967296
      -sSTACK_SIZE=8388608 -sDEFAULT_PTHREAD_STACK_SIZE=16777216
      -sENVIRONMENT=web,worker -sEXIT_RUNTIME=0 -sINVOKE_RUN=0
      -sEXPORTED_RUNTIME_METHODS=ENV,callMain,FS
      --preload-file ${web_resources}@/app/resources)
  elseif(APPLE)
    pop_mac_bundle(recomp_app)
  else()
    # Desktop packages carry manifests beside statically linked core plugins.
    # This avoids shipping a plugin compiled for a different host platform.
    pop_builtin_mods(recomp_app RESOURCES $<TARGET_FILE_DIR:recomp_app>/resources)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND (RECOMP_VIDEO OR RECOMP_GAME_MODULES))
      # Keep staged FFmpeg libraries (and the shared SDL) discoverable beside the executable.
      set_target_properties(recomp_app PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    endif()
  endif()

  # The game as a module of a multi-game app (RECOMP_GAME_MODULES): the same
  # host as libgame.<game.id>.dylib (.so), entered through recomp_game_main by
  # the app whose launcher picked it (sdl/main.cpp). It has no launcher of its
  # own and finds its resources and profile from its own path.
  # tools/assemble_games.py puts it, with its resources, into the app that
  # hosts the launcher.
  if(RECOMP_GAME_MODULES)
    pop_host(recomp_game GEN UI_LAYER OPT 2 SHARED
      SOURCES ${POP_APP_SOURCES}
      LIBS ${POP_CORE_LIBS}
      FRAMEWORKS ${FW_METAL})
    pop_link_sdl(recomp_game)
    # The module uses the app's RecompScreens (sdl/platform_ui_ios.mm): a
    # second copy would register its Objective-C classes twice in the process.
    if(TARGET native_screens)
      target_compile_definitions(recomp_game PRIVATE RECOMP_NATIVE_SCREENS=1)
    endif()
    if(APPLE AND NOT IOS)
      # The static SDL brought AppKit in for metal_surface.mm; the shared one does not.
      target_link_libraries(recomp_game PRIVATE "-framework AppKit")
    endif()
    target_compile_definitions(recomp_game PRIVATE RECOMP_GAME_MODULE=1)
    set_target_properties(recomp_game PROPERTIES
      OUTPUT_NAME "game.${RECOMP_GAME_ID}"
      LIBRARY_OUTPUT_DIRECTORY ${POP_BUILD_DIR}/modules
      BUILD_WITH_INSTALL_RPATH ON
      INSTALL_NAME_DIR "@rpath"
      # SDL and FFmpeg sit beside the module in the app's Frameworks.
      INSTALL_RPATH "@loader_path"
      XCODE_ATTRIBUTE_LD_DYLIB_INSTALL_NAME "@rpath/libgame.${RECOMP_GAME_ID}.dylib"
      XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@loader_path")
    if(IOS OR NOT APPLE)
      pop_builtin_mods(recomp_game)
    endif()
    if(NOT APPLE)
      # ELF has one symbol namespace: the module's runtime and translation
      # carry the same names as the app's, and must bind to their own
      # (Mach-O's two-level namespace does this by itself), so the module
      # exports recomp_game_main alone.
      target_link_options(recomp_game PRIVATE "-Wl,--version-script=${HOST}/sdl/game_module.map")
      set_property(TARGET recomp_game APPEND PROPERTY LINK_DEPENDS ${HOST}/sdl/game_module.map)
    endif()
    if(ANDROID)
      # Beside libmain.so and libSDL3.so in the APK's native library folder,
      # which the system's linker searches by itself.
      target_link_libraries(recomp_game PRIVATE log)
    elseif(NOT APPLE)
      # SDL and FFmpeg sit beside the executable, one folder up from modules/.
      set_target_properties(recomp_game PROPERTIES BUILD_RPATH "$ORIGIN/.." INSTALL_RPATH "$ORIGIN/..")
    endif()
  endif()

  if(APPLE AND NOT IOS AND NOT ANDROID AND EXISTS ${RECOMP_GAME_DIR}/mods/examples/luawalk/main.lua) # Objective-C++ source; a desktop test
    # Real loader, hooks and DirectDraw seal/service with fake GPU acks.
    pop_host(present_events_tests GEN UI_LAYER
      SOURCES ${POP_ROOT}/mods/tests/present_events_tests.mm
              ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/input.cpp ${HOST}/input_gate.cpp
      LIBS recomp_platform recomp_runtime_testing recomp_snapshot recomp_dx recomp_mods_testing lua gpu_backend
      FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL})
    add_test(NAME present_events_tests COMMAND present_events_tests ${POP_BUILD_ROOT}/.. WORKING_DIRECTORY ${POP_ROOT})
    set_tests_properties(present_events_tests PROPERTIES LABELS mods ENVIRONMENT RECOMP_GAME_DIR=${RECOMP_GAME_DIR})
  endif()
endif()

# Host tests: offscreen GPU work and the mixer rendered offline, no window and
# no audio device. Labelled `gpu`: the renderer tests need a device.
# report_lock.cpp is a source here rather than host_common, because the script
# never linked boot.cpp into this suite.
if(NOT IOS AND NOT ANDROID)
pop_host(host_tests UI_LAYER
  SOURCES ${HOST}/tests/host_tests.cpp ${HOST}/tests/test_frame_builder.cpp
          ${POP_ROOT}/runtime/tests/stub_recomp_call.cpp
          ${HOST}/report_lock.cpp ${HOST}/script.cpp ${HOST}/present.cpp
          ${POP_ROOT}/mods/font6x8.cpp
          ${HOST}/game_path.cpp ${HOST}/server_prompt.cpp
          ${HOST}/page_overlay.cpp ${POP_PRESENT_SOURCES} ${HOST}/input.cpp ${HOST}/input_gate.cpp
          ${POP_AUDIO_SOURCES}
          ${POP_ROOT}/dx/com.cpp ${POP_ROOT}/dx/host_api.cpp
          ${POP_ROOT}/dx/display_stubs.cpp ${POP_ROOT}/dx/dinput.cpp
          ${POP_ROOT}/dx/dinput_joystick.cpp
          ${POP_ROOT}/dx/d3d11.cpp ${POP_ROOT}/dx/dxgi.cpp
          ${POP_ROOT}/dx/d3dcompiler.cpp ${POP_ROOT}/dx/d3dx10.cpp
          ${POP_ROOT}/dx/d3d9_shader.cpp ${POP_ROOT}/dx/d3d9_shadergen.cpp
          ${POP_ROOT}/dx/host_d9_default.cpp
  LIBS recomp_platform recomp_runtime gpu_backend gpu_fake ${POP_D9_LIBS}
  FRAMEWORKS ${FW_FOUNDATION} ${FW_METAL})
pop_link_sdl(host_tests)
# The Dolby Digital tests encode and decode AC-3 with the bundled FFmpeg.
pop_link_video(host_tests)
pop_test_binary(host_tests)
add_test(NAME host_tests COMMAND host_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(host_tests PROPERTIES LABELS gpu)
endif()

# The production headless capture path with synthetic guest windows and no GPU.
if(NOT IOS AND NOT ANDROID)
pop_host(headless_tests
  SOURCES ${HOST}/tests/headless_tests.cpp ${POP_ROOT}/runtime/tests/stub_recomp_call.cpp
          ${HOST}/page_overlay.cpp ${POP_AUDIO_SOURCES}
  LIBS recomp_platform recomp_runtime recomp_dx host_common
  FRAMEWORKS ${FW_FOUNDATION})
pop_link_sdl(headless_tests)
pop_test_binary(headless_tests)
add_test(NAME headless_tests COMMAND headless_tests ${CMAKE_CURRENT_BINARY_DIR}/capture-frames
  WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(headless_tests PROPERTIES LABELS nogame)
endif()
