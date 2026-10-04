#pragma once

#include <string_view>
#include <type_traits>

#include "obe/engine/concepts.hpp"
#include "obe/engine/matching_engine.hpp"
#include "obe/engine/reference_engine.hpp"

// The named matching engines.
//
// flow_gen, engine_bench and the tests select one by name (--engine NAME), the
// same way the book implementations are selected with --impl. An engine is a
// class template over its two sinks, so each entry names a template.

namespace obe::engine {

struct ReferenceEngineImpl {
    static constexpr std::string_view kName = "reference";
    static constexpr std::string_view kDescription =
        "std::map levels, std::list queues, std::unordered_map index";
    template <ReportSink R, MarketDataSink M>
    using Engine = ReferenceEngine<R, M>;
};

struct PooledEngineImpl {
    static constexpr std::string_view kName = "pooled";
    static constexpr std::string_view kDescription =
        "MatchingEngine: intrusive queues of pooled orders";
    template <ReportSink R, MarketDataSink M>
    using Engine = MatchingEngine<R, M>;
};

static_assert(EngineLike<ReferenceEngine<NullReports, NullMarketData>>);
static_assert(EngineLike<MatchingEngine<NullReports, NullMarketData>>);

// Calls f(std::type_identity<Impl>{}) once for every engine, the reference
// first.
template <class F>
void for_each_engine(F&& f) {
    f(std::type_identity<ReferenceEngineImpl>{});
    f(std::type_identity<PooledEngineImpl>{});
}

// Calls f(std::type_identity<Impl>{}) for the engine called `name`. Returns
// false, calling nothing, if there is no such engine.
template <class F>
bool with_engine(std::string_view name, F&& f) {
    bool found = false;
    for_each_engine([&]<class Impl>(std::type_identity<Impl> tag) {
        if (!found && Impl::kName == name) {
            found = true;
            f(tag);
        }
    });
    return found;
}

}  // namespace obe::engine
