#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "obe/sim/strategy.hpp"
#include "obe/sim/types.hpp"
#include "obe/sim/volatility.hpp"
#include "obe/types.hpp"

// The Avellaneda-Stoikov market-making strategy: Avellaneda and Stoikov,
// "High-frequency trading in a limit order book" (2008), sections 2 and 3.
//
//   ctest --preset debug -R AvellanedaStoikov
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
// WHAT THE FORMULA MEANS, AND WHERE THIS SIMULATOR BENDS IT
//
//  1. The reservation price.
//     The model's trader maximises expected exponential utility, -exp(-gamma
//     * wealth), which values an uncertain position at its expected value
//     minus gamma / 2 times its variance. Holding q units of a mid that moves
//     like a random walk with variance sigma^2 per second, the position's
//     variance at the horizon is q^2 * sigma^2 * tau. The price at which one
//     more unit (or one fewer) leaves that value unchanged is r = s - q *
//     gamma * sigma^2 * tau. Long (q > 0), each extra unit adds risk, so it
//     is worth less than the mid to this trader: r sits below s, both quotes
//     move down, the ask gets hit more and the bid less, and the inventory
//     drifts back toward zero. The distance is proportional to sigma^2 * tau,
//     not sigma, because a random walk's VARIANCE grows linearly with time,
//     and exponential utility charges for variance.
//
//  2. The two terms of the spread.
//     gamma * sigma^2 * tau is the inventory-risk term: what holding a
//     position for the rest of the session is expected to cost in utility.
//     (2 / gamma) * ln(1 + gamma / k) comes from the fills: the model says a
//     quote delta from the mid is hit at a rate A * exp(-k * delta), and this
//     is the spread that best trades a wider margin against fewer fills. As
//     gamma goes to 0 the risk term vanishes and ln(1 + x) = x - x^2 / 2 + ...
//     gives 2 / k: a risk-neutral trader quotes 1 / k either side, the
//     distance that maximises delta * exp(-k * delta).
//
//  3. As tau goes to zero the inventory term goes with it.
//     The model values what is left at the end at the final mid, with no cost
//     to get rid of it, so at 15:59 it stops leaning against a large position
//     just when a real trader would lean hardest (the position must be closed
//     or carried overnight). An infinite-horizon version keeps the lean
//     constant through the day. max_tau approximates that: tau is capped, so
//     the quotes are the same at 10:00 and at 15:00, and the lean only fades
//     in the last max_tau seconds.
//
//  4. Units.
//     q is a pure number; gamma is per dollar; sigma^2 is dollars squared per
//     second; tau is seconds. q * gamma * sigma^2 * tau is (1/$) * ($^2/s)
//     * s = dollars, added to a per-share price s: dollars per share. q is
//     counted in quotes because in the model one fill moves q by exactly one
//     unit. Counted in shares, one fill would move q by the quote size, so
//     doubling the size would double the lean each fill causes and a gamma
//     tuned at one size would mean twice as much at the other. The price of
//     the convention: ten fills of 200 shares lean as much as ten fills of
//     100, though they are twice the risk.
//
//  5. k is per dollar, and depends on the stock.
//     The paper's k = 1.5 goes with a spread of about two thirds of a
//     dollar either side: a toy stock. For the spread term (about 2 / k) to be
//     one cent, k must be about 200 per dollar, the default here. To estimate
//     it from this simulator: run FixedSpread at several distances d, count
//     fills per second at each, and fit ln(rate) = ln(A) - k * d; the slope
//     is -k.
//
//  6. The model's fills are independent of the next move. These are not.
//     The model has fills arrive at random, unrelated to where the mid goes
//     next. The simulator fills a quote in three ways (sim/types.hpp,
//     FillReason): the orders ahead of it were executed, a worse price than
//     it traded, or an arriving order reached it. All three happen when the
//     market is moving into the quote: the bid gets filled as the price
//     falls through it. So fills are adversely selected, the markouts after a
//     fill should be negative on average, and the profit the model promises
//     (half the spread on every fill) is an upper bound the simulator should
//     not reach. That gap is the most interesting number mm_sim prints.
//
//  7. Rounding outward.
//     The bid rounds down and the ask up, so rounding never quotes tighter
//     than the model asked: a quote rounded toward the mid would be one the
//     model considers too close to be worth having. When the model's spread
//     is narrower than a tick, the rounded quotes can reach or cross the
//     other side of the market; step 8 then puts them one tick inside it.
//     That clamp overrides the model, which assumes a continuous price and a
//     quote that never takes liquidity: prices are discrete and this
//     strategy only provides liquidity.
//
//  8. The hard inventory limit.
//     The lean through r is soft: it is gamma * sigma^2 * tau per unit, which
//     is small in a quiet market or with a small gamma, and the adverse
//     selection of point 6 pushes inventory in the direction of the trend.
//     The limit bounds the worst case whatever the parameters. With a large
//     gamma the lean keeps inventory near zero and the limit is never
//     reached: the model is doing the risk management. With gamma near zero
//     there is no lean, inventory wanders with the trend and the limit is
//     reached all day: then the limit, not the model, is the strategy.

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

    // See the contract above; the step numbers refer to it.
    Quotes quote(const MarketView& view) {
        Quotes out;
        if (!view.two_sided()) {  // 1
            return out;
        }
        const double s = mid_dollars(view.mid2());  // 2
        variance_.observe(view.now, s);
        const double sigma2 = params_.sigma2 > 0.0 ? params_.sigma2 : variance_.value();  // 3
        double tau = 0.0;                                                                 // 4
        if (view.end > view.now) {
            tau = std::min(
                static_cast<double>(view.end - view.now) / static_cast<double>(kNanosPerSecond),
                params_.max_tau);
        }
        const double q =
            static_cast<double>(view.inventory) / static_cast<double>(params_.qty);  // 5

        const double risk = params_.gamma * sigma2 * tau;  // 6
        const double r = s - q * risk;
        const double spread = risk + (2.0 / params_.gamma) * std::log1p(params_.gamma / params_.k);

        Price bid = floor_to_tick(floor_price(r - spread / 2.0), view.tick);  // 7
        Price ask = ceil_to_tick(ceil_price(r + spread / 2.0), view.tick);
        bid = std::min(bid, passive_bid_limit(view));  // 8
        ask = std::max(ask, passive_ask_limit(view));

        if (view.inventory < params_.max_inventory && bid > 0) {  // 9, 10
            out.bid = {bid, params_.qty};
        }
        if (view.inventory > -params_.max_inventory) {
            out.ask = {ask, params_.qty};
        }
        return out;
    }

    [[nodiscard]] const AsParams& params() const noexcept { return params_; }
    [[nodiscard]] const EwmaVariance& variance() const noexcept { return variance_; }

 private:
    AsParams params_;
    EwmaVariance variance_;
};

static_assert(Strategy<AvellanedaStoikov>);

}  // namespace obe::sim
