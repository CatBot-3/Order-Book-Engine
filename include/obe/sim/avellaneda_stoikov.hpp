#pragma once

#include <cstdint>

#include "obe/sim/strategy.hpp"
#include "obe/sim/types.hpp"
#include "obe/sim/volatility.hpp"
#include "obe/types.hpp"
#include "obe/util/todo.hpp"

// The Avellaneda-Stoikov market-making strategy.
//
// THIS ONE IS YOURS TO WRITE. The simulator, the fill model, the volatility
// estimate and the simple strategies in strategy.hpp are done; quote() below
// throws until you fill it in. It is about twenty lines. The paper is
// Avellaneda and Stoikov, "High-frequency trading in a limit order book"
// (2008); read sections 2 and 3 before writing anything.
//
//   ctest --preset debug -L needs-your-code -R AvellanedaStoikov
//   build/release/apps/mm_sim <file> --symbol <S> --strategy as
//
// THE CONTRACT (the tests check exactly this)
//
//   1. No quotes at all unless view.two_sided().
//   2. s, the mid in dollars: mid_dollars(view.mid2()). Give it to the
//      estimator on every call: variance_.observe(view.now, s).
//   3. sigma^2, dollars squared per second: params.sigma2 if that is greater
//      than zero, otherwise variance_.value().
//   4. tau, the time left in seconds: from view.now to view.end, but never
//      more than params.max_tau, and zero once view.end has passed.
//   5. q, the inventory in units of one quote: view.inventory / params.qty,
//      as a real number.
//   6. The reservation price and the total spread:
//
//          r      = s - q * gamma * sigma^2 * tau
//          spread = gamma * sigma^2 * tau + (2 / gamma) * ln(1 + gamma / k)
//
//   7. bid = floor_to_tick(floor_price(r - spread / 2), view.tick)
//      ask = ceil_to_tick(ceil_price(r + spread / 2), view.tick)
//   8. Quotes are passive: bid = min(bid, passive_bid_limit(view)),
//      ask = max(ask, passive_ask_limit(view)).
//   9. No bid when view.inventory >= params.max_inventory or the bid price
//      came out as 0. No ask when view.inventory <= -params.max_inventory.
//  10. Each quote is for params.qty shares.
//
// QUESTIONS TO BE ABLE TO ANSWER BEFORE YOU CALL IT DONE
//
//  1. Where the reservation price comes from.
//     It is the price at which you would be indifferent between holding your
//     inventory and holding one unit more or less. Why does it sit below the
//     mid when you are long? Why is the distance proportional to sigma^2 and
//     to tau, and not to sigma?
//
//  2. The two terms of the spread.
//     One is about the risk of holding inventory and one is about how often
//     quotes get hit. Which is which? Which of them survives when gamma goes
//     to zero, and what is its limit? (Expand ln(1 + x) for small x.)
//
//  3. What happens as tau goes to zero.
//     The inventory term disappears: near the end of the session the model
//     stops caring about its position. Is that what you would want at 15:59
//     with a large position? What would an infinite-horizon version change,
//     and how does max_tau let you approximate one?
//
//  4. Units.
//     Check that q * gamma * sigma^2 * tau comes out in dollars per share.
//     Why is q counted in quotes and not in shares? What would happen to the
//     meaning of gamma if you doubled the quote size otherwise?
//
//  5. What k is, and why 1.5 is not it.
//     The paper models the chance of a quote at distance d from the mid being
//     hit as proportional to exp(-k * d), and uses k = 1.5 with a spread of
//     about a dollar and a half. For a stock that trades a cent wide, what
//     order of magnitude must k have for the spread term to be about a cent?
//     How would you estimate k from this simulator's own output?
//
//  6. What the model assumes, and what this simulator does to it.
//     The model's mid is a random walk with no drift and no jumps, and its
//     fills arrive at random, independent of where the mid goes next. Look at
//     the three ways a quote is filled in simulator.hpp. Are those fills
//     independent of the next move of the mid? What does that predict for the
//     markout figures, and for the profit the model promises?
//
//  7. Rounding.
//     Why does the bid round down and the ask up? What does the strategy do
//     when the model's spread is narrower than one tick, and is the clamp in
//     step 8 then doing the model's job or overriding it?
//
//  8. The inventory limit.
//     The model already leans against inventory through r. Why have a hard
//     limit as well? Find parameters for which the limit is never reached and
//     parameters for which it always is, and say what each tells you.

namespace obe::sim {

struct AsParams {
    // Risk aversion. Larger means the quotes lean harder against inventory
    // and stand further apart. Per dollar.
    double gamma = 0.001;
    // How quickly the chance of being hit falls as a quote moves away from
    // the mid. Per dollar.
    double k = 200.0;
    Qty qty = 100;                       // shares per quote, and the unit of q
    std::int64_t max_inventory = 1'000;  // shares
    // Variance of the mid in dollars squared per second. Zero means "estimate
    // it from the mids seen", which is what a run on real data wants; a fixed
    // value is for tests and for experiments that hold it constant.
    double sigma2 = 0.0;
    Nanos vol_half_life = 60 * kNanosPerSecond;
    // The longest horizon the formula is given, in seconds. The default is a
    // full 6.5-hour session.
    double max_tau = 23'400.0;
};

class AvellanedaStoikov {
 public:
    explicit AvellanedaStoikov(const AsParams& params = {})
        : params_(params), variance_(params.vol_half_life) {}

    // See the contract above.
    Quotes quote(const MarketView& view) {
        util::todo("AvellanedaStoikov::quote", view, variance_);
    }

    [[nodiscard]] const AsParams& params() const noexcept { return params_; }
    [[nodiscard]] const EwmaVariance& variance() const noexcept { return variance_; }

 private:
    AsParams params_;
    EwmaVariance variance_;
};

static_assert(Strategy<AvellanedaStoikov>);

}  // namespace obe::sim
