#include "NNDetector.hpp"

#include "class_loader.hpp"
#include "img_viz.hpp"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace
{
constexpr int kKeypointCount = 5;
constexpr int kKeypointDimension = 3;
constexpr float kDefaultConfidenceThreshold = 0.8F;
constexpr float kDefaultKeypointConfidenceThreshold = 0.8F;
constexpr float kDefaultNmsDistance = 30.0F;
constexpr int kDefaultMinimumValidKeypoints = kKeypointCount;
constexpr int kDefaultWorkspaceSizeMb = 1024;

class TensorRtLogger final : public nvinfer1::ILogger
{
public:
    void log(Severity severity, const char *message) noexcept override
    {
        if (severity == Severity::kINTERNAL_ERROR || severity == Severity::kERROR)
            LOG(ERROR) << "[TensorRT] " << message;
        else if (severity == Severity::kWARNING)
            LOG(WARNING) << "[TensorRT] " << message;
        else if (severity == Severity::kINFO)
            LOG(INFO) << "[TensorRT] " << message;
    }
};

TensorRtLogger g_tensor_rt_logger;

void check_cuda(cudaError_t status, const char *operation)
{
    if (status != cudaSuccess)
        throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
}

std::size_t element_count(const nvinfer1::Dims &dims)
{
    std::size_t result = 1;
    for (int index = 0; index < dims.nbDims; ++index)
    {
        if (dims.d[index] <= 0)
            throw std::runtime_error("TensorRT engine contains a dynamic or invalid tensor dimension");
        result *= static_cast<std::size_t>(dims.d[index]);
    }
    return result;
}

struct DetectorConfig
{
    std::filesystem::path config_path;
    std::filesystem::path model_path;
    std::filesystem::path engine_cache_path;
    bool fp16 = true;
    int workspace_size_mb = kDefaultWorkspaceSizeMb;
    float confidence_threshold = kDefaultConfidenceThreshold;
    float keypoint_confidence_threshold = kDefaultKeypointConfidenceThreshold;
    float nms_distance = kDefaultNmsDistance;
    int minimum_valid_keypoints = kDefaultMinimumValidKeypoints;
};

struct LetterboxResult
{
    cv::Mat image;
    float scale = 1.0F;
    int padding_x = 0;
    int padding_y = 0;
};

struct Detection
{
    int class_id = -1;
    float confidence = 0.0F;
    float quality = 0.0F;
    cv::Point2f center{0.0F, 0.0F};
    std::array<cv::Point2f, kKeypointCount> keypoints{};
    std::array<float, kKeypointCount> keypoint_confidences{};
};

template <typename T>
void read_optional(const cv::FileNode &node, const char *key, T &value)
{
    const cv::FileNode field = node[key];
    if (!field.empty())
        field >> value;
}

DetectorConfig load_detector_config()
{
    DetectorConfig result;
    result.config_path = DETECTOR_CONFIG_DIR / "detect.json";

    J_DETECT.updateJson();
    if (!J_DETECT.config_.isOpened())
        throw std::runtime_error("failed to open detector config: " + result.config_path.string());

    const cv::FileNode node = J_DETECT.config_["rune_detect"];
    if (node.empty() || !node.isMap())
        throw std::runtime_error("detect.json rune_detect configuration must be an object");
    std::string model;
    read_optional(node, "model", model);
    if (node["model"].empty())
        read_optional(node, "model_path", model);
    std::string engine_cache;
    read_optional(node, "engine_cache", engine_cache);
    read_optional(node, "fp16", result.fp16);
    read_optional(node, "workspace_size_mb", result.workspace_size_mb);
    read_optional(node, "conf", result.confidence_threshold);
    if (node["conf"].empty())
        read_optional(node, "confidence_threshold", result.confidence_threshold);
    read_optional(node, "kconf", result.keypoint_confidence_threshold);
    if (node["kconf"].empty())
        read_optional(node, "keypoint_confidence_threshold", result.keypoint_confidence_threshold);
    read_optional(node, "nms", result.nms_distance);
    if (node["nms"].empty())
        read_optional(node, "nms_distance", result.nms_distance);
    if (node["nms"].empty() && node["nms_distance"].empty())
        read_optional(node, "nms_distance_threshold", result.nms_distance);
    read_optional(node, "min_valid_kpts", result.minimum_valid_keypoints);
    if (node["min_valid_kpts"].empty())
        read_optional(node, "min_valid_keypoints", result.minimum_valid_keypoints);
    if (node["min_valid_kpts"].empty() && node["min_valid_keypoints"].empty())
        read_optional(node, "minimum_valid_keypoints", result.minimum_valid_keypoints);

    if (model.empty())
        throw std::runtime_error("detector config is missing the required string field 'model': " +
                                 result.config_path.string());
    if (result.workspace_size_mb <= 0)
        throw std::runtime_error("detector config field 'workspace_size_mb' must be greater than zero");
    if (!std::isfinite(result.confidence_threshold) || result.confidence_threshold < 0.0F ||
        result.confidence_threshold > 1.0F)
        throw std::runtime_error("detector config field 'conf' must be in [0, 1]");
    if (!std::isfinite(result.keypoint_confidence_threshold) ||
        result.keypoint_confidence_threshold < 0.0F || result.keypoint_confidence_threshold > 1.0F)
        throw std::runtime_error("detector config field 'kconf' must be in [0, 1]");
    if (!std::isfinite(result.nms_distance) || result.nms_distance <= 0.0F)
        throw std::runtime_error("detector config field 'nms' must be greater than zero");
    if (result.minimum_valid_keypoints < 1 || result.minimum_valid_keypoints > kKeypointCount)
        throw std::runtime_error("detector config field 'min_valid_kpts' must be in [1, 5]");

    result.model_path = std::filesystem::path(model);
    if (result.model_path.is_relative())
        result.model_path = DETECTOR_CONFIG_DIR / result.model_path;
    result.model_path = result.model_path.lexically_normal();

    if (engine_cache.empty())
    {
        result.engine_cache_path = result.model_path;
        result.engine_cache_path.replace_extension(".trt.engine");
    }
    else
    {
        result.engine_cache_path = std::filesystem::path(engine_cache);
        if (result.engine_cache_path.is_relative())
            result.engine_cache_path = DETECTOR_CONFIG_DIR / result.engine_cache_path;
        result.engine_cache_path = result.engine_cache_path.lexically_normal();
    }

    if (!std::filesystem::is_regular_file(result.model_path))
        throw std::runtime_error("detector model is not a regular file: " + result.model_path.string());

    return result;
}

bool is_rune_mode(AimMode mode)
{
    return mode == AimMode::SmallRune || mode == AimMode::BigRune;
}

int map_class_for_mode(int model_class, AimMode mode)
{
    // RuneDetectionModel classes: 0 inactive, 1 small activated, 2 big activated.
    if (model_class == 0)
        return 0;
    if (mode == AimMode::SmallRune && model_class == 1)
        return 1;
    if (mode == AimMode::BigRune && model_class == 2)
        return 1;
    return -1;
}

cv::Point to_integer_point(const cv::Point2f &point)
{
    return {cvRound(point.x), cvRound(point.y)};
}

std::string class_name(int class_id)
{
    switch (class_id)
    {
    case 0:
        return "inactive";
    case 1:
        return "small activated";
    case 2:
        return "big activated";
    default:
        return "class " + std::to_string(class_id);
    }
}
} // namespace

class NNDetector::Impl
{
public:
    ~Impl()
    {
        if (m_stream != nullptr)
            cudaStreamSynchronize(m_stream);
        if (m_device_output != nullptr)
            cudaFree(m_device_output);
        if (m_device_input != nullptr)
            cudaFree(m_device_input);
        if (m_stream != nullptr)
            cudaStreamDestroy(m_stream);
    }

    void initialize()
    {
        m_config = load_detector_config();

        m_runtime.reset(nvinfer1::createInferRuntime(g_tensor_rt_logger));
        if (!m_runtime)
            throw std::runtime_error("failed to create TensorRT runtime");

        m_engine.reset(load_cached_engine());
        if (!m_engine)
        {
            LOG(INFO) << "[NNDetector] building TensorRT engine from " << m_config.model_path;
            m_engine.reset(build_engine());
            save_engine_cache();
        }
        m_context.reset(m_engine->createExecutionContext());
        if (!m_context)
            throw std::runtime_error("failed to create TensorRT execution context");

        if (m_engine->getNbIOTensors() != 2)
            throw std::runtime_error("rune model must have exactly one input and one output");
        for (int index = 0; index < m_engine->getNbIOTensors(); ++index)
        {
            const char *name = m_engine->getIOTensorName(index);
            if (m_engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
                m_input_name = name;
            else
                m_output_name = name;
        }
        if (m_input_name.empty() || m_output_name.empty())
            throw std::runtime_error("failed to identify TensorRT input and output tensors");
        if (m_engine->getTensorDataType(m_input_name.c_str()) != nvinfer1::DataType::kFLOAT ||
            m_engine->getTensorDataType(m_output_name.c_str()) != nvinfer1::DataType::kFLOAT)
            throw std::runtime_error("rune model input and output tensors must be float32");

        const nvinfer1::Dims input_shape = m_engine->getTensorShape(m_input_name.c_str());
        if (input_shape.nbDims != 4 || input_shape.d[0] != 1 || input_shape.d[1] != 3 ||
            input_shape.d[2] <= 0 || input_shape.d[3] <= 0)
            throw std::runtime_error("rune model input must have static NCHW shape [1,3,H,W]");
        m_input_height = input_shape.d[2];
        m_input_width = input_shape.d[3];

        const nvinfer1::Dims output_shape = m_engine->getTensorShape(m_output_name.c_str());
        if (output_shape.nbDims != 3 || output_shape.d[0] != 1)
            throw std::runtime_error("rune model output must be [1,C,A] or [1,A,C]");

        // The layout test deliberately follows RuneDetectionModel-master.
        if (output_shape.d[1] <= 256 && output_shape.d[2] >= 100)
        {
            m_output_is_nca = true;
            m_output_channels = output_shape.d[1];
            m_anchor_count = output_shape.d[2];
        }
        else
        {
            m_output_is_nca = false;
            m_anchor_count = output_shape.d[1];
            m_output_channels = output_shape.d[2];
        }

        // The published contract is the repository's five-point rune format.
        // RuneDetectionModel-master exports 3 + 5*3 = 18 channels.
        if (m_output_channels != 3 + kKeypointCount * kKeypointDimension)
        {
            std::ostringstream message;
            message << "unsupported rune head: expected 18 channels (3 classes + 5x3 keypoints), got "
                    << m_output_channels;
            throw std::runtime_error(message.str());
        }
        m_class_count = 3;

        m_host_input.resize(element_count(input_shape));
        m_host_output.resize(element_count(output_shape));
        check_cuda(cudaStreamCreate(&m_stream), "cudaStreamCreate");
        check_cuda(cudaMalloc(&m_device_input, m_host_input.size() * sizeof(float)), "cudaMalloc(input)");
        check_cuda(cudaMalloc(&m_device_output, m_host_output.size() * sizeof(float)), "cudaMalloc(output)");
        if (!m_context->setTensorAddress(m_input_name.c_str(), m_device_input) ||
            !m_context->setTensorAddress(m_output_name.c_str(), m_device_output))
            throw std::runtime_error("failed to bind TensorRT tensor addresses");

        LOG(INFO) << "[NNDetector] initialized CUDA/TensorRT rune model; model=" << m_config.model_path
                  << ", engine_cache=" << m_config.engine_cache_path
                  << ", fp16=" << (m_config.fp16 ? "requested" : "disabled")
                  << ", input=" << m_input_width << 'x' << m_input_height
                  << ", output_layout=" << (m_output_is_nca ? "[N,C,A]" : "[N,A,C]")
                  << ", anchors=" << m_anchor_count
                  << ", conf=" << m_config.confidence_threshold
                  << ", kconf=" << m_config.keypoint_confidence_threshold
                  << ", nms=" << m_config.nms_distance
                  << ", min_valid_kpts=" << m_config.minimum_valid_keypoints;
    }

    std::vector<Detection> infer(const cv::Mat &source)
    {
        const cv::Mat bgr = normalize_input(source);
        LetterboxResult letterbox = preprocess_letterbox(bgr);
        if (!letterbox.image.isContinuous())
            letterbox.image = letterbox.image.clone();

        // Preserve the trained model's preprocessing contract exactly: BGR u8 NHWC
        // becomes RGB float32 NCHW in [0,1].
        const std::size_t plane_size = static_cast<std::size_t>(m_input_width) * m_input_height;
        for (int y = 0; y < m_input_height; ++y)
        {
            const cv::Vec3b *row = letterbox.image.ptr<cv::Vec3b>(y);
            for (int x = 0; x < m_input_width; ++x)
            {
                const std::size_t pixel = static_cast<std::size_t>(y) * m_input_width + x;
                m_host_input[pixel] = static_cast<float>(row[x][2]) / 255.0F;
                m_host_input[plane_size + pixel] = static_cast<float>(row[x][1]) / 255.0F;
                m_host_input[2 * plane_size + pixel] = static_cast<float>(row[x][0]) / 255.0F;
            }
        }

        check_cuda(cudaMemcpyAsync(m_device_input,
                                   m_host_input.data(),
                                   m_host_input.size() * sizeof(float),
                                   cudaMemcpyHostToDevice,
                                   m_stream),
                   "cudaMemcpyAsync(input)");
        if (!m_context->enqueueV3(m_stream))
            throw std::runtime_error("TensorRT enqueueV3 failed");
        check_cuda(cudaMemcpyAsync(m_host_output.data(),
                                   m_device_output,
                                   m_host_output.size() * sizeof(float),
                                   cudaMemcpyDeviceToHost,
                                   m_stream),
                   "cudaMemcpyAsync(output)");
        check_cuda(cudaStreamSynchronize(m_stream), "cudaStreamSynchronize");

        return postprocess(letterbox, source.cols, source.rows);
    }

    std::vector<NNRuneInfo> convert_for_mode(const std::vector<Detection> &detections, AimMode mode) const
    {
        std::vector<NNRuneInfo> result;
        result.reserve(detections.size());

        for (const Detection &detection : detections)
        {
            const int published_class = map_class_for_mode(detection.class_id, mode);
            if (published_class < 0)
                continue;

            NNRuneInfo rune;
            rune.top = to_integer_point(detection.keypoints[0]);
            rune.left = to_integer_point(detection.keypoints[1]);
            rune.point_R = to_integer_point(detection.keypoints[2]);
            rune.right = to_integer_point(detection.keypoints[3]);
            rune.bottom = to_integer_point(detection.keypoints[4]);
            rune.class_id = published_class;
            result.emplace_back(std::move(rune));
        }
        return result;
    }

    void visualize(const cv::Mat &source, const std::vector<Detection> &detections, AimMode mode) const
    {
        if (!ImgViz::enabled() || source.empty())
            return;

        static const std::array<cv::Scalar, kKeypointCount> colors{
            cv::Scalar(0, 255, 0),
            cv::Scalar(0, 255, 255),
            cv::Scalar(255, 0, 0),
            cv::Scalar(255, 0, 255),
            cv::Scalar(0, 0, 255)};
        static const std::array<const char *, kKeypointCount> keypoint_names{
            "top", "left", "R", "right", "bottom"};

        cv::Mat canvas = source.clone();
        for (const Detection &detection : detections)
        {
            if (map_class_for_mode(detection.class_id, mode) < 0)
                continue;

            for (std::size_t index = 0; index < detection.keypoints.size(); ++index)
            {
                const int radius = index == 2 ? 5 : 3;
                cv::circle(canvas, detection.keypoints[index], radius, colors[index], cv::FILLED, cv::LINE_AA);
                cv::circle(canvas, detection.keypoints[index], radius, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
                cv::putText(canvas,
                            keypoint_names[index],
                            detection.keypoints[index] + cv::Point2f(6.0F, -6.0F),
                            cv::FONT_HERSHEY_SIMPLEX,
                            0.38,
                            colors[index],
                            1,
                            cv::LINE_AA);
            }

            std::ostringstream label;
            label << class_name(detection.class_id) << " conf=" << std::fixed << std::setprecision(2)
                  << detection.confidence << " q=" << detection.quality;
            cv::putText(canvas,
                        label.str(),
                        detection.center + cv::Point2f(4.0F, -4.0F),
                        cv::FONT_HERSHEY_SIMPLEX,
                        0.45,
                        cv::Scalar(255, 255, 255),
                        1,
                        cv::LINE_AA);
        }

        // ImgViz owns the process-wide HighGUI worker; this plugin never calls HighGUI.
        ImgViz::enqueue_image_zero_copy("Rune/NNDetector", canvas);
    }

private:
    nvinfer1::ICudaEngine *load_cached_engine()
    {
        if (!std::filesystem::is_regular_file(m_config.engine_cache_path))
            return nullptr;

        std::ifstream input(m_config.engine_cache_path, std::ios::binary | std::ios::ate);
        if (!input)
            return nullptr;
        const std::streamsize size = input.tellg();
        if (size <= 0)
            return nullptr;
        input.seekg(0, std::ios::beg);
        std::vector<char> bytes(static_cast<std::size_t>(size));
        if (!input.read(bytes.data(), size))
            return nullptr;

        nvinfer1::ICudaEngine *engine = m_runtime->deserializeCudaEngine(bytes.data(), bytes.size());
        if (engine == nullptr)
            LOG(WARNING) << "[NNDetector] cached TensorRT engine is incompatible; rebuilding: "
                         << m_config.engine_cache_path;
        else
            LOG(INFO) << "[NNDetector] loaded cached TensorRT engine: " << m_config.engine_cache_path;
        return engine;
    }

    nvinfer1::ICudaEngine *build_engine()
    {
        std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(g_tensor_rt_logger));
        if (!builder)
            throw std::runtime_error("failed to create TensorRT builder");
        std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0U));
        if (!network)
            throw std::runtime_error("failed to create TensorRT network");
        std::unique_ptr<nvonnxparser::IParser> parser(
            nvonnxparser::createParser(*network, g_tensor_rt_logger));
        if (!parser || !parser->parseFromFile(
                           m_config.model_path.c_str(),
                           static_cast<int>(nvinfer1::ILogger::Severity::kWARNING)))
            throw std::runtime_error("failed to parse rune ONNX model with TensorRT");

        std::unique_ptr<nvinfer1::IBuilderConfig> builder_config(builder->createBuilderConfig());
        if (!builder_config)
            throw std::runtime_error("failed to create TensorRT builder config");
        builder_config->setMemoryPoolLimit(
            nvinfer1::MemoryPoolType::kWORKSPACE,
            static_cast<std::size_t>(m_config.workspace_size_mb) * 1024U * 1024U);
        if (m_config.fp16)
        {
            if (builder->platformHasFastFp16())
                builder_config->setFlag(nvinfer1::BuilderFlag::kFP16);
            else
                LOG(WARNING) << "[NNDetector] fp16 requested but the CUDA device has no fast FP16 support";
        }

        std::unique_ptr<nvinfer1::IHostMemory> plan(
            builder->buildSerializedNetwork(*network, *builder_config));
        if (!plan)
            throw std::runtime_error("TensorRT failed to build a serialized engine");
        nvinfer1::ICudaEngine *engine = m_runtime->deserializeCudaEngine(plan->data(), plan->size());
        if (!engine)
            throw std::runtime_error("TensorRT failed to deserialize the newly built engine");
        return engine;
    }

    void save_engine_cache() const
    {
        std::unique_ptr<nvinfer1::IHostMemory> plan(m_engine->serialize());
        if (!plan)
        {
            LOG(WARNING) << "[NNDetector] failed to serialize TensorRT engine cache";
            return;
        }

        std::error_code error;
        const std::filesystem::path parent = m_config.engine_cache_path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, error);
        if (error)
        {
            LOG(WARNING) << "[NNDetector] cannot create engine cache directory: " << error.message();
            return;
        }
        std::ofstream output(m_config.engine_cache_path, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(static_cast<const char *>(plan->data()), plan->size()))
        {
            LOG(WARNING) << "[NNDetector] cannot write TensorRT engine cache: "
                         << m_config.engine_cache_path;
            return;
        }
        LOG(INFO) << "[NNDetector] saved TensorRT engine cache: " << m_config.engine_cache_path;
    }

    cv::Mat normalize_input(const cv::Mat &source) const
    {
        if (source.empty())
            throw std::invalid_argument("cannot infer an empty image");

        if (source.type() == CV_8UC3)
            return source;

        cv::Mat bgr;
        if (source.type() == CV_8UC1)
            cv::cvtColor(source, bgr, cv::COLOR_GRAY2BGR);
        else if (source.type() == CV_8UC4)
            cv::cvtColor(source, bgr, cv::COLOR_BGRA2BGR);
        else
            throw std::invalid_argument("rune detector expects an 8-bit 1-, 3-, or 4-channel image");
        return bgr;
    }

    LetterboxResult preprocess_letterbox(const cv::Mat &source) const
    {
        LetterboxResult result;
        result.scale = std::min(
            static_cast<float>(m_input_width) / static_cast<float>(source.cols),
            static_cast<float>(m_input_height) / static_cast<float>(source.rows));

        const int resized_width = static_cast<int>(std::round(source.cols * result.scale));
        const int resized_height = static_cast<int>(std::round(source.rows * result.scale));
        result.padding_x = (m_input_width - resized_width) / 2;
        result.padding_y = (m_input_height - resized_height) / 2;
        const int right_padding = m_input_width - resized_width - result.padding_x;
        const int bottom_padding = m_input_height - resized_height - result.padding_y;

        cv::Mat resized;
        if (resized_width != source.cols || resized_height != source.rows)
            cv::resize(source, resized, cv::Size(resized_width, resized_height));
        else
            resized = source;

        if (result.padding_x > 0 || result.padding_y > 0 || right_padding > 0 || bottom_padding > 0)
        {
            cv::copyMakeBorder(
                resized,
                result.image,
                result.padding_y,
                bottom_padding,
                result.padding_x,
                right_padding,
                cv::BORDER_CONSTANT,
                cv::Scalar(114, 114, 114));
        }
        else
        {
            result.image = resized;
        }
        return result;
    }

    std::vector<Detection> postprocess(
        const LetterboxResult &letterbox,
        int original_width,
        int original_height)
    {
        const float *const output = m_host_output.data();

        const auto value_at = [this, output](int channel, int anchor)
        {
            return m_output_is_nca
                       ? output[channel * m_anchor_count + anchor]
                       : output[anchor * m_output_channels + channel];
        };

        std::vector<Detection> detections;
        detections.reserve(64);
        for (int anchor = 0; anchor < m_anchor_count; ++anchor)
        {
            int best_class = -1;
            float best_confidence = 0.0F;
            for (int class_index = 0; class_index < m_class_count; ++class_index)
            {
                const float score = value_at(class_index, anchor);
                if (std::isfinite(score) && score > best_confidence)
                {
                    best_confidence = score;
                    best_class = class_index;
                }
            }
            if (best_class < 0 || best_confidence < m_config.confidence_threshold)
                continue;

            Detection detection;
            detection.class_id = best_class;
            detection.confidence = best_confidence;

            int valid_keypoints = 0;
            float keypoint_confidence_sum = 0.0F;
            cv::Point2f valid_center{0.0F, 0.0F};
            bool invalid_keypoint = false;

            for (int keypoint = 0; keypoint < kKeypointCount; ++keypoint)
            {
                const int base = m_class_count + keypoint * kKeypointDimension;
                float x = (value_at(base, anchor) - static_cast<float>(letterbox.padding_x)) /
                          letterbox.scale;
                float y = (value_at(base + 1, anchor) - static_cast<float>(letterbox.padding_y)) /
                          letterbox.scale;
                const float keypoint_confidence = value_at(base + 2, anchor);

                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(keypoint_confidence) ||
                    x < 0.0F || y < 0.0F)
                {
                    invalid_keypoint = true;
                    break;
                }

                x = std::clamp(x, 0.0F, static_cast<float>(original_width - 1));
                y = std::clamp(y, 0.0F, static_cast<float>(original_height - 1));
                detection.keypoints[keypoint] = {x, y};
                detection.keypoint_confidences[keypoint] = keypoint_confidence;

                if (keypoint_confidence >= m_config.keypoint_confidence_threshold)
                {
                    ++valid_keypoints;
                    keypoint_confidence_sum += keypoint_confidence;
                    valid_center += detection.keypoints[keypoint];
                }
            }

            if (invalid_keypoint || valid_keypoints < m_config.minimum_valid_keypoints)
                continue;

            const float mean_keypoint_confidence =
                keypoint_confidence_sum / static_cast<float>(valid_keypoints);
            detection.quality = detection.confidence * mean_keypoint_confidence;
            detection.center = valid_center * (1.0F / static_cast<float>(valid_keypoints));
            detections.emplace_back(std::move(detection));
        }

        return center_distance_nms(std::move(detections));
    }

    std::vector<Detection> center_distance_nms(std::vector<Detection> detections) const
    {
        std::sort(
            detections.begin(),
            detections.end(),
            [](const Detection &left, const Detection &right)
            {
                return left.quality > right.quality;
            });

        std::vector<Detection> kept;
        kept.reserve(detections.size());
        std::vector<bool> suppressed(detections.size(), false);
        const float threshold_squared = m_config.nms_distance * m_config.nms_distance;

        for (std::size_t index = 0; index < detections.size(); ++index)
        {
            if (suppressed[index])
                continue;

            kept.emplace_back(detections[index]);
            const cv::Point2f center = detections[index].center;
            for (std::size_t candidate = index + 1; candidate < detections.size(); ++candidate)
            {
                if (suppressed[candidate])
                    continue;
                const cv::Point2f delta = center - detections[candidate].center;
                if (delta.dot(delta) < threshold_squared)
                    suppressed[candidate] = true;
            }
        }
        return kept;
    }

    DetectorConfig m_config;
    std::unique_ptr<nvinfer1::IRuntime> m_runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> m_engine;
    std::unique_ptr<nvinfer1::IExecutionContext> m_context;
    std::string m_input_name;
    std::string m_output_name;
    cudaStream_t m_stream = nullptr;
    void *m_device_input = nullptr;
    void *m_device_output = nullptr;
    std::vector<float> m_host_input;
    std::vector<float> m_host_output;
    int m_input_width = 0;
    int m_input_height = 0;
    int m_output_channels = 0;
    int m_anchor_count = 0;
    int m_class_count = 0;
    bool m_output_is_nca = true;
};

NNDetector::NNDetector()
    : m_impl(std::make_unique<Impl>())
{
    try
    {
        m_impl->initialize();
    }
    catch (const std::exception &error)
    {
        LOG(ERROR) << "[NNDetector] initialization failed: " << error.what();
        throw;
    }
}

NNDetector::~NNDetector() = default;

void NNDetector::process(const app::Context &context)
{
    static auto input = context.get_buffer_subscriber<InputFrame>(this);
    static auto output = context.get_buffer_publisher<InputFrameWithNNResults>(this);

    InputFrameWithNNResults result;
    result.input_frame = input.wait_pop();
    const AimMode mode = result.input_frame.ecs_data.mode;

    if (is_rune_mode(mode))
    {
        try
        {
            const std::vector<Detection> detections = m_impl->infer(result.input_frame.img);
            result.nn_rune_infos = m_impl->convert_for_mode(detections, mode);
            m_impl->visualize(result.input_frame.img, detections, mode);
        }
        catch (const std::exception &error)
        {
            // Keep the pipeline alive and publish the frame without detections. A transient
            // inference error must not starve downstream tracking/fire-control plugins.
            LOG_EVERY_N(ERROR, 100) << "[NNDetector] inference failed: " << error.what();
        }
    }

    output.push(std::move(result));
}

REGISTER_PLUGIN("NNDetector", NNDetector)
