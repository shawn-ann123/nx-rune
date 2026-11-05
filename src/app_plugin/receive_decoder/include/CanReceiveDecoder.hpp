#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "ReceiveDecoder.hpp"
#include "SocketCan.hpp"

namespace app_plugin
{
class CanReceiveDecoder final : public ReceiveDecoder
{
public:
    void process(const app::Context &context) override;

private:
    using Publisher = LatestChannel<ECSData>::Publisher;
    void load_config();
    void initialize_endpoint(const app::Context &context);
    bool connect_can();
    void decode(const can_frame &frame, std::chrono::steady_clock::time_point now);
    ECSData make_snapshot(std::chrono::steady_clock::time_point now) const;

    std::optional<Publisher> m_output;
    hardware_io::SocketCan m_can;
    std::string m_interface = "can0";
    AimMode m_rune_mode = AimMode::BigRune;
    MyColor m_default_color = MyColor::Red;
    std::chrono::milliseconds m_feedback_timeout{50};
    std::chrono::milliseconds m_pair_tolerance{20};
    std::chrono::milliseconds m_poll_timeout{4};
    std::chrono::milliseconds m_reconnect_delay{500};
    std::chrono::steady_clock::time_point m_last_203{};
    std::chrono::steady_clock::time_point m_last_204{};
    double m_roll = 0.0;
    double m_yaw = 0.0;
    double m_pitch = 0.0;
    std::uint8_t m_task_mode = 0;
    std::uint8_t m_self_team = 0;
    bool m_fire_permit = false;
    bool m_have_203 = false;
    bool m_have_204 = false;
    bool m_require_buff_task_mode = true;
    bool m_config_loaded = false;
};
} // namespace app_plugin
