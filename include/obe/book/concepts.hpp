#pragma once

#include <concepts>
#include <cstddef>
#include <optional>

#include "obe/book/types.hpp"
#include "obe/types.hpp"

// The contracts the book logic relies on.
//
// BookManager is a template over an order store and a price-level container.
// The reference versions (std::unordered_map, std::map) and every optimized
// replacement in phase 4 satisfy the same two concepts, so the book logic, the
// scenario tests and the differential test are written once and reused for
// each implementation. Swapping a container is then a one-line change, and
// "the optimized book equals the reference book" is something the build can
// check.

namespace obe::book {

// Maps an order reference number to its record.
//
//   insert(id, rec)  Adds the order. Returns false, changing nothing, if `id`
//                    is already present.
//   find(id)         Pointer to the stored record, or nullptr. The record may
//                    be modified through the pointer. The pointer is valid
//                    only until the next insert or erase on the store.
//   erase(id)        Removes the order. Returns false if it was not present.
//   size()           Number of orders currently stored.
template <class S>
concept OrderStoreLike =
    std::default_initializable<S> &&
    requires(S store, const S const_store, OrderId id, const OrderRecord& rec) {
        { store.insert(id, rec) } -> std::same_as<bool>;
        { store.find(id) } -> std::same_as<OrderRecord*>;
        { const_store.find(id) } -> std::same_as<const OrderRecord*>;
        { store.erase(id) } -> std::same_as<bool>;
        { const_store.size() } -> std::convertible_to<std::size_t>;
    };

// One side of one book: total resting shares at each price.
//
//   L(side)            An empty side. `side` decides which end is "best":
//                      highest price for Buy, lowest for Sell.
//   add(price, qty)    Adds qty shares at price, creating the level if needed.
//                      Precondition: qty > 0.
//   remove(price, qty) Takes qty shares off the level and erases the level if
//                      that empties it. Returns false, changing nothing, if
//                      there is no level at price or it holds fewer than qty
//                      shares. Precondition: qty > 0.
//   best()             The best level, or nullopt if the side is empty.
//   empty(), size()    size() is the number of levels.
//   for_each(f)        Calls f(const Level&) from best to worst. f returns
//                      bool; returning false stops the walk.
namespace detail {
// A visitor for the concept to call for_each with. A named type instead of a
// lambda: lambdas inside a requires-expression need a newer Clang than the
// rest of this project does.
struct AnyLevelVisitor {
    bool operator()(const Level&) const noexcept { return true; }
};
}  // namespace detail

template <class L>
concept PriceLevelsLike = std::constructible_from<L, Side> &&
                          requires(L levels, const L const_levels, Price price, Qty qty) {
                              { levels.add(price, qty) } -> std::same_as<void>;
                              { levels.remove(price, qty) } -> std::same_as<bool>;
                              { const_levels.best() } -> std::same_as<std::optional<Level>>;
                              { const_levels.empty() } -> std::same_as<bool>;
                              { const_levels.size() } -> std::convertible_to<std::size_t>;
                              const_levels.for_each(detail::AnyLevelVisitor{});
                          };

// Receives best-bid-and-offer changes. Resolved at compile time, like the
// feed handler.
template <class T>
concept BboListener = requires(T listener, const BboUpdate& update) { listener.on_bbo(update); };

struct NullBboListener {
    void on_bbo(const BboUpdate&) noexcept {}
};

}  // namespace obe::book
