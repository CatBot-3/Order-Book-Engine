#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

#include "obe/net/socket.hpp"

// UDP sockets for the market-data feed: one that sends, one that receives.
//
// The destination can be a multicast group (224.0.0.0 to 239.255.255.255) or
// an ordinary address. Multicast is what an exchange uses: the sender does not
// know or care who is listening, and a receiver joins the group to start
// getting packets. Over loopback, and anywhere multicast is not routed, a
// plain address works the same way for one receiver, which is what the tests
// use so that they run in any container.

namespace obe::net {

class UdpSender {
 public:
    // `interface` is the local address multicast leaves from; ignored for an
    // ordinary destination. The default is loopback, which keeps a multicast
    // feed on this machine: point it at a real interface on purpose.
    UdpSender(const std::string& host, std::uint16_t port,
              const std::string& interface = "127.0.0.1")
        : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)),
          dest_(make_address(host, port)) {
        if (!fd_.valid()) {
            throw std::runtime_error(errno_text("cannot create a UDP socket"));
        }
        if (is_multicast(host)) {
            const in_addr local = make_address(interface, 0).sin_addr;
            if (::setsockopt(fd_.get(), IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof(local)) < 0) {
                throw std::runtime_error(errno_text("cannot select the multicast interface"));
            }
            // Deliver to receivers on this host too, and do not let packets
            // leave the local network segment.
            const unsigned char loop = 1;
            const unsigned char ttl = 1;
            ::setsockopt(fd_.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
            ::setsockopt(fd_.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        }
    }

    // Sends one datagram. Returns false if the kernel refused it (its send
    // buffer is full, or there is no route); the packet is then simply lost,
    // which is the contract of UDP and something receivers detect as a gap.
    bool send(std::span<const std::byte> packet) noexcept {
        const ssize_t n = ::sendto(fd_.get(), packet.data(), packet.size(), MSG_NOSIGNAL,
                                   reinterpret_cast<const sockaddr*>(&dest_), sizeof(dest_));
        if (n == static_cast<ssize_t>(packet.size())) {
            ++sent_;
            return true;
        }
        ++dropped_;
        return false;
    }

    [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

 private:
    Fd fd_;
    sockaddr_in dest_;
    std::uint64_t sent_ = 0;
    std::uint64_t dropped_ = 0;
};

class UdpReceiver {
 public:
    // Binds to `port` (0 picks a free one; port() says which). If `host` is a
    // multicast group it is joined on `interface`; otherwise the socket is
    // bound to `host` itself.
    UdpReceiver(const std::string& host, std::uint16_t port,
                const std::string& interface = "127.0.0.1")
        : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) {
        if (!fd_.valid()) {
            throw std::runtime_error(errno_text("cannot create a UDP socket"));
        }
        const int one = 1;
        // Several receivers on one machine may listen to the same feed.
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        // A larger receive buffer rides out a burst while this thread is busy.
        // The kernel caps it at net.core.rmem_max; failure is not an error.
        const int buffer = 4 << 20;
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));

        const bool multicast = is_multicast(host);
        // A multicast receiver binds to the group address, so that it gets
        // that group's packets and not every datagram sent to the port.
        const sockaddr_in addr = make_address(host, port);
        if (::bind(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error(
                errno_text("cannot bind UDP " + host + ":" + std::to_string(port)));
        }
        if (multicast) {
            ip_mreq request{};
            request.imr_multiaddr = addr.sin_addr;
            request.imr_interface = make_address(interface, 0).sin_addr;
            if (::setsockopt(fd_.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request)) <
                0) {
                throw std::runtime_error(errno_text("cannot join multicast group " + host));
            }
        }
        port_ = local_port(fd_.get());
    }

    // Reads one datagram into buf without waiting. Returns its size, or -1 if
    // none is queued. A datagram larger than buf is cut short.
    [[nodiscard]] std::ptrdiff_t receive(std::span<std::byte> buf) noexcept {
        const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), 0);
        return n < 0 ? -1 : static_cast<std::ptrdiff_t>(n);
    }

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

 private:
    Fd fd_;
    std::uint16_t port_ = 0;
};

}  // namespace obe::net
