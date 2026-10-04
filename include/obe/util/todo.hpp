#pragma once

#include <stdexcept>
#include <string>

// Marks code that is deliberately left unwritten.
//
// The project spec (section 10) keeps the core data structures hand-written:
// the order store, the price levels, the queue and the matching loop. Their
// headers ship with interfaces, contracts and tests, and bodies that call
// todo(). A test that reaches one fails with a message naming the function; an
// app prints the same message and exits.
//
// When every todo() in a header is gone, that header's tests take over as the
// judge.

namespace obe::util {

struct Unimplemented : std::logic_error {
    using std::logic_error::logic_error;
};

// Throws Unimplemented. The extra arguments are ignored; passing a stub's
// parameters here keeps their names in the signature without "unused
// parameter" warnings.
template <class... Unused>
[[noreturn]] void todo(const char* function, const Unused&... /*unused*/) {
    throw Unimplemented(std::string(function) +
                        " is not implemented yet. It is one of the parts written by hand: see "
                        "the notes above its declaration.");
}

}  // namespace obe::util
