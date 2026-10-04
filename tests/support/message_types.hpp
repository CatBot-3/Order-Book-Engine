#pragma once

#include <cstddef>
#include <type_traits>

#include "obe/feed/messages.hpp"

namespace obe::test {

// Calls f(std::type_identity<M>{}, type_byte, wire_size) once for each of the
// 23 wire message types. Add Order appears twice: 'A' and 'F' share a struct.
template <class F>
void for_each_wire_type(F&& f) {
    using namespace obe::feed;
    f(std::type_identity<SystemEvent>{}, 'S', std::size_t{12});
    f(std::type_identity<StockDirectory>{}, 'R', std::size_t{39});
    f(std::type_identity<TradingAction>{}, 'H', std::size_t{25});
    f(std::type_identity<RegSho>{}, 'Y', std::size_t{20});
    f(std::type_identity<MarketParticipantPosition>{}, 'L', std::size_t{26});
    f(std::type_identity<MwcbDeclineLevel>{}, 'V', std::size_t{35});
    f(std::type_identity<MwcbStatus>{}, 'W', std::size_t{12});
    f(std::type_identity<IpoQuotingPeriod>{}, 'K', std::size_t{28});
    f(std::type_identity<LuldAuctionCollar>{}, 'J', std::size_t{35});
    f(std::type_identity<OperationalHalt>{}, 'h', std::size_t{21});
    f(std::type_identity<AddOrder>{}, 'A', std::size_t{36});
    f(std::type_identity<AddOrder>{}, 'F', std::size_t{40});
    f(std::type_identity<OrderExecuted>{}, 'E', std::size_t{31});
    f(std::type_identity<OrderExecutedWithPrice>{}, 'C', std::size_t{36});
    f(std::type_identity<OrderCancel>{}, 'X', std::size_t{23});
    f(std::type_identity<OrderDelete>{}, 'D', std::size_t{19});
    f(std::type_identity<OrderReplace>{}, 'U', std::size_t{35});
    f(std::type_identity<Trade>{}, 'P', std::size_t{44});
    f(std::type_identity<CrossTrade>{}, 'Q', std::size_t{40});
    f(std::type_identity<BrokenTrade>{}, 'B', std::size_t{19});
    f(std::type_identity<Noii>{}, 'I', std::size_t{50});
    f(std::type_identity<Rpii>{}, 'N', std::size_t{20});
    f(std::type_identity<DirectListingCapitalRaise>{}, 'O', std::size_t{48});
}

}  // namespace obe::test
