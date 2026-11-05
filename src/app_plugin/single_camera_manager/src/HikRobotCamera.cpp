#include "HikRobotCamera.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <thread>

#include <glog/logging.h>
#include <opencv2/imgproc.hpp>

#ifdef RP_HAS_HIKROBOT_MVS
#include <MvCameraControl.h>
#endif

namespace app_plugin
{
class HikRobotCamera::Impl
{
public:
#ifdef RP_HAS_HIKROBOT_MVS
    ~Impl() { close(); }

    bool open(int device_index, int width, int height, double fps,
              double exposure_ms, double gain)
    {
        close();

        MV_CC_DEVICE_INFO_LIST devices{};
        int result = MV_CC_EnumDevices(MV_USB_DEVICE | MV_GIGE_DEVICE, &devices);
        if (result != MV_OK || devices.nDeviceNum == 0)
        {
            LOG(ERROR) << "[HikRobotCamera] no USB/GigE MVS camera found, code=0x"
                       << std::hex << result;
            return false;
        }
        if (device_index < 0 || static_cast<unsigned int>(device_index) >= devices.nDeviceNum)
        {
            LOG(ERROR) << "[HikRobotCamera] device_index=" << device_index
                       << " is outside [0," << devices.nDeviceNum << ')';
            return false;
        }

        result = MV_CC_CreateHandle(&m_handle, devices.pDeviceInfo[device_index]);
        if (result != MV_OK || m_handle == nullptr)
            return fail("MV_CC_CreateHandle", result);
        result = MV_CC_OpenDevice(m_handle);
        if (result != MV_OK)
        {
            fail("MV_CC_OpenDevice", result);
            close();
            return false;
        }

        // These settings mirror the original vehicle project.  The model consumes
        // 8-bit BayerRG frames which are demosaiced on the capture thread.
        MV_CC_SetEnumValue(m_handle, "PixelFormat", PixelType_Gvsp_BayerRG8);
        MV_CC_SetEnumValue(m_handle, "AcquisitionMode", MV_ACQ_MODE_CONTINUOUS);
        MV_CC_SetEnumValue(m_handle, "BalanceWhiteAuto", MV_BALANCEWHITE_AUTO_OFF);
        MV_CC_SetEnumValue(m_handle, "ExposureAuto", MV_EXPOSURE_AUTO_MODE_OFF);
        MV_CC_SetEnumValue(m_handle, "GainAuto", MV_GAIN_MODE_OFF);

        if (!set_centered_roi(width, height))
        {
            close();
            return false;
        }
        MV_CC_SetFloatValue(m_handle, "ExposureTime", static_cast<float>(exposure_ms * 1000.0));
        MV_CC_SetFloatValue(m_handle, "Gain", static_cast<float>(gain));
        MV_CC_SetBoolValue(m_handle, "AcquisitionFrameRateEnable", true);
        result = MV_CC_SetFrameRate(m_handle, static_cast<float>(fps));
        if (result != MV_OK)
            result = MV_CC_SetFloatValue(m_handle, "AcquisitionFrameRate", static_cast<float>(fps));
        if (result != MV_OK)
            LOG(WARNING) << "[HikRobotCamera] failed to set " << fps << " FPS, code=0x"
                         << std::hex << result;

        detect_timestamp_frequency();
        result = MV_CC_StartGrabbing(m_handle);
        if (result != MV_OK)
        {
            fail("MV_CC_StartGrabbing", result);
            close();
            return false;
        }

        m_running.store(true, std::memory_order_release);
        m_capture_thread = std::thread(&Impl::capture_loop, this);
        LOG(INFO) << "[HikRobotCamera] opened device " << device_index << " at "
                  << width << 'x' << height << " @ " << fps << " FPS, exposure="
                  << exposure_ms << " ms, gain=" << gain;
        return true;
    }

    bool read(cv::Mat &image, timetool::Timestamp &timestamp,
              std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_frame_mutex);
        if (!m_frame_ready.wait_for(lock, timeout, [this] {
                return m_has_new_frame || !m_running.load(std::memory_order_acquire);
            }))
            return false;
        if (!m_has_new_frame || m_latest_frame.empty())
            return false;
        image = m_latest_frame;
        timestamp = m_latest_timestamp;
        m_has_new_frame = false;
        return true;
    }

    void close()
    {
        m_running.store(false, std::memory_order_release);
        m_frame_ready.notify_all();
        if (m_capture_thread.joinable())
            m_capture_thread.join();
        if (m_handle != nullptr)
        {
            MV_CC_StopGrabbing(m_handle);
            MV_CC_CloseDevice(m_handle);
            MV_CC_DestroyHandle(m_handle);
            m_handle = nullptr;
        }
        std::lock_guard lock(m_frame_mutex);
        m_latest_frame.release();
        m_has_new_frame = false;
        m_timestamp_offset_initialized = false;
    }

    [[nodiscard]] bool is_open() const { return m_handle != nullptr; }

private:
    bool fail(const char *operation, int result) const
    {
        LOG(ERROR) << "[HikRobotCamera] " << operation << " failed, code=0x"
                   << std::hex << result;
        return false;
    }

    bool set_centered_roi(int requested_width, int requested_height)
    {
        MVCC_INTVALUE width_max{};
        MVCC_INTVALUE height_max{};
        if (MV_CC_GetIntValue(m_handle, "WidthMax", &width_max) != MV_OK ||
            MV_CC_GetIntValue(m_handle, "HeightMax", &height_max) != MV_OK)
        {
            LOG(ERROR) << "[HikRobotCamera] cannot query sensor dimensions";
            return false;
        }
        const int width = std::min(requested_width, static_cast<int>(width_max.nCurValue)) / 16 * 16;
        const int height = std::min(requested_height, static_cast<int>(height_max.nCurValue)) / 2 * 2;
        const int offset_x = ((static_cast<int>(width_max.nCurValue) - width) / 2) / 2 * 2;
        const int offset_y = ((static_cast<int>(height_max.nCurValue) - height) / 2) / 2 * 2;
        if (width <= 0 || height <= 0)
            return false;

        int result = MV_CC_SetIntValue(m_handle, "OffsetX", 0);
        result |= MV_CC_SetIntValue(m_handle, "OffsetY", 0);
        result |= MV_CC_SetIntValue(m_handle, "Width", width);
        result |= MV_CC_SetIntValue(m_handle, "Height", height);
        if (result == MV_OK)
        {
            result |= MV_CC_SetIntValue(m_handle, "OffsetX", offset_x);
            result |= MV_CC_SetIntValue(m_handle, "OffsetY", offset_y);
        }
        if (result != MV_OK)
            return fail("set centered ROI", result);
        return true;
    }

    void detect_timestamp_frequency()
    {
        MVCC_INTVALUE frequency{};
        if (MV_CC_GetIntValue(m_handle, "GevTimestampTickFrequency", &frequency) == MV_OK &&
            frequency.nCurValue > 0)
        {
            m_tick_frequency_hz = static_cast<double>(frequency.nCurValue);
        }
        else
        {
            // Preserve the original car's verified fallback for its USB/firmware combination.
            m_tick_frequency_hz = 100000000.0;
            LOG(WARNING) << "[HikRobotCamera] timestamp frequency unavailable; using vehicle "
                            "fallback 100 MHz";
        }
        LOG(INFO) << "[HikRobotCamera] timestamp frequency=" << m_tick_frequency_hz << " Hz";
    }

    void capture_loop()
    {
        while (m_running.load(std::memory_order_acquire))
        {
            MV_FRAME_OUT frame{};
            const int result = MV_CC_GetImageBuffer(m_handle, &frame, 100);
            if (result != MV_OK)
                continue;

            cv::Mat converted;
            const int width = static_cast<int>(frame.stFrameInfo.nWidth);
            const int height = static_cast<int>(frame.stFrameInfo.nHeight);
            if (frame.stFrameInfo.enPixelType == PixelType_Gvsp_Mono8)
            {
                cv::cvtColor(cv::Mat(height, width, CV_8UC1, frame.pBufAddr),
                             converted, cv::COLOR_GRAY2BGR);
            }
            else
            {
                cv::cvtColor(cv::Mat(height, width, CV_8UC1, frame.pBufAddr),
                             converted, cv::COLOR_BayerRG2RGB);
            }

            const std::uint64_t ticks =
                (static_cast<std::uint64_t>(frame.stFrameInfo.nDevTimeStampHigh) << 32U) |
                static_cast<std::uint64_t>(frame.stFrameInfo.nDevTimeStampLow);
            const auto camera_duration = std::chrono::duration_cast<timetool::Timestamp::duration>(
                std::chrono::duration<double>(static_cast<double>(ticks) / m_tick_frequency_hz));
            const timetool::Timestamp now = timetool::now();
            if (!m_timestamp_offset_initialized)
            {
                m_timestamp_offset = now.time_since_epoch() - camera_duration;
                m_timestamp_offset_initialized = true;
            }
            const timetool::Timestamp capture_timestamp(m_timestamp_offset + camera_duration);

            {
                std::lock_guard lock(m_frame_mutex);
                m_latest_frame = std::move(converted);
                m_latest_timestamp = capture_timestamp;
                m_has_new_frame = true;
            }
            m_frame_ready.notify_one();
            MV_CC_FreeImageBuffer(m_handle, &frame);
        }
    }

    void *m_handle = nullptr;
    std::atomic<bool> m_running{false};
    std::thread m_capture_thread;
    std::mutex m_frame_mutex;
    std::condition_variable m_frame_ready;
    cv::Mat m_latest_frame;
    timetool::Timestamp m_latest_timestamp{};
    bool m_has_new_frame = false;
    double m_tick_frequency_hz = 100000000.0;
    timetool::Timestamp::duration m_timestamp_offset{};
    bool m_timestamp_offset_initialized = false;
#else
    bool open(int, int, int, double, double, double)
    {
        LOG(ERROR) << "[HikRobotCamera] this build has no HikRobot MVS SDK; install MVS under "
                      "/opt/MVS and rebuild, or select backend=v4l2/gstreamer";
        return false;
    }
    bool read(cv::Mat &, timetool::Timestamp &, std::chrono::milliseconds) { return false; }
    void close() {}
    [[nodiscard]] bool is_open() const { return false; }
#endif
};

HikRobotCamera::HikRobotCamera() : m_impl(std::make_unique<Impl>()) {}
HikRobotCamera::~HikRobotCamera() = default;

bool HikRobotCamera::sdk_available()
{
#ifdef RP_HAS_HIKROBOT_MVS
    return true;
#else
    return false;
#endif
}

bool HikRobotCamera::open(int device_index, int width, int height, double fps,
                          double exposure_ms, double gain)
{
    return m_impl->open(device_index, width, height, fps, exposure_ms, gain);
}

bool HikRobotCamera::read(cv::Mat &image, timetool::Timestamp &timestamp,
                          std::chrono::milliseconds timeout)
{
    return m_impl->read(image, timestamp, timeout);
}

void HikRobotCamera::close() { m_impl->close(); }
bool HikRobotCamera::is_open() const { return m_impl->is_open(); }
} // namespace app_plugin
