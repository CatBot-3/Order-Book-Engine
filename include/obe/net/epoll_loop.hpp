#pragma once

#include <sys/epoll.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#include "obe/net/socket.hpp"

// A thin wrapper over Linux epoll.
//
// The model: one thread owns many sockets, none of which it may block on. It
// asks the kernel "which of these are ready?", handles exactly those, and asks
// again. With one thread there is nothing to lock, and a thousand idle
// connections cost nothing until one of them has something to say.
//
// Why epoll and not select or poll: those take the whole set of descriptors
// on every call and the kernel scans all of them, so the cost of one wake-up
// grows with the number of connections, busy or not. epoll keeps the set in
// the kernel and returns only the ready ones.
//
// This uses level-triggered mode, the default: a socket is reported for as
// long as it has unread data. Edge-triggered mode reports it once, when data
// arrives, and never again until more does; it saves a few wake-ups and in
// return every handler must read until the socket is empty or lose the
// notification for ever. Level-triggered cannot lose one, and lets a handler
// stop after a bounded amount of work so one busy client cannot starve the
// others.

namespace obe::net {

class EpollLoop {
 public:
    static constexpr std::size_t kMaxEvents = 256;

    EpollLoop() : fd_(::epoll_create1(EPOLL_CLOEXEC)) {
        if (!fd_.valid()) {
            throw std::runtime_error(errno_text("epoll_create1"));
        }
    }

    // Starts watching fd for `events` (EPOLLIN, EPOLLOUT, ...). `tag` comes
    // back with every event for it: whatever the caller needs to find its own
    // state for the descriptor.
    void add(int fd, std::uint32_t events, std::uint64_t tag) {
        control(EPOLL_CTL_ADD, fd, events, tag);
    }

    // Changes what an already watched fd is watched for.
    void modify(int fd, std::uint32_t events, std::uint64_t tag) {
        control(EPOLL_CTL_MOD, fd, events, tag);
    }

    // Stops watching fd. Closing a descriptor also removes it, but only once
    // every duplicate of it is closed; being explicit avoids relying on that.
    void remove(int fd) noexcept { ::epoll_ctl(fd_.get(), EPOLL_CTL_DEL, fd, nullptr); }

    // Waits up to timeout_ms for something to be ready (0 returns at once, -1
    // waits without limit) and returns the ready events. The span is valid
    // until the next call.
    [[nodiscard]] std::span<const epoll_event> wait(int timeout_ms) {
        const int n =
            ::epoll_wait(fd_.get(), events_.data(), static_cast<int>(events_.size()), timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                // A signal arrived. Return with nothing, so that a caller
                // waiting to be told to stop gets to look at its flag.
                return {};
            }
            throw std::runtime_error(errno_text("epoll_wait"));
        }
        return {events_.data(), static_cast<std::size_t>(n)};
    }

 private:
    void control(int op, int fd, std::uint32_t events, std::uint64_t tag) {
        epoll_event ev{};
        ev.events = events;
        ev.data.u64 = tag;
        if (::epoll_ctl(fd_.get(), op, fd, &ev) < 0) {
            throw std::runtime_error(errno_text("epoll_ctl"));
        }
    }

    Fd fd_;
    std::array<epoll_event, kMaxEvents> events_{};
};

}  // namespace obe::net
