#include "SocketCan.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hardware_io
{
namespace
{
[[noreturn]] void throw_system_error(const char *operation)
{
    throw std::system_error(errno, std::generic_category(), operation);
}
} // namespace

SocketCan::~SocketCan() { close(); }

void SocketCan::open(const std::string &interface_name,
                     const std::vector<can_filter> &filters)
{
    close();
    if (interface_name.empty() || interface_name.size() >= IFNAMSIZ)
        throw std::invalid_argument("invalid SocketCAN interface name");

    m_fd = ::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (m_fd < 0) throw_system_error("socket(PF_CAN)");

    try
    {
        if (!filters.empty() &&
            ::setsockopt(m_fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters.data(),
                         static_cast<socklen_t>(filters.size() * sizeof(can_filter))) < 0)
            throw_system_error("setsockopt(CAN_RAW_FILTER)");

        ifreq request{};
        std::strncpy(request.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);
        if (::ioctl(m_fd, SIOCGIFINDEX, &request) < 0)
            throw_system_error("ioctl(SIOCGIFINDEX)");

        sockaddr_can address{};
        address.can_family = AF_CAN;
        address.can_ifindex = request.ifr_ifindex;
        if (::bind(m_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
            throw_system_error("bind(AF_CAN)");
    }
    catch (...)
    {
        close();
        throw;
    }
}

void SocketCan::close() noexcept
{
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
}

bool SocketCan::is_open() const noexcept { return m_fd >= 0; }

std::optional<can_frame> SocketCan::receive(std::chrono::milliseconds timeout) const
{
    if (m_fd < 0) throw std::logic_error("SocketCAN receive on a closed socket");
    pollfd descriptor{m_fd, POLLIN, 0};
    int status;
    do { status = ::poll(&descriptor, 1, static_cast<int>(timeout.count())); }
    while (status < 0 && errno == EINTR);
    if (status == 0) return std::nullopt;
    if (status < 0) throw_system_error("poll(SocketCAN receive)");
    if ((descriptor.revents & POLLIN) == 0)
        throw std::runtime_error("SocketCAN receive reported a socket error");

    can_frame frame{};
    ssize_t bytes;
    do { bytes = ::read(m_fd, &frame, sizeof(frame)); }
    while (bytes < 0 && errno == EINTR);
    if (bytes < 0) throw_system_error("read(SocketCAN)");
    if (bytes != static_cast<ssize_t>(sizeof(frame)))
        throw std::runtime_error("short SocketCAN frame");
    return frame;
}

bool SocketCan::send(const can_frame &frame) const
{
    if (m_fd < 0) return false;
    ssize_t bytes;
    do { bytes = ::write(m_fd, &frame, sizeof(frame)); }
    while (bytes < 0 && errno == EINTR);
    return bytes == static_cast<ssize_t>(sizeof(frame));
}
} // namespace hardware_io
