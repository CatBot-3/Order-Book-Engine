#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/engine/engines.hpp"
#include "obe/engine/round_trip.hpp"
#include "obe/feed/codec.hpp"
#include "obe/feed/messages.hpp"
#include "obe/net/moldudp.hpp"
#include "obe/net/order_gateway.hpp"
#include "obe/net/protocol.hpp"
#include "obe/net/socket.hpp"
#include "support/engine_harness.hpp"

// The order gateway, over real TCP sockets on loopback.
//
// There are no threads here. The test is the client and also turns the
// gateway's event loop by hand: write to a client socket, call poll() until
// the gateway has reacted, read the client socket. That makes every test
// deterministic, and it is possible because the gateway is single-threaded by
// design: nothing in it happens except inside poll().
//
// These run on whichever engine the build tests (the reference engine in the
// passing suite), because the gateway's behaviour does not depend on it.

namespace {

using namespace obe;
using namespace std::chrono_literals;

using Packet = std::vector<std::byte>;

struct Keep {
    std::vector<Packet>* packets;
    void operator()(std::span<const std::byte> packet) const {
        packets->emplace_back(packet.begin(), packet.end());
    }
};

using Publisher = net::MoldPacketizer<Keep>;
using Gateway = net::OrderGateway<engine::ReferenceEngineImpl, Publisher>;

constexpr Price kP = 1'000'000;
constexpr Price kTick = 100;
const net::MoldSession kSession = net::make_session("GWTEST");

// A client connection the test drives by hand.
class Client {
 public:
    // `receive_buffer` asks the kernel for a small receive buffer, for the
    // slow-client test. It has to be set before connecting to take effect.
    explicit Client(std::uint16_t port, int receive_buffer = 0)
        : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        if (receive_buffer != 0) {
            ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
        }
        const sockaddr_in addr = net::make_address("127.0.0.1", port);
        if (::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            ADD_FAILURE() << net::errno_text("connect");
        }
        net::set_nodelay(fd_.get());
        net::set_nonblocking(fd_.get());
    }

    template <class M>
    void send(const M& message) {
        Packet bytes(feed::wire_size(message));
        feed::encode(message, bytes.data());
        send_raw(bytes);
    }

    // Writes the bytes. Returns how many the socket took (all of them unless
    // its buffer is full).
    std::size_t send_raw(std::span<const std::byte> bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t n =
                ::send(fd_.get(), bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
            if (n <= 0) {
                break;
            }
            done += static_cast<std::size_t>(n);
        }
        return done;
    }

    // Takes in whatever has arrived. Returns false once the gateway has closed
    // the connection.
    bool fill() {
        // Drop what take() has already handed out.
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(taken_));
        taken_ = 0;
        std::byte buf[4096];
        for (;;) {
            const ssize_t n = ::recv(fd_.get(), buf, sizeof(buf), 0);
            if (n > 0) {
                in_.insert(in_.end(), buf, buf + n);
                continue;
            }
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                closed_ = true;
            }
            return !closed_;
        }
    }

    [[nodiscard]] bool closed() const { return closed_; }

    // The type byte of the next complete response, or 0 if there is none.
    [[nodiscard]] char next_type() const {
        if (taken_ == in_.size()) {
            return 0;
        }
        const char type = static_cast<char>(in_[taken_]);
        return in_.size() - taken_ >= net::response_size(type) ? type : char{0};
    }

    // Decodes and removes the next response. Precondition: next_type() == M::kType.
    template <class M>
    M take() {
        const M m = feed::decode<M>(in_.data() + taken_);
        taken_ += feed::wire_size(m);
        return m;
    }

    [[nodiscard]] std::size_t buffered() const { return in_.size() - taken_; }

    void close() { fd_.reset(); }

 private:
    net::Fd fd_;
    Packet in_;
    std::size_t taken_ = 0;  // how much of in_ take() has consumed
    bool closed_ = false;
};

class GatewayTest : public ::testing::Test {
 protected:
    void start(const net::GatewayConfig& cfg = {}) {
        gateway = std::make_unique<Gateway>(publisher, cfg, [this] { return ++clock; });
        ASSERT_TRUE(gateway->engine().add_instrument(1, feed::Symbol::from("ACME"), ++clock));
        ASSERT_TRUE(gateway->engine().add_instrument(2, feed::Symbol::from("OTHR"), ++clock));
        gateway->listen("127.0.0.1", 0);
        ASSERT_NE(gateway->port(), 0);
    }

    Client connect(int receive_buffer = 0) { return Client(gateway->port(), receive_buffer); }

    // Turns the gateway until `done` says stop. False if it took too long.
    template <class Done>
    bool pump_until(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            gateway->poll(1);
        }
        return true;
    }

    // A few turns, for when the point is that nothing should happen.
    void pump(int turns = 5) {
        for (int i = 0; i < turns; ++i) {
            gateway->poll(1);
        }
    }

    // Waits for the next response on `client`, which must be an M.
    template <class M>
    ::testing::AssertionResult read(Client& client, M& out) {
        const bool arrived = pump_until([&] {
            client.fill();
            return client.next_type() != 0 || client.closed();
        });
        if (!arrived || client.next_type() == 0) {
            return ::testing::AssertionFailure() << "no response arrived (connection "
                                                 << (client.closed() ? "closed" : "open") << ")";
        }
        if (client.next_type() != M::kType) {
            return ::testing::AssertionFailure()
                   << "expected a '" << M::kType << "', got a '" << client.next_type() << "'";
        }
        out = client.template take<M>();
        return ::testing::AssertionSuccess();
    }

    // Sends a limit order and returns the id it was accepted under.
    OrderId rest(Client& client, Side side, Price price, Qty qty, Locate locate = 1) {
        client.send(net::EnterOrder{
            .token = ++token, .locate = locate, .side = side, .qty = qty, .price = price});
        net::OrderAccepted accepted;
        EXPECT_TRUE(read(client, accepted));
        return accepted.order_id;
    }

    // True once the gateway has dropped the connection and the client sees it.
    bool closed_by_gateway(Client& client) {
        return pump_until([&] {
            client.fill();
            return client.closed();
        });
    }

    std::vector<Packet> packets;
    Publisher publisher{kSession, Keep{&packets}};
    Nanos clock = 0;
    engine::Token token = 0;
    std::unique_ptr<Gateway> gateway;
};

// --- Ordinary business -------------------------------------------------------

TEST_F(GatewayTest, AcceptsConnectionsAndGivesEachItsOwnOwner) {
    start();
    Client ann = connect();
    Client bob = connect();
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 2; }));
    EXPECT_EQ(gateway->stats().connections_accepted, 2U);

    const OrderId a = rest(ann, Side::Buy, kP, 100);
    const OrderId b = rest(bob, Side::Buy, kP - kTick, 100);
    const auto orders = test::orders_of(gateway->engine(), 1, Side::Buy);
    ASSERT_EQ(orders.size(), 2U);
    EXPECT_EQ(orders[0].id, a);
    EXPECT_EQ(orders[1].id, b);
    EXPECT_NE(orders[0].owner, orders[1].owner);
}

TEST_F(GatewayTest, AnOrderIsAcceptedWithEveryFieldEchoed) {
    start();
    Client ann = connect();
    ann.send(net::EnterOrder{
        .token = 77, .locate = 2, .side = Side::Sell, .qty = 300, .price = kP, .tif = 'D'});
    net::OrderAccepted accepted;
    ASSERT_TRUE(read(ann, accepted));
    EXPECT_EQ(accepted.token, 77U);
    EXPECT_EQ(accepted.order_id, 1U);
    EXPECT_EQ(accepted.locate, 2U);
    EXPECT_EQ(accepted.side, Side::Sell);
    EXPECT_EQ(accepted.qty, 300U);
    EXPECT_EQ(accepted.price, kP);
    EXPECT_EQ(accepted.kind, 'L');
    EXPECT_EQ(accepted.tif, 'D');
    EXPECT_EQ(accepted.timestamp, clock) << "stamped by the gateway's clock";
    EXPECT_EQ(gateway->engine().best(2, Side::Sell), (book::Level{kP, 300}));
    EXPECT_EQ(gateway->stats().requests, 1U);
    EXPECT_EQ(gateway->stats().responses, 1U);
    EXPECT_EQ(gateway->stats().bytes_in, 22U);
    EXPECT_EQ(gateway->stats().bytes_out, 38U);
}

TEST_F(GatewayTest, TwoClientsTradeAndEachHearsOnlyItsOwnSide) {
    start();
    Client ann = connect();
    Client bob = connect();
    const OrderId resting = rest(bob, Side::Sell, kP, 300);

    ann.send(net::EnterOrder{
        .token = 500, .locate = 1, .side = Side::Buy, .qty = 100, .price = kP + kTick});
    net::OrderAccepted accepted;
    net::OrderExecuted ann_fill;
    net::OrderExecuted bob_fill;
    ASSERT_TRUE(read(ann, accepted));
    ASSERT_TRUE(read(ann, ann_fill));
    ASSERT_TRUE(read(bob, bob_fill));

    EXPECT_EQ(ann_fill.order_id, accepted.order_id);
    EXPECT_EQ(ann_fill.token, 500U);
    EXPECT_EQ(ann_fill.qty, 100U);
    EXPECT_EQ(ann_fill.price, kP) << "the resting order's price";
    EXPECT_EQ(ann_fill.leaves, 0U);
    EXPECT_EQ(ann_fill.liquidity, 'R');

    EXPECT_EQ(bob_fill.order_id, resting);
    EXPECT_EQ(bob_fill.qty, 100U);
    EXPECT_EQ(bob_fill.leaves, 200U);
    EXPECT_EQ(bob_fill.liquidity, 'A');
    EXPECT_EQ(bob_fill.match_number, ann_fill.match_number);

    // Nothing else is waiting for either of them.
    pump();
    ann.fill();
    bob.fill();
    EXPECT_EQ(ann.buffered(), 0U);
    EXPECT_EQ(bob.buffered(), 0U);
}

TEST_F(GatewayTest, CancelAndReplaceNameTheOrderByItsId) {
    start();
    Client ann = connect();
    const OrderId id = rest(ann, Side::Buy, kP, 300);

    ann.send(net::ReplaceOrder{.order_id = id, .qty = 100, .price = kP});
    net::OrderReplaced replaced;
    ASSERT_TRUE(read(ann, replaced));
    EXPECT_EQ(replaced.old_order_id, id);
    EXPECT_EQ(replaced.new_order_id, id);
    EXPECT_EQ(replaced.qty, 100U);
    EXPECT_EQ(replaced.kept_priority, 'Y');

    ann.send(net::ReplaceOrder{.order_id = id, .qty = 100, .price = kP + kTick});
    ASSERT_TRUE(read(ann, replaced));
    EXPECT_EQ(replaced.kept_priority, 'N');
    EXPECT_NE(replaced.new_order_id, id);

    ann.send(net::CancelOrder{.order_id = replaced.new_order_id});
    net::OrderCancelled cancelled;
    ASSERT_TRUE(read(ann, cancelled));
    EXPECT_EQ(cancelled.order_id, replaced.new_order_id);
    EXPECT_EQ(cancelled.qty, 100U);
    EXPECT_EQ(cancelled.reason, 'U');
    EXPECT_EQ(gateway->engine().open_orders(), 0U);
}

TEST_F(GatewayTest, AClientCannotTouchAnotherClientsOrder) {
    start();
    Client ann = connect();
    Client bob = connect();
    const OrderId id = rest(ann, Side::Buy, kP, 300);

    bob.send(net::CancelOrder{.order_id = id});
    net::OrderRejected rejected;
    ASSERT_TRUE(read(bob, rejected));
    EXPECT_EQ(rejected.order_id, id);
    EXPECT_EQ(rejected.reason, 'N');

    bob.send(net::ReplaceOrder{.order_id = id, .qty = 1, .price = kP});
    ASSERT_TRUE(read(bob, rejected));
    EXPECT_EQ(rejected.reason, 'N');
    EXPECT_EQ(gateway->engine().best(1, Side::Buy), (book::Level{kP, 300}));
}

TEST_F(GatewayTest, ANewConnectionNeverTakesOverADepartedOnesIdentity) {
    // Owner numbers are not reused. If the slot of a client that left were
    // handed to the next arrival by counting the connections that remain, the
    // newcomer could be given the number of somebody who is still here.
    start();
    Client ann = connect();
    Client bob = connect();
    rest(ann, Side::Buy, kP - 5 * kTick, 100);
    const OrderId bobs = rest(bob, Side::Buy, kP, 300);

    ann.close();
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 1; }));
    Client carl = connect();
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 2; }));

    // Carl is not Bob: Bob's order is not his to cancel.
    carl.send(net::CancelOrder{.order_id = bobs});
    net::OrderRejected rejected;
    ASSERT_TRUE(read(carl, rejected));
    EXPECT_EQ(rejected.reason, 'N');
    EXPECT_EQ(gateway->engine().best(1, Side::Buy), (book::Level{kP, 300}));

    // And Bob is still connected, still owns it, and still hears about it.
    carl.send(
        net::EnterOrder{.token = 77, .locate = 1, .side = Side::Sell, .qty = 100, .price = kP});
    net::OrderExecuted fill;
    ASSERT_TRUE(read(bob, fill));
    EXPECT_EQ(fill.order_id, bobs);
    EXPECT_EQ(fill.leaves, 200U);
    bob.send(net::CancelOrder{.order_id = bobs});
    net::OrderCancelled cancelled;
    ASSERT_TRUE(read(bob, cancelled));
    EXPECT_EQ(cancelled.order_id, bobs);
    EXPECT_FALSE(bob.closed());
}

TEST_F(GatewayTest, RequestsTheEngineRefusesComeBackAsRejections) {
    start();
    Client ann = connect();
    ann.send(net::EnterOrder{.token = 1, .locate = 1, .side = Side::Buy, .qty = 0, .price = kP});
    ann.send(net::EnterOrder{.token = 2, .locate = 9, .side = Side::Buy, .qty = 1, .price = kP});
    ann.send(net::CancelOrder{.order_id = 12345});
    net::OrderRejected rejected;
    ASSERT_TRUE(read(ann, rejected));
    EXPECT_EQ(rejected.token, 1U);
    EXPECT_EQ(rejected.reason, 'Q');
    ASSERT_TRUE(read(ann, rejected));
    EXPECT_EQ(rejected.token, 2U);
    EXPECT_EQ(rejected.reason, 'I');
    ASSERT_TRUE(read(ann, rejected));
    EXPECT_EQ(rejected.order_id, 12345U);
    EXPECT_EQ(rejected.reason, 'O');
}

TEST_F(GatewayTest, AnOrderKindThatDoesNotExistIsRefusedByTheGatewayItself) {
    start();
    Client ann = connect();
    ann.send(net::EnterOrder{
        .token = 9, .locate = 1, .side = Side::Buy, .qty = 100, .price = kP, .kind = '?'});
    ann.send(net::EnterOrder{
        .token = 10, .locate = 1, .side = Side::Buy, .qty = 100, .price = kP, .tif = 'Z'});
    net::OrderRejected rejected;
    ASSERT_TRUE(read(ann, rejected));
    EXPECT_EQ(rejected.token, 9U);
    EXPECT_EQ(rejected.reason, net::kRejectBadField);
    ASSERT_TRUE(read(ann, rejected));
    EXPECT_EQ(rejected.token, 10U);
    EXPECT_EQ(gateway->stats().bad_fields, 2U);
    EXPECT_EQ(gateway->engine().stats().accepted, 0U);
    EXPECT_EQ(gateway->engine().stats().rejected, 0U) << "the engine never saw them";
    // The connection is still good.
    EXPECT_NE(rest(ann, Side::Buy, kP, 100), 0U);
}

// --- Partial reads -----------------------------------------------------------

TEST_F(GatewayTest, ARequestThatArrivesOneByteAtATimeIsServedOnceItIsWhole) {
    start();
    Client ann = connect();
    Packet bytes(net::kEnterOrderSize);
    feed::encode(
        net::EnterOrder{.token = 5, .locate = 1, .side = Side::Buy, .qty = 100, .price = kP},
        bytes.data());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        ASSERT_EQ(ann.send_raw({bytes.data() + i, 1}), 1U);
        pump(2);
        ann.fill();
        if (i + 1 < bytes.size()) {
            ASSERT_EQ(ann.buffered(), 0U) << "answered after only " << i + 1 << " bytes";
            ASSERT_EQ(gateway->stats().requests, 0U);
        }
    }
    net::OrderAccepted accepted;
    ASSERT_TRUE(read(ann, accepted));
    EXPECT_EQ(accepted.token, 5U);
    EXPECT_EQ(gateway->stats().requests, 1U);
    EXPECT_EQ(gateway->stats().bytes_in, 22U);

    // The pieces that were being kept are gone once the request is served: the
    // next request is one more request, not the first one over again.
    ann.send(net::EnterOrder{
        .token = 6, .locate = 1, .side = Side::Buy, .qty = 100, .price = kP - kTick});
    ASSERT_TRUE(read(ann, accepted));
    EXPECT_EQ(accepted.token, 6U);
    pump();
    ann.fill();
    EXPECT_EQ(ann.buffered(), 0U);
    EXPECT_EQ(gateway->stats().requests, 2U);
    EXPECT_EQ(gateway->engine().open_orders(), 2U);
}

TEST_F(GatewayTest, SeveralRequestsAndAPieceOfTheNextInOneWrite) {
    start();
    Client ann = connect();
    Packet stream;
    const auto add = [&stream](const auto& message) {
        const std::size_t at = stream.size();
        stream.resize(at + feed::wire_size(message));
        feed::encode(message, stream.data() + at);
    };
    for (engine::Token t = 1; t <= 4; ++t) {
        add(net::EnterOrder{.token = t,
                            .locate = 1,
                            .side = Side::Buy,
                            .qty = 100,
                            .price = kP - static_cast<Price>(t) * kTick});
    }
    // Three whole requests and the first ten bytes of the fourth.
    const std::size_t cut = 3 * net::kEnterOrderSize + 10;
    ASSERT_EQ(ann.send_raw({stream.data(), cut}), cut);
    net::OrderAccepted accepted;
    for (engine::Token t = 1; t <= 3; ++t) {
        ASSERT_TRUE(read(ann, accepted));
        EXPECT_EQ(accepted.token, t);
    }
    pump();
    ann.fill();
    EXPECT_EQ(ann.buffered(), 0U) << "the fourth is not complete yet";
    EXPECT_EQ(gateway->stats().requests, 3U);

    ASSERT_EQ(ann.send_raw({stream.data() + cut, stream.size() - cut}), stream.size() - cut);
    ASSERT_TRUE(read(ann, accepted));
    EXPECT_EQ(accepted.token, 4U);
    EXPECT_EQ(gateway->engine().open_orders(), 4U);
    pump();
    ann.fill();
    EXPECT_EQ(ann.buffered(), 0U) << "four requests, four answers";
    EXPECT_EQ(gateway->stats().requests, 4U);
}

TEST_F(GatewayTest, MixedRequestTypesInOneWriteAreFramedByTheirTypeBytes) {
    start();
    Client ann = connect();
    const OrderId id = rest(ann, Side::Buy, kP, 300);
    Packet stream;
    const auto add = [&stream](const auto& message) {
        const std::size_t at = stream.size();
        stream.resize(at + feed::wire_size(message));
        feed::encode(message, stream.data() + at);
    };
    add(net::ReplaceOrder{.order_id = id, .qty = 200, .price = kP});  // 17 bytes
    add(net::CancelOrder{.order_id = id});                            // 9 bytes
    add(net::EnterOrder{.token = 8, .locate = 1, .side = Side::Sell, .qty = 50, .price = kP});
    ASSERT_EQ(ann.send_raw(stream), stream.size());

    net::OrderReplaced replaced;
    net::OrderCancelled cancelled;
    net::OrderAccepted accepted;
    ASSERT_TRUE(read(ann, replaced));
    ASSERT_TRUE(read(ann, cancelled));
    ASSERT_TRUE(read(ann, accepted));
    EXPECT_EQ(cancelled.qty, 200U);
    EXPECT_EQ(accepted.token, 8U);
}

// --- Things going wrong ------------------------------------------------------

TEST_F(GatewayTest, AClientThatDisconnectsMidMessageDoesNoHarm) {
    start();
    Client ann = connect();
    Client bob = connect();
    rest(bob, Side::Buy, kP, 100);

    Packet bytes(net::kEnterOrderSize);
    feed::encode(
        net::EnterOrder{.token = 5, .locate = 1, .side = Side::Sell, .qty = 100, .price = kP},
        bytes.data());
    ASSERT_EQ(ann.send_raw({bytes.data(), 13}), 13U);  // a little over half
    pump();
    ann.close();
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 1; }));

    EXPECT_EQ(gateway->stats().closed_by_peer, 1U);
    EXPECT_EQ(gateway->stats().requests, 1U) << "the half message was never acted on";
    EXPECT_EQ(gateway->engine().best(1, Side::Sell), std::nullopt);
    // The gateway is still serving everybody else.
    EXPECT_NE(rest(bob, Side::Buy, kP - kTick, 100), 0U);
}

TEST_F(GatewayTest, AByteThatIsNotARequestTypeEndsTheConnection) {
    start();
    Client ann = connect();
    Client bob = connect();
    rest(ann, Side::Buy, kP, 100);

    const Packet garbage{static_cast<std::byte>('Z'), std::byte{1}, std::byte{2}};
    ASSERT_EQ(ann.send_raw(garbage), garbage.size());
    EXPECT_TRUE(closed_by_gateway(ann));
    EXPECT_EQ(gateway->stats().closed_protocol, 1U);
    EXPECT_EQ(gateway->connections(), 1U);
    EXPECT_NE(rest(bob, Side::Sell, kP + kTick, 100), 0U);
}

TEST_F(GatewayTest, AResponseTypeSentAsARequestIsNotARequest) {
    start();
    Client ann = connect();
    ann.send(net::OrderAccepted{.token = 1});
    EXPECT_TRUE(closed_by_gateway(ann));
    EXPECT_EQ(gateway->stats().closed_protocol, 1U);
}

TEST_F(GatewayTest, OrdersAreCancelledWhenTheirOwnerDisconnects) {
    start();
    Client ann = connect();
    Client bob = connect();
    rest(ann, Side::Buy, kP, 100);
    rest(ann, Side::Buy, kP - kTick, 100);
    rest(ann, Side::Sell, kP + kTick, 100, 2);
    const OrderId bobs = rest(bob, Side::Sell, kP + 5 * kTick, 100);
    ASSERT_EQ(gateway->engine().open_orders(), 4U);

    ann.close();
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 1; }));
    EXPECT_EQ(gateway->engine().open_orders(), 1U);
    EXPECT_EQ(gateway->stats().orders_cancelled_on_disconnect, 3U);
    EXPECT_EQ(test::orders_of(gateway->engine(), 1, Side::Sell).at(0).id, bobs);
    EXPECT_EQ(gateway->engine().stats().cancels, 3U);
}

TEST_F(GatewayTest, OnlyOrdersStillOpenAreCancelledOnDisconnect) {
    start();
    Client ann = connect();
    Client bob = connect();
    const OrderId filled = rest(ann, Side::Sell, kP, 100);
    const OrderId cancelled = rest(ann, Side::Sell, kP + kTick, 100);
    const OrderId replaced = rest(ann, Side::Sell, kP + 2 * kTick, 100);
    rest(ann, Side::Sell, kP + 3 * kTick, 100);
    static_cast<void>(filled);

    // One trades away, one is cancelled, one is replaced into a new id.
    bob.send(
        net::EnterOrder{.token = 900, .locate = 1, .side = Side::Buy, .qty = 100, .price = kP});
    ann.send(net::CancelOrder{.order_id = cancelled});
    ann.send(net::ReplaceOrder{.order_id = replaced, .qty = 100, .price = kP + 4 * kTick});
    ASSERT_TRUE(pump_until([&] { return gateway->stats().requests == 7; }));
    ASSERT_EQ(gateway->engine().open_orders(), 2U);

    ann.close();
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 1; }));
    EXPECT_EQ(gateway->engine().open_orders(), 0U);
    EXPECT_EQ(gateway->stats().orders_cancelled_on_disconnect, 2U);
    // No cancel was attempted for an order that was already gone.
    EXPECT_EQ(gateway->engine().stats().rejected, 0U);
}

TEST_F(GatewayTest, OrdersCanBeLeftInTheBookIfConfiguredSo) {
    start({.cancel_on_disconnect = false});
    Client ann = connect();
    Client bob = connect();
    rest(ann, Side::Sell, kP, 100);
    ann.close();
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 1; }));
    EXPECT_EQ(gateway->engine().open_orders(), 1U);

    // Bob trades with the departed client's order. Bob is told; the report
    // for the other side has nowhere to go and is dropped.
    bob.send(net::EnterOrder{.token = 1, .locate = 1, .side = Side::Buy, .qty = 100, .price = kP});
    net::OrderAccepted accepted;
    net::OrderExecuted fill;
    ASSERT_TRUE(read(bob, accepted));
    ASSERT_TRUE(read(bob, fill));
    EXPECT_EQ(fill.qty, 100U);
    EXPECT_EQ(gateway->stats().responses_dropped, 1U);
}

TEST_F(GatewayTest, AClientThatStopsReadingIsDroppedAndTheOthersAreNot) {
    // A small limit, and a client with a small receive buffer that never
    // reads. Its own acknowledgements pile up: first in the kernel's buffers,
    // then in the gateway's, until the gateway gives up on it.
    start({.max_pending_output = std::size_t{16} * 1024});
    Client slow = connect(4096);
    Client good = connect();
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 2; }));

    // Cancels of orders that do not exist: each is 9 bytes in and a 26-byte
    // rejection out, and none of them touches the book.
    Packet chunk;
    for (int i = 0; i < 2'000; ++i) {
        const std::size_t at = chunk.size();
        chunk.resize(at + net::kCancelOrderSize);
        feed::encode(net::CancelOrder{.order_id = 1'000'000 + static_cast<OrderId>(i)},
                     chunk.data() + at);
    }
    const bool dropped = pump_until([&] {
        if (gateway->stats().closed_slow == 0) {
            slow.send_raw(chunk);  // whatever fits; never reads the answers
        }
        return gateway->stats().closed_slow != 0;
    });
    ASSERT_TRUE(dropped) << "sent " << gateway->stats().requests << " requests";
    EXPECT_EQ(gateway->stats().closed_slow, 1U);
    EXPECT_GT(gateway->stats().blocked_writes, 0U);
    EXPECT_EQ(gateway->connections(), 1U);

    // All the while, and afterwards, the well-behaved client is served.
    EXPECT_NE(rest(good, Side::Buy, kP, 100), 0U);
}

TEST_F(GatewayTest, ABigBurstFromAClientThatIsReadingIsNotMistakenForSlowness) {
    // Far more answers than the limit allows to queue, all caused by one
    // write. The client reads them as they come, so nothing ever backs up in
    // the socket and the client must be left alone.
    start({.max_pending_output = 1024});
    Client ann = connect();
    Packet burst;
    constexpr int kRequests = 2'000;
    for (int i = 0; i < kRequests; ++i) {
        const std::size_t at = burst.size();
        burst.resize(at + net::kCancelOrderSize);
        feed::encode(net::CancelOrder{.order_id = 5'000'000 + static_cast<OrderId>(i)},
                     burst.data() + at);
    }
    std::size_t sent = 0;
    int answers = 0;
    const bool done = pump_until([&] {
        sent += ann.send_raw({burst.data() + sent, burst.size() - sent});
        ann.fill();
        while (ann.next_type() == net::OrderRejected::kType) {
            static_cast<void>(ann.take<net::OrderRejected>());
            ++answers;
        }
        return answers == kRequests || ann.closed();
    });
    ASSERT_TRUE(done);
    EXPECT_FALSE(ann.closed());
    EXPECT_EQ(answers, kRequests);
    EXPECT_EQ(gateway->stats().closed_slow, 0U);
    EXPECT_EQ(gateway->connections(), 1U);
}

TEST_F(GatewayTest, AnswersTheSocketWouldNotTakeArriveInOrderOnceTheClientReads) {
    // A client that pauses, as opposed to one that has gone. The limit is far
    // away, so the gateway has to keep what the socket refuses and send it
    // later, without being prompted by anything but the socket having room.
    start({.max_pending_output = std::size_t{1} << 30});
    Client ann = connect(4096);

    // Cancels of orders that do not exist. Each rejection names the order it
    // was asked about, which makes every answer recognisable.
    constexpr OrderId kFirst = 7'000'000;
    OrderId next_id = kFirst;
    Packet unsent;
    std::size_t sent = 0;
    const auto send_some = [&](std::size_t limit) {
        const std::size_t took = ann.send_raw({unsent.data(), std::min(limit, unsent.size())});
        unsent.erase(unsent.begin(), unsent.begin() + static_cast<std::ptrdiff_t>(took));
        sent += took;
    };

    // Write and never read, until the gateway has had a write refused.
    const bool blocked = pump_until([&] {
        if (unsent.empty()) {
            unsent.resize(1'000 * net::kCancelOrderSize);
            for (std::size_t i = 0; i < 1'000; ++i) {
                feed::encode(net::CancelOrder{.order_id = next_id++},
                             unsent.data() + i * net::kCancelOrderSize);
            }
        }
        send_some(unsent.size());
        return gateway->stats().blocked_writes != 0;
    });
    ASSERT_TRUE(blocked) << "the gateway's writes never backed up";
    // Finish the request that was cut off in the middle, if one was.
    ASSERT_TRUE(pump_until([&] {
        send_some((net::kCancelOrderSize - sent % net::kCancelOrderSize) % net::kCancelOrderSize);
        return sent % net::kCancelOrderSize == 0;
    }));
    const std::uint64_t requests = sent / net::kCancelOrderSize;
    ASSERT_TRUE(pump_until([&] { return gateway->stats().requests == requests; }));

    // Now the client only reads. Every answer must come, once and in order.
    OrderId expected = kFirst;
    std::uint64_t out_of_order = 0;
    const bool all = pump_until([&] {
        ann.fill();
        while (ann.next_type() == net::OrderRejected::kType) {
            out_of_order += ann.take<net::OrderRejected>().order_id == expected ? 0U : 1U;
            ++expected;
        }
        return expected == kFirst + requests || ann.closed() || ann.next_type() != 0;
    });
    ASSERT_TRUE(all) << "got " << expected - kFirst << " of " << requests << " answers";
    EXPECT_EQ(expected - kFirst, requests);
    EXPECT_EQ(out_of_order, 0U);
    EXPECT_EQ(ann.next_type(), 0) << "something other than a rejection arrived";
    EXPECT_EQ(ann.buffered(), 0U);
    EXPECT_FALSE(ann.closed());
    EXPECT_EQ(gateway->stats().closed_slow, 0U);

    // And the connection is in working order afterwards.
    EXPECT_NE(rest(ann, Side::Buy, kP, 100), 0U);
}

TEST_F(GatewayTest, ConnectionsOverTheLimitAreRefused) {
    start({.max_connections = 2});
    Client one = connect();
    Client two = connect();
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 2; }));
    Client three = connect();
    EXPECT_TRUE(closed_by_gateway(three));
    EXPECT_EQ(gateway->stats().connections_refused, 1U);
    EXPECT_EQ(gateway->connections(), 2U);
    EXPECT_NE(rest(one, Side::Buy, kP, 100), 0U);

    // A slot freed by a departure can be taken again.
    two.close();
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 1; }));
    Client four = connect();
    EXPECT_NE(rest(four, Side::Buy, kP - kTick, 100), 0U);
}

TEST_F(GatewayTest, ManyConnectionsAtOnce) {
    start();
    std::vector<std::unique_ptr<Client>> clients;
    clients.reserve(200);
    for (int i = 0; i < 200; ++i) {
        clients.push_back(std::make_unique<Client>(gateway->port()));
    }
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 200; }));
    // Every client sends before anyone reads: one turn of the loop sees many
    // sockets ready at once.
    for (std::size_t i = 0; i < clients.size(); ++i) {
        clients[i]->send(net::EnterOrder{.token = i + 1,
                                         .locate = 1,
                                         .side = Side::Buy,
                                         .qty = 100,
                                         .price = kP - static_cast<Price>(i) * kTick});
    }
    for (std::size_t i = 0; i < clients.size(); ++i) {
        net::OrderAccepted accepted;
        ASSERT_TRUE(read(*clients[i], accepted)) << "client " << i;
        EXPECT_EQ(accepted.token, i + 1);
    }
    EXPECT_EQ(gateway->engine().open_orders(), 200U);

    clients.clear();
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 0; }));
    EXPECT_EQ(gateway->engine().open_orders(), 0U);
    EXPECT_EQ(gateway->stats().orders_cancelled_on_disconnect, 200U);
}

// --- Market data -------------------------------------------------------------

TEST_F(GatewayTest, WhatTheGatewayPublishesRebuildsItsBook) {
    start();
    Client ann = connect();
    Client bob = connect();
    for (int i = 0; i < 20; ++i) {
        rest(ann, Side::Buy, kP - static_cast<Price>(i) * kTick, 100);
        rest(bob, Side::Sell, kP + static_cast<Price>(i + 1) * kTick, 100);
    }
    bob.send(net::EnterOrder{
        .token = 1000, .locate = 1, .side = Side::Sell, .qty = 450, .price = kP - 3 * kTick});
    net::OrderAccepted accepted;
    ASSERT_TRUE(read(bob, accepted));
    ann.close();  // cancels what is left of Ann's bids
    ASSERT_TRUE(pump_until([&] { return gateway->connections() == 1; }));
    publisher.flush();

    using Mirror = book::BookManager<book::OrderStore, book::PriceLevels>;
    const auto mirror = std::make_unique<Mirror>();
    net::MoldReceiver receiver(kSession, *mirror);
    for (const Packet& p : packets) {
        ASSERT_TRUE(receiver.on_packet(p));
    }
    EXPECT_TRUE(receiver.complete());
    EXPECT_TRUE(engine::compare_depth(gateway->engine(), *mirror).ok());
    EXPECT_EQ(mirror->counters(), book::Counters{});
    EXPECT_EQ(mirror->orders().size(), gateway->engine().open_orders());
    EXPECT_GT(mirror->stats().bbo_updates, 20U);
}

}  // namespace
