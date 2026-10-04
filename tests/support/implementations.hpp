#pragma once

#include <gtest/gtest.h>

#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"

// The implementations the book tests run against.
//
// Every suite in tests/book is a typed test over one of these lists, so a new
// container is judged by exactly the tests the reference had to pass. When you
// write an optimized order store or price-level container in phase 4, include
// its header here and add it to the lists. Nothing else in the tests changes.

namespace obe::test {

using OrderStoreTypes = ::testing::Types<book::OrderStore>;

using PriceLevelsTypes = ::testing::Types<book::PriceLevels>;

// A full book is an order store paired with a price-level container.
template <class StoreT, class LevelsT>
struct BookImpl {
    using Store = StoreT;
    using Levels = LevelsT;
};

using BookTypes = ::testing::Types<BookImpl<book::OrderStore, book::PriceLevels>>;

}  // namespace obe::test
