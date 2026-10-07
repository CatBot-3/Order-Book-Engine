#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "obe/store/delta_codec.hpp"
#include "obe/store/tick_codec.hpp"

// The named tick codecs. tick_store and tick_bench select one with
// --codec NAME, and a store's header says which one wrote it.

namespace obe::store {

// Calls f(std::type_identity<Codec>{}) once for every codec, the reference
// first.
template <class F>
void for_each_codec(F&& f) {
    f(std::type_identity<RawCodec>{});
    f(std::type_identity<DeltaCodec>{});
}

// Calls f(std::type_identity<Codec>{}) for the codec called `name`. Returns
// false, calling nothing, if there is no such codec.
template <class F>
bool with_codec(std::string_view name, F&& f) {
    bool found = false;
    for_each_codec([&]<class Codec>(std::type_identity<Codec> tag) {
        if (!found && Codec::kName == name) {
            found = true;
            f(tag);
        }
    });
    return found;
}

// The same, by the id a store's header carries.
template <class F>
bool with_codec_id(std::uint16_t id, F&& f) {
    bool found = false;
    for_each_codec([&]<class Codec>(std::type_identity<Codec> tag) {
        if (!found && Codec::kId == id) {
            found = true;
            f(tag);
        }
    });
    return found;
}

}  // namespace obe::store
