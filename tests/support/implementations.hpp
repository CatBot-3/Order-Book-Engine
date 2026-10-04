#pragma once

#include <gtest/gtest.h>

#include "obe/book/implementations.hpp"

// The implementations the book tests run against.
//
// Every suite in tests/book is a typed test over one of these lists, so each
// container is judged by exactly the tests the reference has to pass. The same
// test sources are built twice:
//
//   obe_book_tests           the reference containers. Part of the passing
//                            suite.
//   obe_hand_written_tests   built with OBE_TEST_HAND_WRITTEN defined: the
//                            optimized containers of phase 4. Labelled
//                            needs-your-code; each test fails with the name of
//                            the missing function until that container is
//                            written.
//
// To put a new container under test, add it to the lists below (and, to make
// it selectable with --impl, to obe/book/implementations.hpp).

namespace obe::test {

#if defined(OBE_TEST_HAND_WRITTEN)

using OrderStoreTypes = ::testing::Types<book::FlatOrderStore, book::PooledOrderStore>;

using PriceLevelsTypes = ::testing::Types<book::VectorPriceLevels, book::PooledPriceLevels>;

using BookTypes = ::testing::Types<book::FlatStoreImpl, book::VectorLevelsImpl, book::PooledImpl,
                                   book::FlatVectorImpl>;

#else

using OrderStoreTypes = ::testing::Types<book::OrderStore>;

using PriceLevelsTypes = ::testing::Types<book::PriceLevels>;

using BookTypes = ::testing::Types<book::ReferenceImpl>;

#endif

}  // namespace obe::test
