# 哨兵实车硬件迁移记录

## 迁移边界

本次只迁移实车硬件接入和车辆参数，不替换 `src/core/algorithm/power_rune` 算法主体。

参考工程：`/home/shawn/Documents/Codex/2026-10-24/xian/work/sentry/home/ubuntu22/vivsionn`

## 已迁移参数

| 项目 | 参考位置 | 当前值 |
| --- | --- | --- |
| 相机驱动 | `src/HKVideoCapture` | HikRobot MVS，USB/GigE 枚举 |
| 分辨率 | `src/param.yaml` | 1280×1024 |
| 帧率 | `FrameCapture::init()` | 210 FPS |
| 打符曝光 | `HITBUF_EXPOSURE_TIME` | 5 ms |
| 打符增益 | `HITBUF_GAIN` | 10 |
| 图像方向 | `CAMERA_UPSIDE_DOWN` | 180° 翻转 |
| 内参 | `CAMERA_MATRIX` | fx=1873.1327, fy=1874.1579, cx=523.0461, cy=562.0943 |
| 畸变 | `RADIAL_DISTORTION` | -0.056031, 0.153727, -0.001629, 0.000165, 0 |
| 相机到云台旋转 | `BuffDetector/buff_config.yaml` | `[0,0,1; -1,0,0; 0,-1,0]` |
| 相机到云台平移 | `t_camera2gimbal` | `[0,0,0.07]` m |
| 默认弹速 | `V` | 22 m/s |

原车设置 `FLIP_CAMERA_INTRINSICS_WITH_IMAGE=0`，因此本工程同样在 180° 翻图后保留原标定矩阵，不对主点做二次镜像。

## 海康驱动行为

- CMake 找到 `/opt/MVS/include/MvCameraControl.h` 和 `libMvCameraControl.so` 时定义 `RP_HAS_HIKROBOT_MVS`。
- 采集线程只保留最新帧，消费者不会积压旧帧。
- BayerRG8 颜色转换、100 MHz 时间戳后备值与原实车工程一致。
- 相机硬件时间戳通过首帧偏移映射为 `timetool::Timestamp`，可继续与 CAN/ECS 数据配对。
- 没安装 MVS SDK 的机器仍可编译后备实现，但 `backend=hikrobot` 会明确报错；调试时可改用 `v4l2` 或 `gstreamer`。

## Debug 流

浏览器 MJPEG 服务集成在 `ImgViz`，默认监听所有网卡的 TCP 8088：

- `GET /`：调试页面和关键点图例。
- `GET /stream`：`multipart/x-mixed-replace` JPEG 流。
- 检测框旁显示类别、置信度和关键点质量。
- 五个关键点在图内标记为 `top`、`left`、`R`、`right`、`bottom`。
- 识别线程只提交最新图；JPEG 编码默认最多 30 FPS。

## 上车前检查

1. 确认 Jetson 已安装与架构匹配的 HikRobot MVS SDK，构建日志出现 `HikRobot MVS camera support enabled`。
2. 用 MVS 工具确认设备 0 就是目标相机，实际输出为 1280×1024，曝光/增益写入成功。
3. 对着标定板检查 180° 翻转后画面方向与原车一致；若相机安装方向已经改变，应修改 `flip_180` 并重新标定。
4. 在浏览器核对五关键点落在真实特征上，尤其是 `R` 点和叶片四向点的顺序。
5. 架空、关闭摩擦轮，验证云台坐标方向和 70 mm 外参；机械安装位置不同必须重新量取。
6. 弹速 22 m/s 是原车配置值，应以本车裁判系统实测值为准。

## 当前验证范围

本机已完成新增 C++ 文件的语法编译、配置解析和 MJPEG 组件编译。完整工程配置在本机止于 CUDA Toolkit 检测，因为本机没有 `nvcc`；海康 SDK 实机分支也必须在安装了 `/opt/MVS` 的 Jetson 上完成最终链接与相机联调。
