#include "obe/book/book_manager.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/feed/messages.hpp"
#include "support/implementations.hpp"
#include "support/printers.hpp"

// Scenario tests: one small, hand-written situation per test, each pinning one
// rule of how ITCH messages change the displayed book (spec sections 4.4 and
// 4.5). They run against every store/levels pair in support/implementations.hpp.

namespace {

using namespace obe;
using book::Bbo;
using book::BboUpdate;
using book::Level;

struct Recorder {
    std::vector<BboUpdate> updates;
    void on_bbo(const BboUpdate& u) { updates.push_back(u); }
};

// $123.45 -> 1'234'500. Whole cents are enough for these tests.
constexpr Price px(std::uint32_t dollars, std::uint32_t cents = 0) {
    return dollars * 10'000 + cents * 100;
}

constexpr Locate kStock = 17;
constexpr Locate kOther = 18;

template <class Impl>
class BookScenario : public ::testing::Test {
 protected:
    using Manager = book::BookManager<typename Impl::Store, typename Impl::Levels, Recorder>;

    Manager m;
    Nanos clock = 34'200'000'000'000ULL;  // 09:30:00

    feed::Header hdr(Locate locate) {
        clock += 1'000;
        return {.locate = locate, .tracking = 0, .timestamp = clock};
    }

    void add(OrderId ref, Side side, Qty shares, Price price, Locate locate = kStock) {
        feed::AddOrder msg{.hdr = hdr(locate), .order_ref = ref, .side = side, .shares = shares};
        msg.price = price;
        m.on_add(msg);
    }
    void execute(OrderId ref, Qty shares, Locate locate = kStock) {
        m.on_execute({.hdr = hdr(locate), .order_ref = ref, .shares = shares, .match_number = 1});
    }
    void execute_at(OrderId ref, Qty shares, char printable, Price price, Locate locate = kStock) {
        feed::OrderExecutedWithPrice msg{.hdr = hdr(locate), .order_ref = ref, .shares = shares};
        msg.printable = printable;
        msg.price = price;
        m.on_execute_with_price(msg);
    }
    void cancel(OrderId ref, Qty shares, Locate locate = kStock) {
        m.on_cancel({.hdr = hdr(locate), .order_ref = ref, .shares = shares});
    }
    void remove(OrderId ref, Locate locate = kStock) {
        m.on_delete({.hdr = hdr(locate), .order_ref = ref});
    }
    void replace(OrderId orig, OrderId next, Qty shares, Price price, Locate locate = kStock) {
        m.on_replace({.hdr = hdr(locate),
                      .orig_order_ref = orig,
                      .new_order_ref = next,
                      .shares = shares,
                      .price = price});
    }

    static std::vector<Level> walk(const typename Impl::Levels& levels) {
        std::vector<Level> out;
        levels.for_each([&out](const Level& level) {
            out.push_back(level);
            return true;
        });
        return out;
    }
    std::vector<Level> bids(Locate locate = kStock) const { return walk(m.book(locate).bids()); }
    std::vector<Level> asks(Locate locate = kStock) const { return walk(m.book(locate).asks()); }
    Bbo bbo(Locate locate = kStock) const { return m.book(locate).bbo(); }
    const std::vector<BboUpdate>& updates() const { return m.listener().updates; }

    // Everything the spec asks to be zero at the end of a replay.
    void expect_clean() const {
        EXPECT_EQ(m.counters(), book::Counters{});
        const book::Audit audit = m.audit();
        EXPECT_EQ(audit.empty_levels, 0U);
        EXPECT_EQ(audit.level_shares, audit.open_shares);
    }
};
TYPED_TEST_SUITE(BookScenario, test::BookTypes);

using Levels = std::vector<Level>;

// --- Add ('A', 'F') -----------------------------------------------------------------

TYPED_TEST(BookScenario, StartsWithNoBookAndNoUpdates) {
    EXPECT_EQ(this->bbo(), Bbo{});
    EXPECT_TRUE(this->bids().empty());
    EXPECT_TRUE(this->asks().empty());
    EXPECT_TRUE(this->updates().empty());
    EXPECT_EQ(this->m.orders().size(), 0U);
    this->expect_clean();
}

TYPED_TEST(BookScenario, AddCreatesALevelAndPublishesTheNewBest) {
    this->add(1, Side::Buy, 300, px(100));
    EXPECT_EQ(this->bids(), (Levels{{px(100), 300}}));
    EXPECT_TRUE(this->asks().empty());
    EXPECT_EQ(this->bbo(), (Bbo{px(100), 300, 0, 0}));
    ASSERT_EQ(this->updates().size(), 1U);
    EXPECT_EQ(this->updates()[0], (BboUpdate{kStock, this->clock, Bbo{px(100), 300, 0, 0}}));
    EXPECT_EQ(this->m.orders().size(), 1U);
    this->expect_clean();
}

TYPED_TEST(BookScenario, AddBehindTheBestChangesDepthButPublishesNothing) {
    this->add(1, Side::Sell, 100, px(101));
    this->add(2, Side::Sell, 500, px(102));  // worse than the best ask
    EXPECT_EQ(this->asks(), (Levels{{px(101), 100}, {px(102), 500}}));
    EXPECT_EQ(this->updates().size(), 1U) << "the best ask did not change";
    this->expect_clean();
}

TYPED_TEST(BookScenario, AddAtTheBestPublishesTheLargerSize) {
    this->add(1, Side::Buy, 100, px(100));
    this->add(2, Side::Buy, 250, px(100));
    EXPECT_EQ(this->bids(), (Levels{{px(100), 350}}));
    ASSERT_EQ(this->updates().size(), 2U);
    EXPECT_EQ(this->updates()[1].bbo, (Bbo{px(100), 350, 0, 0}));
    this->expect_clean();
}

TYPED_TEST(BookScenario, AddThatImprovesTheBestPublishesTheNewPrice) {
    this->add(1, Side::Buy, 100, px(100));
    this->add(2, Side::Sell, 100, px(101));
    this->add(3, Side::Buy, 40, px(100, 50));
    this->add(4, Side::Sell, 60, px(100, 75));
    EXPECT_EQ(this->bbo(), (Bbo{px(100, 50), 40, px(100, 75), 60}));
    EXPECT_EQ(this->bids(), (Levels{{px(100, 50), 40}, {px(100), 100}}));
    EXPECT_EQ(this->asks(), (Levels{{px(100, 75), 60}, {px(101), 100}}));
    EXPECT_EQ(this->updates().size(), 4U);
    this->expect_clean();
}

TYPED_TEST(BookScenario, AttributedAddBehavesLikeAPlainAdd) {
    feed::AddOrder msg{.hdr = this->hdr(kStock), .order_ref = 1, .side = Side::Sell, .shares = 200};
    msg.price = px(50);
    msg.attributed = true;
    msg.mpid = {'V', 'I', 'R', 'T'};
    this->m.on_add(msg);
    EXPECT_EQ(this->asks(), (Levels{{px(50), 200}}));
    this->expect_clean();
}

// --- Execute ('E') ------------------------------------------------------------------

TYPED_TEST(BookScenario, PartialExecutionReducesTheOrderAndItsLevel) {
    this->add(1, Side::Sell, 300, px(101));
    this->add(2, Side::Sell, 200, px(101));
    this->execute(1, 100);
    EXPECT_EQ(this->asks(), (Levels{{px(101), 400}}));
    ASSERT_NE(this->m.orders().find(1), nullptr);
    EXPECT_EQ(this->m.orders().find(1)->qty, 200U);
    EXPECT_EQ(this->m.orders().find(2)->qty, 200U) << "the other order at the price is untouched";
    EXPECT_EQ(this->m.book(kStock).executed_shares(), 100U);
    EXPECT_EQ(this->updates().back().bbo, (Bbo{0, 0, px(101), 400}));
    this->expect_clean();
}

TYPED_TEST(BookScenario, FullExecutionRemovesTheOrderWithNoDeleteMessage) {
    this->add(1, Side::Buy, 300, px(100));
    this->execute(1, 300);
    EXPECT_EQ(this->m.orders().find(1), nullptr);
    EXPECT_EQ(this->m.orders().size(), 0U);
    EXPECT_TRUE(this->bids().empty());
    this->expect_clean();

    // ITCH sends nothing more for a filled order. If something does arrive,
    // the order is unknown.
    this->remove(1);
    EXPECT_EQ(this->m.counters().unknown_order, 1U);
}

TYPED_TEST(BookScenario, ExecutionThatEmptiesTheBestLevelExposesTheNextOne) {
    this->add(1, Side::Sell, 100, px(101));
    this->add(2, Side::Sell, 700, px(102));
    this->execute(1, 100);
    EXPECT_EQ(this->asks(), (Levels{{px(102), 700}})) << "the emptied level must be gone";
    EXPECT_EQ(this->updates().back().bbo, (Bbo{0, 0, px(102), 700}));
    this->expect_clean();
}

TYPED_TEST(BookScenario, ExecutionThatEmptiesTheBookPublishesAnEmptySide) {
    this->add(1, Side::Buy, 100, px(100));
    this->execute(1, 100);
    ASSERT_EQ(this->updates().size(), 2U);
    EXPECT_EQ(this->updates()[1].bbo, Bbo{});
    this->expect_clean();
}

TYPED_TEST(BookScenario, ExecutionsAreSeveralPartialFillsOfOneOrder) {
    this->add(1, Side::Buy, 1'000, px(100));
    this->execute(1, 100);
    this->execute(1, 400);
    this->execute(1, 500);
    EXPECT_EQ(this->m.orders().size(), 0U);
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->m.book(kStock).executed_shares(), 1'000U);
    this->expect_clean();
}

// --- Execute with price ('C') ---------------------------------------------------------

TYPED_TEST(BookScenario, ExecutionWithPriceComesOffTheDisplayLevelNotTheExecutionPrice) {
    this->add(1, Side::Sell, 300, px(101));
    this->execute_at(1, 100, 'Y', px(100, 90));  // traded better than displayed
    EXPECT_EQ(this->asks(), (Levels{{px(101), 200}}));
    EXPECT_EQ(this->m.book(kStock).executed_shares(), 100U);
    this->expect_clean();
}

TYPED_TEST(BookScenario, NonPrintableExecutionReducesTheBookButNotVolume) {
    this->add(1, Side::Sell, 300, px(101));
    this->execute_at(1, 100, 'N', px(101));
    EXPECT_EQ(this->asks(), (Levels{{px(101), 200}})) << "the shares still leave the book";
    EXPECT_EQ(this->m.book(kStock).executed_shares(), 0U)
        << "a later cross message reports this volume; counting it here would double it";
    this->execute_at(1, 50, 'Y', px(101));
    EXPECT_EQ(this->m.book(kStock).executed_shares(), 50U);
    this->expect_clean();
}

// --- Cancel ('X') and delete ('D') -------------------------------------------------------

TYPED_TEST(BookScenario, CancelRemovesTheGivenSharesAndIsNotVolume) {
    this->add(1, Side::Buy, 500, px(100));
    this->cancel(1, 200);
    EXPECT_EQ(this->bids(), (Levels{{px(100), 300}}));
    EXPECT_EQ(this->m.orders().find(1)->qty, 300U) << "'shares' is the amount cancelled";
    EXPECT_EQ(this->m.book(kStock).executed_shares(), 0U);
    EXPECT_EQ(this->updates().back().bbo, (Bbo{px(100), 300, 0, 0}));
    this->expect_clean();
}

TYPED_TEST(BookScenario, CancelOfEverythingLeftRemovesTheOrder) {
    this->add(1, Side::Buy, 500, px(100));
    this->cancel(1, 500);
    EXPECT_EQ(this->m.orders().size(), 0U);
    EXPECT_TRUE(this->bids().empty());
    this->expect_clean();
}

TYPED_TEST(BookScenario, DeleteRemovesWhateverIsLeft) {
    this->add(1, Side::Sell, 500, px(101));
    this->add(2, Side::Sell, 100, px(101));
    this->execute(1, 200);
    this->remove(1);
    EXPECT_EQ(this->asks(), (Levels{{px(101), 100}}))
        << "delete takes the remaining 300, not the original 500";
    EXPECT_EQ(this->m.orders().find(1), nullptr);
    EXPECT_EQ(this->m.orders().size(), 1U);
    this->expect_clean();
}

// --- Replace ('U') -----------------------------------------------------------------------

TYPED_TEST(BookScenario, ReplaceMovesTheOrderToANewPriceUnderANewReference) {
    this->add(1, Side::Buy, 300, px(100));
    this->add(2, Side::Buy, 100, px(99));
    this->replace(1, 3, 300, px(99, 50));
    EXPECT_EQ(this->bids(), (Levels{{px(99, 50), 300}, {px(99), 100}}));
    EXPECT_EQ(this->m.orders().find(1), nullptr) << "the original reference is dead";
    ASSERT_NE(this->m.orders().find(3), nullptr);
    EXPECT_EQ(*this->m.orders().find(3), (book::OrderRecord{px(99, 50), 300, Side::Buy}));
    EXPECT_EQ(this->m.orders().size(), 2U);
    this->expect_clean();
}

TYPED_TEST(BookScenario, ReplacePublishesOnceNotOncePerHalf) {
    this->add(1, Side::Buy, 300, px(100));
    const std::size_t before = this->updates().size();
    this->replace(1, 2, 300, px(100, 10));
    ASSERT_EQ(this->updates().size(), before + 1)
        << "the instant with the old order gone and the new one not yet added was never displayed";
    EXPECT_EQ(this->updates().back().bbo, (Bbo{px(100, 10), 300, 0, 0}));
}

TYPED_TEST(BookScenario, ReplaceCanChangeOnlyTheSize) {
    this->add(1, Side::Sell, 300, px(101));
    this->add(2, Side::Sell, 100, px(101));
    this->replace(1, 3, 50, px(101));
    EXPECT_EQ(this->asks(), (Levels{{px(101), 150}}));
    EXPECT_EQ(this->m.orders().find(3)->qty, 50U);
    this->expect_clean();
}

TYPED_TEST(BookScenario, ReplaceKeepsTheSideOfTheOriginal) {
    // The replace message has no side field. The new order must land on the
    // ask side because the original was a sell.
    this->add(1, Side::Sell, 300, px(101));
    this->replace(1, 2, 300, px(102));
    EXPECT_EQ(this->asks(), (Levels{{px(102), 300}}));
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->m.orders().find(2)->side, Side::Sell);
    this->expect_clean();
}

TYPED_TEST(BookScenario, ReplaceOfAPartlyExecutedOrderRemovesOnlyWhatWasLeft) {
    this->add(1, Side::Buy, 500, px(100));
    this->add(2, Side::Buy, 100, px(100));
    this->execute(1, 200);  // 300 left
    this->replace(1, 3, 1'000, px(99));
    EXPECT_EQ(this->bids(), (Levels{{px(100), 100}, {px(99), 1'000}}));
    this->expect_clean();
}

TYPED_TEST(BookScenario, LaterMessagesUseTheNewReference) {
    this->add(1, Side::Buy, 500, px(100));
    this->replace(1, 2, 500, px(100));
    this->execute(2, 100);
    this->cancel(2, 100);
    this->replace(2, 3, 300, px(100, 5));
    this->remove(3);
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->m.orders().size(), 0U);
    this->expect_clean();

    this->execute(1, 1);  // the original reference stayed dead throughout
    EXPECT_EQ(this->m.counters().unknown_order, 1U);
}

// --- Several securities -----------------------------------------------------------------

TYPED_TEST(BookScenario, EachLocateHasItsOwnBook) {
    this->add(1, Side::Buy, 100, px(10), kStock);
    this->add(2, Side::Buy, 200, px(500), kOther);
    this->add(3, Side::Sell, 300, px(501), kOther);
    EXPECT_EQ(this->bbo(kStock), (Bbo{px(10), 100, 0, 0}));
    EXPECT_EQ(this->bbo(kOther), (Bbo{px(500), 200, px(501), 300}));
    this->execute(2, 200, kOther);
    EXPECT_EQ(this->bbo(kStock), (Bbo{px(10), 100, 0, 0}));
    EXPECT_EQ(this->bbo(kOther), (Bbo{0, 0, px(501), 300}));
    EXPECT_EQ(this->updates().back().locate, kOther);
    this->expect_clean();
}

TYPED_TEST(BookScenario, TheHighestAndLowestLocatesWork) {
    this->add(1, Side::Buy, 100, px(10), 0);
    this->add(2, Side::Buy, 100, px(20), 65'535);
    EXPECT_EQ(this->bbo(0), (Bbo{px(10), 100, 0, 0}));
    EXPECT_EQ(this->bbo(65'535), (Bbo{px(20), 100, 0, 0}));
    this->expect_clean();
}

TYPED_TEST(BookScenario, StockDirectoryMapsLocatesToSymbols) {
    EXPECT_FALSE(this->m.listed(kStock));
    EXPECT_EQ(this->m.find_locate("AAPL"), std::nullopt);
    this->m.on_stock_directory({.hdr = this->hdr(kStock), .stock = feed::Symbol::from("AAPL")});
    this->m.on_stock_directory({.hdr = this->hdr(kOther), .stock = feed::Symbol::from("AAPLW")});
    EXPECT_TRUE(this->m.listed(kStock));
    EXPECT_EQ(this->m.symbol(kStock), "AAPL");
    EXPECT_EQ(this->m.find_locate("AAPL"), kStock);
    EXPECT_EQ(this->m.find_locate("AAPLW"), kOther);
    EXPECT_EQ(this->m.find_locate("AAP"), std::nullopt) << "a prefix is not a match";
}

// --- Locked and crossed books --------------------------------------------------------------

TYPED_TEST(BookScenario, ACrossedBookIsRecordedAsDisplayedNotRejected) {
    // The feed-side book mirrors what the exchange displayed. During a halt or
    // before an auction the displayed bid can sit above the displayed ask. The
    // book must hold that state faithfully; it is counted, not treated as an
    // error.
    this->m.on_trading_action(
        {.hdr = this->hdr(kStock), .stock = feed::Symbol::from("XYZ"), .trading_state = 'H'});
    this->add(1, Side::Sell, 100, px(100));
    this->add(2, Side::Buy, 100, px(100));  // locked
    this->add(3, Side::Buy, 100, px(101));  // crossed
    EXPECT_TRUE(this->bbo().crossed());
    EXPECT_EQ(this->m.stats().locked_while_not_trading, 1U);
    EXPECT_EQ(this->m.stats().crossed_while_not_trading, 1U);
    EXPECT_EQ(this->m.stats().crossed_while_trading, 0U);
    EXPECT_EQ(this->m.book(kStock).trading_state(), 'H');

    this->m.on_trading_action(
        {.hdr = this->hdr(kStock), .stock = feed::Symbol::from("XYZ"), .trading_state = 'T'});
    this->add(4, Side::Buy, 100, px(102));
    EXPECT_EQ(this->m.stats().crossed_while_trading, 1U);
    this->expect_clean();
}

// --- Bad input: counted, survived, never fatal -------------------------------------------

TYPED_TEST(BookScenario, MessagesForUnknownOrdersAreCountedAndIgnored) {
    this->add(1, Side::Buy, 100, px(100));
    const std::size_t before = this->updates().size();
    this->execute(99, 10);
    this->execute_at(99, 10, 'Y', px(100));
    this->cancel(99, 10);
    this->remove(99);
    this->replace(99, 100, 10, px(100));
    EXPECT_EQ(this->m.counters().unknown_order, 5U);
    EXPECT_EQ(this->bids(), (Levels{{px(100), 100}}));
    EXPECT_EQ(this->updates().size(), before);
    EXPECT_EQ(this->m.orders().find(100), nullptr)
        << "a replace of an unknown order must not add the new one: its side is unknown";
    EXPECT_EQ(this->m.book(kStock).executed_shares(), 0U);
}

TYPED_TEST(BookScenario, TakingMoreThanIsLeftIsCountedAndClampedToWhatIsThere) {
    this->add(1, Side::Buy, 100, px(100));
    this->add(2, Side::Buy, 100, px(100));
    this->execute(1, 150);
    EXPECT_EQ(this->m.counters().overfill, 1U);
    EXPECT_EQ(this->m.orders().find(1), nullptr) << "the order is gone, not at a negative size";
    EXPECT_EQ(this->bids(), (Levels{{px(100), 100}})) << "the other order's shares are intact";
    EXPECT_EQ(this->m.book(kStock).executed_shares(), 100U);

    this->cancel(2, 4'000'000'000U);
    EXPECT_EQ(this->m.counters().overfill, 2U);
    EXPECT_TRUE(this->bids().empty());
    const book::Audit audit = this->m.audit();
    EXPECT_TRUE(audit.clean()) << "levels and orders still agree after clamping";
}

TYPED_TEST(BookScenario, AReusedReferenceNumberIsCountedAndTheOriginalKept) {
    this->add(1, Side::Buy, 100, px(100));
    this->add(1, Side::Sell, 999, px(50));
    EXPECT_EQ(this->m.counters().duplicate_order, 1U);
    EXPECT_EQ(this->bids(), (Levels{{px(100), 100}}));
    EXPECT_TRUE(this->asks().empty());

    this->add(2, Side::Buy, 100, px(99));
    this->replace(2, 1, 100, px(98));  // the new reference collides with a live order
    EXPECT_EQ(this->m.counters().duplicate_order, 2U);
    EXPECT_EQ(this->bids(), (Levels{{px(100), 100}})) << "the replaced order is gone either way";
    EXPECT_TRUE(this->m.audit().clean());
}

TYPED_TEST(BookScenario, ZeroShareMessagesAreCountedAndCreateNoEmptyLevels) {
    this->add(1, Side::Buy, 0, px(100));
    EXPECT_TRUE(this->bids().empty());
    this->add(2, Side::Buy, 100, px(100));
    this->execute(2, 0);
    this->cancel(2, 0);
    this->replace(2, 3, 0, px(100));
    EXPECT_EQ(this->m.counters().zero_shares, 4U);
    EXPECT_TRUE(this->bids().empty()) << "a replace to zero shares leaves nothing behind";
    EXPECT_EQ(this->m.orders().size(), 0U);
    EXPECT_EQ(this->m.audit().empty_levels, 0U);
    EXPECT_TRUE(this->m.audit().clean());
}

TYPED_TEST(BookScenario, AMessageCarryingTheWrongLocateIsCaughtAsALevelMismatch) {
    // The order rests in kStock's book. An execution that names it but carries
    // another security's locate finds no such level there.
    this->add(1, Side::Buy, 100, px(100), kStock);
    this->execute(1, 100, kOther);
    EXPECT_EQ(this->m.counters().level_mismatch, 1U);
    EXPECT_FALSE(this->m.audit().clean());
}

}  // namespace
