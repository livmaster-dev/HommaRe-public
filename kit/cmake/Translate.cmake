# The translated game lives only in the developer's build/recomp/gen; the
# kit never tracks generated code (spec section 11). Generated C is platform
# independent, so every preset (macOS, iOS) compiles the same directory.
set(POP_GEN_DIR ${POP_BUILD_ROOT}/recomp/gen)
if(POP_TRANSLATE STREQUAL "STUB")
  # A link-only translation for builds without game code (CI).
  set(POP_GEN_DIR ${CMAKE_BINARY_DIR}/stub-gen)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/gen_stub_translation.py --out ${POP_GEN_DIR}
    RESULT_VARIABLE POP_STUB_RESULT)
  if(NOT POP_STUB_RESULT EQUAL 0)
    message(FATAL_ERROR "tools/gen_stub_translation.py failed")
  endif()
endif()
set(POP_HAVE_GEN OFF)
# POP_REAL_GEN: the translation is the game's, not the link-only stub. Tests
# that call into generated functions by address are defined only then.
set(POP_REAL_GEN OFF)
if(POP_TRANSLATE STREQUAL "OFF")
  message(STATUS "POP_TRANSLATE=OFF: targets that need the generated code are not defined")
elseif(EXISTS ${POP_GEN_DIR}/table.c)
  set(POP_HAVE_GEN ON)
  if(NOT POP_TRANSLATE STREQUAL "STUB")
    set(POP_REAL_GEN ON)
  endif()
  message(STATUS "Translation: ${POP_GEN_DIR}")
elseif(POP_TRANSLATE STREQUAL "ON")
  message(FATAL_ERROR "POP_TRANSLATE=ON but no translation: run tools/build.py --regenerate")
else()
  message(STATUS "No translation found: hosts and game-backed tests are not defined")
endif()

if(POP_HAVE_GEN)
  file(GLOB POP_GEN_SOURCES CONFIGURE_DEPENDS ${POP_GEN_DIR}/chunk_*.c ${POP_GEN_DIR}/table.c)
  add_library(recomp_gen STATIC ${POP_GEN_SOURCES})
  set_target_properties(recomp_gen PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${POP_ARCHIVE_DIR} OUTPUT_NAME recomp_gen)
  # -I<gen> for x86.h beside the sources, -I<root> for the canonical copy,
  # -I<runtime> for intrinsics.h: the same three the shell script passed.
  target_include_directories(recomp_gen PRIVATE ${POP_GEN_DIR} ${POP_ROOT} ${POP_ROOT}/runtime)
  target_include_directories(recomp_gen INTERFACE ${POP_GEN_DIR})
  target_compile_options(recomp_gen PRIVATE ${POP_WARN_GEN})
  # [translate] store_hooks = false: the translated code's stores skip the watch/dirty check.
  # Only here - the runtime and its tests keep the check for DirectDraw locks.
  if(DEFINED RECOMP_STORE_HOOKS)
    target_compile_definitions(recomp_gen PRIVATE RECOMP_STORE_HOOKS=${RECOMP_STORE_HOOKS})
  endif()
  # [translate] fixed_guest_base: guest memory from a constant (macOS only;
  # the runtime maps it there, runtime/memory.cpp).
  if(RECOMP_FIXED_GUEST_BASE AND APPLE AND NOT IOS)
    target_compile_definitions(recomp_gen PRIVATE RECOMP_GUEST_FIXED_BASE=1)
  endif()
  # [translate] lean_calls: CALL_FN without the profiler/frame-watch checks.
  if(RECOMP_LEAN_CALLS)
    target_compile_definitions(recomp_gen PRIVATE RECOMP_LEAN_CALLS=1)
  endif()
  # [translate] write_track = "protect": no per-store lazy-buffer marking.
  # WebAssembly has no page protection to catch those stores with, so the
  # web's translated stores mark whatever the game asks for (and the runtime
  # tracks by them, tools/gen_game_config.py).
  if(EMSCRIPTEN)
    set(RECOMP_WRITE_TRACK_STORES 1)
  endif()
  if(DEFINED RECOMP_WRITE_TRACK_STORES)
    target_compile_definitions(recomp_gen PRIVATE RECOMP_WRITE_TRACK_STORES=${RECOMP_WRITE_TRACK_STORES})
  endif()
  # A game's native replacements. funcs.h includes this header before it
  # defines FN_<addr>, so every call site, tail call and jump-table case for a
  # replaced address goes to the native function instead.
  if(RECOMP_OVERRIDE_HEADER)
    if(NOT EXISTS ${RECOMP_OVERRIDE_HEADER})
      message(FATAL_ERROR "RECOMP_OVERRIDE_HEADER does not exist: ${RECOMP_OVERRIDE_HEADER}")
    endif()
    target_compile_definitions(recomp_gen PRIVATE
      RECOMP_OVERRIDE_HEADER="${RECOMP_OVERRIDE_HEADER}")
    get_filename_component(_override_dir ${RECOMP_OVERRIDE_HEADER} DIRECTORY)
    target_include_directories(recomp_gen PRIVATE ${_override_dir})
    message(STATUS "Native overrides: ${RECOMP_OVERRIDE_HEADER}")
  endif()
  pop_optimize(recomp_gen 2)
  pop_pgo(recomp_gen)
  # Auxiliary modules (game.toml [modules.aux.<key>]) are translated into
  # gen/aux-<key>/ with their own funcs.h and prefixed tables, so each is its
  # own library; their table.c registers with the runtime's module registry.
  set(POP_GEN_AUX_TARGETS "")
  file(GLOB POP_GEN_AUX_DIRS CONFIGURE_DEPENDS LIST_DIRECTORIES true ${POP_GEN_DIR}/aux-*)
  foreach(dir ${POP_GEN_AUX_DIRS})
    if(NOT EXISTS ${dir}/table.c)
      continue()
    endif()
    get_filename_component(key ${dir} NAME)
    string(REPLACE "aux-" "recomp_gen_" aux_target ${key})
    file(GLOB aux_sources CONFIGURE_DEPENDS ${dir}/chunk_*.c ${dir}/table.c)
    add_library(${aux_target} STATIC ${aux_sources})
    set_target_properties(${aux_target} PROPERTIES
      ARCHIVE_OUTPUT_DIRECTORY ${POP_ARCHIVE_DIR} OUTPUT_NAME ${aux_target})
    target_include_directories(${aux_target} PRIVATE ${dir} ${POP_GEN_DIR} ${POP_ROOT} ${POP_ROOT}/runtime)
    # A module's own funcs.h reads the same overrides header, so a native
    # replacement can stand in for one of its functions as for the image's.
    if(RECOMP_OVERRIDE_HEADER)
      target_compile_definitions(${aux_target} PRIVATE
        RECOMP_OVERRIDE_HEADER="${RECOMP_OVERRIDE_HEADER}")
      target_include_directories(${aux_target} PRIVATE ${_override_dir})
    endif()
    target_compile_options(${aux_target} PRIVATE ${POP_WARN_GEN})
    if(DEFINED RECOMP_STORE_HOOKS)
      target_compile_definitions(${aux_target} PRIVATE RECOMP_STORE_HOOKS=${RECOMP_STORE_HOOKS})
    endif()
    if(DEFINED RECOMP_WRITE_TRACK_STORES)
      target_compile_definitions(${aux_target} PRIVATE RECOMP_WRITE_TRACK_STORES=${RECOMP_WRITE_TRACK_STORES})
    endif()
    if(RECOMP_FIXED_GUEST_BASE AND APPLE AND NOT IOS)
      target_compile_definitions(${aux_target} PRIVATE RECOMP_GUEST_FIXED_BASE=1)
    endif()
    if(RECOMP_LEAN_CALLS)
      target_compile_definitions(${aux_target} PRIVATE RECOMP_LEAN_CALLS=1)
    endif()
    pop_optimize(${aux_target} 2)
    pop_pgo(${aux_target})
    list(APPEND POP_GEN_AUX_TARGETS ${aux_target})
    message(STATUS "Auxiliary module translation: ${dir}")
  endforeach()
  # The web: the translation's tail calls (JUMP_FN) become return_call, which
  # the WebAssembly backend leaves as call + return before a shared return
  # block (tools/wasm_tail_calls.py). The archive is rewritten whenever it is.
  if(EMSCRIPTEN)
    foreach(gen_target recomp_gen ${POP_GEN_AUX_TARGETS})
      add_custom_command(TARGET ${gen_target} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/wasm_tail_calls.py $<TARGET_FILE:${gen_target}>
        VERBATIM)
    endforeach()
  endif()
  # The game's native replacements (game.toml [translate] native).
  if(RECOMP_NATIVE_HEADER AND NOT POP_TRANSLATE STREQUAL "STUB")
    target_sources(recomp_gen PRIVATE ${RECOMP_NATIVE_SOURCES})
    target_compile_definitions(recomp_gen PRIVATE RECOMP_OVERRIDE_HEADER="${RECOMP_NATIVE_HEADER}")
    # The modules' chunks read it too, so a native function can replace one
    # of theirs (granny2.dll's spline sampling) as it replaces the image's.
    get_filename_component(_native_dir ${RECOMP_NATIVE_HEADER} DIRECTORY)
    foreach(aux_target ${POP_GEN_AUX_TARGETS})
      target_compile_definitions(${aux_target} PRIVATE RECOMP_OVERRIDE_HEADER="${RECOMP_NATIVE_HEADER}")
      target_include_directories(${aux_target} PRIVATE ${_native_dir})
    endforeach()
    # They keep the arena pointer under fixed_guest_base (runtime/x86.h).
    set_property(SOURCE ${RECOMP_NATIVE_SOURCES} APPEND PROPERTY
      COMPILE_DEFINITIONS RECOMP_NO_FIXED_BASE=1)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # Host code that calls POSIX (clock_gettime, nanosleep) as the Apple
      # headers allow under -std=c11; glibc hides it there without this.
      set_property(SOURCE ${RECOMP_NATIVE_SOURCES} APPEND PROPERTY
        COMPILE_DEFINITIONS _DEFAULT_SOURCE)
    endif()
    # The game's addresses for its native sources ([translate] native
    # addresses): defined on those sources alone, so the generated chunks
    # neither include the header nor rebuild when it changes.
    if(RECOMP_ADDRESS_HEADER)
      if(NOT EXISTS ${RECOMP_ADDRESS_HEADER})
        message(FATAL_ERROR "RECOMP_ADDRESS_HEADER does not exist: ${RECOMP_ADDRESS_HEADER}")
      endif()
      set_property(SOURCE ${RECOMP_NATIVE_SOURCES} APPEND PROPERTY
        COMPILE_DEFINITIONS RECOMP_ADDRESS_HEADER="${RECOMP_ADDRESS_HEADER}")
    endif()
  endif()
endif()

# The portable spelling of -Wl,-force_load: every generated object is kept
# whether or not anything references it, because the dispatch table is
# reached by address.
function(pop_link_gen target)
  target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,recomp_gen>")
  foreach(aux_target ${POP_GEN_AUX_TARGETS})
    target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${aux_target}>")
  endforeach()
  target_include_directories(${target} PRIVATE ${POP_GEN_DIR})
  if(RECOMP_PGO STREQUAL "generate")
    target_link_options(${target} PRIVATE -fprofile-instr-generate)
  endif()
endfunction()
