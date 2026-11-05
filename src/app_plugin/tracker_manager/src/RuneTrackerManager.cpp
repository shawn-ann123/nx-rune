#include "RuneTrackerManager.hpp"

#include <array>
#include <stdexcept>
#include <utility>

#include <Eigen/Geometry>
#include <glog/logging.h>
#include <opencv2/imgproc.hpp>

#include "class_loader.hpp"
#include "json.hpp"
#include "power_rune_interface.hpp"

namespace app_plugin
{

void RuneTrackerManager::initialize_endpoints(const app::Context &context)
{
    if (!m_input)
        m_input.emplace(context.get_buffer_subscriber<InputFrameWithNNResults>(this));
    if (!m_output)
        m_output.emplace(context.get_buffer_publisher<TrackResult>(this));
}

void RuneTrackerManager::initialize_tf_tree()
{
    J_TRACK.updateJson();
    if (!J_TRACK.config_.isOpened())
        throw std::runtime_error(
            "failed to open tracker config: " +
            (TRACKER_CONFIG_DIR / "track.json").string());

    const cv::FileNode transform = J_TRACK.config_["transform"];
    const cv::FileNode gimbal_true = J_TRACK.config_["G_R_G_true"];
    m_x_offset_m = static_cast<double>(transform["x_offset"]) / 1000.0;
    m_y_offset_m = static_cast<double>(transform["y_offset"]) / 1000.0;
    m_z_offset_m = static_cast<double>(transform["z_offset"]) / 1000.0;
    const cv::FileNode rotation = transform["camera_to_gimbal_rotation"];
    if (!rotation.empty())
    {
        if (!rotation.isSeq() || rotation.size() != 9)
            throw std::runtime_error("track.json camera_to_gimbal_rotation must contain 9 values");
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                m_gimbal_from_camera(row, col) =
                    static_cast<double>(rotation[static_cast<std::size_t>(row * 3 + col)]);
    }
    m_gimbal_true_y_degree = static_cast<double>(gimbal_true["Y_rotation_angle"]);
    m_gimbal_true_x_degree = static_cast<double>(gimbal_true["X_rotation_angle"]);

    // 车系与云台系使用同一套约定：x 前、y 左、z 上，两帧重合。
    m_tf_tree.add_TF(transform_tools::TF(), car_frame, ecs_world_frame);

    m_tf_tree.add_TF(transform_tools::TF(), ecs_world_frame, gimbal_frame);
    m_tf_tree.add_TF(
        transform_tools::TF(Eigen::Vector3d(m_x_offset_m, m_y_offset_m, m_z_offset_m)),
        gimbal_frame,
        camera_frame);

    m_tf_tree[camera_frame].set_rotation(m_gimbal_from_camera);
    m_tf_tree.add_TF(transform_tools::TF(), car_frame, unbiased_camera_frame);
    m_tf_initialized = true;

    LOG(INFO) << "[TrackerManager] 26-style TFTree initialized; camera_offset_m=("
              << m_x_offset_m << ", " << m_y_offset_m << ", " << m_z_offset_m << ')';
}

void RuneTrackerManager::update_tf_tree(const ECSData &ecs_data)
{
    using transform_tools::Angle;
    using transform_tools::Pitch;
    using transform_tools::Roll;
    using transform_tools::RotationAxis;
    using transform_tools::Yaw;

    // 电控角速度约定：yaw 增大为 ω=-z(右转)、pitch 增大为 ω=-y(抬头)，
    // 与 Rz(+)/Ry(+) 隐含的 +z/+y 相反，因此车系姿态里两角都取负。
    const Eigen::Matrix3d ecs_from_gimbal =
        Yaw(Angle::from_degree(-ecs_data.yaw), RotationAxis::Z).to_rotation_mat() *
        Pitch(Angle::from_degree(-ecs_data.pitch), RotationAxis::Y).to_rotation_mat() *
        Roll(Angle::from_degree(ecs_data.roll), RotationAxis::X).to_rotation_mat();

    const Eigen::Matrix3d gimbal_bias =
        (Eigen::AngleAxisd(
             Angle::from_degree(m_gimbal_true_y_degree).to_rad(),
             Eigen::Vector3d::UnitY()) *
         Eigen::AngleAxisd(
             Angle::from_degree(m_gimbal_true_x_degree).to_rad(),
             Eigen::Vector3d::UnitX()))
            .matrix();

    m_tf_tree[gimbal_frame].set_rotation(ecs_from_gimbal * gimbal_bias);

    const transform_tools::TF car_from_camera = m_tf_tree.calculate_absolute_TF(camera_frame);
    m_tf_tree[unbiased_camera_frame].setTF(
        car_from_camera.get_offset(),
        car_from_camera.calculate_yaw());
}

void RuneTrackerManager::process(const app::Context &context)
{
    initialize_endpoints(context);
    if (!m_tf_initialized)
        initialize_tf_tree();

    InputFrameWithNNResults frame = m_input->wait_pop();
    const ECSData &ecs_data = frame.input_frame.ecs_data;
    update_tf_tree(ecs_data);

    const bool is_rune_mode =
        ecs_data.mode == AimMode::SmallRune || ecs_data.mode == AimMode::BigRune;

    if (is_rune_mode)
    {
        power_rune::RuneInput rune_input;
        rune_input.is_big_rune = ecs_data.mode == AimMode::BigRune;
        rune_input.ori_mat = frame.input_frame.img;
        rune_input.tf_tree = m_tf_tree;
        rune_input.timestamp = frame.input_frame.timestamp;
        rune_input.cd_my_color = static_cast<int>(ecs_data.my_color);
        rune_input.nn_rune_infos.reserve(frame.nn_rune_infos.size());

        for (const NNRuneInfo &detection : frame.nn_rune_infos)
        {
            power_rune::RuneInput::NNRuneInfo converted;
            converted.top = detection.top;
            converted.left = detection.left;
            converted.right = detection.right;
            converted.bottom = detection.bottom;
            converted.point_R = detection.point_R;
            converted.class_id = detection.class_id;
            rune_input.nn_rune_infos.emplace_back(converted);
        }

        power_rune::process_power_rune(rune_input);
    }

    cv::Mat overlay = frame.input_frame.img.clone();
    for (const NNRuneInfo &detection : frame.nn_rune_infos)
    {
        const std::array<cv::Point, 4> blade{
            detection.top, detection.right, detection.bottom, detection.left};
        for (std::size_t index = 0; index < blade.size(); ++index)
        {
            cv::line(overlay, blade[index], blade[(index + 1) % blade.size()],
                     cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
            cv::circle(overlay, blade[index], 4, cv::Scalar(0, 255, 255), -1, cv::LINE_AA);
        }
        cv::circle(overlay, detection.point_R, 5, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
    }

    TrackResult result{};
    result.outpost.armor_state.setZero();
    result.outpost.height = OutPost::Low;
    result.outpost.is_tracking = false;
    result.img = std::move(overlay);
    result.timestamp = frame.input_frame.timestamp;
    result.tf_tree = m_tf_tree;
    result.mode = ecs_data.mode;
    m_output->push(std::move(result));
}

} // namespace app_plugin

REGISTER_PLUGIN("TrackerManager", app_plugin::RuneTrackerManager)
