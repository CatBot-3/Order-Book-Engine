#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "obe/engine/concepts.hpp"
#include "obe/engine/feed_writer.hpp"
#include "obe/gen/order_flow.hpp"
#include "support/engine_harness.hpp"

// An engine driven by seeded random flow, with everything it says kept.
//
//   requests   gen::OrderFlow, which also listens to the reports to learn
//              which orders are resting
//   reports    kept in `log`
//   market     kept twice: decoded in `messages`, and as the bytes a file
//              would hold in `writer`

namespace obe::test {

template <class Impl>
struct FlowRun {
    using Reports = engine::TeeReports<gen::OrderFlow, ReportLog>;
    using MarketData = engine::TeeMarketData<MdLog, engine::ItchFeedWriter>;
    using Engine = typename Impl::template Engine<Reports, MarketData>;

    explicit FlowRun(const gen::FlowConfig& cfg)
        : flow(cfg),
          reports(flow, log),
          market_data(messages, writer),
          engine(std::make_unique<Engine>(reports, market_data)) {
        flow.open(*engine);
    }

    // One request. Returns it, so a failure can say what was asked.
    gen::Command step() {
        const gen::Command command = flow.next();
        gen::apply(*engine, command);
        return command;
    }

    void run(std::uint64_t commands) {
        for (std::uint64_t i = 0; i < commands; ++i) {
            step();
        }
    }

    gen::OrderFlow flow;
    ReportLog log;
    MdLog messages;
    engine::ItchFeedWriter writer;
    Reports reports;
    MarketData market_data;
    std::unique_ptr<Engine> engine;
};

// A small, busy market: few symbols and few resting orders, so that orders
// meet each other often, and a high share of deliberately bad requests.
[[nodiscard]] inline gen::FlowConfig busy_config(std::uint64_t seed) {
    return gen::FlowConfig{.seed = seed,
                           .symbols = 3,
                           .owners = 4,
                           .target_live_orders = 120,
                           .bad_per_million = 30'000};
}

// A handful of resting orders: books empty out and refill constantly, market
// orders run out of liquidity, fill-or-kill orders are killed.
[[nodiscard]] inline gen::FlowConfig thin_config(std::uint64_t seed) {
    return gen::FlowConfig{.seed = seed,
                           .symbols = 2,
                           .owners = 3,
                           .target_live_orders = 4,
                           .bad_per_million = 50'000};
}

// The flows the property tests run over. Between them they take every path
// through the engine; EngineProperty.TheFlowsReachEveryPath checks that claim.
[[nodiscard]] inline std::vector<gen::FlowConfig> property_configs() {
    return {busy_config(1), busy_config(2),   busy_config(3),
            busy_config(4), thin_config(101), thin_config(102)};
}

[[nodiscard]] inline std::string describe(const gen::FlowConfig& cfg) {
    return "seed " + std::to_string(cfg.seed) + ", " + std::to_string(cfg.symbols) +
           " symbols, target " + std::to_string(cfg.target_live_orders) + " resting orders";
}

}  // namespace obe::test
