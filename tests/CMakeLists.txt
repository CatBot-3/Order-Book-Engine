include(ObeDependencies)
obe_fetch_googletest()
include(GoogleTest)

# obe_add_test(<name> LABELS <labels...> SOURCES <files...>)
function(obe_add_test name)
  cmake_parse_arguments(ARG "" "" "LABELS;SOURCES" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE obe::obe GTest::gtest GTest::gmock GTest::gtest_main)
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 PROPERTIES LABELS "${ARG_LABELS}")
endfunction()

# Everything that is complete. These pass on a fresh clone.
obe_add_test(
  obe_tests
  LABELS unit
  SOURCES smoke_test.cpp
          feed/endian_test.cpp
          feed/codec_test.cpp
          feed/parser_test.cpp
          util/format_test.cpp
          util/latency_histogram_test.cpp
          util/measurement_test.cpp
          book/bbo_hash_test.cpp)

# The book. These exercise OrderStore and PriceLevels, which are written by hand
# (include/obe/book/order_store.hpp and price_levels.hpp). Until those are
# filled in, every test here fails with a message naming the missing function.
# To run only the finished parts: ctest --preset debug -LE needs-your-code
obe_add_test(
  obe_book_tests
  LABELS needs-your-code
  SOURCES book/order_store_test.cpp book/price_levels_test.cpp book/book_manager_test.cpp
          book/book_replay_test.cpp)

# The integration tests below are shell scripts. They are run through bash
# explicitly so they work on a checkout that lost its executable bits, which is
# what a repository kept on a Windows drive and built from WSL tends to be.
find_program(OBE_BASH bash)

# Success criterion 1 on a synthetic file: the C++ parser and the independent
# Python counter must report identical per-type counts.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND OBE_BUILD_APPS AND OBE_BASH)
  add_test(
    NAME counts_match_independent_counter
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/check_counts.sh" --synth $<TARGET_FILE:itch_synth>
      --stats $<TARGET_FILE:itch_stats> --python "${Python3_EXECUTABLE}" --workdir
      "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(counts_match_independent_counter PROPERTIES LABELS "integration")
endif()

# Success criterion 2 on a synthetic file: a full replay through the real app
# ends with every invariant counter at zero.
if(OBE_BUILD_APPS AND OBE_BASH)
  add_test(
    NAME book_replay_synthetic_is_clean
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/replay_synthetic.sh" $<TARGET_FILE:itch_synth>
      $<TARGET_FILE:book_replay> "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(book_replay_synthetic_is_clean PROPERTIES LABELS "needs-your-code")
endif()

# The benchmark harness must keep running end to end. These check that it
# works, not how fast anything is: a sanitizer build's timings mean nothing.
if(OBE_BUILD_APPS AND OBE_BUILD_BENCH AND OBE_BASH)
  add_test(
    NAME replay_bench_parse_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/bench_smoke.sh" parse $<TARGET_FILE:itch_synth>
      $<TARGET_FILE:replay_bench> "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(replay_bench_parse_smoke PROPERTIES LABELS "integration")

  add_test(
    NAME replay_bench_book_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/bench_smoke.sh" book $<TARGET_FILE:itch_synth>
      $<TARGET_FILE:replay_bench> "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(replay_bench_book_smoke PROPERTIES LABELS "needs-your-code")
endif()
