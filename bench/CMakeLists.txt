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

# Micro benchmarks of the building blocks (Google Benchmark).
add_executable(micro_bench micro_bench.cpp)
target_link_libraries(micro_bench PRIVATE obe::obe benchmark::benchmark benchmark::benchmark_main)
set_target_properties(micro_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench")
