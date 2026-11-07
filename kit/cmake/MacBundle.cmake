# pop_mac_bundle(<target>): make <target> a .app in build/, named by
# RECOMP_APP_NAME, with Info.plist copied verbatim and the resources,
# core mods, texture pack and ad-hoc signature applied after the link.
function(pop_mac_bundle target)
  # Core plugins are compiled during bundle finishing. Relink/repackage when
  # their source or manifests change, even if the host itself is unchanged.
  file(GLOB_RECURSE core_inputs CONFIGURE_DEPENDS
    ${RECOMP_GAME_DIR}/mods/core/*.c ${RECOMP_GAME_DIR}/mods/core/*.h
    ${RECOMP_GAME_DIR}/mods/core/*.toml)
  if(core_inputs)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${core_inputs})
  endif()
  # CMake also writes this plist during later configure-only/test builds. Its
  # bytes must already contain the final version, or they invalidate the app's
  # signature after a successful POST_BUILD finish without triggering a relink.
  set(RECOMP_BUNDLE_BUILD_VERSION "1")
  set(RECOMP_BUNDLE_SHORT_VERSION "0.1")
  if(POP_RECOMP_VERSION)
    string(REGEX REPLACE "^v" "" RECOMP_BUNDLE_BUILD_VERSION "${POP_RECOMP_VERSION}")
    set(RECOMP_BUNDLE_SHORT_VERSION "${RECOMP_BUNDLE_BUILD_VERSION}")
  endif()
  configure_file(${POP_ROOT}/host/Info.plist.in ${CMAKE_BINARY_DIR}/generated/Info.plist @ONLY)
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_BINARY_DIR}/generated/Info.plist)
  set_target_properties(${target} PROPERTIES
    MACOSX_BUNDLE ON
    OUTPUT_NAME ${RECOMP_APP_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${POP_BUILD_DIR}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/generated/Info.plist)
  set(video_args)
  if(RECOMP_VIDEO)
    # Use the shipped libraries even in a developer build; no build-tree rpath
    # may make an incomplete bundle appear to work.
    set_target_properties(${target} PROPERTIES
      BUILD_WITH_INSTALL_RPATH ON
      INSTALL_RPATH "@executable_path/../Frameworks")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
      ${POP_ROOT}/third_party/ffmpeg/NOTICE.md)
    foreach(library IN LISTS RECOMP_FFMPEG_LIBRARIES)
      list(APPEND video_args --ffmpeg-library "${library}")
    endforeach()
  endif()
  if(RECOMP_GAME_MODULES)
    # The shared SDL the app and its game modules use (Dependencies.cmake).
    list(APPEND video_args --ffmpeg-library "$<TARGET_FILE:SDL3::SDL3-shared>")
    if(NOT RECOMP_VIDEO)
      set_target_properties(${target} PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_RPATH "@executable_path/../Frameworks")
    endif()
  endif()
  # [macos] ui_mods: the game's interface mods as recipes (ui-mods/<name>.recipe),
  # which host/ui_mods.cpp applies to the player's install at start. What an
  # earlier build put there goes first, as does its profile-seed/.
  set(pop_mac_resources ${POP_BUILD_DIR}/${RECOMP_APP_NAME}.app/Contents/Resources)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${pop_mac_resources}/ui-mods ${pop_mac_resources}/profile-seed
    VERBATIM)
  foreach(recipe IN LISTS RECOMP_MACOS_UI_MODS)
    if(EXISTS "${recipe}")
      get_filename_component(recipe_name "${recipe}" NAME)
      set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${recipe}")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${recipe}" ${pop_mac_resources}/ui-mods/${recipe_name}
        VERBATIM)
    else()
      message(WARNING "[macos] ui_mods: ${recipe} is not there")
    endif()
  endforeach()
  # [macos] profile_files: copied under profile-seed/ in the bundle (before the
  # signature below); the host puts them in the player's profile at start
  # (host/sdl/platform_ui_desktop.cpp).
  foreach(rec IN LISTS RECOMP_MACOS_PROFILE_FILES)
    string(REPLACE "|" ";" pair "${rec}")
    list(GET pair 0 dest)
    list(GET pair 1 src)
    if(EXISTS "${src}")
      set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${src}")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${src}"
                ${POP_BUILD_DIR}/${RECOMP_APP_NAME}.app/Contents/Resources/profile-seed/${dest}
        VERBATIM)
    else()
      message(STATUS "[macos] profile_files: ${src} is not there; ${dest} stays out")
    endif()
  endforeach()
  # The fonts of the game's native views (native-screens/fonts/, with their
  # licences; the Mac shows the skill wheel), registered by the views at run
  # time; before the signature below.
  if(TARGET native_screens AND EXISTS ${RECOMP_GAME_DIR}/native-screens/fonts)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${RECOMP_GAME_DIR}/native-screens/fonts
              ${pop_mac_resources}/native-screens-fonts
      VERBATIM)
  endif()
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    ${POP_ROOT}/tools/recomp/finish_bundle.py)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/recomp/finish_bundle.py
            --bundle ${POP_BUILD_DIR}/${RECOMP_APP_NAME}.app
            --name ${RECOMP_APP_NAME} --cc ${CMAKE_C_COMPILER} --version ${POP_RECOMP_VERSION}
            --build-root ${POP_BUILD_ROOT} --game-dir ${RECOMP_GAME_DIR}
            --launcher-dir ${RECOMP_LAUNCHER_CONFIG_DIR}
            ${video_args}
    WORKING_DIRECTORY ${POP_ROOT}
    COMMENT "Finishing ${RECOMP_APP_NAME}.app"
    VERBATIM)
endfunction()
