include(ObeDependencies)
obe_fetch_googletest()
include(GoogleTest)
find_package(Threads REQUIRED)

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

# The matching engine (phase 5). The same split as the book: the suites run on
# the reference engine in the passing build, and again on the hand-written
# MatchingEngine, with the differential test against the reference added.
set(OBE_ENGINE_TEST_SOURCES engine/engine_scenario_test.cpp engine/instructions_test.cpp
                            engine/engine_property_test.cpp engine/round_trip_test.cpp)
obe_add_test(
  obe_engine_tests
  LABELS unit
  SOURCES ${OBE_ENGINE_TEST_SOURCES} engine/feed_writer_test.cpp gen/order_flow_test.cpp)

obe_add_test(
  obe_hand_written_engine_tests
  LABELS needs-your-code
  SOURCES ${OBE_ENGINE_TEST_SOURCES} engine/engine_differential_test.cpp)
target_compile_definitions(obe_hand_written_engine_tests PRIVATE OBE_TEST_HAND_WRITTEN)

# Queues, the seqlock and the three-thread pipeline (phase 6). The same split
# again: the suites run on the mutex queue in the passing build, and on the
# hand-written lock-free ring in the needs-your-code build. Run both under the
# tsan preset as well: that is where a wrong memory ordering shows.
set(OBE_QUEUE_TEST_SOURCES util/queue_test.cpp util/channel_test.cpp pipeline/pipeline_test.cpp)
obe_add_test(obe_concurrency_tests LABELS unit SOURCES ${OBE_QUEUE_TEST_SOURCES}
                                                       util/seqlock_test.cpp)
target_link_libraries(obe_concurrency_tests PRIVATE Threads::Threads)

obe_add_test(obe_hand_written_concurrency_tests LABELS needs-your-code SOURCES
             ${OBE_QUEUE_TEST_SOURCES})
target_link_libraries(obe_hand_written_concurrency_tests PRIVATE Threads::Threads)
target_compile_definitions(obe_hand_written_concurrency_tests PRIVATE OBE_TEST_HAND_WRITTEN)

# The network layer (phase 7): the wire formats, the packet sequencing, and the
# gateway over real loopback sockets.
obe_add_test(
  obe_net_tests
  LABELS unit
  SOURCES net/protocol_test.cpp net/moldudp_test.cpp net/udp_test.cpp net/gateway_test.cpp
          net/open_loop_test.cpp)

# The market-making simulator: the fill model on hand-built messages, its
# accounting over generated flow, and the strategies. The Avellaneda-Stoikov
# strategy is written by hand and has its own executable.
obe_add_test(
  obe_sim_tests
  LABELS unit
  SOURCES sim/sim_scenario_test.cpp sim/sim_property_test.cpp sim/strategy_test.cpp)
obe_add_test(obe_hand_written_sim_tests LABELS needs-your-code SOURCES
             sim/avellaneda_stoikov_test.cpp)

# The journal (stretch idea 1): the file format and its reader, recovery by
# replay, snapshots, and the file layer. Recovery by replay needs nothing from
# an engine but the ordinary contract, so that suite is typed over the engines
# and built a second time for the hand-written one.
obe_add_test(
  obe_journal_tests
  LABELS unit
  SOURCES journal/journal_format_test.cpp journal/recovery_test.cpp journal/snapshot_test.cpp
          journal/journal_file_test.cpp)
obe_add_test(obe_hand_written_journal_tests LABELS needs-your-code SOURCES
             journal/recovery_test.cpp)
target_compile_definitions(obe_hand_written_journal_tests PRIVATE OBE_TEST_HAND_WRITTEN)

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

# Success criterion 7 through the real apps: the matching engine's published
# feed, written to a file, replays cleanly through book_replay and is never
# locked or crossed.
if(OBE_BUILD_APPS AND OBE_BASH)
  add_test(
    NAME engine_feed_round_trip
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/flow_round_trip.sh" $<TARGET_FILE:flow_gen>
      $<TARGET_FILE:book_replay> "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(engine_feed_round_trip PROPERTIES LABELS "integration")

  # The same, for the hand-written engine, and its output against the
  # reference engine's.
  add_test(
    NAME hand_written_engine_feed_round_trip
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/flow_round_trip.sh" $<TARGET_FILE:flow_gen>
      $<TARGET_FILE:book_replay> "${CMAKE_CURRENT_BINARY_DIR}" --engine pooled)
  set_tests_properties(hand_written_engine_feed_round_trip PROPERTIES LABELS "needs-your-code")

  add_test(NAME engines_match_reference
           COMMAND "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/diff_engines.sh" --flow-gen
                   $<TARGET_FILE:flow_gen>)
  set_tests_properties(engines_match_reference PROPERTIES LABELS "needs-your-code")
endif()

if(OBE_BUILD_BENCH AND OBE_BASH)
  add_test(
    NAME engine_bench_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/engine_bench_smoke.sh" null
      $<TARGET_FILE:engine_bench> "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(engine_bench_smoke PROPERTIES LABELS "integration")

  add_test(
    NAME engine_bench_feed_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/engine_bench_smoke.sh" feed
      $<TARGET_FILE:engine_bench> "${CMAKE_CURRENT_BINARY_DIR}" --sinks feed)
  set_tests_properties(engine_bench_feed_smoke PROPERTIES LABELS "integration")
endif()

# The queue benchmark doubles as an ordered stress run: every item must arrive
# once, in order, intact. Short here; scripts/queue_stress.sh runs a billion.
if(OBE_BUILD_BENCH AND OBE_BASH)
  foreach(queue mutex mutex-spin)
    add_test(
      NAME queue_bench_${queue}_smoke
      COMMAND
        "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/queue_bench_smoke.sh" ${queue}
        $<TARGET_FILE:queue_bench> "${CMAKE_CURRENT_BINARY_DIR}" --queue ${queue})
    set_tests_properties(queue_bench_${queue}_smoke PROPERTIES LABELS "integration")
  endforeach()
  add_test(
    NAME queue_bench_ring_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/queue_bench_smoke.sh" ring
      $<TARGET_FILE:queue_bench> "${CMAKE_CURRENT_BINARY_DIR}" --queue ring)
  set_tests_properties(queue_bench_ring_smoke PROPERTIES LABELS "needs-your-code")
endif()

# The pipeline benchmark runs end to end and its output equals the single
# thread's.
if(OBE_BUILD_APPS AND OBE_BUILD_BENCH AND OBE_BASH)
  add_test(
    NAME pipeline_bench_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/pipeline_bench_smoke.sh" mutex
      $<TARGET_FILE:flow_gen> $<TARGET_FILE:pipeline_bench> "${CMAKE_CURRENT_BINARY_DIR}"
      --queue mutex --readers 1)
  set_tests_properties(pipeline_bench_smoke PROPERTIES LABELS "integration")

  add_test(
    NAME pipeline_bench_ring_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/pipeline_bench_smoke.sh" ring
      $<TARGET_FILE:flow_gen> $<TARGET_FILE:pipeline_bench> "${CMAKE_CURRENT_BINARY_DIR}"
      --queue ring)
  set_tests_properties(pipeline_bench_ring_smoke PROPERTIES LABELS "needs-your-code")
endif()

# Success criterion 8 in miniature: the exchange, a market-data subscriber and
# the load generator, as three processes on loopback.
if(OBE_BUILD_APPS AND OBE_BASH)
  add_test(
    NAME gateway_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/gateway_smoke.sh"
      $<TARGET_FILE:exchange_server> $<TARGET_FILE:load_gen> $<TARGET_FILE:md_listen>
      "${CMAKE_CURRENT_BINARY_DIR}")
  # They bind loopback ports; let nothing else run beside them.
  set_tests_properties(gateway_smoke PROPERTIES LABELS "integration" RUN_SERIAL TRUE)

  add_test(
    NAME hand_written_engine_gateway_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/gateway_smoke.sh"
      $<TARGET_FILE:exchange_server> $<TARGET_FILE:load_gen> $<TARGET_FILE:md_listen>
      "${CMAKE_CURRENT_BINARY_DIR}" --engine pooled)
  set_tests_properties(hand_written_engine_gateway_smoke PROPERTIES LABELS "needs-your-code"
                                                                    RUN_SERIAL TRUE)
endif()

# The market-making simulator through the real programs, on a generated
# market. Each run has its own directory: both write the same file names.
if(Python3_Interpreter_FOUND AND OBE_BUILD_APPS AND OBE_BASH)
  add_test(
    NAME mm_sim_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/mm_sim_smoke.sh" $<TARGET_FILE:flow_gen>
      $<TARGET_FILE:mm_sim> "${CMAKE_CURRENT_BINARY_DIR}/mm_sim_smoke")
  set_tests_properties(mm_sim_smoke PROPERTIES LABELS "integration")

  # The generated market moves as far in a few seconds as a real one does in a
  # day. With the default horizon of a whole session the formula would keep
  # the quotes out of reach of every trade, so the horizon is one second here.
  add_test(
    NAME mm_sim_avellaneda_stoikov_smoke
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/mm_sim_smoke.sh" $<TARGET_FILE:flow_gen>
      $<TARGET_FILE:mm_sim> "${CMAKE_CURRENT_BINARY_DIR}/mm_sim_as_smoke" --strategy as
      --max-tau 1)
  set_tests_properties(mm_sim_avellaneda_stoikov_smoke PROPERTIES LABELS "needs-your-code")
endif()

# The journal through the real program: an engine that is killed with SIGKILL
# in the middle of its writes, again and again, must end with the journal a
# run that was never killed writes, byte for byte. The second test is the same
# on the hand-written engine, which recovers by replay like any other.
if(OBE_BUILD_APPS AND OBE_BASH)
  add_test(
    NAME journal_crash_test
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/journal_crash_test.sh"
      $<TARGET_FILE:engine_journal> "${CMAKE_CURRENT_BINARY_DIR}/journal_crash_test")
  set_tests_properties(journal_crash_test PROPERTIES LABELS "integration")

  add_test(
    NAME journal_crash_test_hand_written_engine
    COMMAND
      "${OBE_BASH}" "${PROJECT_SOURCE_DIR}/scripts/journal_crash_test.sh"
      $<TARGET_FILE:engine_journal> "${CMAKE_CURRENT_BINARY_DIR}/journal_crash_test_pooled"
      20261006 pooled)
  set_tests_properties(journal_crash_test_hand_written_engine PROPERTIES LABELS
                                                                          "needs-your-code")
endif()
