include(ObeDependencies)
obe_fetch_googletest()
include(GoogleTest)

# obe_add_test(<name> LABELS <labels...> SOURCES <files...>)
function(obe_add_test name)
  cmake_parse_arguments(ARG "" "" "LABELS;SOURCES" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE obe::obe GTest::gtest GTest::gmock GTest::gtest_main)
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  # The timeout turns a container that has stopped making progress (an
  # open-addressing table clogged with deleted markers, say) into a failure
  # instead of a hung test run.
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT 300)
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
          util/huge_pages_test.cpp
          book/bbo_hash_test.cpp
          book/implementations_test.cpp)

# The book, on the reference containers.
set(OBE_BOOK_TEST_SOURCES book/order_store_test.cpp book/price_levels_test.cpp
                          book/book_manager_test.cpp book/book_replay_test.cpp)
obe_add_test(obe_book_tests LABELS unit SOURCES ${OBE_BOOK_TEST_SOURCES})

# The same book tests again, on the phase 4 containers that are written by hand
# (FlatOrderStore, VectorPriceLevels, util::Pool), plus the tests that only
# make sense for them: the differential check against the reference, and the
# pool's own suite. Until a container is written, its tests fail with a message
# naming the missing function.
#
#   ctest --preset debug -LE needs-your-code     everything that is finished
#   ctest --preset debug -L needs-your-code      the hand-written parts
obe_add_test(
  obe_hand_written_tests
  LABELS needs-your-code
  SOURCES ${OBE_BOOK_TEST_SOURCES} book/differential_test.cpp util/pool_test.cpp)
target_compile_definitions(obe_hand_written_tests PRIVATE OBE_TEST_HAND_WRITTEN)

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
  set_tests_properties(book_replay_synthetic_is_clean PROPERTIES LABELS "integration")
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
  set_tests_properties(replay_bench_book_smoke PROPERTIES LABELS "integration")

  # The experiment switches, on the reference book.
  add_test(
    NAME replay_bench_prefetch_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/bench_smoke.sh" prefetch
      $<TARGET_FILE:itch_synth> $<TARGET_FILE:replay_bench> "${CMAKE_CURRENT_BINARY_DIR}"
      --prefetch --reserve 8000)
  set_tests_properties(replay_bench_prefetch_smoke PROPERTIES LABELS "integration")

  add_test(
    NAME replay_bench_filter_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/bench_smoke.sh" filter
      $<TARGET_FILE:itch_synth> $<TARGET_FILE:replay_bench> "${CMAKE_CURRENT_BINARY_DIR}"
      --symbols S00001,S00017,S00042)
  set_tests_properties(replay_bench_filter_smoke PROPERTIES LABELS "integration")
endif()

# The feed profiler runs end to end and its level accounting agrees with the
# book it profiled.
if(OBE_BUILD_APPS AND OBE_BASH)
  add_test(
    NAME feed_profile_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/profile_smoke.sh" $<TARGET_FILE:itch_synth>
      $<TARGET_FILE:feed_profile> $<TARGET_FILE:book_replay> "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(feed_profile_smoke PROPERTIES LABELS "integration")

  # Success criterion 3 through the real app: every optimized implementation
  # must print the same per-security hashes as the reference.
  add_test(
    NAME optimized_books_match_reference
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/diff_books.sh" --replay
      $<TARGET_FILE:book_replay> --synth $<TARGET_FILE:itch_synth> --workdir
      "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(optimized_books_match_reference PROPERTIES LABELS "needs-your-code")
endif()
