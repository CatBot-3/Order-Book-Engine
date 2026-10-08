#pragma once

#include <linux/io_uring.h>
#include <linux/time_types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "obe/net/socket.hpp"

// io_uring with nothing between this code and the kernel.
//
// liburing is the library everybody uses for this, and for good reason. It is
// left out here on purpose: the interface underneath is three system calls
// and two rings of shared memory, and it is worth seeing once that there is
// nothing else.
//
// THE TWO RINGS
//
//   io_uring_setup(entries, &params) -> fd
//
// creates them, and the program maps them into its own memory with mmap. From
// then on both sides read and write the same pages.
//
//   Submission queue (SQ). The program fills in an entry (an "SQE": an
//   operation code, a file descriptor, a buffer, and 64 bits of its own
//   called user_data), then advances the queue's tail. The kernel takes
//   entries from the head.
//
//   Completion queue (CQ). The kernel writes an entry (a "CQE": the same
//   user_data, and a result) and advances the tail. The program takes entries
//   from the head.
//
//   io_uring_enter(fd, to_submit, min_complete, flags, ...)
//
// tells the kernel "there are to_submit new entries; and do not return until
// min_complete completions are available". One call can do both, or either.
// Nothing else ever crosses into the kernel: an operation is a write to
// shared memory, and so is collecting its result.
//
// Each queue is the single-producer, single-consumer ring of
// util/spsc_ring.hpp with the kernel at the other end. The index one side
// writes is published with a release store and read by the other with an
// acquire load, for the reason given there.
//
// WHAT user_data IS FOR
//
// Completions arrive in whatever order the operations finish, and a CQE says
// nothing about which operation it belongs to except the 64 bits the program
// put in the SQE. Everything a program needs to find its own state again has
// to fit in them: an index, a pointer, a pointer with a tag in its low bits.
//
// THE RULE THAT MAKES COMPLETION-BASED I/O HARD
//
// From the moment an SQE is submitted until its CQE has been collected, the
// kernel may read or write the buffer the SQE points at, at any time, from
// another context. The memory must stay where it is and stay alive for all of
// that time. Closing the socket does not end the wait; freeing the buffer
// does not end it either, it only turns it into a use-after-free that no
// sanitizer can see, because the write comes from the kernel. An operation
// that is no longer wanted has to be cancelled, and its CQE (with -ECANCELED,
// or a real result if it finished first) waited for like any other.
//
// This class is only the rings. It knows nothing about sockets, buffers or
// who owns them: that is the transport's side (uring_transport.hpp).

namespace obe::net {

class Uring {
 public:
    // Whether this kernel will give this process a ring that can do what the
    // transport needs. False on kernels older than 5.11, and wherever a
    // sandbox or a system setting refuses io_uring to the process.
    //
    // It makes a small ring and puts one operation through it, so that a
    // sandbox which allows the ring to be created and then refuses to let it
    // be used is found out here and not in the middle of serving.
    [[nodiscard]] static bool available() noexcept {
        try {
            Uring ring(4);
            io_uring_sqe* sqe = ring.get_sqe();
            if (sqe == nullptr) {
                return false;
            }
            sqe->opcode = IORING_OP_NOP;
            sqe->user_data = 0x0BE;
            if (ring.submit_and_wait(1'000) != 1) {
                return false;
            }
            bool came_back = false;
            ring.reap([&came_back](const io_uring_cqe& cqe) {
                came_back = cqe.user_data == 0x0BE && cqe.res == 0;
            });
            return came_back;
        } catch (const std::exception&) {
            return false;
        }
    }

    // A ring with room for at least `entries` submissions between two calls
    // to submit(). The kernel rounds it up to a power of two, and gives the
    // completion queue twice as many. Throws std::runtime_error if the kernel
    // refuses.
    explicit Uring(unsigned entries) {
        io_uring_params params{};
        fd_ = Fd(static_cast<int>(::syscall(SYS_io_uring_setup, entries, &params)));
        if (!fd_.valid()) {
            throw std::runtime_error(errno_text("io_uring_setup"));
        }
        if ((params.features & kNeeded) != kNeeded) {
            throw std::runtime_error(
                "io_uring: this kernel lacks a feature the ring needs "
                "(Linux 5.11 or later)");
        }
        sq_entries_ = params.sq_entries;
        cq_entries_ = params.cq_entries;
        features_ = params.features;

        // Since Linux 5.4 one mapping holds both rings' indices.
        sq_ring_size_ = params.sq_off.array + params.sq_entries * sizeof(std::uint32_t);
        const std::size_t cq_ring_size =
            params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
        sq_ring_size_ = sq_ring_size_ > cq_ring_size ? sq_ring_size_ : cq_ring_size;
        sq_ring_ = map(sq_ring_size_, IORING_OFF_SQ_RING);
        sqes_size_ = params.sq_entries * sizeof(io_uring_sqe);
        try {
            sqes_ = static_cast<io_uring_sqe*>(map(sqes_size_, IORING_OFF_SQES));
        } catch (...) {
            // No destructor runs for an object whose constructor threw.
            ::munmap(sq_ring_, sq_ring_size_);
            throw;
        }

        auto* base = static_cast<std::byte*>(sq_ring_);
        sq_head_ = at<std::uint32_t>(base, params.sq_off.head);
        sq_tail_ = at<std::uint32_t>(base, params.sq_off.tail);
        sq_mask_ = *at<std::uint32_t>(base, params.sq_off.ring_mask);
        sq_flags_ = at<std::uint32_t>(base, params.sq_off.flags);
        sq_array_ = at<std::uint32_t>(base, params.sq_off.array);
        cq_head_ = at<std::uint32_t>(base, params.cq_off.head);
        cq_tail_ = at<std::uint32_t>(base, params.cq_off.tail);
        cq_mask_ = *at<std::uint32_t>(base, params.cq_off.ring_mask);
        cqes_ = at<io_uring_cqe>(base, params.cq_off.cqes);
        tail_ = *sq_tail_;
    }

    Uring(const Uring&) = delete;
    Uring& operator=(const Uring&) = delete;

    // Does not wait for operations in flight. The kernel cancels them when
    // the ring goes, but in its own time, and not necessarily before this
    // returns. An owner whose buffers the kernel may still be using must
    // cancel those operations and collect their completions BEFORE the ring
    // is destroyed.
    ~Uring() {
        fd_.reset();
        if (sqes_ != nullptr) {
            ::munmap(sqes_, sqes_size_);
        }
        if (sq_ring_ != nullptr) {
            ::munmap(sq_ring_, sq_ring_size_);
        }
    }

    // An empty submission entry to fill in, or nullptr if the queue is full:
    // submit() makes room. The entry is zeroed. It counts as filled in from
    // this moment, so every entry taken must be made into a real operation.
    [[nodiscard]] io_uring_sqe* get_sqe() noexcept {
        const std::uint32_t head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
        if (tail_ - head >= sq_entries_) {
            return nullptr;
        }
        const std::uint32_t index = tail_ & sq_mask_;
        io_uring_sqe* sqe = &sqes_[index];
        std::memset(sqe, 0, sizeof(*sqe));
        sq_array_[index] = index;
        ++tail_;
        return sqe;
    }

    // Entries filled in and not yet taken by the kernel.
    [[nodiscard]] unsigned unsubmitted() const noexcept {
        return tail_ - __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
    }

    // Hands every filled-in entry to the kernel, without waiting for anything
    // to complete. Returns how many it took, or a negative errno if the call
    // failed and took none.
    //
    // An operation that can finish at once, such as a send to a socket with
    // room, has finished by the time this returns, and its completion is
    // there to be reaped, if the completion queue had room for it.
    //
    // WHEN THE COMPLETION QUEUE IS FULL the kernel does not stop taking
    // submissions (older kernels did, answering -EBUSY, so be ready for
    // either). It keeps the completions that do not fit on a list of its
    // own, and moves them into the ring when room has been made AND the
    // program next asks for completions. That second condition is taken care
    // of here: once reap() has made room, the next submit() or
    // submit_and_wait(), with or without anything to submit, brings them in.
    // What remains the caller's is the first: completions it does not reap
    // cannot be replaced by the ones that are waiting behind them.
    int submit() noexcept { return enter(0, -1); }

    // The same, and then waits until at least one completion is available or
    // timeout_ms has passed (-1 waits without limit, 0 does not wait). A wait
    // that timed out, or was interrupted by a signal, is not an error.
    int submit_and_wait(int timeout_ms) noexcept { return enter(1, timeout_ms); }

    // Calls f(const io_uring_cqe&) for every completion that is available and
    // marks them collected. Returns how many there were. f may fill in new
    // submission entries; it must not call reap.
    template <class F>
    unsigned reap(F&& f) {
        std::uint32_t head = *cq_head_;  // only this side writes it
        const std::uint32_t tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
        unsigned count = 0;
        while (head != tail) {
            // Copied out, and the slot given back, before f runs: f sees a
            // value, not a place in the ring the kernel may write to next.
            const io_uring_cqe cqe = cqes_[head & cq_mask_];
            ++head;
            __atomic_store_n(cq_head_, head, __ATOMIC_RELEASE);
            f(std::as_const(cqe));
            ++count;
        }
        return count;
    }

    // Completions waiting to be reaped.
    [[nodiscard]] unsigned ready() const noexcept {
        return __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE) - *cq_head_;
    }

    [[nodiscard]] unsigned sq_entries() const noexcept { return sq_entries_; }
    [[nodiscard]] unsigned cq_entries() const noexcept { return cq_entries_; }
    [[nodiscard]] std::uint32_t features() const noexcept { return features_; }
    // Calls to io_uring_enter so far: the system calls this ring has cost.
    [[nodiscard]] std::uint64_t enters() const noexcept { return enters_; }

 private:
    // NODROP: a completion is never lost when the completion queue is full;
    // the kernel keeps it until there is room. EXT_ARG: a wait can carry a
    // timeout.
    static constexpr std::uint32_t kNeeded =
        IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP | IORING_FEAT_EXT_ARG;

    template <class T>
    [[nodiscard]] static T* at(std::byte* base, std::uint32_t offset) noexcept {
        // The mapping is the kernel's memory, laid out and aligned by it for
        // exactly these types.
        return reinterpret_cast<T*>(base + offset);
    }

    [[nodiscard]] void* map(std::size_t size, std::uint64_t offset) {
        void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                         fd_.get(), static_cast<off_t>(offset));
        if (p == MAP_FAILED) {
            throw std::runtime_error(errno_text("mmap of an io_uring ring"));
        }
        return p;
    }

    int enter(unsigned min_complete, int timeout_ms) noexcept {
        // Publishing the tail is what makes the entries the kernel's.
        __atomic_store_n(sq_tail_, tail_, __ATOMIC_RELEASE);
        const std::uint32_t head_before = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
        const unsigned to_submit = tail_ - head_before;
        unsigned flags = 0;
        io_uring_getevents_arg arg{};
        __kernel_timespec ts{};
        void* argp = nullptr;
        std::size_t argsz = 0;
        if (min_complete != 0) {
            if (timeout_ms == 0 || ready() != 0) {
                min_complete = 0;  // nothing to wait for
            } else {
                flags |= IORING_ENTER_GETEVENTS;
                if (timeout_ms > 0) {
                    ts.tv_sec = timeout_ms / 1000;
                    ts.tv_nsec = static_cast<long long>(timeout_ms % 1000) * 1'000'000;
                    arg.ts = reinterpret_cast<std::uint64_t>(&ts);
                    flags |= IORING_ENTER_EXT_ARG;
                    argp = &arg;
                    argsz = sizeof(arg);
                }
            }
        }
        // Completions that found the ring full wait on the kernel's side,
        // and it says so in a flag in the shared memory. It only moves them
        // into the ring during a call that asks for completions, so ask,
        // even when this call would otherwise not wait or not happen at all.
        // With min_complete at 0 the kernel moves what fits and returns.
        if ((__atomic_load_n(sq_flags_, __ATOMIC_RELAXED) & IORING_SQ_CQ_OVERFLOW) != 0) {
            flags |= IORING_ENTER_GETEVENTS;
        }
        if (to_submit == 0 && min_complete == 0 && flags == 0) {
            return 0;  // nothing to say to the kernel
        }
        ++enters_;
        const long ret =
            ::syscall(SYS_io_uring_enter, fd_.get(), to_submit, min_complete, flags, argp, argsz);
        const int err = ret < 0 ? errno : 0;
        // How many entries the kernel took is read from the queue itself,
        // which is right whatever the call returned: it advances the head as
        // it takes them, and it takes them before it starts to wait.
        const std::uint32_t taken = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE) - head_before;
        if (err != 0 && err != ETIME && err != EINTR && taken == 0) {
            return -err;
        }
        return static_cast<int>(taken);
    }

    Fd fd_;
    void* sq_ring_ = nullptr;
    std::size_t sq_ring_size_ = 0;
    io_uring_sqe* sqes_ = nullptr;
    std::size_t sqes_size_ = 0;
    std::uint32_t* sq_head_ = nullptr;  // the kernel advances it
    std::uint32_t* sq_tail_ = nullptr;  // this side advances it
    std::uint32_t* sq_array_ = nullptr;
    std::uint32_t* sq_flags_ = nullptr;  // the kernel's notes to this side
    std::uint32_t sq_mask_ = 0;
    std::uint32_t* cq_head_ = nullptr;  // this side advances it
    std::uint32_t* cq_tail_ = nullptr;  // the kernel advances it
    io_uring_cqe* cqes_ = nullptr;
    std::uint32_t cq_mask_ = 0;
    std::uint32_t tail_ = 0;  // entries filled in
    unsigned sq_entries_ = 0;
    unsigned cq_entries_ = 0;
    std::uint32_t features_ = 0;
    std::uint64_t enters_ = 0;
};

// Filling in an entry. Each takes an entry from get_sqe(), which is zeroed,
// and makes it one operation. `user_data` comes back in the completion.
//
// The result of a completion (cqe.res) is what the matching system call would
// have returned, with errors as negative errno values: -EAGAIN, -ECONNRESET,
// -ECANCELED and so on. errno itself is not involved.

// Does nothing and completes at once with 0.
inline void prep_nop(io_uring_sqe* sqe, std::uint64_t user_data) noexcept {
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = user_data;
}

// accept4(listener, nullptr, nullptr, flags). Completes with the new socket.
//
// The listener should be an ordinary BLOCKING socket. Nothing blocks: the
// waiting is the ring's. But on the older kernels this file supports, an
// accept through the ring on a listener marked O_NONBLOCK is answered the
// way a non-blocking accept4 would be, at once and with -EAGAIN when nobody
// is waiting, and a transport that asks again each time spins. Newer kernels
// wait either way.
inline void prep_accept(io_uring_sqe* sqe, int listener, int flags,
                        std::uint64_t user_data) noexcept {
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = listener;
    sqe->accept_flags = static_cast<std::uint32_t>(flags);
    sqe->user_data = user_data;
}

// recv(fd, buffer, size, 0). Completes with the number of bytes, 0 when the
// peer has closed. The buffer is the kernel's to write until then.
inline void prep_recv(io_uring_sqe* sqe, int fd, std::byte* buffer, std::size_t size,
                      std::uint64_t user_data) noexcept {
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<std::uint64_t>(buffer);
    sqe->len = static_cast<std::uint32_t>(size);
    sqe->user_data = user_data;
}

// send(fd, buffer, size, msg_flags). Completes with the number of bytes the
// kernel took, which may be fewer than asked. The buffer is the kernel's to
// read until then.
inline void prep_send(io_uring_sqe* sqe, int fd, const std::byte* buffer, std::size_t size,
                      int msg_flags, std::uint64_t user_data) noexcept {
    sqe->opcode = IORING_OP_SEND;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<std::uint64_t>(buffer);
    sqe->len = static_cast<std::uint32_t>(size);
    sqe->msg_flags = static_cast<std::uint32_t>(msg_flags);
    sqe->user_data = user_data;
}

// close(fd).
inline void prep_close(io_uring_sqe* sqe, int fd, std::uint64_t user_data) noexcept {
    sqe->opcode = IORING_OP_CLOSE;
    sqe->fd = fd;
    sqe->user_data = user_data;
}

// Asks for the operation that was submitted with user_data == target to be
// cancelled. This completes with 0 if it found it, -ENOENT if there was no
// such operation, -EALREADY if it was too far along to stop. The operation
// itself still completes, with -ECANCELED or with its real result: there are
// always two completions to collect.
inline void prep_cancel(io_uring_sqe* sqe, std::uint64_t target, std::uint64_t user_data) noexcept {
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->fd = -1;
    sqe->addr = target;
    sqe->user_data = user_data;
}

}  // namespace obe::net
