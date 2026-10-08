#pragma once

#include <cstddef>
#include <span>
#include <vector>

// The bytes queued for one connection that the kernel has not taken yet.
//
// Bytes are appended at the back as answers are produced and taken from the
// front as the socket accepts them. The kernel takes what it likes: a send()
// of 1000 bytes may accept 300, and the other 700 have to be offered again,
// from where it stopped.
//
// The one thing to get right is what happens to the part already sent. The
// simplest buffer only advances a mark and forgets everything once the mark
// reaches the end. That is correct, and it has a hole: a client that reads
// steadily, a little slower than it is written to, never lets the mark reach
// the end. Its backlog stays small, so it is never judged slow, and the
// buffer holds every byte ever sent to it. The limit on a slow client's
// backlog would bound nothing.
//
// So the sent part is dropped as soon as it is at least as large as what is
// left. Dropping it means moving what is left to the front, which costs one
// copy of those bytes, and at that moment there are no more of them than
// bytes sent since the last time: the copying adds at most one byte moved per
// byte sent, and the memory held is never more than twice the backlog.
//
// pending() points into the buffer and is good until the next call that
// changes it. That is all a readiness transport needs: it reads the buffer
// only inside send(), which has returned before anything else can happen. It
// is exactly what a completion transport cannot live with, since there the
// kernel reads the bytes later, while the program goes on appending. See the
// questions in uring_transport.hpp.

namespace obe::net {

class OutBuffer {
 public:
    void append(std::span<const std::byte> bytes) {
        data_.insert(data_.end(), bytes.begin(), bytes.end());
    }

    // What to offer the kernel next: everything not yet taken, in order.
    [[nodiscard]] std::span<const std::byte> pending() const noexcept {
        return {data_.data() + sent_, data_.size() - sent_};
    }

    // The kernel took the first `count` bytes of pending().
    void took(std::size_t count) {
        sent_ += count;
        const std::size_t left = data_.size() - sent_;
        if (left == 0) {
            data_.clear();
            sent_ = 0;
        } else if (sent_ >= left) {
            data_.erase(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(sent_));
            sent_ = 0;
        }
    }

    [[nodiscard]] std::size_t unsent() const noexcept { return data_.size() - sent_; }

    // Bytes this buffer keeps in memory: unsent() plus whatever of the sent
    // part has not been dropped yet. Never more than twice unsent().
    [[nodiscard]] std::size_t held() const noexcept { return data_.size(); }

    [[nodiscard]] bool empty() const noexcept { return sent_ == data_.size(); }

 private:
    std::vector<std::byte> data_;
    std::size_t sent_ = 0;  // how much of data_ the kernel has taken
};

}  // namespace obe::net
