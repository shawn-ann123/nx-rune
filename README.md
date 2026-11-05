# 深大完整打符链路 - 哨兵实车硬件迁移版

本子工程是在原迁移工程副本上继续做的哨兵实车适配。算法主体保持不变，硬件层和车辆参数对齐参考工程 `vivsionn`：海康 MVS 相机、相机硬件时间戳、1280×1024/210 FPS、打符曝光与增益、相机标定、相机到云台外参，以及浏览器关键点调试流。详细来源和核对项见 `docs/HARDWARE_MIGRATION.md`。

本目录是可独立搬到哨兵车载 Jetson NX 上的完整打符工程。核心算法、五关键点 ONNX 模型和算法参数来自模拟器迁移工程；`src/core/algorithm/power_rune` 未做逻辑修改。实车迁移只替换了四处边界：

1. `RealCameraManager` 默认通过海康 MVS SDK 采集，使用相机设备时间戳并映射到系统时钟；仍保留 V4L2/GStreamer 后备路径。
2. `CanReceiveDecoder` 从 SocketCAN 接收 `0x203 + 0x204`，组合出队伍颜色、任务模式和云台 Roll/Yaw/Pitch；两帧超时或配对失败会进入安全状态。
3. `CanSendEncoder` 按固定 10 ms 周期发送 `0x100`，把算法输出的 Pitch/Yaw 转成小端 `int16 × 100`，并编码打符、视觉状态和发射请求。
4. `NNDetector` 使用 CUDA + TensorRT 10 执行推理，首次启动从 ONNX 构建 FP16 engine 并缓存，后续启动直接加载 engine。

工程不包含模拟器 SDK、视频回放、GT 探针、测试探针、演示素材或旧二进制。

## 目录

- `config/camera.json`：相机设备、分辨率、帧率、曝光与 GStreamer 管线。
- `config/can.json`：CAN 口、大小符模式、反馈新鲜度、角度方向/零偏/限幅和发射策略。
- `config/power_rune.json`：相机内参与畸变、弹道、算法参数和算法输出零偏。
- `config/track.json`：相机到云台的平移外参与安装角偏差。
- `config/detect.json`、`config/openvino/`：五关键点 TensorRT 推理配置和后端无关的 ONNX 模型（目录名沿用旧工程）。
- `src/app_plugin/`：相机、CAN 和算法编排适配层。
- `src/core/algorithm/power_rune/`：完整打符算法主体。

配置均从可执行文件旁的 `../config` 读取，脚本也只根据自身位置解析工程目录；工程移动后不需要改路径。相机设备可用 `device_index`，若必须指定设备节点再填写 `device`。模型路径相对于 `config/`。

## 上车前必须核对

相机和标定默认值已从原实车工程迁入，但换车、拆装相机或更换镜头后仍必须重新核对，否则禁止实弹：

1. `config/power_rune.json` 的 `camera.cam`、`camera.dis` 和图像宽高。
2. `config/track.json` 的相机相对云台 XYZ 外参及安装角。
3. `config/power_rune.json` 的弹速、延迟、枪管长度和算法 `offset`。
4. `config/can.json` 的 SocketCAN 接口、Yaw/Pitch 正负号、零偏和机械限位。
5. `config/can.json` 的 `rune_mode`：`2` 为小符，`3` 为大符；通信协议只给出 `BUFF_AIM`，没有提供大小符位，因此由本配置明确选择。
6. `shoot_buff_mode` 的具体非零值要和下位机最终约定一致；两份说明只规定“非零表示打符”，没有定义各非零值映射。

首次上电把 `shoot_mode_when_fire` 保持为 `3`（单发），架空底盘、关闭摩擦轮，用 `candump` 核验角度方向与限幅后再实弹。若下位机要求连续发射，可改成 `2`。

## CAN 协议实现

- 接收 `0x203`：校验 DLC=8、Byte0=`0x33`，读取 task/team/Roll/Yaw。
- 接收 `0x204`：校验 DLC=8、Byte7=`0xEE`，读取 Pitch 和 fire_permit。Byte2-3 的 Yaw 角速度当前算法接口不用，保持忽略。
- 两段反馈必须都在 `feedback_timeout_ms` 内，且时间差不超过 `pair_tolerance_ms`。
- 默认要求 `task_mode == 0x02 (BUFF_AIM)` 才运行打符；其他模式只发送安全的“不瞄准、不发射”帧。
- 发送 `0x100`：Byte0=`0x33`，Byte3=`0xEE`，Byte4-5=Pitch，Byte6-7=Yaw，均为小端 0.01 度。
- 算法结果超过 `result_timeout_ms`、没有目标或出现非有限角度时，不再沿用旧瞄准量。
- 单发只在 `is_enable_fire` 的上升沿发出一次 `shoot_mode=3`，避免每个 10 ms 控制帧都触发一发；帧计数持续循环递增。
- 打符不需要 `0x101` 的普通装甲目标 ID/位置，因此本工程不发送该帧。

## 构建

目标环境为 Jetson NX、Ubuntu 22.04、JetPack 6.x（CUDA 12 / TensorRT 10）、CMake 3.22+ 和 C++20。需要 OpenCV、CUDA Toolkit、TensorRT 开发包、Eigen3、Ceres、Boost、fmt 和 glog；Sophus 在系统缺失时自动使用 `third_party/sophus` 的头文件版本。安装好依赖后：

```bash
./build.sh
```

首次启动会在目标机用 `model-0624.onnx` 生成 `config/tensorrt/model-0624-trt10.engine`，耗时会明显长于后续启动。TensorRT engine 与 GPU、TensorRT 版本和平台绑定，不应从另一台机器复制；缓存不兼容时程序会自动重建。

如果依赖放在自定义前缀，可把已有工具链作为相对路径传入：

```bash
RP26_DEPS_ROOT=/path/to/deps ./build.sh -DCMAKE_TOOLCHAIN_FILE=cmake/rp26-user-deps.cmake
```

## 启动与检查

先按 `config/can.json` 的接口名初始化 CAN，例如：

```bash
sudo ./setup_can.sh can0 1000000
candump can0,203:7FF,204:7FF
```

确认能持续收到 8 字节 `0x203/0x204` 后启动：

```bash
./run.sh
```

另开终端核验输出：

```bash
candump can0,100:7FF
```

重点检查 `0x100` 的第 4 个数据字节是 `EE`，而不是最后一个字节；Pitch 位于 Byte4-5，Yaw 位于 Byte6-7。MCU 约 50 ms 未收到新 `0x100` 会失鲜，本工程默认每 10 ms 发送一次。

## 相机接入

默认 `backend=hikrobot`，构建时会在 `/opt/MVS/include` 和 `/opt/MVS/lib/{aarch64,64}` 查找海康 MVS SDK。普通 UVC/V4L2 相机可把后端改为 `v4l2` 并设置 `device_index` 或 `device`；工业相机若已有 GStreamer 源，可改成 `gstreamer` 并在 `pipeline` 写完整管线（以 `appsink` 结尾）。

海康路径读取 `nDevTimeStampHigh/Low`，优先使用相机报告的 tick frequency；读取不到时沿用原车验证过的 100 MHz。首帧建立相机时钟到系统时钟的偏移，后续帧时间不受图像解码完成时间影响。

## 关键点 Debug 流

`./run.sh` 默认启用浏览器 MJPEG，在同网段电脑访问 `http://<Jetson-IP>:8088/`。画面会显示识别类别、置信度及 `top/left/R/right/bottom` 五个关键点。可用环境变量调整或关闭：

```bash
RP_DEBUG_MJPEG_PORT=8088 RP_DEBUG_MJPEG_QUALITY=75 RP_DEBUG_MJPEG_FPS=30 ./run.sh
RP_DEBUG_DISPLAY_BACKEND=none ./run.sh
RP_DEBUG_DISPLAY_BACKEND=opencv ./run.sh
```

服务只保留最新 JPEG，默认限速 30 FPS，不让浏览器慢客户端反压 210 FPS 的识别链路。

Jetson 实车构建默认关闭原工程的原生 Foxglove 后端，因为基线附带的预编译静态库是 x86-64；算法使用的时间和 schema 类型仍保留，Foxglove 发布调用在关闭状态下为无操作。关键点调试由上述 MJPEG 流承担。只有准备了对应 aarch64 `libfoxglove.a` 时才应传入 `-DRP26_ENABLE_FOXGLOVE=ON`。

## 车辆配置

这是哨兵版本。新 NX 载板与步兵载板的 CAN 控制器、收发器和 Linux 枚举可能不同，因此没有照抄步兵的 `can1`，当前默认保留 `can0`。上车后先用 `ip -br link` 和 `ip -details link show can0` 核对接口；如果实际枚举不同，同时修改 `config/can.json` 和 `setup_can.sh` 的调用参数。载板若要求单独配置引脚复用或使能收发器，必须先按载板手册完成，`setup_can.sh` 只负责配置已经出现的 SocketCAN 接口。

操作仲裁要点：右拨上为最高优先级保护；右拨中时仅在左拨中/下允许 NX 接管云台，发射仍由遥控器仲裁；右拨下时才由新鲜 `0x100` 同时提供云台与发射请求。下位机上报 `task_mode=0x02` 后，本程序才进入打符算法。

第一次联调建议右拨中、左拨中，只验证 NX 云台角度而不让 NX 控制发射；确认方向、零偏、限位和失鲜保护后，再切换右拨下测试 `shoot_mode`。
