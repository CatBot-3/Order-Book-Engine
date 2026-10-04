#pragma once

#include <ostream>

#include "obe/book/types.hpp"

// Readable failure messages. GoogleTest finds these by argument-dependent
// lookup, which is why they live in obe::book.

namespace obe::book {

inline void PrintTo(const Level& level, std::ostream* os) {
    *os << "{price " << level.price << ", qty " << level.qty << "}";
}

inline void PrintTo(const Bbo& bbo, std::ostream* os) {
    *os << "{bid " << bbo.bid_qty << " @ " << bbo.bid_price << ", ask " << bbo.ask_qty << " @ "
        << bbo.ask_price << "}";
}

inline void PrintTo(const BboUpdate& update, std::ostream* os) {
    *os << "{locate " << update.locate << ", t " << update.timestamp << ", ";
    PrintTo(update.bbo, os);
    *os << "}";
}

inline void PrintTo(const OrderRecord& rec, std::ostream* os) {
    *os << "{" << (rec.side == Side::Buy ? "buy " : "sell ") << rec.qty << " @ " << rec.price
        << "}";
}

}  // namespace obe::book
