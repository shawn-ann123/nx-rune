#include "CanSendEncoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>

#include <glog/logging.h>
#include "class_loader.hpp"
#include "json.hpp"

namespace
{
void write_i16_le(std::uint8_t *bytes, std::int16_t value)
{
    const std::uint16_t raw = static_cast<std::uint16_t>(value);
    bytes[0] = static_cast<std::uint8_t>(raw & 0xFFU);
    bytes[1] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
}

std::int16_t degrees_to_centidegrees(double value, double minimum, double maximum)
{
    const double bounded = std::clamp(value, minimum, maximum);
    const long rounded = std::lround(bounded * 100.0);
    return static_cast<std::int16_t>(std::clamp(rounded, -32768L, 32767L));
}
} // namespace

namespace app_plugin
{
void CanSendEncoder::load_config()
{
    J_SEND_ENCODER.updateJson();
    if (!J_SEND_ENCODER.config_.isOpened()) throw std::runtime_error("failed to open config/can.json");
    const cv::FileNode can = J_SEND_ENCODER.config_["can"];
    const cv::FileNode send = J_SEND_ENCODER.config_["send"];
    if (can.empty() || send.empty()) throw std::runtime_error("can.json needs can and send objects");
    if (!can["interface"].empty()) can["interface"] >> m_interface;
    if (!send["period_ms"].empty())
        m_period = std::chrono::milliseconds(std::max(1, static_cast<int>(send["period_ms"])));
    if (!send["result_timeout_ms"].empty())
        m_result_timeout = std::chrono::milliseconds(std::max(1, static_cast<int>(send["result_timeout_ms"])));
    if (!can["reconnect_delay_ms"].empty())
        m_reconnect_delay = std::chrono::milliseconds(std::max(1, static_cast<int>(can["reconnect_delay_ms"])));
    if (!send["yaw_offset_deg"].empty()) send["yaw_offset_deg"] >> m_yaw_offset_deg;
    if (!send["pitch_offset_deg"].empty()) send["pitch_offset_deg"] >> m_pitch_offset_deg;
    if (!send["yaw_scale"].empty()) send["yaw_scale"] >> m_yaw_scale;
    if (!send["pitch_scale"].empty()) send["pitch_scale"] >> m_pitch_scale;
    if (!send["yaw_min_deg"].empty()) send["yaw_min_deg"] >> m_yaw_min_deg;
    if (!send["yaw_max_deg"].empty()) send["yaw_max_deg"] >> m_yaw_max_deg;
    if (!send["pitch_min_deg"].empty()) send["pitch_min_deg"] >> m_pitch_min_deg;
    if (!send["pitch_max_deg"].empty()) send["pitch_max_deg"] >> m_pitch_max_deg;
    if (!send["shoot_buff_mode"].empty()) m_buff_mode = static_cast<int>(send["shoot_buff_mode"]);
    if (!send["shoot_mode_when_fire"].empty()) m_fire_mode = static_cast<int>(send["shoot_mode_when_fire"]);
    if (m_interface.empty()) throw std::runtime_error("can.interface cannot be empty");
    if (m_period >= std::chrono::milliseconds(50))
        throw std::runtime_error("send.period_ms must remain below the MCU 50 ms freshness limit");
    if (m_buff_mode > 3 || (m_fire_mode != 2 && m_fire_mode != 3))
        throw std::runtime_error("invalid shoot_buff_mode or shoot_mode_when_fire");
    if (m_yaw_min_deg > m_yaw_max_deg || m_pitch_min_deg > m_pitch_max_deg)
        throw std::runtime_error("invalid send angle limits");
    m_next_send = std::chrono::steady_clock::now();
    m_config_loaded = true;
}

void CanSendEncoder::initialize_endpoint(const app::Context &context)
{
    if (!m_input) m_input.emplace(context.get_buffer_subscriber<FireResult>(this));
}

bool CanSendEncoder::connect_can()
{
    try
    {
        m_can.open(m_interface);
        LOG(INFO) << "[SendEncoder] sending 0x100 on " << m_interface
                  << " every " << m_period.count() << " ms";
        return true;
    }
    catch (const std::exception &error)
    {
        LOG_EVERY_N(ERROR, 10) << "[SendEncoder] SocketCAN open failed: " << error.what();
        return false;
    }
}

can_frame CanSendEncoder::build_frame(std::chrono::steady_clock::time_point now)
{
    const bool fresh = m_latest && now - m_last_result <= m_result_timeout;
    const bool found = fresh && m_latest->is_find_buff;
    const bool fire_requested = found && m_latest->is_enable_fire;

    if (fire_requested && !m_previous_fire_request) m_single_fire_pending = true;
    m_previous_fire_request = fire_requested;

    std::uint8_t shoot_mode = found ? 1U : 0U;
    if (m_fire_mode == 2U && fire_requested)
        shoot_mode = 2U;
    else if (m_fire_mode == 3U && m_single_fire_pending)
    {
        shoot_mode = 3U;
        m_single_fire_pending = false;
    }

    const std::uint8_t aim_state = found ? 3U : 2U;
    const std::uint8_t flags = static_cast<std::uint8_t>(
        (shoot_mode & 0x03U) | ((m_buff_mode & 0x03U) << 2U) | ((aim_state & 0x03U) << 4U));

    can_frame frame{};
    frame.can_id = 0x100U;
    frame.can_dlc = 8;
    frame.data[0] = 0x33U;
    frame.data[1] = flags;
    frame.data[2] = m_frame_count++;
    frame.data[3] = 0xEEU;
    if (found)
    {
        const double pitch = m_pitch_scale * m_latest->pitch + m_pitch_offset_deg;
        const double yaw = m_yaw_scale * m_latest->yaw + m_yaw_offset_deg;
        write_i16_le(frame.data + 4, degrees_to_centidegrees(pitch, m_pitch_min_deg, m_pitch_max_deg));
        write_i16_le(frame.data + 6, degrees_to_centidegrees(yaw, m_yaw_min_deg, m_yaw_max_deg));
    }
    return frame;
}

void CanSendEncoder::process(const app::Context &context)
{
    if (!m_config_loaded) load_config();
    initialize_endpoint(context);

    if (const std::optional<FireResult> result = m_input->try_pop())
    {
        if (std::isfinite(result->yaw) && std::isfinite(result->pitch))
        {
            m_latest = *result;
            m_last_result = std::chrono::steady_clock::now();
        }
        else
        {
            LOG_EVERY_N(ERROR, 20) << "[SendEncoder] rejected non-finite pitch/yaw";
            m_latest.reset();
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < m_next_send) std::this_thread::sleep_until(m_next_send);
    const auto send_time = std::chrono::steady_clock::now();
    m_next_send += m_period;
    if (send_time > m_next_send + 4 * m_period) m_next_send = send_time + m_period;

    if (!m_can.is_open() && !connect_can())
    {
        std::this_thread::sleep_for(m_reconnect_delay);
        return;
    }
    const can_frame frame = build_frame(send_time);
    if (!m_can.send(frame))
    {
        LOG_EVERY_N(ERROR, 20) << "[SendEncoder] 0x100 transmit failed; reconnecting";
        m_can.close();
    }
}
} // namespace app_plugin

REGISTER_PLUGIN("SendEncoder", app_plugin::CanSendEncoder)
