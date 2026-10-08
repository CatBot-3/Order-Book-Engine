// Does io_uring work on this machine? Exit status 0 if one no-op goes
// through a ring and comes back.
//
// This is compiled and run when the build is configured (tests/CMakeLists.txt)
// and shares no code with include/obe/net/uring.hpp, on purpose. The tests of
// the ring skip themselves where the ring says it cannot be used. A ring with
// a bug could say that too, and its tests would all be skipped. So the tests
// are told what this independent check found: where it succeeded, "cannot be
// used here" is a failure and not a reason to skip.

#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

int main() {
    io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    const long fd = ::syscall(SYS_io_uring_setup, 2, &params);
    if (fd < 0) {
        return 1;
    }
    const unsigned needed = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP | IORING_FEAT_EXT_ARG;
    if ((params.features & needed) != needed) {
        return 2;
    }
    std::size_t ring_size = params.sq_off.array + params.sq_entries * sizeof(std::uint32_t);
    const std::size_t cq_size = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
    if (cq_size > ring_size) {
        ring_size = cq_size;
    }
    void* ring = ::mmap(nullptr, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                        static_cast<int>(fd), IORING_OFF_SQ_RING);
    void* entries =
        ::mmap(nullptr, params.sq_entries * sizeof(io_uring_sqe), PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_POPULATE, static_cast<int>(fd), IORING_OFF_SQES);
    if (ring == MAP_FAILED || entries == MAP_FAILED) {
        return 3;
    }
    auto* base = static_cast<unsigned char*>(ring);
    const auto word = [base](std::uint32_t offset) {
        return reinterpret_cast<std::uint32_t*>(base + offset);
    };
    auto* sqe = static_cast<io_uring_sqe*>(entries);
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = 0x0BE;
    word(params.sq_off.array)[0] = 0;
    __atomic_store_n(word(params.sq_off.tail), *word(params.sq_off.tail) + 1, __ATOMIC_RELEASE);
    if (::syscall(SYS_io_uring_enter, static_cast<int>(fd), 1, 1, IORING_ENTER_GETEVENTS, nullptr,
                  0) != 1) {
        return 4;
    }
    if (__atomic_load_n(word(params.cq_off.tail), __ATOMIC_ACQUIRE) == *word(params.cq_off.head)) {
        return 5;
    }
    const auto* cqe = reinterpret_cast<const io_uring_cqe*>(base + params.cq_off.cqes);
    return cqe->user_data == 0x0BE && cqe->res == 0 ? 0 : 6;
}
