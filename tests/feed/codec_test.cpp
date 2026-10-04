#include "obe/feed/codec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "obe/feed/parser.hpp"
#include "obe/gen/synthetic_feed.hpp"
#include "support/message_types.hpp"
#include "support/wire.hpp"

// Every decoder is checked against a message built at the absolute offsets of
// the Nasdaq specification. Each field gets a distinct value so that a swapped
// or shifted field cannot go unnoticed.

namespace {

using namespace obe;
using namespace obe::feed;
using obe::test::Wire;

constexpr std::uint16_t kLocate = 0x1234;
constexpr std::uint16_t kTracking = 0x5678;
constexpr std::uint64_t kTimestamp = 0x0a0b0c0d0e0fULL;

void expect_header(const Header& hdr) {
    EXPECT_EQ(hdr.locate, kLocate);
    EXPECT_EQ(hdr.tracking, kTracking);
    EXPECT_EQ(hdr.timestamp, kTimestamp);
}

Wire wire(char type, std::size_t size) {
    Wire w(type, size);
    w.header(kLocate, kTracking, kTimestamp);
    return w;
}

// --- Literal bytes ---------------------------------------------------------

TEST(Decode, AddOrderFromLiteralBytes) {
    // Add Order, 36 bytes, written out in full.
    const std::array<std::uint8_t, 36> raw{
        'A',                                 // message type
        0x00, 0x2a,                          // stock locate 42
        0x00, 0x00,                          // tracking number
        0x1f, 0x1a, 0xce, 0xd9, 0xf0, 0x00,  // 09:30:00.000000000 = 34'200'000'000'000 ns
        0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x42, 0x40,  // order reference 1'000'000
        'B',                                             // buy
        0x00, 0x00, 0x01, 0x2c,                          // 300 shares
        'A',  'A',  'P',  'L',  ' ',  ' ',  ' ',  ' ',   // stock
        0x00, 0x1a, 0x5e, 0x24,                          // price 1'728'036 = $172.8036
    };
    std::array<std::byte, 36> bytes{};
    std::memcpy(bytes.data(), raw.data(), raw.size());

    const AddOrder m = decode<AddOrder>(bytes.data());
    EXPECT_EQ(m.hdr.locate, 42);
    EXPECT_EQ(m.hdr.tracking, 0);
    EXPECT_EQ(m.hdr.timestamp, 34'200'000'000'000ULL);
    EXPECT_EQ(m.order_ref, 1'000'000ULL);
    EXPECT_EQ(m.side, Side::Buy);
    EXPECT_EQ(m.shares, 300U);
    EXPECT_EQ(m.stock.view(), "AAPL");
    EXPECT_EQ(m.price, 1'728'036U);
    EXPECT_FALSE(m.attributed);
    EXPECT_EQ(m.type(), 'A');
}

TEST(Decode, TimestampUsesAll48Bits) {
    Wire w('S', 12);
    w.header(0, 0, 0xffffffffffffULL).ch(11, 'O');
    EXPECT_EQ(decode<SystemEvent>(w.data()).hdr.timestamp, 281'474'976'710'655ULL);

    // The bytes either side of the timestamp must not leak into it.
    Wire x('S', 12);
    x.u16(3, 0xffff).u48(5, 0).ch(11, static_cast<char>(0xff));
    EXPECT_EQ(decode<SystemEvent>(x.data()).hdr.timestamp, 0U);
}

// --- One test per message type, offsets from the specification ------------------

TEST(Decode, SystemEvent) {
    const Wire w = wire('S', 12).ch(11, 'Q');
    const SystemEvent m = decode<SystemEvent>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.event_code, 'Q');
}

TEST(Decode, StockDirectory) {
    const Wire w = wire('R', 39)
                       .text(11, "MSFT    ")
                       .ch(19, 'Q')
                       .ch(20, 'N')
                       .u32(21, 100)
                       .ch(25, 'N')
                       .ch(26, 'C')
                       .text(27, "Z ")
                       .ch(29, 'P')
                       .ch(30, 'Y')
                       .ch(31, 'I')
                       .ch(32, '1')
                       .ch(33, 'E')
                       .u32(34, 0x01020304U)
                       .ch(38, 'V');
    const StockDirectory m = decode<StockDirectory>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.stock.view(), "MSFT");
    EXPECT_EQ(m.market_category, 'Q');
    EXPECT_EQ(m.financial_status, 'N');
    EXPECT_EQ(m.round_lot_size, 100U);
    EXPECT_EQ(m.round_lots_only, 'N');
    EXPECT_EQ(m.issue_classification, 'C');
    EXPECT_EQ(m.issue_subtype, (std::array<char, 2>{'Z', ' '}));
    EXPECT_EQ(m.authenticity, 'P');
    EXPECT_EQ(m.short_sale_threshold, 'Y');
    EXPECT_EQ(m.ipo_flag, 'I');
    EXPECT_EQ(m.luld_tier, '1');
    EXPECT_EQ(m.etp_flag, 'E');
    EXPECT_EQ(m.etp_leverage, 0x01020304U);
    EXPECT_EQ(m.inverse_indicator, 'V');
}

TEST(Decode, TradingAction) {
    const Wire w = wire('H', 25).text(11, "TSLA    ").ch(19, 'H').ch(20, 'r').text(21, "LUDP");
    const TradingAction m = decode<TradingAction>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.stock.view(), "TSLA");
    EXPECT_EQ(m.trading_state, 'H');
    EXPECT_EQ(m.reserved, 'r');
    EXPECT_EQ(m.reason, (std::array<char, 4>{'L', 'U', 'D', 'P'}));
}

TEST(Decode, RegSho) {
    const Wire w = wire('Y', 20).text(11, "GME     ").ch(19, '1');
    const RegSho m = decode<RegSho>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.stock.view(), "GME");
    EXPECT_EQ(m.action, '1');
}

TEST(Decode, MarketParticipantPosition) {
    const Wire w =
        wire('L', 26).text(11, "GSCO").text(15, "NVDA    ").ch(23, 'Y').ch(24, 'N').ch(25, 'A');
    const MarketParticipantPosition m = decode<MarketParticipantPosition>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.mpid, (std::array<char, 4>{'G', 'S', 'C', 'O'}));
    EXPECT_EQ(m.stock.view(), "NVDA");
    EXPECT_EQ(m.primary_market_maker, 'Y');
    EXPECT_EQ(m.market_maker_mode, 'N');
    EXPECT_EQ(m.participant_state, 'A');
}

TEST(Decode, MwcbDeclineLevel) {
    const Wire w = wire('V', 35)
                       .u64(11, 0x1111111111111111ULL)
                       .u64(19, 0x2222222222222222ULL)
                       .u64(27, 0x3333333333333333ULL);
    const MwcbDeclineLevel m = decode<MwcbDeclineLevel>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.level1, 0x1111111111111111ULL);
    EXPECT_EQ(m.level2, 0x2222222222222222ULL);
    EXPECT_EQ(m.level3, 0x3333333333333333ULL);
}

TEST(Decode, MwcbStatus) {
    const Wire w = wire('W', 12).ch(11, '2');
    const MwcbStatus m = decode<MwcbStatus>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.breached_level, '2');
}

TEST(Decode, IpoQuotingPeriod) {
    const Wire w = wire('K', 28).text(11, "NEWCO   ").u32(19, 34'200).ch(23, 'A').u32(24, 250'000);
    const IpoQuotingPeriod m = decode<IpoQuotingPeriod>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.stock.view(), "NEWCO");
    EXPECT_EQ(m.release_time, 34'200U);
    EXPECT_EQ(m.release_qualifier, 'A');
    EXPECT_EQ(m.ipo_price, 250'000U);
}

TEST(Decode, LuldAuctionCollar) {
    const Wire w = wire('J', 35)
                       .text(11, "AMD     ")
                       .u32(19, 1'000'000)
                       .u32(23, 1'050'000)
                       .u32(27, 950'000)
                       .u32(31, 3);
    const LuldAuctionCollar m = decode<LuldAuctionCollar>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.stock.view(), "AMD");
    EXPECT_EQ(m.reference_price, 1'000'000U);
    EXPECT_EQ(m.upper_price, 1'050'000U);
    EXPECT_EQ(m.lower_price, 950'000U);
    EXPECT_EQ(m.extension, 3U);
}

TEST(Decode, OperationalHalt) {
    const Wire w = wire('h', 21).text(11, "IBM     ").ch(19, 'Q').ch(20, 'H');
    const OperationalHalt m = decode<OperationalHalt>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.stock.view(), "IBM");
    EXPECT_EQ(m.market_code, 'Q');
    EXPECT_EQ(m.action, 'H');
}

TEST(Decode, AddOrder) {
    const Wire w = wire('A', 36)
                       .u64(11, 0x0102030405060708ULL)
                       .ch(19, 'S')
                       .u32(20, 500)
                       .text(24, "GOOG    ")
                       .u32(32, 1'405'000);
    const AddOrder m = decode<AddOrder>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.order_ref, 0x0102030405060708ULL);
    EXPECT_EQ(m.side, Side::Sell);
    EXPECT_EQ(m.shares, 500U);
    EXPECT_EQ(m.stock.view(), "GOOG");
    EXPECT_EQ(m.price, 1'405'000U);
    EXPECT_FALSE(m.attributed);
    EXPECT_EQ(m.mpid, (std::array<char, 4>{' ', ' ', ' ', ' '}));
}

TEST(Decode, AddOrderWithAttribution) {
    const Wire w = wire('F', 40)
                       .u64(11, 77)
                       .ch(19, 'B')
                       .u32(20, 100)
                       .text(24, "META    ")
                       .u32(32, 3'000'000)
                       .text(36, "VIRT");
    const AddOrder m = decode<AddOrder>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.order_ref, 77U);
    EXPECT_EQ(m.side, Side::Buy);
    EXPECT_EQ(m.shares, 100U);
    EXPECT_EQ(m.stock.view(), "META");
    EXPECT_EQ(m.price, 3'000'000U);
    EXPECT_TRUE(m.attributed);
    EXPECT_EQ(m.mpid, (std::array<char, 4>{'V', 'I', 'R', 'T'}));
    EXPECT_EQ(m.type(), 'F');
}

TEST(Decode, OrderExecuted) {
    const Wire w = wire('E', 31).u64(11, 0xaaaaULL).u32(19, 250).u64(23, 0xbbbbbbbbULL);
    const OrderExecuted m = decode<OrderExecuted>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.order_ref, 0xaaaaULL);
    EXPECT_EQ(m.shares, 250U);
    EXPECT_EQ(m.match_number, 0xbbbbbbbbULL);
}

TEST(Decode, OrderExecutedWithPrice) {
    const Wire w =
        wire('C', 36).u64(11, 9001).u32(19, 75).u64(23, 123'456'789).ch(31, 'N').u32(32, 998'800);
    const OrderExecutedWithPrice m = decode<OrderExecutedWithPrice>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.order_ref, 9001U);
    EXPECT_EQ(m.shares, 75U);
    EXPECT_EQ(m.match_number, 123'456'789U);
    EXPECT_EQ(m.printable, 'N');
    EXPECT_FALSE(m.is_printable());
    EXPECT_EQ(m.price, 998'800U);
}

TEST(Decode, OrderCancel) {
    const Wire w = wire('X', 23).u64(11, 31337).u32(19, 40);
    const OrderCancel m = decode<OrderCancel>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.order_ref, 31337U);
    EXPECT_EQ(m.shares, 40U);
}

TEST(Decode, OrderDelete) {
    const Wire w = wire('D', 19).u64(11, 0xfeedfacecafebeefULL);
    const OrderDelete m = decode<OrderDelete>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.order_ref, 0xfeedfacecafebeefULL);
}

TEST(Decode, OrderReplace) {
    const Wire w = wire('U', 35).u64(11, 1111).u64(19, 2222).u32(27, 333).u32(31, 444'400);
    const OrderReplace m = decode<OrderReplace>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.orig_order_ref, 1111U);
    EXPECT_EQ(m.new_order_ref, 2222U);
    EXPECT_EQ(m.shares, 333U);
    EXPECT_EQ(m.price, 444'400U);
}

TEST(Decode, Trade) {
    const Wire w = wire('P', 44)
                       .u64(11, 0)
                       .ch(19, 'B')
                       .u32(20, 1'000)
                       .text(24, "ORCL    ")
                       .u32(32, 1'230'000)
                       .u64(36, 0x99ULL);
    const Trade m = decode<Trade>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.order_ref, 0U);
    EXPECT_EQ(m.side, Side::Buy);
    EXPECT_EQ(m.shares, 1'000U);
    EXPECT_EQ(m.stock.view(), "ORCL");
    EXPECT_EQ(m.price, 1'230'000U);
    EXPECT_EQ(m.match_number, 0x99ULL);
}

TEST(Decode, CrossTrade) {
    const Wire w = wire('Q', 40)
                       .u64(11, 5'000'000'000ULL)  // more than fits in 32 bits
                       .text(19, "SPY     ")
                       .u32(27, 4'500'000)
                       .u64(31, 424'242)
                       .ch(39, 'C');
    const CrossTrade m = decode<CrossTrade>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.shares, 5'000'000'000ULL);
    EXPECT_EQ(m.stock.view(), "SPY");
    EXPECT_EQ(m.cross_price, 4'500'000U);
    EXPECT_EQ(m.match_number, 424'242U);
    EXPECT_EQ(m.cross_type, 'C');
}

TEST(Decode, BrokenTrade) {
    const Wire w = wire('B', 19).u64(11, 0x0123456789abcdefULL);
    const BrokenTrade m = decode<BrokenTrade>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.match_number, 0x0123456789abcdefULL);
}

TEST(Decode, Noii) {
    const Wire w = wire('I', 50)
                       .u64(11, 111'111)
                       .u64(19, 22'222)
                       .ch(27, 'S')
                       .text(28, "QQQ     ")
                       .u32(36, 3'000'001)
                       .u32(40, 3'000'002)
                       .u32(44, 3'000'003)
                       .ch(48, 'O')
                       .ch(49, 'L');
    const Noii m = decode<Noii>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.paired_shares, 111'111U);
    EXPECT_EQ(m.imbalance_shares, 22'222U);
    EXPECT_EQ(m.imbalance_direction, 'S');
    EXPECT_EQ(m.stock.view(), "QQQ");
    EXPECT_EQ(m.far_price, 3'000'001U);
    EXPECT_EQ(m.near_price, 3'000'002U);
    EXPECT_EQ(m.reference_price, 3'000'003U);
    EXPECT_EQ(m.cross_type, 'O');
    EXPECT_EQ(m.price_variation, 'L');
}

TEST(Decode, Rpii) {
    const Wire w = wire('N', 20).text(11, "F       ").ch(19, 'A');
    const Rpii m = decode<Rpii>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.stock.view(), "F");
    EXPECT_EQ(m.interest_flag, 'A');
}

TEST(Decode, DirectListingCapitalRaise) {
    const Wire w = wire('O', 48)
                       .text(11, "DLCR    ")
                       .ch(19, 'Y')
                       .u32(20, 100'001)
                       .u32(24, 100'002)
                       .u32(28, 100'003)
                       .u64(32, 0x0000111122223333ULL)
                       .u32(40, 100'004)
                       .u32(44, 100'005);
    const DirectListingCapitalRaise m = decode<DirectListingCapitalRaise>(w.data());
    expect_header(m.hdr);
    EXPECT_EQ(m.stock.view(), "DLCR");
    EXPECT_EQ(m.open_eligibility, 'Y');
    EXPECT_EQ(m.min_allowable_price, 100'001U);
    EXPECT_EQ(m.max_allowable_price, 100'002U);
    EXPECT_EQ(m.near_execution_price, 100'003U);
    EXPECT_EQ(m.near_execution_time, 0x0000111122223333ULL);
    EXPECT_EQ(m.lower_collar, 100'004U);
    EXPECT_EQ(m.upper_collar, 100'005U);
}

// --- Encoding -----------------------------------------------------------------------

TEST(Encode, AddOrderMatchesHandBuiltBytes) {
    AddOrder m;
    m.hdr = {.locate = kLocate, .tracking = kTracking, .timestamp = kTimestamp};
    m.order_ref = 0x0102030405060708ULL;
    m.side = Side::Sell;
    m.shares = 500;
    m.stock = Symbol::from("GOOG");
    m.price = 1'405'000;

    const Wire expected = wire('A', 36)
                              .u64(11, 0x0102030405060708ULL)
                              .ch(19, 'S')
                              .u32(20, 500)
                              .text(24, "GOOG    ")
                              .u32(32, 1'405'000);
    std::vector<std::byte> out(wire_size(m));
    EXPECT_EQ(encode(m, out.data()), 36U);
    EXPECT_EQ(out, expected.bytes());
}

TEST(Encode, AttributedAddIsFortyBytesAndTypeF) {
    AddOrder m;
    m.attributed = true;
    m.mpid = {'V', 'I', 'R', 'T'};
    EXPECT_EQ(wire_size(m), 40U);
    std::vector<std::byte> out(40);
    EXPECT_EQ(encode(m, out.data()), 40U);
    EXPECT_EQ(static_cast<char>(out[0]), 'F');
    EXPECT_EQ(static_cast<char>(out[36]), 'V');
    EXPECT_EQ(static_cast<char>(out[39]), 'T');
}

TEST(Encode, AppendFramedWritesBigEndianLengthPrefix) {
    std::vector<std::byte> stream;
    append_framed(stream, SystemEvent{.hdr = {}, .event_code = 'O'});
    append_framed(stream, Noii{});
    ASSERT_EQ(stream.size(), 2U + 12U + 2U + 50U);
    EXPECT_EQ(stream[0], std::byte{0});
    EXPECT_EQ(stream[1], std::byte{12});
    EXPECT_EQ(static_cast<char>(stream[2]), 'S');
    EXPECT_EQ(stream[14], std::byte{0});
    EXPECT_EQ(stream[15], std::byte{50});
    EXPECT_EQ(static_cast<char>(stream[16]), 'I');
}

// decode followed by encode must reproduce the input exactly, for every type
// and for arbitrary field bytes. This is what lets the engine's published feed
// (phase 5) be compared byte for byte.
TEST(Codec, DecodeThenEncodeIsIdentityOnRandomBytes) {
    gen::SplitMix64 rng(20261003);
    test::for_each_wire_type([&]<class M>(std::type_identity<M>, char type, std::size_t size) {
        EXPECT_EQ(message_size(type), size) << type;
        for (int round = 0; round < 500; ++round) {
            std::vector<std::byte> in(size);
            for (std::byte& b : in) {
                b = static_cast<std::byte>(rng.below(256));
            }
            in.at(0) = static_cast<std::byte>(type);

            const M m = decode<M>(in.data());
            ASSERT_EQ(m.type(), type);
            ASSERT_EQ(wire_size(m), size) << type;

            std::vector<std::byte> out(size);
            ASSERT_EQ(encode(m, out.data()), size) << type;
            ASSERT_EQ(in, out) << "type " << type << " round " << round;
            ASSERT_EQ(decode<M>(out.data()), m) << type;
        }
    });
}

TEST(Symbol, PadsAndTrims) {
    EXPECT_EQ(Symbol::from("AAPL").view(), "AAPL");
    EXPECT_EQ(Symbol::from("").view(), "");
    EXPECT_EQ(Symbol::from("ABCDEFGH").view(), "ABCDEFGH");
    EXPECT_EQ(Symbol::from("ABCDEFGHIJ").view(), "ABCDEFGH");  // extra characters are dropped
    EXPECT_EQ(Symbol::from("BRK.B").raw,
              (std::array<char, 8>{'B', 'R', 'K', '.', 'B', ' ', ' ', ' '}));
    EXPECT_EQ(Symbol::from("A B").view(), "A B");  // only trailing padding is trimmed
    EXPECT_EQ(Symbol::from("X"), Symbol::from("X       "));
}

}  // namespace
