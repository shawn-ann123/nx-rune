#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "SendEncoder.hpp"
#include "SocketCan.hpp"

namespace app_plugin
{
class CanSendEncoder final : public SendEncoder
{
public:
    void process(const app::Context &context) override;

private:
    using FireSubscriber = LatestBuffer<FireResult>::Subscriber;
    void load_config();
    void initialize_endpoint(const app::Context &context);
    bool connect_can();
    can_frame build_frame(std::chrono::steady_clock::time_point now);

    std::optional<FireSubscriber> m_input;
    hardware_io::SocketCan m_can;
    std::optional<FireResult> m_latest;
    std::string m_interface = "can0";
    std::chrono::milliseconds m_period{10};
    std::chrono::milliseconds m_result_timeout{100};
    std::chrono::milliseconds m_reconnect_delay{500};
    std::chrono::steady_clock::time_point m_next_send{};
    std::chrono::steady_clock::time_point m_last_result{};
    double m_yaw_offset_deg = 0.0;
    double m_pitch_offset_deg = 0.0;
    double m_yaw_scale = 1.0;
    double m_pitch_scale = 1.0;
    double m_yaw_min_deg = -180.0;
    double m_yaw_max_deg = 180.0;
    double m_pitch_min_deg = -45.0;
    double m_pitch_max_deg = 45.0;
    std::uint8_t m_frame_count = 0;
    std::uint8_t m_buff_mode = 1;
    std::uint8_t m_fire_mode = 3;
    bool m_previous_fire_request = false;
    bool m_single_fire_pending = false;
    bool m_config_loaded = false;
};
} // namespace app_plugin
