#include "obe/util/huge_pages.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace {

using obe::util::HugePageBuffer;

constexpr std::size_t kMiB = std::size_t{1} << 20;

TEST(HugePageBuffer, DefaultIsEmpty) {
    const HugePageBuffer buffer;
    EXPECT_EQ(buffer.data(), nullptr);
    EXPECT_EQ(buffer.size(), 0U);
    EXPECT_EQ(buffer.huge_bytes(), 0U);

    const HugePageBuffer zero(0);
    EXPECT_EQ(zero.data(), nullptr);
    EXPECT_EQ(zero.size(), 0U);
}

TEST(HugePageBuffer, IsAlignedRoundedUpZeroFilledAndWritable) {
    HugePageBuffer buffer(3 * kMiB);
    ASSERT_NE(buffer.data(), nullptr);
    EXPECT_EQ(buffer.size(), 4 * kMiB) << "rounded up to a whole number of 2 MB pages";
    EXPECT_EQ(reinterpret_cast<std::size_t>(buffer.data()) % HugePageBuffer::kHugePageSize, 0U)
        << "an unaligned range cannot be backed by huge pages";
    EXPECT_TRUE(buffer.requested_huge());

    // Fresh anonymous memory is zero. Check both ends and the middle, then
    // write to every byte; AddressSanitizer would not see a bad mmap, but a
    // fault here would.
    EXPECT_EQ(buffer.data()[0], std::byte{0});
    EXPECT_EQ(buffer.data()[buffer.size() / 2], std::byte{0});
    EXPECT_EQ(buffer.data()[buffer.size() - 1], std::byte{0});
    std::memset(buffer.data(), 0x5a, buffer.size());
    EXPECT_EQ(buffer.data()[buffer.size() - 1], std::byte{0x5a});

    // Whether the kernel granted huge pages depends on the machine. The call
    // must not fail either way, and cannot report more than the buffer holds.
    EXPECT_LE(buffer.huge_bytes(), buffer.size());
}

TEST(HugePageBuffer, AnExactMultipleIsNotRoundedFurther) {
    const HugePageBuffer buffer(2 * kMiB);
    EXPECT_EQ(buffer.size(), 2 * kMiB);
}

TEST(HugePageBuffer, CanAskForOrdinaryPagesForTheBeforeMeasurement) {
    HugePageBuffer buffer(2 * kMiB, false);
    ASSERT_NE(buffer.data(), nullptr);
    EXPECT_FALSE(buffer.requested_huge());
    std::memset(buffer.data(), 1, buffer.size());
    EXPECT_EQ(buffer.huge_bytes(), 0U) << "huge pages were explicitly declined for this range";
}

TEST(HugePageBuffer, MovingTransfersOwnership) {
    HugePageBuffer a(2 * kMiB);
    std::byte* data = a.data();
    data[100] = std::byte{42};

    HugePageBuffer b(std::move(a));
    // Reading a moved-from object is the point of these two lines: the class
    // promises it is left empty.
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(a.data(), nullptr);
    EXPECT_EQ(a.size(), 0U);
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(b.data(), data);
    EXPECT_EQ(b.data()[100], std::byte{42});

    HugePageBuffer c(4 * kMiB);
    c = std::move(b);
    EXPECT_EQ(c.data(), data);
    EXPECT_EQ(c.size(), 2 * kMiB);
    EXPECT_EQ(c.data()[100], std::byte{42});
}

TEST(HugePages, SettingAndProcessTotalCanBeRead) {
    // Both read /proc or /sys and return an empty or zero answer if they
    // cannot. Neither may throw.
    const std::string setting = obe::util::transparent_huge_page_setting();
    if (!setting.empty()) {
        EXPECT_NE(setting.find('['), std::string::npos)
            << "the active choice is shown in brackets: " << setting;
    }
    EXPECT_EQ(obe::util::process_huge_bytes() % 1024, 0U) << "reported in whole kilobytes";
}

}  // namespace
