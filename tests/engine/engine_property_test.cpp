#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/gen/rng.hpp"
#include "support/engine_harness.hpp"
#include "support/flow_run.hpp"
#include "support/naive_engine.hpp"

// Property tests: things that must hold for any sequence of requests, checked
// over seeded random flow.
//
// The spec names four (phase 5): the book is never crossed after an operation,
// shares are conserved, earlier orders at a price fill first, and the same
// seed gives identical output. Each has its own test below, and each is
// checked from a different vantage point on purpose:
//
//   never crossed       from the engine's own book
//   shares conserved    from the reports alone
//   price-time order    from the market data alone, as an outsider would
//   determinism         from the bytes
//
// A last test compares the engine with the oracle, request by request.

namespace {

using namespace obe;
using book::Level;
using engine::Accepted;
using engine::Cancelled;
using engine::Executed;
using engine::Rejected;
using engine::Replaced;
using engine::RestingOrder;
using test::FlowRun;
using test::MdMessage;
using test::Report;

constexpr std::uint64_t kCommands = 6'000;

template <class Impl>
class EngineProperty : public ::testing::Test {};
TYPED_TEST_SUITE(EngineProperty, test::EngineTypes);

// --- The book, seen from inside ---------------------------------------------

// Everything that must be true of one side of one book.
template <class Engine>
void check_side(const Engine& engine, Locate locate, Side side, std::set<OrderId>& seen,
                std::size_t& orders_seen) {
    const std::vector<Level> levels = test::levels_of(engine, locate, side);
    const std::vector<RestingOrder> orders = test::orders_of(engine, locate, side);

    for (std::size_t i = 0; i < levels.size(); ++i) {
        ASSERT_GT(levels[i].qty, 0U) << "an empty level was left behind";
        if (i != 0) {
            // Strictly best to worst, so no price appears twice.
            if (side == Side::Buy) {
                ASSERT_LT(levels[i].price, levels[i - 1].price);
            } else {
                ASSERT_GT(levels[i].price, levels[i - 1].price);
            }
        }
    }
    ASSERT_EQ(engine.best(locate, side),
              levels.empty() ? std::nullopt : std::optional<Level>(levels.front()));

    // The orders, grouped by price in the same order, add up to the levels.
    std::vector<Level> summed;
    for (const RestingOrder& o : orders) {
        ASSERT_GT(o.qty, 0U) << "order " << o.id << " rests with nothing showing";
        ASSERT_TRUE(seen.insert(o.id).second) << "order " << o.id << " appears twice";
        if (o.hidden != 0) {
            // Only an iceberg hides shares, and it never shows more than its
            // display size while it has some hidden.
            ASSERT_GT(o.display, 0U) << "order " << o.id << " hides shares without being told to";
            ASSERT_LE(o.qty, o.display) << "order " << o.id << " shows more than it may";
        }
        if (summed.empty() || summed.back().price != o.price) {
            summed.push_back(Level{o.price, 0});
        }
        summed.back().qty += o.qty;
    }
    ASSERT_EQ(summed, levels) << "the level totals disagree with the orders";
    orders_seen += orders.size();
}

TYPED_TEST(EngineProperty, TheBookIsNeverCrossedAndAlwaysAddsUp) {
    for (const gen::FlowConfig& cfg : test::property_configs()) {
        SCOPED_TRACE(test::describe(cfg));
        FlowRun<TypeParam> run(cfg);
        for (std::uint64_t i = 0; i < kCommands; ++i) {
            run.step();
            for (std::uint32_t s = 1; s <= cfg.symbols; ++s) {
                const auto locate = static_cast<Locate>(s);
                const std::optional<Level> bid = run.engine->best(locate, Side::Buy);
                const std::optional<Level> ask = run.engine->best(locate, Side::Sell);
                if (bid && ask) {
                    // Not even locked: an order that could trade, did.
                    ASSERT_LT(bid->price, ask->price)
                        << "locate " << locate << " after request " << i;
                }
            }
            if (i % 40 == 0 || i + 1 == kCommands) {
                std::set<OrderId> seen;
                std::size_t orders = 0;
                for (std::uint32_t s = 1; s <= cfg.symbols; ++s) {
                    for (const Side side : {Side::Buy, Side::Sell}) {
                        check_side(*run.engine, static_cast<Locate>(s), side, seen, orders);
                        ASSERT_FALSE(this->HasFatalFailure()) << "after request " << i;
                    }
                }
                ASSERT_EQ(orders, run.engine->open_orders());
                // The generator follows the book through the reports alone.
                ASSERT_EQ(run.flow.live(), run.engine->open_orders());
            }
        }
    }
}

// --- Conservation, seen from the reports -------------------------------------

// Follows every order through its reports. Shares enter with Accepted, or with
// a Replaced that makes a new order, and leave by trading, by being cancelled
// or by a replace that shrinks the order. What is left must be in the book.
struct Ledger {
    struct Open {
        engine::OwnerId owner;
        engine::Token token;
        Qty qty;
    };
    std::unordered_map<OrderId, Open> open;
    std::uint64_t entered = 0;
    std::uint64_t traded_added = 0;
    std::uint64_t traded_removed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t shrunk = 0;
    std::uint64_t retired = 0;  // shares of an order a replace took out to re-enter
    OrderId last_id = 0;
    // Each new slice of an iceberg takes an id for its market-data reference,
    // so a flow with icebergs skips ids. Without them ids are consecutive.
    bool ids_may_skip = false;
    std::optional<Executed> half;  // the Added side of a trade, awaiting its other side

    void next_id(OrderId id) {
        if (ids_may_skip) {
            ASSERT_GT(id, last_id) << "ids must increase";
        } else {
            ASSERT_EQ(id, last_id + 1) << "ids must count up without gaps";
        }
        last_id = id;
    }

    void operator()(const Accepted& r) {
        next_id(r.order_id);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        ASSERT_GT(r.qty, 0U);
        ASSERT_FALSE(half) << "a trade was reported from one side only";
        open[r.order_id] = Open{r.owner, r.token, r.qty};
        entered += r.qty;
    }
    void operator()(const Executed& r) {
        const auto it = open.find(r.order_id);
        ASSERT_NE(it, open.end()) << "execution of an order that is not open: " << r.order_id;
        ASSERT_EQ(r.owner, it->second.owner);
        ASSERT_EQ(r.token, it->second.token);
        ASSERT_GT(r.qty, 0U);
        ASSERT_LE(r.qty, it->second.qty) << "order " << r.order_id << " traded more than it had";
        it->second.qty -= r.qty;
        ASSERT_EQ(r.leaves, it->second.qty);
        if (r.liquidity == engine::Liquidity::Added) {
            ASSERT_FALSE(half);
            half = r;
            traded_added += r.qty;
        } else {
            // The two sides of a trade are reported together and agree.
            ASSERT_TRUE(half) << "the passive side must be reported first";
            ASSERT_EQ(r.match_number, half->match_number);
            ASSERT_EQ(r.qty, half->qty);
            ASSERT_EQ(r.price, half->price);
            ASSERT_NE(r.order_id, half->order_id);
            half.reset();
            traded_removed += r.qty;
        }
        if (it->second.qty == 0) {
            open.erase(it);
        }
    }
    void operator()(const Cancelled& r) {
        const auto it = open.find(r.order_id);
        ASSERT_NE(it, open.end());
        ASSERT_EQ(r.owner, it->second.owner);
        ASSERT_EQ(r.token, it->second.token);
        ASSERT_EQ(r.qty, it->second.qty) << "a cancel removes everything that is left";
        ASSERT_EQ(r.leaves, 0U);
        ASSERT_FALSE(half);
        cancelled += r.qty;
        open.erase(it);
    }
    void operator()(const Replaced& r) {
        const auto it = open.find(r.old_id);
        ASSERT_NE(it, open.end());
        ASSERT_EQ(r.owner, it->second.owner);
        ASSERT_EQ(r.token, it->second.token);
        ASSERT_GT(r.qty, 0U);
        ASSERT_FALSE(half);
        if (r.kept_priority) {
            ASSERT_EQ(r.new_id, r.old_id);
            ASSERT_LE(r.qty, it->second.qty) << "an order cannot grow and keep its place";
            shrunk += it->second.qty - r.qty;
            it->second.qty = r.qty;
        } else {
            next_id(r.new_id);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
            const Open old = it->second;
            retired += old.qty;
            open.erase(it);
            open[r.new_id] = Open{old.owner, old.token, r.qty};
            entered += r.qty;
        }
    }
    void operator()(const Rejected&) { ASSERT_FALSE(half); }
};

TYPED_TEST(EngineProperty, SharesAreConserved) {
    for (const gen::FlowConfig& cfg : test::property_configs()) {
        SCOPED_TRACE(test::describe(cfg));
        FlowRun<TypeParam> run(cfg);
        run.run(kCommands);

        Ledger ledger;
        ledger.ids_may_skip = cfg.iceberg_per_million != 0;
        for (const Report& report : run.log.all) {
            std::visit(ledger, report);
            ASSERT_FALSE(this->HasFatalFailure());
        }
        ASSERT_FALSE(ledger.half) << "the last trade was reported from one side only";

        // Every share that entered is accounted for exactly once.
        std::uint64_t resting = 0;
        for (const auto& [id, order] : ledger.open) {
            resting += order.qty;
        }
        EXPECT_EQ(ledger.traded_added, ledger.traded_removed);
        EXPECT_EQ(ledger.entered, ledger.traded_added + ledger.traded_removed + ledger.cancelled +
                                      ledger.shrunk + ledger.retired + resting);

        // What the reports say is still open is exactly what the book holds.
        std::unordered_map<OrderId, Qty> in_book;
        for (std::uint32_t s = 1; s <= cfg.symbols; ++s) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                for (const RestingOrder& o :
                     test::orders_of(*run.engine, static_cast<Locate>(s), side)) {
                    in_book[o.id] = o.qty;
                    const auto it = ledger.open.find(o.id);
                    ASSERT_NE(it, ledger.open.end()) << "order " << o.id << " rests unreported";
                    // The owner is told about every open share, shown or not.
                    EXPECT_EQ(it->second.qty, o.open());
                    EXPECT_EQ(it->second.owner, o.owner);
                    EXPECT_EQ(it->second.token, o.token);
                }
            }
        }
        EXPECT_EQ(in_book.size(), ledger.open.size());

        // And the engine's own totals agree with the reports.
        const engine::EngineStats& stats = run.engine->stats();
        EXPECT_EQ(stats.traded_shares, ledger.traded_added);
        EXPECT_EQ(stats.trades, run.messages.template of<feed::OrderExecuted>().size());
        EXPECT_EQ(stats.accepted, run.log.template of<Accepted>().size());
        EXPECT_EQ(stats.rejected, run.log.template of<Rejected>().size());
        EXPECT_EQ(stats.replaces, run.log.template of<Replaced>().size());
    }
}

// --- Priority, seen from the market data -------------------------------------

// The book as somebody outside the exchange can rebuild it, with one thing the
// feed handler does not keep: when each order joined its queue.
struct PublicBook {
    struct Shown {
        Locate locate;
        Side side;
        Price price;
        Qty qty;
        std::uint64_t joined;
    };
    std::unordered_map<OrderId, Shown> orders;
    std::uint64_t clock = 0;
    std::uint64_t last_match = 0;
    OrderId last_ref = 0;
    // An order that trades through icebergs on its way in rests under its own
    // id, which is older than the references their new slices took meanwhile.
    // So with icebergs about, references are unique and not always rising.
    bool refs_may_fall = false;
    std::set<OrderId> used;  // every reference ever shown

    void show(OrderId ref, Locate locate, Side side, Price price, Qty qty) {
        if (!refs_may_fall) {
            ASSERT_GT(ref, last_ref) << "reference numbers must increase";
        }
        last_ref = ref;
        ASSERT_TRUE(used.insert(ref).second) << "reference " << ref << " was used before";
        ASSERT_GT(qty, 0U);
        ASSERT_TRUE(orders.emplace(ref, Shown{locate, side, price, qty, ++clock}).second);
    }

    void operator()(const feed::StockDirectory&) {}
    void operator()(const feed::TradingAction&) {}
    void operator()(const feed::AddOrder& m) {
        show(m.order_ref, m.hdr.locate, m.side, m.price, m.shares);
    }
    void operator()(const feed::OrderExecuted& m) {
        ASSERT_EQ(m.match_number, last_match + 1);
        last_match = m.match_number;
        const auto it = orders.find(m.order_ref);
        ASSERT_NE(it, orders.end()) << "execution of an order the market was never shown";
        const Shown& hit = it->second;
        ASSERT_EQ(hit.locate, m.hdr.locate);
        ASSERT_LE(m.shares, hit.qty);
        ASSERT_GT(m.shares, 0U);
        // The rule itself: nobody on that side had a better claim.
        for (const auto& [ref, other] : orders) {
            if (ref == m.order_ref || other.locate != hit.locate || other.side != hit.side) {
                continue;
            }
            const bool better_price =
                hit.side == Side::Buy ? other.price > hit.price : other.price < hit.price;
            ASSERT_FALSE(better_price)
                << "order " << m.order_ref << " at " << hit.price << " traded while order " << ref
                << " waited at " << other.price;
            ASSERT_FALSE(other.price == hit.price && other.joined < hit.joined)
                << "order " << m.order_ref << " traded ahead of order " << ref
                << ", which joined the same price earlier";
        }
        it->second.qty -= m.shares;
        if (it->second.qty == 0) {
            orders.erase(it);
        }
    }
    void operator()(const feed::OrderCancel& m) {
        const auto it = orders.find(m.order_ref);
        ASSERT_NE(it, orders.end());
        ASSERT_LT(m.shares, it->second.qty) << "a reduce to zero should have been a delete";
        ASSERT_GT(m.shares, 0U);
        it->second.qty -= m.shares;  // and it keeps its place
    }
    void operator()(const feed::OrderDelete& m) { ASSERT_EQ(orders.erase(m.order_ref), 1U); }
    void operator()(const feed::OrderReplace& m) {
        const auto it = orders.find(m.orig_order_ref);
        ASSERT_NE(it, orders.end());
        const Shown old = it->second;
        orders.erase(it);
        ASSERT_EQ(old.locate, m.hdr.locate);
        show(m.new_order_ref, old.locate, old.side, m.price, m.shares);  // at the back
    }
};

TYPED_TEST(EngineProperty, TradesFollowPriceThenTimeAsSeenFromTheFeed) {
    for (const gen::FlowConfig& cfg : test::property_configs()) {
        SCOPED_TRACE(test::describe(cfg));
        FlowRun<TypeParam> run(cfg);
        run.run(kCommands);

        PublicBook market;
        market.refs_may_fall = cfg.iceberg_per_million != 0;
        std::size_t index = 0;
        for (const MdMessage& message : run.messages.all) {
            std::visit(market, message);
            ASSERT_FALSE(this->HasFatalFailure()) << "at market data message " << index;
            ++index;
        }
        // The outsider's book and the engine's hold the same orders.
        EXPECT_EQ(market.orders.size(), run.engine->open_orders());
        EXPECT_GT(market.last_match, 100U) << "the flow should have produced trades to check";
    }
}

// --- Determinism -------------------------------------------------------------

TYPED_TEST(EngineProperty, TheSameSeedGivesTheSameBytesAndReports) {
    const gen::FlowConfig cfg = test::busy_config(77);
    FlowRun<TypeParam> first(cfg);
    FlowRun<TypeParam> second(cfg);
    first.run(kCommands);
    second.run(kCommands);
    EXPECT_EQ(first.writer.bytes(), second.writer.bytes());
    EXPECT_EQ(first.log.all, second.log.all);

    gen::FlowConfig other = cfg;
    other.seed = 78;
    FlowRun<TypeParam> third(other);
    third.run(kCommands);
    EXPECT_NE(first.writer.bytes(), third.writer.bytes());
}

// --- Against the oracle ------------------------------------------------------

// Runs the same flow through the engine under test and through the oracle and
// reports the first place their output differs.
template <class Impl>
void expect_same_as_oracle(const gen::FlowConfig& cfg, std::uint64_t commands) {
    FlowRun<Impl> run(cfg);
    FlowRun<test::NaiveEngineImpl> oracle(cfg);
    for (std::uint64_t i = 0; i < commands; ++i) {
        const std::size_t reports_before = run.log.all.size();
        const std::size_t messages_before = run.messages.all.size();
        const gen::Command asked = run.step();
        const gen::Command oracle_asked = oracle.step();
        // If the two engines had already differed, the generators would have
        // been told different things and would now ask for different things.
        ASSERT_EQ(asked, oracle_asked) << "at request " << i;

        ASSERT_EQ(run.log.all.size(), oracle.log.all.size()) << "at request " << i;
        for (std::size_t r = reports_before; r < run.log.all.size(); ++r) {
            ASSERT_EQ(run.log.all[r], oracle.log.all[r]) << "report " << r << ", request " << i;
        }
        ASSERT_EQ(run.messages.all.size(), oracle.messages.all.size()) << "at request " << i;
        for (std::size_t m = messages_before; m < run.messages.all.size(); ++m) {
            ASSERT_EQ(run.messages.all[m], oracle.messages.all[m])
                << "market data message " << m << ", request " << i;
        }
    }
    EXPECT_EQ(run.writer.bytes(), oracle.writer.bytes());
    EXPECT_EQ(run.engine->stats(), oracle.engine->stats());
    for (std::uint32_t s = 1; s <= cfg.symbols; ++s) {
        for (const Side side : {Side::Buy, Side::Sell}) {
            const auto locate = static_cast<Locate>(s);
            EXPECT_EQ(test::orders_of(*run.engine, locate, side),
                      test::orders_of(*oracle.engine, locate, side));
            EXPECT_EQ(test::levels_of(*run.engine, locate, side),
                      test::levels_of(*oracle.engine, locate, side));
        }
    }
}

TYPED_TEST(EngineProperty, AgreesWithTheOracle) {
    for (const gen::FlowConfig& cfg : test::property_configs()) {
        SCOPED_TRACE(test::describe(cfg));
        expect_same_as_oracle<TypeParam>(cfg, kCommands);
        ASSERT_FALSE(this->HasFatalFailure());
    }
}

TYPED_TEST(EngineProperty, AgreesWithTheOracleWhenOneOwnerTradesWithItself) {
    expect_same_as_oracle<TypeParam>(
        {.seed = 11, .symbols = 1, .owners = 1, .target_live_orders = 30, .bad_per_million = 0},
        kCommands);
}

TYPED_TEST(EngineProperty, AgreesWithTheOracleOnADeepBook) {
    expect_same_as_oracle<TypeParam>({.seed = 13,
                                      .symbols = 8,
                                      .owners = 16,
                                      .target_live_orders = 1'500,
                                      .bad_per_million = 1'000},
                                     2 * kCommands);
}

// Fill-or-kill is where the three instructions meet: whether such an order
// can be filled depends on hidden shares and on whose orders are where in
// each queue (docs/design.md, section 8.6). The engine under test has to work
// that out before it trades; the oracle simply does the trades with its output
// off and undoes them. The ordinary flows send few fill-or-kill orders, so
// this one adds them: every few requests, the same extra order goes to both
// engines, sized around what the book holds, in each prevention mode.
TYPED_TEST(EngineProperty, FillOrKillAgreesWithTheOracleAmongInstructions) {
    std::uint64_t filled = 0;
    std::uint64_t killed = 0;
    for (std::uint64_t seed = 301; seed <= 304; ++seed) {
        const gen::FlowConfig cfg = test::instructed_config(seed);
        SCOPED_TRACE(test::describe(cfg));
        FlowRun<TypeParam> run(cfg);
        FlowRun<test::NaiveEngineImpl> oracle(cfg);
        gen::SplitMix64 rng(seed);
        engine::Token token = engine::Token{1} << 40;  // clear of the generator's own

        for (std::uint64_t i = 0; i < 4'000; ++i) {
            ASSERT_EQ(run.step(), oracle.step()) << "at request " << i;
            if (i % 3 != 0) {
                continue;
            }
            const auto locate = static_cast<Locate>(1 + rng.below(cfg.symbols));
            const Side side = rng.below(2) == 0 ? Side::Buy : Side::Sell;
            const Side other = side == Side::Buy ? Side::Sell : Side::Buy;
            // Around the size of the first few levels it would trade with, so
            // that it is often just fillable and often just not.
            std::uint64_t near = 0;
            Price limit = 0;
            int levels = 0;
            for (const RestingOrder& order : test::orders_of(*oracle.engine, locate, other)) {
                if (order.price != limit) {
                    if (++levels > 3) {
                        break;
                    }
                    limit = order.price;
                }
                near += order.open();
            }
            if (near == 0) {
                continue;
            }
            const engine::NewOrder order{
                .owner = static_cast<engine::OwnerId>(1 + rng.below(cfg.owners)),
                .token = ++token,
                .locate = locate,
                .side = side,
                .qty = static_cast<Qty>(1 + rng.below(near + near / 4)),
                .price = limit,
                .tif = engine::TimeInForce::FillOrKill,
                .self_match = static_cast<engine::SelfMatch>(rng.below(4))};
            const Nanos now = run.flow.now();
            const std::size_t reports_before = run.log.all.size();
            const std::size_t messages_before = run.messages.all.size();
            run.engine->submit(order, now);
            oracle.engine->submit(order, now);

            ASSERT_EQ(run.log.all.size(), oracle.log.all.size()) << "after request " << i;
            for (std::size_t r = reports_before; r < run.log.all.size(); ++r) {
                ASSERT_EQ(run.log.all[r], oracle.log.all[r])
                    << "report " << r << " of a fill-or-kill for " << order.qty << " after request "
                    << i;
            }
            ASSERT_EQ(run.messages.all.size(), oracle.messages.all.size());
            for (std::size_t m = messages_before; m < run.messages.all.size(); ++m) {
                ASSERT_EQ(run.messages.all[m], oracle.messages.all[m]) << "after request " << i;
            }
            // All or nothing, whichever it was.
            const Report& last = run.log.all.back();
            const Cancelled* cancel = std::get_if<Cancelled>(&last);
            if (cancel != nullptr && cancel->token == order.token) {
                ASSERT_EQ(cancel->reason, engine::CancelReason::FillOrKill);
                ASSERT_EQ(cancel->qty, order.qty) << "killed whole";
                ASSERT_EQ(run.log.all.size(), reports_before + 2) << "and nothing else happened";
                ASSERT_EQ(run.messages.all.size(), messages_before);
                ++killed;
            } else {
                const Executed* fill = std::get_if<Executed>(&last);
                ASSERT_NE(fill, nullptr);
                ASSERT_EQ(fill->token, order.token);
                ASSERT_EQ(fill->leaves, 0U) << "filled whole";
                ++filled;
            }
        }
        EXPECT_EQ(run.engine->stats(), oracle.engine->stats());
    }
    // Both outcomes, often, or the comparison above proved little.
    EXPECT_GT(filled, 500U);
    EXPECT_GT(killed, 500U);
}

// --- The tests above are only as good as the flow ----------------------------

TYPED_TEST(EngineProperty, TheFlowsReachEveryPath) {
    std::set<engine::RejectReason> rejects;
    std::set<engine::CancelReason> cancels;
    std::uint64_t kept = 0;
    std::uint64_t moved = 0;
    std::uint64_t partial = 0;
    std::uint64_t complete = 0;
    std::map<char, std::uint64_t> messages;
    std::string trace;
    // The instructions.
    std::uint64_t replenished = 0;          // new slices of icebergs
    std::uint64_t stopped_resting = 0;      // resting orders cancelled by prevention
    std::uint64_t stopped_incoming = 0;     // incoming orders stopped by prevention
    std::uint64_t post_only_cancelled = 0;  // post-only orders that would have traded
    std::uint64_t post_only_asked = 0;
    std::uint64_t hidden_cut = 0;  // in-place replaces that the market did not hear of

    for (const gen::FlowConfig& cfg : test::property_configs()) {
        FlowRun<TypeParam> run(cfg);
        std::unordered_map<OrderId, Qty> last_open;  // open shares of each order, by the reports
        for (std::uint64_t i = 0; i < kCommands; ++i) {
            const std::size_t reports_before = run.log.all.size();
            const std::size_t messages_before = run.messages.all.size();
            const gen::Command c = run.step();
            if (c.kind == gen::CommandKind::New && c.order.post_only) {
                ++post_only_asked;
            }
            // A kept-priority replace that made the order smaller and told the
            // market nothing took all of it from hidden shares.
            if (c.kind == gen::CommandKind::Replace && run.log.all.size() == reports_before + 1 &&
                run.messages.all.size() == messages_before) {
                const Replaced* r = std::get_if<Replaced>(&run.log.all.back());
                if (r != nullptr && r->kept_priority && r->qty < last_open[r->old_id]) {
                    ++hidden_cut;
                }
            }
            for (std::size_t k = reports_before; k < run.log.all.size(); ++k) {
                if (const Accepted* a = std::get_if<Accepted>(&run.log.all[k])) {
                    last_open[a->order_id] = a->qty;
                } else if (const Executed* e = std::get_if<Executed>(&run.log.all[k])) {
                    last_open[e->order_id] = e->leaves;
                } else if (const Replaced* r = std::get_if<Replaced>(&run.log.all[k])) {
                    last_open[r->new_id] = r->qty;
                } else if (const Cancelled* x = std::get_if<Cancelled>(&run.log.all[k])) {
                    if (x->reason == engine::CancelReason::SelfMatch) {
                        const bool is_incoming =
                            c.kind == gen::CommandKind::New && x->token == c.order.token;
                        ++(is_incoming ? stopped_incoming : stopped_resting);
                    } else if (x->reason == engine::CancelReason::PostOnly) {
                        ++post_only_cancelled;
                    }
                }
            }
        }
        // An 'A' whose reference no order was accepted or replaced under is a
        // new slice of an iceberg.
        std::set<OrderId> own_ids;
        for (const Accepted& a : run.log.template of<Accepted>()) {
            own_ids.insert(a.order_id);
        }
        for (const Replaced& r : run.log.template of<Replaced>()) {
            own_ids.insert(r.new_id);
        }
        for (const feed::AddOrder& m : run.messages.template of<feed::AddOrder>()) {
            replenished += own_ids.contains(m.order_ref) ? 0U : 1U;
        }
        for (const Rejected& r : run.log.template of<Rejected>()) {
            rejects.insert(r.reason);
        }
        for (const Cancelled& r : run.log.template of<Cancelled>()) {
            cancels.insert(r.reason);
        }
        for (const Replaced& r : run.log.template of<Replaced>()) {
            ++(r.kept_priority ? kept : moved);
        }
        // Resting orders filled in part, and resting orders filled completely.
        for (const Executed& r : run.log.template of<Executed>()) {
            if (r.liquidity == engine::Liquidity::Added) {
                ++(r.leaves == 0 ? complete : partial);
            }
        }
        for (const char type : {'A', 'E', 'X', 'D', 'U'}) {
            messages[type] += run.writer.count(type);
        }
        trace += run.messages.trace();
        trace += '|';
    }

    EXPECT_EQ(rejects.size(), 8U) << "every reject reason should occur";
    EXPECT_EQ(cancels.size(), 6U) << "every cancel reason should occur";
    EXPECT_GT(replenished, 500U) << "icebergs should be traded through";
    EXPECT_GT(stopped_resting, 80U) << "self-match prevention should cancel resting orders";
    EXPECT_GT(stopped_incoming, 40U) << "self-match prevention should stop incoming orders";
    EXPECT_GT(post_only_cancelled, 25U);
    EXPECT_GT(post_only_asked, 4 * post_only_cancelled) << "most post-only orders should rest";
    EXPECT_GT(hidden_cut, 60U) << "icebergs should be reduced in place";
    EXPECT_GT(kept, 200U);
    EXPECT_GT(moved, 200U);
    EXPECT_GT(partial, 200U);
    EXPECT_GT(complete, 200U);
    for (const auto& [type, count] : messages) {
        EXPECT_GT(count, 200U) << "message type " << type;
    }
    // A replace that trades shows up as a delete followed by an execution,
    // with or without an add for what was left. A sweep is several executions
    // in a row, and an incoming order that rests after trading is an
    // execution followed by an add.
    EXPECT_NE(trace.find("DEA"), std::string::npos);
    EXPECT_NE(trace.find("DED"), std::string::npos);
    EXPECT_NE(trace.find("EEE"), std::string::npos);
    EXPECT_NE(trace.find("AEA"), std::string::npos);
}

}  // namespace
