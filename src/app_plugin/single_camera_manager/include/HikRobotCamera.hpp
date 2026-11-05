#pragma once

#include <chrono>
#include <memory>

#include <opencv2/core.hpp>

#include "time/time.hpp"

namespace app_plugin
{
class HikRobotCamera
{
public:
    HikRobotCamera();
    ~HikRobotCamera();

    HikRobotCamera(const HikRobotCamera &) = delete;
    HikRobotCamera &operator=(const HikRobotCamera &) = delete;

    [[nodiscard]] static bool sdk_available();
    bool open(int device_index, int width, int height, double fps,
              double exposure_ms, double gain);
    bool read(cv::Mat &image, timetool::Timestamp &timestamp,
              std::chrono::milliseconds timeout);
    void close();
    [[nodiscard]] bool is_open() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace app_plugin
