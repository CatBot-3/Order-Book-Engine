#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "obe/engine/types.hpp"
#include "obe/feed/codec.hpp"
#include "obe/net/protocol.hpp"
#include "support/wire.hpp"

// The order-entry wire format. Sizes are pinned in the header by static
// assertions; these tests pin the byte offsets, with messages built by hand
// (support/wire.hpp), and the mapping to and from the engine's own types.

namespace {

using namespace obe;

template <class M>
std::vector<std::byte> bytes_of(const M& m) {
    std::vector<std::byte> out(feed::wire_size(m));
    feed::encode(m, out.data());
    return out;
}

TEST(Protocol, EnterOrderHasItsFieldsWhereTheSpecificationPutsThem) {
    test::Wire wire('O', 22);
    wire.u64(1, 0x1122334455667788ULL)  // token
        .u16(9, 7)                      // locate
        .ch(11, 'S')                    // side
        .u32(12, 300)                   // quantity
        .u32(16, 1'000'100)             // price
        .ch(20, 'L')                    // kind
        .ch(21, 'I');                   // time in force
    const net::EnterOrder expected{.token = 0x1122334455667788ULL,
                                   .locate = 7,
                                   .side = Side::Sell,
                                   .qty = 300,
                                   .price = 1'000'100,
                                   .kind = 'L',
                                   .tif = 'I'};
    EXPECT_EQ(feed::decode<net::EnterOrder>(wire.data()), expected);
    EXPECT_EQ(bytes_of(expected), wire.bytes());
}

TEST(Protocol, ReplaceAndCancelLayouts) {
    test::Wire replace('U', 17);
    replace.u64(1, 42).u32(9, 500).u32(13, 999'900);
    const net::ReplaceOrder r{.order_id = 42, .qty = 500, .price = 999'900};
    EXPECT_EQ(feed::decode<net::ReplaceOrder>(replace.data()), r);
    EXPECT_EQ(bytes_of(r), replace.bytes());

    test::Wire cancel('X', 9);
    cancel.u64(1, 0xfffffffffffffffeULL);
    const net::CancelOrder c{.order_id = 0xfffffffffffffffeULL};
    EXPECT_EQ(feed::decode<net::CancelOrder>(cancel.data()), c);
    EXPECT_EQ(bytes_of(c), cancel.bytes());
}

TEST(Protocol, AcceptedLayout) {
    test::Wire wire('A', 38);
    wire.u64(1, 123'456'789)  // timestamp
        .u64(9, 55)           // token
        .u64(17, 1001)        // order id
        .u16(25, 3)           // locate
        .ch(27, 'B')          // side
        .u32(28, 200)         // quantity
        .u32(32, 1'234'500)   // price
        .ch(36, 'M')          // kind
        .ch(37, 'F');         // time in force
    const net::OrderAccepted expected{.timestamp = 123'456'789,
                                      .token = 55,
                                      .order_id = 1001,
                                      .locate = 3,
                                      .side = Side::Buy,
                                      .qty = 200,
                                      .price = 1'234'500,
                                      .kind = 'M',
                                      .tif = 'F'};
    EXPECT_EQ(feed::decode<net::OrderAccepted>(wire.data()), expected);
    EXPECT_EQ(bytes_of(expected), wire.bytes());
}

TEST(Protocol, ExecutedLayout) {
    test::Wire wire('E', 46);
    wire.u64(1, 9)      // timestamp
        .u64(9, 55)     // token
        .u64(17, 1001)  // order id
        .u32(25, 100)   // quantity
        .u32(29, 999)   // price
        .u32(33, 50)    // leaves
        .u64(37, 77)    // match number
        .ch(45, 'R');   // liquidity
    const net::OrderExecuted expected{.timestamp = 9,
                                      .token = 55,
                                      .order_id = 1001,
                                      .qty = 100,
                                      .price = 999,
                                      .leaves = 50,
                                      .match_number = 77,
                                      .liquidity = 'R'};
    EXPECT_EQ(feed::decode<net::OrderExecuted>(wire.data()), expected);
    EXPECT_EQ(bytes_of(expected), wire.bytes());
}

TEST(Protocol, CancelledReplacedAndRejectedLayouts) {
    test::Wire cancelled('C', 30);
    cancelled.u64(1, 9).u64(9, 55).u64(17, 1001).u32(25, 300).ch(29, 'I');
    const net::OrderCancelled c{
        .timestamp = 9, .token = 55, .order_id = 1001, .qty = 300, .reason = 'I'};
    EXPECT_EQ(feed::decode<net::OrderCancelled>(cancelled.data()), c);
    EXPECT_EQ(bytes_of(c), cancelled.bytes());

    test::Wire replaced('R', 42);
    replaced.u64(1, 9).u64(9, 55).u64(17, 1001).u64(25, 1002).u32(33, 400).u32(37, 888).ch(41, 'Y');
    const net::OrderReplaced r{.timestamp = 9,
                               .token = 55,
                               .old_order_id = 1001,
                               .new_order_id = 1002,
                               .qty = 400,
                               .price = 888,
                               .kept_priority = 'Y'};
    EXPECT_EQ(feed::decode<net::OrderReplaced>(replaced.data()), r);
    EXPECT_EQ(bytes_of(r), replaced.bytes());

    test::Wire rejected('J', 26);
    rejected.u64(1, 9).u64(9, 55).u64(17, 1001).ch(25, 'N');
    const net::OrderRejected j{.timestamp = 9, .token = 55, .order_id = 1001, .reason = 'N'};
    EXPECT_EQ(feed::decode<net::OrderRejected>(rejected.data()), j);
    EXPECT_EQ(bytes_of(j), rejected.bytes());
}

TEST(Protocol, TheTypeByteDecidesTheSize) {
    EXPECT_EQ(net::request_size('O'), 22U);
    EXPECT_EQ(net::request_size('U'), 17U);
    EXPECT_EQ(net::request_size('X'), 9U);
    EXPECT_EQ(net::response_size('A'), 38U);
    EXPECT_EQ(net::response_size('E'), 46U);
    EXPECT_EQ(net::response_size('C'), 30U);
    EXPECT_EQ(net::response_size('R'), 42U);
    EXPECT_EQ(net::response_size('J'), 26U);

    // Every other byte is not a message, in either direction. A response type
    // is not a request type and the other way round.
    int requests = 0;
    int responses = 0;
    for (int b = 0; b < 256; ++b) {
        const char type = static_cast<char>(b);
        requests += net::request_size(type) != 0 ? 1 : 0;
        responses += net::response_size(type) != 0 ? 1 : 0;
        EXPECT_LE(net::request_size(type), net::kMaxRequestSize);
        EXPECT_LE(net::response_size(type), net::kMaxResponseSize);
    }
    EXPECT_EQ(requests, 3);
    EXPECT_EQ(responses, 5);
    EXPECT_EQ(net::request_size('A'), 0U);
    EXPECT_EQ(net::response_size('O'), 0U);
}

TEST(Protocol, AnEnterOrderBecomesTheEnginesRequest) {
    const net::EnterOrder m{.token = 9,
                            .locate = 4,
                            .side = Side::Sell,
                            .qty = 250,
                            .price = 1'000'000,
                            .kind = 'L',
                            .tif = 'D'};
    engine::NewOrder out;
    ASSERT_TRUE(net::to_engine(m, 17, out));
    EXPECT_EQ(out, (engine::NewOrder{.owner = 17,
                                     .token = 9,
                                     .locate = 4,
                                     .side = Side::Sell,
                                     .qty = 250,
                                     .price = 1'000'000,
                                     .kind = engine::OrderKind::Limit,
                                     .tif = engine::TimeInForce::Day}));

    net::EnterOrder market = m;
    market.kind = 'M';
    market.tif = 'F';
    ASSERT_TRUE(net::to_engine(market, 17, out));
    EXPECT_EQ(out.kind, engine::OrderKind::Market);
    EXPECT_EQ(out.tif, engine::TimeInForce::FillOrKill);

    net::EnterOrder ioc = m;
    ioc.tif = 'I';
    ASSERT_TRUE(net::to_engine(ioc, 17, out));
    EXPECT_EQ(out.tif, engine::TimeInForce::ImmediateOrCancel);
}

TEST(Protocol, AKindOrTimeInForceThatDoesNotExistIsRefused) {
    const engine::NewOrder untouched{.owner = 99, .token = 99};
    for (const char bad : {'\0', 'l', 'X', ' '}) {
        net::EnterOrder m{.token = 1, .locate = 1, .qty = 1, .price = 1};
        m.kind = bad;
        engine::NewOrder out = untouched;
        EXPECT_FALSE(net::to_engine(m, 1, out)) << "kind " << static_cast<int>(bad);
        EXPECT_EQ(out, untouched);

        m.kind = 'L';
        m.tif = bad;
        EXPECT_FALSE(net::to_engine(m, 1, out)) << "tif " << static_cast<int>(bad);
        EXPECT_EQ(out, untouched);
    }
}

TEST(Protocol, EveryFieldOfEveryReportReachesTheWire) {
    const engine::Accepted accepted{.order_id = 1,
                                    .owner = 2,
                                    .token = 3,
                                    .locate = 4,
                                    .side = Side::Sell,
                                    .qty = 5,
                                    .price = 6,
                                    .kind = engine::OrderKind::Market,
                                    .tif = engine::TimeInForce::ImmediateOrCancel,
                                    .timestamp = 7};
    EXPECT_EQ(net::to_wire(accepted), (net::OrderAccepted{.timestamp = 7,
                                                          .token = 3,
                                                          .order_id = 1,
                                                          .locate = 4,
                                                          .side = Side::Sell,
                                                          .qty = 5,
                                                          .price = 6,
                                                          .kind = 'M',
                                                          .tif = 'I'}));

    const engine::Executed executed{.order_id = 1,
                                    .owner = 2,
                                    .token = 3,
                                    .qty = 4,
                                    .price = 5,
                                    .leaves = 6,
                                    .match_number = 7,
                                    .liquidity = engine::Liquidity::Removed,
                                    .timestamp = 8};
    EXPECT_EQ(net::to_wire(executed), (net::OrderExecuted{.timestamp = 8,
                                                          .token = 3,
                                                          .order_id = 1,
                                                          .qty = 4,
                                                          .price = 5,
                                                          .leaves = 6,
                                                          .match_number = 7,
                                                          .liquidity = 'R'}));

    const engine::Cancelled cancelled{.order_id = 1,
                                      .owner = 2,
                                      .token = 3,
                                      .qty = 4,
                                      .leaves = 0,
                                      .reason = engine::CancelReason::FillOrKill,
                                      .timestamp = 5};
    EXPECT_EQ(
        net::to_wire(cancelled),
        (net::OrderCancelled{.timestamp = 5, .token = 3, .order_id = 1, .qty = 4, .reason = 'F'}));

    const engine::Replaced replaced{.old_id = 1,
                                    .new_id = 2,
                                    .owner = 3,
                                    .token = 4,
                                    .qty = 5,
                                    .price = 6,
                                    .kept_priority = true,
                                    .timestamp = 7};
    EXPECT_EQ(net::to_wire(replaced), (net::OrderReplaced{.timestamp = 7,
                                                          .token = 4,
                                                          .old_order_id = 1,
                                                          .new_order_id = 2,
                                                          .qty = 5,
                                                          .price = 6,
                                                          .kept_priority = 'Y'}));

    const engine::Rejected rejected{.owner = 1,
                                    .token = 2,
                                    .order_id = 3,
                                    .reason = engine::RejectReason::NotOwner,
                                    .timestamp = 4};
    EXPECT_EQ(net::to_wire(rejected),
              (net::OrderRejected{.timestamp = 4, .token = 2, .order_id = 3, .reason = 'N'}));
}

TEST(Protocol, EveryReasonHasItsOwnByte) {
    std::set<char> rejects{net::kRejectBadField};
    for (const engine::RejectReason r :
         {engine::RejectReason::ZeroQuantity, engine::RejectReason::ZeroPrice,
          engine::RejectReason::BadSide, engine::RejectReason::UnknownInstrument,
          engine::RejectReason::UnknownOrder, engine::RejectReason::NotOwner}) {
        EXPECT_TRUE(rejects.insert(net::to_wire(r)).second);
    }
    EXPECT_EQ(rejects.size(), 7U);

    std::set<char> cancels;
    for (const engine::CancelReason r :
         {engine::CancelReason::Requested, engine::CancelReason::ImmediateOrCancel,
          engine::CancelReason::FillOrKill, engine::CancelReason::NoLiquidity}) {
        EXPECT_TRUE(cancels.insert(net::to_wire(r)).second);
    }
    EXPECT_EQ(net::to_wire(engine::CancelReason::Requested), 'U');

    EXPECT_EQ(net::to_wire(engine::TimeInForce::Day), 'D');
    EXPECT_EQ(net::to_wire(engine::TimeInForce::ImmediateOrCancel), 'I');
    EXPECT_EQ(net::to_wire(engine::TimeInForce::FillOrKill), 'F');
    EXPECT_EQ(net::to_wire(engine::OrderKind::Limit), 'L');
    EXPECT_EQ(net::to_wire(engine::OrderKind::Market), 'M');
}

}  // namespace
