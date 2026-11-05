#include "RunePlannerControl.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <glog/logging.h>

#include "class_loader.hpp"
#include "img_viz.hpp"
#include "json.hpp"
#include "power_rune_interface.hpp"
#include "transform_tools/transform_tools.h"

namespace
{
double wrap_180(double degree)
{
    degree = std::fmod(degree + 180.0, 360.0);
    if (degree < 0.0) degree += 360.0;
    return degree - 180.0;
}
} // namespace

namespace app_plugin
{

void RunePlannerControl::initialize(const app::Context &context)
{
    if (!m_ecs_input)
        m_ecs_input.emplace(context.get_channel_subscriber<ECSData>(this, {}, true));
    if (!m_track_input)
        m_track_input.emplace(context.get_buffer_subscriber<TrackResult>(this));
    if (!m_output)
        m_output.emplace(context.get_buffer_publisher<FireResult>(this));

    J_FIRE_CONTROL.updateJson();
    if (!J_FIRE_CONTROL.config_.isOpened())
        throw std::runtime_error(
            "failed to open fire control config: " +
            (FIRE_CONTROL_CONFIG_DIR / "fire_control_system.json").string());
    m_visualize = static_cast<int>(
        J_FIRE_CONTROL.config_["visualize"]["enabled"]) != 0;
    m_initialized = true;
}

void RunePlannerControl::publish_status(const FireResult &result) const
{
    if (!m_visualize || !ImgViz::enabled() || !m_latest_ecs)
        return;

    // result.yaw 是车系的逆时针角，电控 yaw 顺时针为正，叠加前统一换成电控约定。
    const ECSData &ecs = *m_latest_ecs;
    const double target_yaw_ecs = -result.yaw;
    const char *mode_name = result.mode == AimMode::BigRune ? "BIG RUNE"
        : result.mode == AimMode::SmallRune ? "SMALL RUNE"
        : "NOT RUNE";
    std::ostringstream text;
    text << std::fixed << std::setprecision(2)
         << mode_name
         << "\ncur  yaw " << ecs.yaw << "   pitch " << ecs.pitch;
    if (result.is_find_buff)
    {
        text << "\ntgt  yaw " << target_yaw_ecs << "   pitch " << result.pitch
             << "\nerr  yaw " << wrap_180(target_yaw_ecs - ecs.yaw)
             << "   pitch " << (result.pitch - ecs.pitch);
    }
    else
    {
        text << "\ntgt  --\nerr  --";
    }
    text << "\nready " << (ecs.is_ready ? 1 : 0)
         << "  found " << (result.is_find_buff ? 1 : 0)
         << "  fire " << (result.is_enable_fire ? 1 : 0);
    ImgViz::set_stream_status(text.str());
}

void RunePlannerControl::process(const app::Context &context)
{
    if (!m_initialized)
        initialize(context);

    // 严格保持参考流程：先等待最新 ECS，再等待本轮 TrackResult。
    m_latest_ecs = *m_ecs_input->wait_next();
    m_track_input->wait_pop();

    FireResult result;
    result.mode = m_latest_ecs->mode;
    result.is_keep_shooting = false;

    if (result.mode == AimMode::SmallRune || result.mode == AimMode::BigRune)
    {
        const bool is_big_rune = result.mode == AimMode::BigRune;
        const power_rune::RuneSendData rune_data = power_rune::get_rune_data(is_big_rune);

        result.is_find_target = static_cast<bool>(rune_data.is_find_buff);
        result.is_find_buff = static_cast<bool>(rune_data.is_find_buff);
        result.is_enable_fire =
            static_cast<bool>(rune_data.is_enable_fire) && result.is_find_buff && m_latest_ecs->is_ready;

        // RuneSendData 在未找到目标时不保证 yaw/pitch 有效，因此只在 found 后读取；
        // 角度已是车体系绝对角，与电控 0x203 同一套约定，rad → deg 不做取反。
        if (result.is_find_buff)
        {
            result.yaw = transform_tools::Angle::from_rad(rune_data.yaw).to_degree();
            result.pitch = transform_tools::Angle::from_rad(rune_data.pitch).to_degree();
        }
    }

    const bool finite = std::isfinite(result.yaw) && std::isfinite(result.pitch);
    if (!finite)
    {
        LOG(ERROR) << "[PlannerControl] non-finite rune output rejected: yaw="
                   << result.yaw << ", pitch=" << result.pitch;
        result.yaw = 0.0;
        result.pitch = 0.0;
        result.is_find_target = false;
        result.is_find_buff = false;
        result.is_enable_fire = false;
    }

    publish_status(result);
    m_output->push(std::move(result));
}

} // namespace app_plugin

REGISTER_PLUGIN("PlannerControl", app_plugin::RunePlannerControl)
