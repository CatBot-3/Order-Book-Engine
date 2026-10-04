#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "obe/types.hpp"

// The display edge. Prices and times are integers everywhere inside the
// engine; this is the only place they become text. No floating point is used
// even here, so what is printed is exactly what was on the wire.

namespace obe::util {

// 1234500 -> "123.4500"
[[nodiscard]] inline std::string format_price(Price p) {
    std::array<char, 24> buf{};
    const int n =
        std::snprintf(buf.data(), buf.size(), "%u.%04u", p / kPriceScale, p % kPriceScale);
    return {buf.data(), static_cast<std::size_t>(n)};
}

// Nanoseconds since midnight -> "09:30:00.123456789"
[[nodiscard]] inline std::string format_time(Nanos ns) {
    const std::uint64_t total_seconds = ns / 1'000'000'000ULL;
    const auto frac = static_cast<unsigned>(ns % 1'000'000'000ULL);
    const auto h = static_cast<unsigned>(total_seconds / 3600);
    const auto m = static_cast<unsigned>((total_seconds / 60) % 60);
    const auto s = static_cast<unsigned>(total_seconds % 60);
    std::array<char, 40> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%02u:%02u:%02u.%09u", h, m, s, frac);
    return {buf.data(), static_cast<std::size_t>(n)};
}

// "HH:MM:SS" or "HH:MM:SS.fffffffff" (1 to 9 fractional digits) -> nanoseconds
// since midnight. Returns nullopt for anything else.
[[nodiscard]] inline std::optional<Nanos> parse_time(std::string_view text) {
    const auto two_digits = [](std::string_view s, unsigned& out) {
        if (s.size() != 2) {
            return false;
        }
        const auto r = std::from_chars(s.data(), s.data() + 2, out);
        return r.ec == std::errc{} && r.ptr == s.data() + 2;
    };
    if (text.size() < 8 || text[2] != ':' || text[5] != ':') {
        return std::nullopt;
    }
    unsigned h = 0;
    unsigned m = 0;
    unsigned s = 0;
    if (!two_digits(text.substr(0, 2), h) || !two_digits(text.substr(3, 2), m) ||
        !two_digits(text.substr(6, 2), s) || h > 23 || m > 59 || s > 59) {
        return std::nullopt;
    }
    std::uint64_t frac = 0;
    if (text.size() > 8) {
        const std::string_view digits = text.substr(9);
        if (text[8] != '.' || digits.empty() || digits.size() > 9) {
            return std::nullopt;
        }
        for (const char c : digits) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            frac = frac * 10 + static_cast<std::uint64_t>(c - '0');
        }
        for (std::size_t i = digits.size(); i < 9; ++i) {
            frac *= 10;
        }
    }
    const std::uint64_t seconds = std::uint64_t{h} * 3600 + std::uint64_t{m} * 60 + s;
    return seconds * 1'000'000'000ULL + frac;
}

// 1234567 -> "1,234,567"
[[nodiscard]] inline std::string with_commas(std::uint64_t v) {
    std::string digits = std::to_string(v);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t lead = digits.size() % 3;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (i % 3) == lead) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return out;
}

}  // namespace obe::util
