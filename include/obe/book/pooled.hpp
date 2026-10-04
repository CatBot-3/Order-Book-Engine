#pragma once

#include <cstdint>
#include <utility>

#include "obe/book/concepts.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/book/types.hpp"
#include "obe/types.hpp"
#include "obe/util/pool_allocator.hpp"

// The reference containers with their nodes taken from pools (phase 4,
// experiment 4).
//
// Nothing about the containers changes: the same std::unordered_map and
// std::map, the same code in order_store.hpp and price_levels.hpp. Only the
// source of node memory differs. That makes this the cleanest measurement of
// what the general-purpose allocator was costing the reference, separate from
// what its pointer-chasing layout costs. They work once util::Pool is written.

namespace obe::book {

using PooledOrderStore =
    BasicOrderStore<util::PoolAllocator<std::pair<const OrderId, OrderRecord>>>;

using PooledPriceLevels =
    BasicPriceLevels<util::PoolAllocator<std::pair<const Price, std::uint64_t>>>;

static_assert(OrderStoreLike<PooledOrderStore>);
static_assert(PriceLevelsLike<PooledPriceLevels>);

}  // namespace obe::book
