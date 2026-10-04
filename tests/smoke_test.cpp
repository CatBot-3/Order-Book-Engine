#include <gtest/gtest.h>

#include <bit>
#include <span>

// Proves the toolchain: the test runner links, and the compiler really is in
// C++20 mode with the library features the project leans on.
TEST(Smoke, ToolchainIsCpp20) {
    EXPECT_GE(__cplusplus, 202002L);
    EXPECT_TRUE(std::endian::native == std::endian::little ||
                std::endian::native == std::endian::big);
    const int values[] = {1, 2, 3};
    EXPECT_EQ(std::span<const int>(values).size(), 3U);
}
