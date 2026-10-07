#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/gen/rng.hpp"
#include "obe/types.hpp"

// Best-bid-and-offer updates made up from a seed, for the tick store's tests
// and benchmark when there is no feed to take them from.
//
// They have the shape of a market and none of its substance: time moves
// forward in steps of up to 50 microseconds, every tick belongs to one of a
// fixed set of securities picked evenly, and each tick changes one thing, a
// quote moving a cent or one side's size becoming another number of round
// lots. A real feed is burstier, its securities are far from evenly busy, and
// its sizes are not all round lots. A compression ratio measured on this is a
// statement about this generator. `tick_store profile` on a real file says how
// far apart the two are.

namespace obe::gen {

[[nodiscard]] inline std::vector<book::BboUpdate> market_ticks(std::uint64_t seed,
                                                               std::size_t count,
                                                               std::uint32_t securities = 12) {
    SplitMix64 rng(seed);
    std::vector<book::Bbo> quotes(securities);
    for (book::Bbo& q : quotes) {
        const auto mid = static_cast<Price>(rng.between(50'000, 5'000'000));
        q = {mid - 100, 100 * rng.between(1, 20), mid + 100, 100 * rng.between(1, 20)};
    }
    std::vector<book::BboUpdate> out;
    out.reserve(count);
    Nanos now = 34'200ULL * 1'000'000'000ULL;  // 09:30
    for (std::size_t i = 0; i < count; ++i) {
        now += rng.below(50'000);
        const auto s = static_cast<std::uint32_t>(rng.below(securities));
        book::Bbo& q = quotes[s];
        switch (rng.below(6)) {
            case 0:
                q.bid_price += 100;
                q.ask_price += 100;
                break;
            case 1:
                if (q.bid_price > 1'000) {
                    q.bid_price -= 100;
                    q.ask_price -= 100;
                }
                break;
            case 2:
            case 3:
                q.bid_qty = 100 * rng.between(1, 30);
                break;
            default:
                q.ask_qty = 100 * rng.between(1, 30);
                break;
        }
        out.push_back({static_cast<Locate>(s + 1), now, q});
    }
    return out;
}

}  // namespace obe::gen
