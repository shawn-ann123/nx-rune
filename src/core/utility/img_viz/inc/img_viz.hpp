#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <opencv2/core.hpp>

/// @brief 进程级图像可视化管理器。
///
/// 实现位于 libimg_viz_lib.so，所有主程序和插件都会链接同一份 instance()，从而共享唯一的
/// HighGUI 线程。构造实例或设置开关不会启动线程；首次提交图像或 GUI 任务时才会启动。
class ImgViz
{
public:
    static ImgViz &instance();

    ImgViz(const ImgViz &) = delete;
    ImgViz &operator=(const ImgViz &) = delete;

    /// @brief 初始化可视化管理器。只能在启动时调用一次。
    static void init(bool enabled);

    [[nodiscard]] static bool enabled();

    /// @brief 深拷贝图像并加入待显示队列。
    static void enqueue_image_copy(std::string_view window_name, const cv::Mat &image);

    /// @brief 不复制像素数据地加入待显示队列。
    /// 调用方必须保证底层像素内存在显示完成前有效，且不被并发修改或复用。
    static void enqueue_image_zero_copy(std::string_view window_name, const cv::Mat &image);

    /// @brief 设置叠加在 debug 流左上角的状态文本，'\n' 分行；传空串清除。
    /// 与具体窗口无关，每帧发布时绘制一次，避免多个窗口互相覆盖。
    static void set_stream_status(std::string text);

private:
    ImgViz() = default;
    ~ImgViz();

    // 用于可视化线程缓启动
    void ensure_worker();
    void visualize();
    bool start_browser_server();
    void stop_browser_server();
    void accept_browser_clients();
    void serve_browser_client(int client_fd, bool private_client);
    void run_browser_client(int client_fd, bool private_client,
                            const std::shared_ptr<std::atomic<bool>> &finished);
    void reap_finished_browser_clients_locked();
    void publish_browser_frame(const cv::Mat &image);

    enum class State
    {
        Uninitialized,
        Enabled,
        Disabled,
        Stopping
    };

    enum class Backend
    {
        BrowserMjpeg,
        OpenCv
    };

    struct WindowState
    {
        cv::Mat image;
        bool is_dirty = false;
    };

    std::atomic<State> m_state{State::Uninitialized};
    Backend m_backend = Backend::BrowserMjpeg;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, WindowState> m_windows;
    std::unique_ptr<std::thread> m_worker;

    int m_server_port = 8088;
    int m_jpeg_quality = 75;
    int m_max_stream_fps = 30;
    double m_stream_scale = 0.5;
    mutable std::mutex m_status_mutex;
    std::string m_stream_status;
    int m_listen_fd = -1;
    std::thread m_accept_thread;
    std::mutex m_client_threads_mutex;
    /// 连接线程可能与 accept 线程并发结束，必须等 join 才能释放其栈。
    struct BrowserClient
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<BrowserClient> m_client_threads;
    std::mutex m_stream_mutex;
    std::condition_variable m_stream_changed;
    std::vector<unsigned char> m_latest_jpeg;
    std::uint64_t m_frame_id = 0;
    std::chrono::steady_clock::time_point m_last_stream_frame{};
};
