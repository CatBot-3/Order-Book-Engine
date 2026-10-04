#include "obe/book/implementations.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "obe/book/book_manager.hpp"

// The registry of named implementations, and the optional prefetch hook. None
// of this depends on the hand-written containers being finished.

namespace {

using namespace obe;

TEST(Implementations, NamesAreUniqueAndTheReferenceComesFirst) {
    std::vector<std::string> names;
    book::for_each_implementation([&]<class Impl>(std::type_identity<Impl>) {
        names.emplace_back(Impl::kName);
        EXPECT_FALSE(Impl::kDescription.empty());
    });
    ASSERT_FALSE(names.empty());
    EXPECT_EQ(names.front(), "reference");
    EXPECT_EQ(std::set<std::string>(names.begin(), names.end()).size(), names.size());
}

TEST(Implementations, WithImplementationFindsByExactName) {
    int calls = 0;
    EXPECT_TRUE(book::with_implementation("flat-store", [&]<class Impl>(std::type_identity<Impl>) {
        ++calls;
        EXPECT_TRUE((std::is_same_v<Impl, book::FlatStoreImpl>));
    }));
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(book::with_implementation("flat", [&](auto) { ++calls; }));
    EXPECT_FALSE(book::with_implementation("", [&](auto) { ++calls; }));
    EXPECT_FALSE(book::with_implementation("Reference", [&](auto) { ++calls; }));
    EXPECT_EQ(calls, 1) << "an unknown name must call nothing";
}

struct SizedStore : book::OrderStore {
    SizedStore() = default;
    explicit SizedStore(std::size_t expected) : reserved(expected) {}
    std::size_t reserved = 0;
};

struct UnsizedStore {
    bool insert(OrderId, const book::OrderRecord&) { return true; }
    book::OrderRecord* find(OrderId) { return nullptr; }
    const book::OrderRecord* find(OrderId) const { return nullptr; }
    bool erase(OrderId) { return false; }
    std::size_t size() const { return 0; }
};
static_assert(book::OrderStoreLike<UnsizedStore>);

TEST(Implementations, MakeStorePassesTheReservationOnlyWhenThereIsOne) {
    EXPECT_EQ(book::make_store<SizedStore>(5'000).reserved, 5'000U);
    EXPECT_EQ(book::make_store<SizedStore>(0).reserved, 0U)
        << "0 means no reservation was asked for";
    // A store with no size constructor is simply default-constructed.
    static_cast<void>(book::make_store<UnsizedStore>(5'000));
}

// --- The prefetch hook ---------------------------------------------------------------

struct PrefetchingStore : book::OrderStore {
    void prefetch(OrderId id) const noexcept {
        ++hints;
        last = id;
    }
    mutable int hints = 0;
    mutable OrderId last = 0;
};

TEST(BookManagerPrefetch, IsForwardedToAStoreThatOffersIt) {
    book::BookManager<PrefetchingStore, book::PriceLevels> manager;
    manager.prefetch(42);
    manager.prefetch(43);
    EXPECT_EQ(manager.orders().hints, 2);
    EXPECT_EQ(manager.orders().last, 43U);
}

TEST(BookManagerPrefetch, IsANoOpForAStoreThatDoesNot) {
    book::BookManager<book::OrderStore, book::PriceLevels> manager;
    manager.prefetch(42);  // compiles, and does nothing
    EXPECT_EQ(manager.orders().size(), 0U);
}

}  // namespace
