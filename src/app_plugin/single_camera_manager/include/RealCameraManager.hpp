#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <opencv2/videoio.hpp>
#include "CameraManager.hpp"
#include "HikRobotCamera.hpp"

namespace app_plugin
{
class RealCameraManager final : public CameraManager
{
public:
    void process(const app::Context &context) override;

private:
    using EcsSubscriber = LatestChannel<ECSData>::Subscriber;
    using FramePublisher = LatestBuffer<InputFrame>::Publisher;
    void load_config();
    void initialize_endpoints(const app::Context &context);
    bool open_camera();
    bool capture(cv::Mat &image, timetool::Timestamp &timestamp);

    std::optional<EcsSubscriber> m_ecs_input;
    std::optional<FramePublisher> m_output;
    cv::VideoCapture m_capture;
    HikRobotCamera m_hikrobot_camera;
    std::string m_device;
    int m_device_index = 0;
    std::string m_pipeline;
    std::string m_backend = "v4l2";
    std::string m_fourcc = "MJPG";
    int m_width = 1440;
    int m_height = 1080;
    int m_fps = 30;
    int m_buffer_size = 1;
    double m_exposure = -1.0;
    double m_gain = -1.0;
    double m_auto_exposure = -1.0;
    bool m_flip_180 = false;
    std::chrono::milliseconds m_capture_timeout{50};
    std::chrono::milliseconds m_reconnect_delay{500};
    bool m_config_loaded = false;
};
} // namespace app_plugin
