#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// The small amount of POSIX socket handling everything in obe/net shares.
// Linux only, IPv4 only.
//
// Set-up failures throw: a server that cannot bind its port has nothing useful
// to do next. Failures on a live connection never throw; they are ordinary
// events (the peer went away, the buffer is full) and the code that owns the
// connection decides what they mean.

namespace obe::net {

// "what: the system's description of errno".
[[nodiscard]] inline std::string errno_text(std::string_view what, int err = errno) {
    return std::string(what) + ": " + std::strerror(err);
}

// Owns a file descriptor and closes it.
class Fd {
 public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

 private:
    int fd_ = -1;
};

// A host and a port, as "127.0.0.1:9001".
struct Endpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
};

// Parses "host:port". The host must be a dotted IPv4 address; names are not
// resolved. Returns false, leaving out alone, for anything else.
[[nodiscard]] inline bool parse_endpoint(std::string_view text, Endpoint& out) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size()) {
        return false;
    }
    const std::string_view port_text = text.substr(colon + 1);
    std::uint16_t port = 0;
    const auto r = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (r.ec != std::errc{} || r.ptr != port_text.data() + port_text.size()) {
        return false;
    }
    const std::string host(text.substr(0, colon));
    in_addr parsed{};
    if (::inet_pton(AF_INET, host.c_str(), &parsed) != 1) {
        return false;
    }
    out.host = host;
    out.port = port;
    return true;
}

[[nodiscard]] inline sockaddr_in make_address(const std::string& host, std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("'" + host + "' is not an IPv4 address");
    }
    return addr;
}

// True for an address in 224.0.0.0/4, the multicast range.
[[nodiscard]] inline bool is_multicast(const std::string& host) {
    in_addr parsed{};
    return ::inet_pton(AF_INET, host.c_str(), &parsed) == 1 && IN_MULTICAST(ntohl(parsed.s_addr));
}

inline void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error(errno_text("cannot make a socket non-blocking"));
    }
}

// Turns Nagle's algorithm off.
//
// Nagle holds back a small write while an earlier one is unacknowledged, so
// that many tiny writes travel as one packet. That is the right default for a
// terminal session and the wrong one here: an order or an acknowledgement is a
// few dozen bytes that must leave now, not up to tens of milliseconds later
// when the peer's delayed acknowledgement arrives.
inline void set_nodelay(int fd) {
    const int one = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
        throw std::runtime_error(errno_text("cannot set TCP_NODELAY"));
    }
}

// The port a bound socket ended up on. Needed after binding to port 0.
[[nodiscard]] inline std::uint16_t local_port(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        throw std::runtime_error(errno_text("getsockname"));
    }
    return ntohs(addr.sin_port);
}

// A non-blocking TCP socket, bound and listening. Port 0 lets the system pick
// a free one; local_port() says which.
[[nodiscard]] inline Fd listen_tcp(const std::string& host, std::uint16_t port,
                                   int backlog = 1024) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd.valid()) {
        throw std::runtime_error(errno_text("cannot create a TCP socket"));
    }
    const int one = 1;
    // Lets a restarted server bind while connections of the previous run are
    // still winding down.
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    const sockaddr_in addr = make_address(host, port);
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error(errno_text("cannot bind " + host + ":" + std::to_string(port)));
    }
    if (::listen(fd.get(), backlog) < 0) {
        throw std::runtime_error(errno_text("cannot listen"));
    }
    return fd;
}

// A connected TCP socket with Nagle off. The connect itself blocks; pass
// nonblocking = true to switch the socket afterwards.
[[nodiscard]] inline Fd connect_tcp(const std::string& host, std::uint16_t port, bool nonblocking) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd.valid()) {
        throw std::runtime_error(errno_text("cannot create a TCP socket"));
    }
    const sockaddr_in addr = make_address(host, port);
    int rc = 0;
    do {
        rc = ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        throw std::runtime_error(
            errno_text("cannot connect to " + host + ":" + std::to_string(port)));
    }
    set_nodelay(fd.get());
    if (nonblocking) {
        set_nonblocking(fd.get());
    }
    return fd;
}

}  // namespace obe::net
