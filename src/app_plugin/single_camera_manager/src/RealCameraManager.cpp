#include "RealCameraManager.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>

#include <glog/logging.h>
#include <opencv2/imgproc.hpp>
#include "class_loader.hpp"
#include "json.hpp"
#include "time/time.hpp"

namespace app_plugin
{
void RealCameraManager::load_config()
{
    J_CAMERA.updateJson();
    if (!J_CAMERA.config_.isOpened()) throw std::runtime_error("failed to open config/camera.json");
    const cv::FileNode camera = J_CAMERA.config_["camera"];
    if (camera.empty() || !camera.isMap())
        throw std::runtime_error("camera.json must contain a camera object");
    if (!camera["device"].empty()) camera["device"] >> m_device;
    if (!camera["device_index"].empty()) camera["device_index"] >> m_device_index;
    if (!camera["pipeline"].empty()) camera["pipeline"] >> m_pipeline;
    if (!camera["backend"].empty()) camera["backend"] >> m_backend;
    if (!camera["fourcc"].empty()) camera["fourcc"] >> m_fourcc;
    if (!camera["width"].empty()) camera["width"] >> m_width;
    if (!camera["height"].empty()) camera["height"] >> m_height;
    if (!camera["fps"].empty()) camera["fps"] >> m_fps;
    if (!camera["buffer_size"].empty()) camera["buffer_size"] >> m_buffer_size;
    if (!camera["exposure"].empty()) camera["exposure"] >> m_exposure;
    if (!camera["gain"].empty()) camera["gain"] >> m_gain;
    if (!camera["auto_exposure"].empty()) camera["auto_exposure"] >> m_auto_exposure;
    if (!camera["flip_180"].empty()) camera["flip_180"] >> m_flip_180;
    if (!camera["capture_timeout_ms"].empty())
        m_capture_timeout = std::chrono::milliseconds(
            std::max(1, static_cast<int>(camera["capture_timeout_ms"])));
    if (!camera["reconnect_delay_ms"].empty())
        m_reconnect_delay = std::chrono::milliseconds(
            std::max(1, static_cast<int>(camera["reconnect_delay_ms"])));
    if (m_width <= 0 || m_height <= 0 || m_fps <= 0)
        throw std::runtime_error("camera width, height and fps must be positive");
    if (m_fourcc.size() != 4) throw std::runtime_error("camera fourcc must contain four characters");
    m_config_loaded = true;
}

void RealCameraManager::initialize_endpoints(const app::Context &context)
{
    if (!m_ecs_input) m_ecs_input.emplace(context.get_channel_subscriber<ECSData>(this, {}, true));
    if (!m_output) m_output.emplace(context.get_buffer_publisher<InputFrame>(this));
}

bool RealCameraManager::open_camera()
{
    if (m_backend == "hikrobot")
    {
        if (m_hikrobot_camera.is_open()) return true;
        return m_hikrobot_camera.open(
            m_device_index, m_width, m_height, m_fps, m_exposure, m_gain);
    }
    const int backend = m_backend == "gstreamer" ? cv::CAP_GSTREAMER
                      : m_backend == "any" ? cv::CAP_ANY : cv::CAP_V4L2;
    const std::string source = !m_pipeline.empty() ? m_pipeline
                             : !m_device.empty() ? m_device
                             : std::to_string(m_device_index);
    const bool opened = !m_pipeline.empty() || !m_device.empty()
        ? m_capture.open(source, backend)
        : m_capture.open(m_device_index, backend);
    if (!opened)
    {
        LOG_EVERY_N(ERROR, 10) << "[CameraManager] cannot open " << source;
        return false;
    }
    if (m_pipeline.empty())
    {
        m_capture.set(cv::CAP_PROP_FOURCC,
            cv::VideoWriter::fourcc(m_fourcc[0], m_fourcc[1], m_fourcc[2], m_fourcc[3]));
        m_capture.set(cv::CAP_PROP_FRAME_WIDTH, m_width);
        m_capture.set(cv::CAP_PROP_FRAME_HEIGHT, m_height);
        m_capture.set(cv::CAP_PROP_FPS, m_fps);
        m_capture.set(cv::CAP_PROP_BUFFERSIZE, m_buffer_size);
        if (m_auto_exposure >= 0.0) m_capture.set(cv::CAP_PROP_AUTO_EXPOSURE, m_auto_exposure);
        if (m_exposure >= 0.0) m_capture.set(cv::CAP_PROP_EXPOSURE, m_exposure);
        if (m_gain >= 0.0) m_capture.set(cv::CAP_PROP_GAIN, m_gain);
    }
    LOG(INFO) << "[CameraManager] opened " << source << " at "
              << m_capture.get(cv::CAP_PROP_FRAME_WIDTH) << 'x'
              << m_capture.get(cv::CAP_PROP_FRAME_HEIGHT) << " @ "
              << m_capture.get(cv::CAP_PROP_FPS) << " fps";
    return true;
}

bool RealCameraManager::capture(cv::Mat &image, timetool::Timestamp &timestamp)
{
    if (m_backend == "hikrobot")
    {
        if (!m_hikrobot_camera.is_open() && !open_camera()) return false;
        if (!m_hikrobot_camera.read(image, timestamp, m_capture_timeout))
        {
            LOG_EVERY_N(ERROR, 10) << "[CameraManager] HikRobot frame timeout; reconnecting";
            m_hikrobot_camera.close();
            return false;
        }
        if (m_flip_180) cv::flip(image, image, -1);
        return true;
    }
    if (!m_capture.isOpened() && !open_camera()) return false;
    if (!m_capture.grab())
    {
        LOG_EVERY_N(ERROR, 10) << "[CameraManager] frame grab failed; reconnecting";
        m_capture.release();
        return false;
    }
    timestamp = timetool::now();
    if (!m_capture.retrieve(image) || image.empty())
    {
        LOG_EVERY_N(ERROR, 10) << "[CameraManager] frame retrieve failed; reconnecting";
        m_capture.release();
        return false;
    }
    if (m_flip_180) cv::flip(image, image, -1);
    if (image.cols != m_width || image.rows != m_height)
    {
        LOG_EVERY_N(WARNING, 120) << "[CameraManager] actual frame size " << image.cols << 'x'
                                  << image.rows << " differs from configured "
                                  << m_width << 'x' << m_height;
    }
    return true;
}

void RealCameraManager::process(const app::Context &context)
{
    if (!m_config_loaded) load_config();
    initialize_endpoints(context);
    cv::Mat image;
    timetool::Timestamp timestamp;
    if (!capture(image, timestamp))
    {
        std::this_thread::sleep_for(m_reconnect_delay);
        return;
    }
    const std::shared_ptr<const ECSData> ecs = m_ecs_input->wait_next();
    InputFrame frame{*ecs, timestamp, std::move(image)};
    m_output->push(std::move(frame));
}
} // namespace app_plugin

REGISTER_PLUGIN("CameraManager", app_plugin::RealCameraManager)
