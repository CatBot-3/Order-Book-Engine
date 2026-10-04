#include "obe/util/format.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "obe/util/mapped_file.hpp"

namespace {

using namespace obe;
using namespace obe::util;

TEST(Format, PriceKeepsAllFourDecimals) {
    EXPECT_EQ(format_price(0), "0.0000");
    EXPECT_EQ(format_price(1), "0.0001");
    EXPECT_EQ(format_price(1'234'500), "123.4500");
    EXPECT_EQ(format_price(10'000), "1.0000");
    EXPECT_EQ(format_price(4'294'967'295U), "429496.7295");  // largest 32-bit price
}

TEST(Format, TimeIsNanosecondsSinceMidnight) {
    EXPECT_EQ(format_time(0), "00:00:00.000000000");
    EXPECT_EQ(format_time(34'200'000'000'000ULL), "09:30:00.000000000");
    EXPECT_EQ(format_time(57'600'123'456'789ULL), "16:00:00.123456789");
    EXPECT_EQ(format_time(86'399'999'999'999ULL), "23:59:59.999999999");
}

TEST(Format, ParseTimeAcceptsWholeAndFractionalSeconds) {
    EXPECT_EQ(parse_time("09:30:00"), 34'200'000'000'000ULL);
    EXPECT_EQ(parse_time("09:30:00.5"), 34'200'500'000'000ULL);
    EXPECT_EQ(parse_time("16:00:00.123456789"), 57'600'123'456'789ULL);
    EXPECT_EQ(parse_time("00:00:00.000000001"), 1ULL);
}

TEST(Format, ParseTimeRejectsEverythingElse) {
    for (const char* bad :
         {"", "9:30:00", "09:30", "09-30-00", "24:00:00", "09:60:00", "09:30:60", "09:30:00.",
          "09:30:00.1234567890", "09:30:00.12a", "09:30:00 ", "ab:cd:ef", "+9:30:00"}) {
        EXPECT_FALSE(parse_time(bad).has_value()) << "'" << bad << "'";
    }
}

TEST(Format, ParseTimeRoundTripsThroughFormatTime) {
    for (const Nanos t : {0ULL, 1ULL, 34'200'000'000'000ULL, 86'399'999'999'999ULL}) {
        EXPECT_EQ(parse_time(format_time(t)), t);
    }
}

TEST(Format, WithCommas) {
    EXPECT_EQ(with_commas(0), "0");
    EXPECT_EQ(with_commas(999), "999");
    EXPECT_EQ(with_commas(1'000), "1,000");
    EXPECT_EQ(with_commas(123'456), "123,456");
    EXPECT_EQ(with_commas(1'234'567), "1,234,567");
    EXPECT_EQ(with_commas(18'446'744'073'709'551'615ULL), "18,446,744,073,709,551,615");
}

TEST(MappedFile, MapsContentsAndHandlesEmptyAndMissingFiles) {
    const std::string dir = ::testing::TempDir();
    const std::string path = dir + "obe_mapped_file_test.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out << "hello";
    }
    {
        const MappedFile file(path);
        ASSERT_EQ(file.size(), 5U);
        EXPECT_EQ(static_cast<char>(file.bytes()[0]), 'h');
        EXPECT_EQ(static_cast<char>(file.bytes()[4]), 'o');
        EXPECT_FALSE(looks_gzipped(file.bytes()));
    }
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); }
    {
        const MappedFile empty(path);
        EXPECT_EQ(empty.size(), 0U);
        EXPECT_TRUE(empty.bytes().empty());
    }
    std::remove(path.c_str());
    EXPECT_THROW(MappedFile{path}, std::runtime_error);
    EXPECT_THROW(MappedFile{dir}, std::runtime_error);  // a directory is not a regular file
}

TEST(MappedFile, RecognisesGzipMagic) {
    const std::array<std::byte, 3> gz{std::byte{0x1f}, std::byte{0x8b}, std::byte{0x08}};
    const std::array<std::byte, 1> tiny{std::byte{0x1f}};
    EXPECT_TRUE(looks_gzipped(gz));
    EXPECT_FALSE(looks_gzipped(tiny));
}

}  // namespace
