#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "obe/feed/codec.hpp"
#include "obe/feed/messages.hpp"
#include "obe/gen/rng.hpp"
#include "obe/types.hpp"

// A seeded generator of well-formed ITCH streams.
//
// What it is for: test fixtures, CI and smoke-testing the benchmark harness
// without redistributing Nasdaq data. Every message is consistent with the ones
// before it (executions and cancels refer to live orders and never exceed what
// is left), so a correct book replays the stream with every invariant counter
// at zero.
//
// What it is not: a market model. Executions pick a random resting order, not
// the best-priced one, and the two sides can cross. Numbers measured on this
// stream say nothing about real data. Where the shape of the book matters, use
// flow_gen: it drives the matching engine with obe/gen/order_flow.hpp, so its
// trades follow price-time priority and its books are never crossed. This
// generator remains the one that emits every ITCH message type.
//
// The same config gives the same bytes on every platform and standard library:
// the generator uses its own PRNG and no std distributions.

namespace obe::gen {

struct SyntheticConfig {
    std::uint64_t seed = 1;
    std::uint32_t symbols = 16;        // 1 to 60000
    std::uint64_t messages = 100'000;  // messages after the opening preamble
    std::uint32_t target_live_orders = 2'000;
    bool admin_noise = true;  // sprinkle in the non-book message types
};

namespace detail {

struct LiveOrder {
    OrderId ref;
    Locate locate;
    Side side;
    Price price;
    Qty shares;
};

class Generator {
 public:
    explicit Generator(const SyntheticConfig& cfg) : cfg_(cfg), rng_(cfg.seed) {
        if (cfg_.symbols == 0) {
            cfg_.symbols = 1;
        }
        if (cfg_.symbols > 60'000) {
            cfg_.symbols = 60'000;
        }
        if (cfg_.target_live_orders == 0) {
            cfg_.target_live_orders = 1;
        }
    }

    std::vector<std::byte> run() {
        out_.reserve(static_cast<std::size_t>(cfg_.messages) * 34 +
                     static_cast<std::size_t>(cfg_.symbols) * 80 + 256);
        preamble();
        for (std::uint64_t i = 0; i < cfg_.messages; ++i) {
            step();
        }
        system_event('M');
        system_event('E');
        system_event('C');
        return std::move(out_);
    }

 private:
    static constexpr Price kTick = 100;  // one cent

    feed::Header header(Locate locate) {
        now_ += rng_.below(40'000);  // up to 40 microseconds between messages
        return {.locate = locate, .tracking = 0, .timestamp = now_};
    }

    feed::Symbol symbol_of(Locate locate) const {
        std::array<char, 9> name{};
        std::snprintf(name.data(), name.size(), "S%05u", static_cast<unsigned>(locate));
        return feed::Symbol::from(name.data());
    }

    void system_event(char code) {
        feed::append_framed(out_, feed::SystemEvent{.hdr = header(0), .event_code = code});
    }

    void preamble() {
        system_event('O');
        mids_.resize(cfg_.symbols + 1U);
        for (std::uint32_t i = 1; i <= cfg_.symbols; ++i) {
            const auto locate = static_cast<Locate>(i);
            mids_[i] = static_cast<Price>(rng_.between(500, 50'000)) * kTick;  // $5 to $500
            feed::StockDirectory dir{.hdr = header(locate), .stock = symbol_of(locate)};
            dir.market_category = 'Q';
            dir.financial_status = 'N';
            dir.round_lot_size = 100;
            dir.round_lots_only = 'N';
            dir.issue_classification = 'C';
            dir.issue_subtype = {'Z', ' '};
            dir.authenticity = 'P';
            dir.short_sale_threshold = 'N';
            dir.ipo_flag = 'N';
            dir.luld_tier = '1';
            dir.etp_flag = 'N';
            dir.inverse_indicator = 'N';
            feed::append_framed(out_, dir);
            feed::TradingAction action{.hdr = header(locate), .stock = symbol_of(locate)};
            action.trading_state = 'T';
            feed::append_framed(out_, action);
        }
        system_event('S');
        system_event('Q');
    }

    Locate random_locate() { return static_cast<Locate>(rng_.between(1, cfg_.symbols)); }

    Price price_near(Locate locate, Side side) {
        // The mid takes a one-tick step now and then, so old orders drift away
        // from the touch and the two sides occasionally cross.
        Price& mid = mids_[locate];
        const std::uint64_t roll = rng_.below(16);
        if (roll == 0 && mid > 40 * kTick) {
            mid -= kTick;
        } else if (roll == 1) {
            mid += kTick;
        }
        const auto offset = static_cast<Price>(rng_.between(1, 20)) * kTick;
        return side == Side::Buy ? mid - offset : mid + offset;
    }

    Qty random_shares() { return static_cast<Qty>(rng_.between(1, 20)) * 50; }

    void remove_live(std::size_t index) {
        live_[index] = live_.back();
        live_.pop_back();
    }

    void add() {
        const Locate locate = random_locate();
        const Side side = rng_.below(2) == 0 ? Side::Buy : Side::Sell;
        const LiveOrder order{.ref = next_ref_++,
                              .locate = locate,
                              .side = side,
                              .price = price_near(locate, side),
                              .shares = random_shares()};
        feed::AddOrder msg{.hdr = header(locate),
                           .order_ref = order.ref,
                           .side = order.side,
                           .shares = order.shares,
                           .stock = symbol_of(locate),
                           .price = order.price};
        if (rng_.below(8) == 0) {
            msg.attributed = true;
            msg.mpid = {'M', 'P', 'I', 'D'};
        }
        feed::append_framed(out_, msg);
        live_.push_back(order);
    }

    void execute(bool with_price) {
        const std::size_t index = rng_.below(live_.size());
        LiveOrder& order = live_[index];
        const Qty shares =
            rng_.below(2) == 0 ? order.shares : static_cast<Qty>(rng_.between(1, order.shares));
        if (with_price) {
            feed::OrderExecutedWithPrice msg{.hdr = header(order.locate),
                                             .order_ref = order.ref,
                                             .shares = shares,
                                             .match_number = next_match_++};
            msg.printable = rng_.below(4) == 0 ? 'N' : 'Y';
            msg.price = order.price;
            feed::append_framed(out_, msg);
        } else {
            feed::append_framed(out_, feed::OrderExecuted{.hdr = header(order.locate),
                                                          .order_ref = order.ref,
                                                          .shares = shares,
                                                          .match_number = next_match_++});
        }
        order.shares -= shares;
        if (order.shares == 0) {
            remove_live(index);
        }
    }

    void remove() {
        const std::size_t index = rng_.below(live_.size());
        const LiveOrder order = live_[index];
        feed::append_framed(out_,
                            feed::OrderDelete{.hdr = header(order.locate), .order_ref = order.ref});
        remove_live(index);
    }

    void cancel() {
        const std::size_t index = rng_.below(live_.size());
        LiveOrder& order = live_[index];
        if (order.shares < 2) {
            remove();
            return;
        }
        const auto shares = static_cast<Qty>(rng_.between(1, order.shares - 1));
        feed::append_framed(
            out_, feed::OrderCancel{
                      .hdr = header(order.locate), .order_ref = order.ref, .shares = shares});
        order.shares -= shares;
    }

    void replace() {
        const std::size_t index = rng_.below(live_.size());
        LiveOrder& order = live_[index];
        const OrderId new_ref = next_ref_++;
        const Qty shares = random_shares();
        // Half of the replaces keep the price and change only the size.
        const Price price = rng_.below(2) == 0 ? order.price : price_near(order.locate, order.side);
        feed::append_framed(out_, feed::OrderReplace{.hdr = header(order.locate),
                                                     .orig_order_ref = order.ref,
                                                     .new_order_ref = new_ref,
                                                     .shares = shares,
                                                     .price = price});
        order.ref = new_ref;
        order.shares = shares;
        order.price = price;
    }

    void hidden_trade() {
        const Locate locate = random_locate();
        feed::Trade msg{.hdr = header(locate)};
        msg.order_ref = 0;
        msg.side = Side::Buy;
        msg.shares = random_shares();
        msg.stock = symbol_of(locate);
        msg.price = mids_[locate];
        msg.match_number = next_match_++;
        feed::append_framed(out_, msg);
    }

    // One of the thirteen message types that never touch the book.
    void noise() {
        const Locate locate = random_locate();
        const feed::Symbol stock = symbol_of(locate);
        switch (rng_.below(13)) {
            case 0: {
                feed::CrossTrade msg{.hdr = header(locate)};
                msg.shares = rng_.between(100, 1'000'000);
                msg.stock = stock;
                msg.cross_price = mids_[locate];
                msg.match_number = next_match_++;
                msg.cross_type = 'O';
                feed::append_framed(out_, msg);
                break;
            }
            case 1:
                feed::append_framed(out_, feed::BrokenTrade{.hdr = header(locate),
                                                            .match_number = next_match_ - 1});
                break;
            case 2: {
                feed::Noii msg{.hdr = header(locate)};
                msg.paired_shares = rng_.between(0, 500'000);
                msg.imbalance_shares = rng_.between(0, 50'000);
                msg.imbalance_direction = 'B';
                msg.stock = stock;
                msg.far_price = mids_[locate];
                msg.near_price = mids_[locate];
                msg.reference_price = mids_[locate];
                msg.cross_type = 'C';
                msg.price_variation = 'L';
                feed::append_framed(out_, msg);
                break;
            }
            case 3: {
                feed::Rpii msg{.hdr = header(locate), .stock = stock};
                msg.interest_flag = 'B';
                feed::append_framed(out_, msg);
                break;
            }
            case 4: {
                feed::TradingAction msg{.hdr = header(locate), .stock = stock};
                msg.trading_state = 'T';
                feed::append_framed(out_, msg);
                break;
            }
            case 5: {
                feed::RegSho msg{.hdr = header(locate), .stock = stock};
                msg.action = '0';
                feed::append_framed(out_, msg);
                break;
            }
            case 6: {
                feed::MarketParticipantPosition msg{.hdr = header(locate)};
                msg.mpid = {'M', 'P', 'I', 'D'};
                msg.stock = stock;
                msg.primary_market_maker = 'Y';
                msg.market_maker_mode = 'N';
                msg.participant_state = 'A';
                feed::append_framed(out_, msg);
                break;
            }
            case 7: {
                feed::MwcbDeclineLevel msg{.hdr = header(0)};
                msg.level1 = 2'500 * kPrice8Scale;
                msg.level2 = 2'300 * kPrice8Scale;
                msg.level3 = 2'000 * kPrice8Scale;
                feed::append_framed(out_, msg);
                break;
            }
            case 8:
                feed::append_framed(out_,
                                    feed::MwcbStatus{.hdr = header(0), .breached_level = '1'});
                break;
            case 9: {
                feed::IpoQuotingPeriod msg{.hdr = header(locate), .stock = stock};
                msg.release_time = 34'200;
                msg.release_qualifier = 'A';
                msg.ipo_price = mids_[locate];
                feed::append_framed(out_, msg);
                break;
            }
            case 10: {
                feed::LuldAuctionCollar msg{.hdr = header(locate), .stock = stock};
                msg.reference_price = mids_[locate];
                msg.upper_price = mids_[locate] + 10 * kTick;
                msg.lower_price = mids_[locate] - 10 * kTick;
                msg.extension = 1;
                feed::append_framed(out_, msg);
                break;
            }
            case 11: {
                feed::OperationalHalt msg{.hdr = header(locate), .stock = stock};
                msg.market_code = 'Q';
                msg.action = 'T';
                feed::append_framed(out_, msg);
                break;
            }
            default: {
                feed::DirectListingCapitalRaise msg{.hdr = header(locate), .stock = stock};
                msg.open_eligibility = 'N';
                msg.min_allowable_price = mids_[locate] - 10 * kTick;
                msg.max_allowable_price = mids_[locate] + 10 * kTick;
                msg.near_execution_price = mids_[locate];
                msg.near_execution_time = now_;
                msg.lower_collar = mids_[locate] - 20 * kTick;
                msg.upper_collar = mids_[locate] + 20 * kTick;
                feed::append_framed(out_, msg);
                break;
            }
        }
    }

    void step() {
        // Below the target the generator mostly adds; above it, removals win.
        // The live-order count therefore hovers around the target.
        const bool room = live_.size() < cfg_.target_live_orders;
        if (live_.empty()) {
            add();
            return;
        }
        const std::uint64_t roll = rng_.below(100);
        const std::uint64_t add_share = room ? 55 : 30;
        if (roll < add_share) {
            add();
        } else if (roll < add_share + 12) {
            replace();
        } else if (roll < add_share + 20) {
            execute(false);
        } else if (roll < add_share + 23) {
            execute(true);
        } else if (roll < add_share + 29) {
            cancel();
        } else if (roll < 96) {
            remove();
        } else if (roll < 98 || !cfg_.admin_noise) {
            hidden_trade();
        } else {
            noise();
        }
    }

    SyntheticConfig cfg_;
    SplitMix64 rng_;
    std::vector<std::byte> out_;
    std::vector<LiveOrder> live_;
    std::vector<Price> mids_;
    Nanos now_ = 4ULL * 3600 * 1'000'000'000ULL;  // 04:00:00, when Nasdaq's day starts
    OrderId next_ref_ = 1;
    std::uint64_t next_match_ = 1;
};

}  // namespace detail

// A complete framed stream: start-of-day events, one Stock Directory per
// symbol, `messages` messages of order flow, end-of-day events.
[[nodiscard]] inline std::vector<std::byte> make_synthetic_feed(const SyntheticConfig& cfg) {
    return detail::Generator(cfg).run();
}

}  // namespace obe::gen
