#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <linux/can.h>

namespace hardware_io
{
class SocketCan final
{
public:
    SocketCan() = default;
    ~SocketCan();
    SocketCan(const SocketCan &) = delete;
    SocketCan &operator=(const SocketCan &) = delete;

    void open(const std::string &interface_name,
              const std::vector<can_filter> &filters = {});
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::optional<can_frame> receive(std::chrono::milliseconds timeout) const;
    [[nodiscard]] bool send(const can_frame &frame) const;

private:
    int m_fd = -1;
};
} // namespace hardware_io
