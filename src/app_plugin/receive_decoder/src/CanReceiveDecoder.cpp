#include "CanReceiveDecoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>

#include <glog/logging.h>
#include "class_loader.hpp"
#include "json.hpp"

namespace
{
std::int16_t read_i16_le(const std::uint8_t *bytes)
{
    const std::uint16_t raw = static_cast<std::uint16_t>(bytes[0]) |
                              (static_cast<std::uint16_t>(bytes[1]) << 8U);
    return static_cast<std::int16_t>(raw);
}

AimMode parse_rune_mode(int mode)
{
    if (mode == static_cast<int>(AimMode::SmallRune)) return AimMode::SmallRune;
    if (mode == static_cast<int>(AimMode::BigRune)) return AimMode::BigRune;
    throw std::runtime_error("can.rune_mode must be 2 (small rune) or 3 (big rune)");
}
} // namespace

namespace app_plugin
{
void CanReceiveDecoder::load_config()
{
    J_RECEIVE_DECODER.updateJson();
    if (!J_RECEIVE_DECODER.config_.isOpened())
        throw std::runtime_error("failed to open config/can.json");
    const cv::FileNode can = J_RECEIVE_DECODER.config_["can"];
    if (can.empty() || !can.isMap()) throw std::runtime_error("can.json must contain a can object");

    if (!can["interface"].empty()) can["interface"] >> m_interface;
    if (!can["rune_mode"].empty()) m_rune_mode = parse_rune_mode(static_cast<int>(can["rune_mode"]));
    if (!can["default_team"].empty())
    {
        const int team = static_cast<int>(can["default_team"]);
        m_default_color = team == 1 ? MyColor::Blue : MyColor::Red;
    }
    if (!can["require_buff_task_mode"].empty())
        m_require_buff_task_mode = static_cast<int>(can["require_buff_task_mode"]) != 0;
    if (!can["feedback_timeout_ms"].empty())
        m_feedback_timeout = std::chrono::milliseconds(std::max(1, static_cast<int>(can["feedback_timeout_ms"])));
    if (!can["pair_tolerance_ms"].empty())
        m_pair_tolerance = std::chrono::milliseconds(std::max(1, static_cast<int>(can["pair_tolerance_ms"])));
    if (!can["poll_timeout_ms"].empty())
        m_poll_timeout = std::chrono::milliseconds(std::max(1, static_cast<int>(can["poll_timeout_ms"])));
    if (!can["reconnect_delay_ms"].empty())
        m_reconnect_delay = std::chrono::milliseconds(std::max(1, static_cast<int>(can["reconnect_delay_ms"])));
    if (m_interface.empty()) throw std::runtime_error("can.interface cannot be empty");
    m_config_loaded = true;
}

void CanReceiveDecoder::initialize_endpoint(const app::Context &context)
{
    if (!m_output) m_output.emplace(context.get_channel_publisher<ECSData>(this));
}

bool CanReceiveDecoder::connect_can()
{
    try
    {
        m_can.open(m_interface, {{0x203U, CAN_SFF_MASK}, {0x204U, CAN_SFF_MASK}});
        LOG(INFO) << "[ReceiveDecoder] listening on " << m_interface << " for 0x203/0x204";
        return true;
    }
    catch (const std::exception &error)
    {
        LOG_EVERY_N(ERROR, 10) << "[ReceiveDecoder] SocketCAN open failed: " << error.what();
        return false;
    }
}

void CanReceiveDecoder::decode(const can_frame &frame, std::chrono::steady_clock::time_point now)
{
    const canid_t id = frame.can_id & CAN_SFF_MASK;
    if (frame.can_dlc != 8) return;
    if (id == 0x203U)
    {
        if (frame.data[0] != 0x33U) return;
        m_task_mode = frame.data[1];
        m_self_team = frame.data[2];
        m_roll = static_cast<double>(read_i16_le(frame.data + 4)) / 100.0;
        m_yaw = static_cast<double>(read_i16_le(frame.data + 6)) / 100.0;
        m_last_203 = now;
        m_have_203 = true;
    }
    else if (id == 0x204U)
    {
        if (frame.data[7] != 0xEEU) return;
        m_pitch = static_cast<double>(read_i16_le(frame.data)) / 100.0;
        m_fire_permit = frame.data[4] != 0;
        m_last_204 = now;
        m_have_204 = true;
    }
}

ECSData CanReceiveDecoder::make_snapshot(std::chrono::steady_clock::time_point now) const
{
    ECSData data{};
    data.my_color = m_self_team == 0xBBU ? MyColor::Blue
                  : m_self_team == 0xAAU ? MyColor::Red : m_default_color;
    const bool fresh_203 = m_have_203 && now - m_last_203 <= m_feedback_timeout;
    const bool fresh_204 = m_have_204 && now - m_last_204 <= m_feedback_timeout;
    const auto separation = m_last_203 > m_last_204 ? m_last_203 - m_last_204 : m_last_204 - m_last_203;
    const bool paired = fresh_203 && fresh_204 && separation <= m_pair_tolerance;
    const bool buff_active = !m_require_buff_task_mode || m_task_mode == 0x02U;
    data.mode = paired && buff_active ? m_rune_mode : AimMode::AutoAim;
    data.is_start = paired && buff_active;
    data.is_ready = data.is_start && m_fire_permit;
    data.roll = paired ? m_roll : 0.0;
    data.yaw = paired ? m_yaw : 0.0;
    data.pitch = paired ? m_pitch : 0.0;
    return data;
}

void CanReceiveDecoder::process(const app::Context &context)
{
    if (!m_config_loaded) load_config();
    initialize_endpoint(context);
    if (!m_can.is_open() && !connect_can())
    {
        m_output->publish(std::make_shared<const ECSData>(make_snapshot(std::chrono::steady_clock::now())));
        std::this_thread::sleep_for(m_reconnect_delay);
        return;
    }

    try
    {
        const auto frame = m_can.receive(m_poll_timeout);
        const auto now = std::chrono::steady_clock::now();
        if (frame) decode(*frame, now);
        m_output->publish(std::make_shared<const ECSData>(make_snapshot(now)));
    }
    catch (const std::exception &error)
    {
        LOG_EVERY_N(ERROR, 10) << "[ReceiveDecoder] SocketCAN receive failed: " << error.what();
        m_can.close();
    }
}
} // namespace app_plugin

REGISTER_PLUGIN("ReceiveDecoder", app_plugin::CanReceiveDecoder)
