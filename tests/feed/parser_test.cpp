#include "obe/feed/parser.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "obe/gen/synthetic_feed.hpp"
#include "support/message_types.hpp"
#include "support/trace_handler.hpp"
#include "support/wire.hpp"

namespace {

using namespace obe;
using namespace obe::feed;
using obe::test::TraceHandler;
using obe::test::Wire;

// One zero-filled message of every type, in specification order.
std::vector<std::byte> one_of_each() {
    std::vector<std::byte> stream;
    for (const char type : kMessageTypes) {
        Wire(type, message_size(type)).append_framed_to(stream);
    }
    return stream;
}

ParseResult parse(std::span<const std::byte> buf, TraceHandler& handler) {
    ItchParser parser(handler);
    return parser.parse(buf);
}

// --- Static properties ------------------------------------------------------------

struct WrongParameterType : HandlerBase {
    void on_add(int) {}  // hides HandlerBase::on_add, so adds would be undeliverable
};
static_assert(ItchHandler<TraceHandler>);
static_assert(!ItchHandler<WrongParameterType>);
static_assert(!ItchHandler<int>);

TEST(MessageSize, AgreesWithTheCodecForEveryType) {
    std::set<char> seen;
    test::for_each_wire_type([&]<class M>(std::type_identity<M>, char type, std::size_t size) {
        EXPECT_EQ(message_size(type), size) << type;
        EXPECT_LE(size, kMaxMessageSize);
        seen.insert(type);
    });
    EXPECT_EQ(seen.size(), 23U);
    for (const char type : kMessageTypes) {
        EXPECT_TRUE(seen.contains(type)) << type;
    }
}

TEST(MessageSize, IsZeroForEveryOtherByte) {
    const std::set<char> known(kMessageTypes.begin(), kMessageTypes.end());
    for (int byte = 0; byte < 256; ++byte) {
        const char c = static_cast<char>(byte);
        if (!known.contains(c)) {
            EXPECT_EQ(message_size(c), 0U) << byte;
        }
    }
}

// --- Well-formed input ------------------------------------------------------------

TEST(Parser, EmptyBufferIsOk) {
    TraceHandler handler;
    const ParseResult r = parse({}, handler);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.messages, 0U);
    EXPECT_EQ(r.offset, 0U);
    EXPECT_TRUE(handler.trace.empty());
}

TEST(Parser, DeliversEveryTypeToItsCallbackInOrder) {
    const std::vector<std::byte> stream = one_of_each();
    TraceHandler handler;
    const ParseResult r = parse(stream, handler);
    EXPECT_TRUE(r.ok()) << to_string(r.status);
    EXPECT_EQ(r.messages, 23U);
    EXPECT_EQ(r.offset, stream.size());
    EXPECT_EQ(handler.trace, "SRHYLVWKJhAFECXDUPQBINO");
}

TEST(Parser, DispatchDecodesOneUnframedMessage) {
    struct Capture : HandlerBase {
        OrderReplace last{};
        int calls = 0;
        void on_replace(const OrderReplace& m) {
            last = m;
            ++calls;
        }
    } handler;
    const Wire w = Wire('U', 35)
                       .header(7, 0, 123'456'789)
                       .u64(11, 10)
                       .u64(19, 11)
                       .u32(27, 200)
                       .u32(31, 55'500);
    ItchParser parser(handler);
    ASSERT_EQ(parser.dispatch(w.data(), w.size()), ParseStatus::Ok);
    EXPECT_EQ(handler.calls, 1);
    EXPECT_EQ(handler.last.hdr.locate, 7);
    EXPECT_EQ(handler.last.hdr.timestamp, 123'456'789U);
    EXPECT_EQ(handler.last.orig_order_ref, 10U);
    EXPECT_EQ(handler.last.new_order_ref, 11U);
    EXPECT_EQ(handler.last.shares, 200U);
    EXPECT_EQ(handler.last.price, 55'500U);
}

TEST(Parser, PeekReadsHeaderFieldsWithoutDecoding) {
    const Wire w = Wire('D', 19).header(0xbeef, 0, 0x0000a1a2a3a4ULL);
    const Frame frame{.data = w.data(), .size = w.size()};
    EXPECT_EQ(frame.type(), 'D');
    EXPECT_EQ(peek_locate(frame), 0xbeef);
    EXPECT_EQ(peek_timestamp(frame), 0x0000a1a2a3a4ULL);
}

// --- Malformed input ----------------------------------------------------------------

TEST(Parser, StrayByteAfterLastMessageIsTruncatedLength) {
    std::vector<std::byte> stream = one_of_each();
    const std::size_t good = stream.size();
    stream.push_back(std::byte{0x00});
    TraceHandler handler;
    const ParseResult r = parse(stream, handler);
    EXPECT_EQ(r.status, ParseStatus::TruncatedLength);
    EXPECT_EQ(r.messages, 23U);  // everything before the problem was delivered
    EXPECT_EQ(r.offset, good);
}

TEST(Parser, MessageCutShortIsTruncatedMessage) {
    std::vector<std::byte> stream;
    Wire('S', 12).append_framed_to(stream);
    const std::size_t second = stream.size();
    Wire('A', 36).append_framed_to(stream);
    stream.resize(stream.size() - 1);
    TraceHandler handler;
    const ParseResult r = parse(stream, handler);
    EXPECT_EQ(r.status, ParseStatus::TruncatedMessage);
    EXPECT_EQ(r.messages, 1U);
    EXPECT_EQ(r.offset, second);
    EXPECT_EQ(handler.trace, "S");
}

TEST(Parser, UnknownTypeStopsTheParse) {
    std::vector<std::byte> stream;
    Wire('S', 12).append_framed_to(stream);
    Wire('Z', 12).append_framed_to(stream);
    Wire('S', 12).append_framed_to(stream);
    TraceHandler handler;
    const ParseResult r = parse(stream, handler);
    EXPECT_EQ(r.status, ParseStatus::UnknownType);
    EXPECT_EQ(r.messages, 1U);
    EXPECT_EQ(r.offset, 14U);
    EXPECT_EQ(handler.trace, "S");
}

TEST(Parser, WrongLengthForTypeIsRejectedBeforeDecoding) {
    // If the decoder ran on a short message it would read past the frame.
    // Each buffer is sized exactly, so AddressSanitizer would catch that.
    test::for_each_wire_type([]<class M>(std::type_identity<M>, char type, std::size_t size) {
        for (const std::size_t wrong : {size - 1, size + 1, std::size_t{1}}) {
            std::vector<std::byte> stream;
            Wire(type, wrong).append_framed_to(stream);
            stream.shrink_to_fit();
            TraceHandler handler;
            const ParseResult r = parse(stream, handler);
            EXPECT_EQ(r.status, ParseStatus::LengthMismatch) << type << " with length " << wrong;
            EXPECT_EQ(r.messages, 0U);
            EXPECT_TRUE(handler.trace.empty());
        }
    });
}

TEST(Parser, AddAndAttributedAddDoNotAcceptEachOthersLength) {
    for (const auto& [type, wrong] :
         {std::pair{'A', std::size_t{40}}, std::pair{'F', std::size_t{36}}}) {
        std::vector<std::byte> stream;
        Wire(type, wrong).append_framed_to(stream);
        TraceHandler handler;
        EXPECT_EQ(parse(stream, handler).status, ParseStatus::LengthMismatch) << type;
    }
}

TEST(Parser, ZeroLengthRecordIsRejected) {
    const std::vector<std::byte> stream{std::byte{0}, std::byte{0}};
    TraceHandler handler;
    const ParseResult r = parse(stream, handler);
    EXPECT_EQ(r.status, ParseStatus::LengthMismatch);
    EXPECT_EQ(r.offset, 0U);

    ItchParser parser(handler);
    EXPECT_EQ(parser.dispatch(nullptr, 0), ParseStatus::LengthMismatch);
}

TEST(Parser, TruncatingAValidStreamAnywhereIsHandled) {
    // Cut a valid stream at every byte offset. The parse succeeds exactly when
    // the cut lands on a record boundary, and never reads past the cut: each
    // prefix is copied to its own exactly-sized heap block.
    const std::vector<std::byte> stream =
        gen::make_synthetic_feed({.seed = 7, .symbols = 3, .messages = 120});

    std::set<std::size_t> boundaries{0};
    {
        FrameReader reader(stream);
        Frame frame;
        while (!reader.done()) {
            ASSERT_EQ(reader.next(frame), ParseStatus::Ok);
            boundaries.insert(reader.offset());
        }
    }
    ASSERT_GT(boundaries.size(), 120U);

    for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
        std::vector<std::byte> prefix(stream.begin(),
                                      stream.begin() + static_cast<std::ptrdiff_t>(cut));
        prefix.shrink_to_fit();
        TraceHandler handler;
        const ParseResult r = parse(prefix, handler);
        if (boundaries.contains(cut)) {
            ASSERT_TRUE(r.ok()) << "cut at " << cut;
            ASSERT_EQ(r.offset, cut);
        } else {
            ASSERT_TRUE(r.status == ParseStatus::TruncatedLength ||
                        r.status == ParseStatus::TruncatedMessage)
                << "cut at " << cut << ": " << to_string(r.status);
            ASSERT_TRUE(boundaries.contains(r.offset)) << "cut at " << cut;
        }
        ASSERT_EQ(r.messages, handler.trace.size());
    }
}

TEST(Parser, RandomBytesNeverCrash) {
    gen::SplitMix64 rng(0xfeed);
    for (int round = 0; round < 20'000; ++round) {
        std::vector<std::byte> buf(rng.below(200));
        for (std::byte& b : buf) {
            b = static_cast<std::byte>(rng.below(256));
        }
        // Bias some buffers towards plausible framing so the decoders run too.
        if (buf.size() >= 3 && rng.below(2) == 0) {
            const char type = kMessageTypes[rng.below(kMessageTypes.size())];
            buf[0] = std::byte{0};
            buf[1] = static_cast<std::byte>(message_size(type));
            buf[2] = static_cast<std::byte>(type);
        }
        buf.shrink_to_fit();
        TraceHandler handler;
        const ParseResult r = parse(buf, handler);
        ASSERT_LE(r.offset, buf.size());
        ASSERT_EQ(r.messages, handler.trace.size());
    }
}

TEST(Parser, CorruptedValidStreamNeverCrashes) {
    const std::vector<std::byte> clean =
        gen::make_synthetic_feed({.seed = 11, .symbols = 4, .messages = 400});
    gen::SplitMix64 rng(0xc0ffee);
    for (int round = 0; round < 2'000; ++round) {
        std::vector<std::byte> buf = clean;
        const std::uint64_t flips = rng.between(1, 8);
        for (std::uint64_t i = 0; i < flips; ++i) {
            buf[rng.below(buf.size())] = static_cast<std::byte>(rng.below(256));
        }
        TraceHandler handler;
        const ParseResult r = parse(buf, handler);
        ASSERT_LE(r.offset, buf.size());
        ASSERT_EQ(r.messages, handler.trace.size());
    }
}

// --- The synthetic generator, which later tests rely on ------------------------------

TEST(SyntheticFeed, IsDeterministicPerSeed) {
    const gen::SyntheticConfig cfg{.seed = 42, .symbols = 5, .messages = 5'000};
    const std::vector<std::byte> a = gen::make_synthetic_feed(cfg);
    const std::vector<std::byte> b = gen::make_synthetic_feed(cfg);
    EXPECT_EQ(a, b);

    gen::SyntheticConfig other = cfg;
    other.seed = 43;
    EXPECT_NE(a, gen::make_synthetic_feed(other));
}

TEST(SyntheticFeed, ParsesCleanlyAndCoversEveryMessageType) {
    const std::vector<std::byte> stream =
        gen::make_synthetic_feed({.seed = 3, .symbols = 8, .messages = 50'000});
    TraceHandler handler;
    const ParseResult r = parse(stream, handler);
    ASSERT_TRUE(r.ok()) << to_string(r.status) << " at " << r.offset;
    // preamble: 'O', then R and H per symbol, 'S', 'Q'. Trailer: 'M', 'E', 'C'.
    EXPECT_EQ(r.messages, 50'000U + 1 + 2 * 8 + 2 + 3);
    for (const char type : kMessageTypes) {
        EXPECT_NE(handler.trace.find(type), std::string::npos) << "no '" << type << "' generated";
    }
    // The file layout check in spec section 4.2: the first record is a System Event.
    EXPECT_EQ(handler.trace.front(), 'S');
    EXPECT_EQ(handler.trace.back(), 'S');
}

TEST(SyntheticFeed, TimestampsNeverGoBackwards) {
    const std::vector<std::byte> stream =
        gen::make_synthetic_feed({.seed = 5, .symbols = 4, .messages = 10'000});
    FrameReader reader(stream);
    Frame frame;
    Nanos previous = 0;
    while (!reader.done()) {
        ASSERT_EQ(reader.next(frame), ParseStatus::Ok);
        const Nanos now = peek_timestamp(frame);
        ASSERT_GE(now, previous);
        previous = now;
    }
}

}  // namespace
