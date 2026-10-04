#pragma once

#include <concepts>
#include <cstddef>
#include <string_view>
#include <type_traits>

#include "obe/book/flat_order_store.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/pooled.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/book/vector_price_levels.hpp"

// The named book implementations.
//
// A book is an order store paired with a price-level container. Each pairing
// has a name, and book_replay, replay_bench and the tests select one by that
// name (--impl NAME). Adding a pairing here is all it takes to make it
// replayable, benchmarkable and covered by the differential check.
//
// Each experiment changes one thing against the reference, so its effect can
// be read off a single comparison:
//
//   reference       std::unordered_map + std::map            the baseline
//   flat-store      FlatOrderStore     + std::map            experiment 1
//   vector-levels   std::unordered_map + VectorPriceLevels   experiment 3
//   pooled          the reference containers on pools        experiment 4
//   flat-vector     FlatOrderStore     + VectorPriceLevels   the two together

namespace obe::book {

struct ReferenceImpl {
    static constexpr std::string_view kName = "reference";
    static constexpr std::string_view kDescription = "std::unordered_map orders, std::map levels";
    using Store = OrderStore;
    using Levels = PriceLevels;
};

struct FlatStoreImpl {
    static constexpr std::string_view kName = "flat-store";
    static constexpr std::string_view kDescription = "FlatOrderStore orders, std::map levels";
    using Store = FlatOrderStore;
    using Levels = PriceLevels;
};

struct VectorLevelsImpl {
    static constexpr std::string_view kName = "vector-levels";
    static constexpr std::string_view kDescription =
        "std::unordered_map orders, VectorPriceLevels levels";
    using Store = OrderStore;
    using Levels = VectorPriceLevels;
};

struct PooledImpl {
    static constexpr std::string_view kName = "pooled";
    static constexpr std::string_view kDescription =
        "std::unordered_map orders and std::map levels, nodes from util::Pool";
    using Store = PooledOrderStore;
    using Levels = PooledPriceLevels;
};

struct FlatVectorImpl {
    static constexpr std::string_view kName = "flat-vector";
    static constexpr std::string_view kDescription =
        "FlatOrderStore orders, VectorPriceLevels levels";
    using Store = FlatOrderStore;
    using Levels = VectorPriceLevels;
};

// Calls f(std::type_identity<Impl>{}) once for every implementation, the
// reference first.
template <class F>
void for_each_implementation(F&& f) {
    f(std::type_identity<ReferenceImpl>{});
    f(std::type_identity<FlatStoreImpl>{});
    f(std::type_identity<VectorLevelsImpl>{});
    f(std::type_identity<PooledImpl>{});
    f(std::type_identity<FlatVectorImpl>{});
}

// Calls f(std::type_identity<Impl>{}) for the implementation called `name`.
// Returns false, calling nothing, if there is no such implementation.
template <class F>
bool with_implementation(std::string_view name, F&& f) {
    bool found = false;
    for_each_implementation([&]<class Impl>(std::type_identity<Impl> tag) {
        if (!found && Impl::kName == name) {
            found = true;
            f(tag);
        }
    });
    return found;
}

// A store pre-sized for `expected_orders` live orders, if the store type can
// be constructed from a size and a size was given; otherwise a default one.
// feed_profile reports the number to pass.
template <OrderStoreLike Store>
[[nodiscard]] Store make_store(std::size_t expected_orders) {
    if constexpr (std::constructible_from<Store, std::size_t>) {
        if (expected_orders != 0) {
            return Store(expected_orders);
        }
    }
    return Store{};
}

}  // namespace obe::book
