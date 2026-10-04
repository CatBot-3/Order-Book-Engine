#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

#include "obe/feed/handler.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"

// What travels from the parser thread to the book thread.
//
// The parser thread decodes each ITCH message that can change a book into a
// BookEvent and pushes it through a queue. The book thread pops it and
// applies it. The event is the decoded message, flattened into one fixed-size
// record of plain integers:
//
//   - fixed size, so the queue is an array of them with no allocation;
//   - trivially copyable, so pushing is a few moves and nothing is shared
//     between the threads afterwards;
//   - 40 bytes, so three fit in two cache lines.
//
// Messages that never touch the book (trades, auction data, most
// administrative types) are dropped by the parser thread. Sending them across
// would spend queue capacity on something the next stage ignores.
//
// The alternative design is to push a pointer to the undecoded message and
// decode on the book thread. The parser thread would then do almost nothing,
// and the pipeline would be two stages in practice. Decoding here is what
// gives the first stage real work to take off the second.

namespace obe::pipeline {

struct BookEvent {
    // For 'R' the eight symbol characters are carried in `ref`.
    OrderId ref = 0;
    OrderId new_ref = 0;  // 'U' only
    Nanos timestamp = 0;
    Price price = 0;
    Qty shares = 0;
    Locate locate = 0;
    char type = 0;  // the ITCH type byte: R H A F E C X D U
    char aux = 0;   // 'A' and 'F': the side. 'C': printable. 'H': trading state

    friend bool operator==(const BookEvent&, const BookEvent&) = default;
};

static_assert(std::is_trivially_copyable_v<BookEvent>);
static_assert(sizeof(BookEvent) == 40);

// An ITCH handler that turns the book-changing messages into BookEvents and
// hands each to `emit`. Give it to ItchParser.
template <class Emit>
class EventEncoder : public feed::HandlerBase {
 public:
    explicit EventEncoder(Emit emit) : emit_(std::move(emit)) {}

    void on_stock_directory(const feed::StockDirectory& m) {
        BookEvent e = base('R', m.hdr);
        std::memcpy(&e.ref, m.stock.raw.data(), sizeof(e.ref));
        emit_(e);
    }
    void on_trading_action(const feed::TradingAction& m) {
        BookEvent e = base('H', m.hdr);
        e.aux = m.trading_state;
        emit_(e);
    }
    void on_add(const feed::AddOrder& m) {
        BookEvent e = base(m.type(), m.hdr);
        e.ref = m.order_ref;
        e.aux = static_cast<char>(m.side);
        e.shares = m.shares;
        e.price = m.price;
        emit_(e);
    }
    void on_execute(const feed::OrderExecuted& m) {
        BookEvent e = base('E', m.hdr);
        e.ref = m.order_ref;
        e.shares = m.shares;
        emit_(e);
    }
    void on_execute_with_price(const feed::OrderExecutedWithPrice& m) {
        BookEvent e = base('C', m.hdr);
        e.ref = m.order_ref;
        e.shares = m.shares;
        e.price = m.price;
        e.aux = m.printable;
        emit_(e);
    }
    void on_cancel(const feed::OrderCancel& m) {
        BookEvent e = base('X', m.hdr);
        e.ref = m.order_ref;
        e.shares = m.shares;
        emit_(e);
    }
    void on_delete(const feed::OrderDelete& m) {
        BookEvent e = base('D', m.hdr);
        e.ref = m.order_ref;
        emit_(e);
    }
    void on_replace(const feed::OrderReplace& m) {
        BookEvent e = base('U', m.hdr);
        e.ref = m.orig_order_ref;
        e.new_ref = m.new_order_ref;
        e.shares = m.shares;
        e.price = m.price;
        emit_(e);
    }

 private:
    [[nodiscard]] static BookEvent base(char type, const feed::Header& hdr) noexcept {
        BookEvent e;
        e.type = type;
        e.locate = hdr.locate;
        e.timestamp = hdr.timestamp;
        return e;
    }

    Emit emit_;
};

template <class Emit>
EventEncoder(Emit) -> EventEncoder<Emit>;

// Applies one event to an ITCH handler (a BookManager) by rebuilding the
// message it came from and calling the same callback the parser would have.
// Fields the book does not read (the match number, the symbol on an add, the
// attribution) are not carried and come out as their defaults.
template <feed::ItchHandler Handler>
void apply_event(const BookEvent& e, Handler& handler) {
    const feed::Header hdr{.locate = e.locate, .tracking = 0, .timestamp = e.timestamp};
    switch (e.type) {
        case 'A':
        case 'F': {
            feed::AddOrder m{.hdr = hdr, .order_ref = e.ref, .side = static_cast<Side>(e.aux)};
            m.shares = e.shares;
            m.price = e.price;
            m.attributed = e.type == 'F';
            handler.on_add(m);
            break;
        }
        case 'E':
            handler.on_execute(
                feed::OrderExecuted{.hdr = hdr, .order_ref = e.ref, .shares = e.shares});
            break;
        case 'C': {
            feed::OrderExecutedWithPrice m{.hdr = hdr, .order_ref = e.ref, .shares = e.shares};
            m.printable = e.aux;
            m.price = e.price;
            handler.on_execute_with_price(m);
            break;
        }
        case 'X':
            handler.on_cancel(
                feed::OrderCancel{.hdr = hdr, .order_ref = e.ref, .shares = e.shares});
            break;
        case 'D':
            handler.on_delete(feed::OrderDelete{.hdr = hdr, .order_ref = e.ref});
            break;
        case 'U':
            handler.on_replace(feed::OrderReplace{.hdr = hdr,
                                                  .orig_order_ref = e.ref,
                                                  .new_order_ref = e.new_ref,
                                                  .shares = e.shares,
                                                  .price = e.price});
            break;
        case 'R': {
            feed::StockDirectory m{.hdr = hdr};
            std::memcpy(m.stock.raw.data(), &e.ref, sizeof(e.ref));
            handler.on_stock_directory(m);
            break;
        }
        case 'H': {
            feed::TradingAction m{.hdr = hdr};
            m.trading_state = e.aux;
            handler.on_trading_action(m);
            break;
        }
        default:
            break;
    }
}

}  // namespace obe::pipeline
