include(ObeDependencies)
obe_fetch_benchmark()

# The macro benchmark: a full replay of a file held in memory. See
# docs/benchmark-method.md for what it measures.
add_executable(replay_bench replay_bench.cpp)
target_link_libraries(replay_bench PRIVATE obe::obe)
target_compile_definitions(
  replay_bench PRIVATE "OBE_BUILD_FLAGS=\"${OBE_BUILD_FLAGS}\""
                       "OBE_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"")
set_target_properties(replay_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench")

# The matching engine on a recorded tape of requests.
add_executable(engine_bench engine_bench.cpp)
target_link_libraries(engine_bench PRIVATE obe::obe)
target_compile_definitions(
  engine_bench PRIVATE "OBE_BUILD_FLAGS=\"${OBE_BUILD_FLAGS}\""
                       "OBE_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"")
set_target_properties(engine_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench")

# One thread pushes numbered items through a queue, another checks them. Both
# the phase 6 queue comparison and the ordered stress run of criterion 6.
find_package(Threads REQUIRED)
add_executable(queue_bench queue_bench.cpp)
target_link_libraries(queue_bench PRIVATE obe::obe Threads::Threads)
target_compile_definitions(
  queue_bench PRIVATE "OBE_BUILD_FLAGS=\"${OBE_BUILD_FLAGS}\""
                      "OBE_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"")
set_target_properties(queue_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench")

# The three-thread pipeline against the single-threaded replay.
add_executable(pipeline_bench pipeline_bench.cpp)
target_link_libraries(pipeline_bench PRIVATE obe::obe Threads::Threads)
target_compile_definitions(
  pipeline_bench PRIVATE "OBE_BUILD_FLAGS=\"${OBE_BUILD_FLAGS}\""
                         "OBE_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"")
set_target_properties(pipeline_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                "${CMAKE_BINARY_DIR}/bench")

# What journaling every request costs, by batch size and with and without
# fdatasync, and what recovery costs.
add_executable(journal_bench journal_bench.cpp)
target_link_libraries(journal_bench PRIVATE obe::obe)
target_compile_definitions(
  journal_bench PRIVATE "OBE_BUILD_FLAGS=\"${OBE_BUILD_FLAGS}\""
                        "OBE_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"")
set_target_properties(journal_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench")

# Micro benchmarks of the building blocks (Google Benchmark).
add_executable(micro_bench micro_bench.cpp)
target_link_libraries(micro_bench PRIVATE obe::obe benchmark::benchmark benchmark::benchmark_main)
set_target_properties(micro_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench")
