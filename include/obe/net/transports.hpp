#pragma once

#include <string_view>
#include <type_traits>

#include "obe/net/epoll_transport.hpp"
#include "obe/net/transport.hpp"
#include "obe/net/uring_transport.hpp"

// The named transports. exchange_server and transport_bench select one with
// --io NAME.

namespace obe::net {

// Calls f(std::type_identity<Net>{}) once for every transport, the reference
// first.
template <class F>
void for_each_transport(F&& f) {
    f(std::type_identity<EpollTransport>{});
    f(std::type_identity<UringTransport>{});
}

// Calls f(std::type_identity<Net>{}) for the transport called `name`. Returns
// false, calling nothing, if there is no such transport.
template <class F>
bool with_transport(std::string_view name, F&& f) {
    bool found = false;
    for_each_transport([&]<class Net>(std::type_identity<Net> tag) {
        if (!found && Net::kName == name) {
            found = true;
            f(tag);
        }
    });
    return found;
}

}  // namespace obe::net
