if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "OBE_BUILD_FUZZ needs Clang: libFuzzer ships with it. Use the 'fuzz' preset.")
endif()

# obe_add_fuzzer(<name> <sources...>)
function(obe_add_fuzzer name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE obe::obe)
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

obe_add_fuzzer(parser_fuzz parser_fuzz.cpp)
