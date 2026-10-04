#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// Builds a message byte by byte at absolute offsets.
//
// This is deliberately independent of obe/feed: it uses plain shifts, not
// store_be, and offsets copied from the Nasdaq specification, not the field
// lists in messages.hpp. A decoder test that built its input with the project's
// own encoder would pass even if both had the same field in the wrong place.

namespace obe::test {

class Wire {
 public:
    Wire(char type, std::size_t size) : bytes_(size, std::byte{0}) {
        bytes_[0] = static_cast<std::byte>(type);
    }

    Wire& u8(std::size_t at, std::uint8_t v) {
        bytes_.at(at) = static_cast<std::byte>(v);
        return *this;
    }
    Wire& ch(std::size_t at, char c) { return u8(at, static_cast<std::uint8_t>(c)); }
    Wire& u16(std::size_t at, std::uint16_t v) { return be(at, v, 2); }
    Wire& u32(std::size_t at, std::uint32_t v) { return be(at, v, 4); }
    Wire& u48(std::size_t at, std::uint64_t v) { return be(at, v, 6); }
    Wire& u64(std::size_t at, std::uint64_t v) { return be(at, v, 8); }
    Wire& text(std::size_t at, std::string_view s) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            ch(at + i, s[i]);
        }
        return *this;
    }

    // Bytes 1 to 10, common to every message.
    Wire& header(std::uint16_t locate, std::uint16_t tracking, std::uint64_t timestamp) {
        return u16(1, locate).u16(3, tracking).u48(5, timestamp);
    }

    [[nodiscard]] const std::byte* data() const { return bytes_.data(); }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }
    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }

    // Append this message to a stream with its two-byte length prefix.
    void append_framed_to(std::vector<std::byte>& stream) const {
        stream.push_back(static_cast<std::byte>((bytes_.size() >> 8) & 0xff));
        stream.push_back(static_cast<std::byte>(bytes_.size() & 0xff));
        stream.insert(stream.end(), bytes_.begin(), bytes_.end());
    }

 private:
    Wire& be(std::size_t at, std::uint64_t v, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            const std::size_t shift = 8 * (width - 1 - i);
            bytes_.at(at + i) = static_cast<std::byte>((v >> shift) & 0xff);
        }
        return *this;
    }

    std::vector<std::byte> bytes_;
};

}  // namespace obe::test
