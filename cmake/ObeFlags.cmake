# Warnings, sanitizers and the flag string that the benchmark report prints.

# Warnings apply only to this project's targets, never to fetched dependencies.
add_library(obe_warnings INTERFACE)
target_compile_options(
  obe_warnings
  INTERFACE -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wold-style-cast
            -Wcast-align
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wdouble-promotion)
# -Wnull-dereference is left out on purpose. With GCC at -O3 it reports
# "potential null pointer dereference" inside GoogleTest's own headers and on
# every find(...)->field in the tests, and with warnings as errors that breaks
# the Release build. AddressSanitizer covers the real cases.
if(OBE_WARNINGS_AS_ERRORS)
  target_compile_options(obe_warnings INTERFACE -Werror)
endif()

# Sanitizers are global on purpose: ThreadSanitizer in particular gives false
# reports when only part of the program is instrumented.
if(OBE_SANITIZE)
  string(REPLACE "," ";" _obe_san_list "${OBE_SANITIZE}")
  string(REPLACE ";" "," _obe_san_arg "${_obe_san_list}")
  add_compile_options(-fsanitize=${_obe_san_arg} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${_obe_san_arg})
  message(STATUS "obe: sanitizers enabled: ${_obe_san_arg}")
  # With AddressSanitizer, also have libstdc++ mark the unused part of every
  # std::vector as off limits. Without it, reading v[v.size()] is undefined but
  # lands inside the vector's own allocation, where AddressSanitizer cannot see
  # it. It must be set for every translation unit that touches a vector, which
  # is why it is here, next to the sanitizer flags, and global. libc++ does the
  # same without being asked.
  if("address" IN_LIST _obe_san_list)
    add_compile_definitions(_GLIBCXX_SANITIZE_VECTOR)
  endif()
endif()

if(OBE_NATIVE)
  add_compile_options(-march=native)
endif()

# Link-time optimization (phase 4, experiment 10). Expect little: the library is
# header-only and every app is one translation unit, so the compiler already
# sees everything LTO would show it. Whether "little" is "nothing" is the
# experiment.
set(_obe_extra_flags "")
if(OBE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _obe_ipo_ok OUTPUT _obe_ipo_msg)
  if(_obe_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    string(APPEND _obe_extra_flags " -flto")
  else()
    message(WARNING "obe: link-time optimization is not supported here: ${_obe_ipo_msg}")
  endif()
endif()

# Profile-guided optimization (also experiment 10), in two builds of the same
# directory: "generate" produces an instrumented binary that writes a profile
# when run, "use" recompiles with that profile. scripts/pgo_build.sh drives it.
# The directory must be the same for both, because GCC names its profile files
# after the object files' paths.
if(OBE_PGO)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(OBE_PGO STREQUAL "generate")
      add_compile_options(-fprofile-generate -fprofile-dir=${OBE_PGO_DIR})
      add_link_options(-fprofile-generate)
    elseif(OBE_PGO STREQUAL "use")
      add_compile_options(-fprofile-use -fprofile-dir=${OBE_PGO_DIR} -fprofile-correction
                          -Wno-missing-profile)
    else()
      message(FATAL_ERROR "OBE_PGO must be 'generate' or 'use', not '${OBE_PGO}'")
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(OBE_PGO STREQUAL "generate")
      add_compile_options(-fprofile-generate=${OBE_PGO_DIR})
      add_link_options(-fprofile-generate=${OBE_PGO_DIR})
    elseif(OBE_PGO STREQUAL "use")
      add_compile_options(-fprofile-use=${OBE_PGO_DIR}/merged.profdata
                          -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
    else()
      message(FATAL_ERROR "OBE_PGO must be 'generate' or 'use', not '${OBE_PGO}'")
    endif()
  else()
    message(FATAL_ERROR "OBE_PGO supports GCC and Clang only")
  endif()
  string(APPEND _obe_extra_flags " -fprofile-${OBE_PGO}")
  message(STATUS "obe: profile-guided optimization: ${OBE_PGO} (profiles in ${OBE_PGO_DIR})")
endif()

# A benchmark number is only meaningful next to the flags that produced it, so
# the flags are baked into the binaries and printed by replay_bench.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _obe_bt)
set(OBE_BUILD_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_obe_bt}}")
if(OBE_NATIVE)
  string(APPEND OBE_BUILD_FLAGS " -march=native")
endif()
if(OBE_SANITIZE)
  string(APPEND OBE_BUILD_FLAGS " -fsanitize=${OBE_SANITIZE}")
endif()
string(APPEND OBE_BUILD_FLAGS "${_obe_extra_flags}")
string(STRIP "${OBE_BUILD_FLAGS}" OBE_BUILD_FLAGS)
string(REGEX REPLACE " +" " " OBE_BUILD_FLAGS "${OBE_BUILD_FLAGS}")
