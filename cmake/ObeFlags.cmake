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
endif()

if(OBE_NATIVE)
  add_compile_options(-march=native)
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
string(STRIP "${OBE_BUILD_FLAGS}" OBE_BUILD_FLAGS)
string(REGEX REPLACE " +" " " OBE_BUILD_FLAGS "${OBE_BUILD_FLAGS}")
