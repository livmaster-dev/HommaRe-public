# The warning sets the shell scripts used, named so a target says which one it
# wants instead of repeating the flags.
set(POP_WARN_HOST -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter)
set(POP_WARN_STRICT -Wall -Wextra -Wno-unused-parameter)
set(POP_WARN_WERROR -Wall -Wextra -Werror)
# libstdc++ before 13 (Debian 12, the linux-arm64 sysroot) calls the deprecated
# std::get_temporary_buffer inside std::stable_sort, which newer clang blames
# on the caller; on Linux a deprecation stays a warning.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND POP_WARN_WERROR -Wno-error=deprecated-declarations)
endif()
set(POP_WARN_GEN -Wall -Wextra -Wno-unused)

# Optimization is per target, as the scripts had it: -O2 for what players run,
# -O1 for hosts and tests. A Debug configuration leaves this out and gets -O0.
function(pop_optimize target level)
  target_compile_options(${target} PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O${level}>)
endfunction()

# Profile-guided optimization of the translated code (tools/build.py --pgo).
# RECOMP_PGO=generate instruments it; run the game with
# LLVM_PROFILE_FILE=<dir>/game-%c%p.profraw (%c keeps the counters current
# without a clean exit), merge with `xcrun llvm-profdata merge`, then
# RECOMP_PGO=<file>.profdata optimizes with that profile.
set(RECOMP_PGO "" CACHE STRING "Translated-code PGO: empty, generate, or a .profdata path")
function(pop_pgo target)
  if(RECOMP_PGO STREQUAL "generate")
    target_compile_options(${target} PRIVATE -fprofile-instr-generate)
  elseif(RECOMP_PGO)
    target_compile_options(${target} PRIVATE -fprofile-instr-use=${RECOMP_PGO}
      -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing)
  endif()
endfunction()

# Every test executable registers here so `check_binaries` builds exactly the
# suite this platform has; tools/test.py --compile-only builds that target.
function(pop_test_binary target)
  add_dependencies(check_binaries ${target})
endfunction()
