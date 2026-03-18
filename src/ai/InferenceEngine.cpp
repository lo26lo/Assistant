#include "InferenceEngine.h"
#include "ModelManager.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <numeric>

namespace ibom::ai {

InferenceEngine::InferenceEngine(ModelManager& modelManager)
    : m_modelManager(modelManager)
{
}

InferenceEngine::~InferenceEngine() = default;

bool InferenceEngine::initialize(bool useTensorRT, int gpuDeviceId)
{
    try {
        m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "MicroscopeIBOM");

        m_sessionOptions = std::make_unique<Ort::SessionOptions>();
        m_sessionOptions->SetIntraOpNumThreads(4);
        m_sessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef IBOM_HAS_TENSORRT
        if (useTensorRT) {
            OrtTensorRTProviderOptions trtOptions{};
            trtOptions.device_id = gpuDeviceId;
            trtOptions.trt_max_workspace_size = 1ULL << 30; // 1 GB
            trtOptions.trt_fp16_enable = 1;                  // FP16 for speed
            trtOptions.trt_engine_cache_enable = 1;          // Cache compiled engines
            trtOptions.trt_engine_cache_path = "trt_cache";

            m_sessionOptions->AppendExecutionProvider_TensorRT(trtOptions);
            spdlog::info("TensorRT execution provider enabled (device {})", gpuDeviceId);
        }
#endif

#ifdef IBOM_HAS_CUDA
        OrtCUDAProviderOptions cudaOptions{};
        cudaOptions.device_id = gpuDeviceId;
        m_sessionOptions->AppendExecutionProvider_CUDA(cudaOptions);
        spdlog::info("CUDA execution provider enabled (device {})", gpuDeviceId);
#endif

        spdlog::info("Inference engine initialized.");
        return true;

    } catch (const Ort::Exception& e) {
        spdlog::error("ONNX Runtime init failed: {}", e.what());
        return false;
    }
}

bool InferenceEngine::loadModel(const std::string& modelPath)
{
    if (!m_env || !m_sessionOptions) {
        spdlog::error("Engine not initialized. Call initialize() first.");
        return false;
    }

    try {
        spdlog::info("Loading model: {}", modelPath);

#ifdef _WIN32
        // ONNX Runtime on Windows needs wide strings
        std::wstring wpath(modelPath.begin(), modelPath.end());
        m_session = std::make_unique<Ort::Session>(*m_env, wpath.c_str(), *m_sessionOptions);
#else
        m_session = std::make_unique<Ort::Session>(*m_env, modelPath.c_str(), *m_sessionOptions);
#endif

        Ort::AllocatorWithDefaultOptions allocator;

        // Get input info
        m_inputNames.clear();
        m_inputNamePtrs.clear();
        size_t numInputs = m_session->GetInputCount();
        for (size_t i = 0; i < numInputs; ++i) {
            auto name = m_session->GetInputNameAllocated(i, allocator);
            m_inputNames.push_back(name.get());
        }
        for (auto& n : m_inputNames) m_inputNamePtrs.push_back(n.c_str());

        // Get output info
        m_outputNames.clear();
        m_outputNamePtrs.clear();
        size_t numOutputs = m_session->GetOutputCount();
        for (size_t i = 0; i < numOutputs; ++i) {
            auto name = m_session->GetOutputNameAllocated(i, allocator);
            m_outputNames.push_back(name.get());
        }
        for (auto& n : m_outputNames) m_outputNamePtrs.push_back(n.c_str());

        // Determine input size from first input shape
        auto inputShape = m_session->GetInputTypeInfo(0)
            .GetTensorTypeAndShapeInfo().GetShape();
        if (inputShape.size() >= 4) {
            m_inputSize.height = static_cast<int>(inputShape[2]);
            m_inputSize.width  = static_cast<int>(inputShape[3]);
        }

        m_ready = true;
        spdlog::info("Model loaded: {} inputs, {} outputs, input size {}x{}",
            numInputs, numOutputs, m_inputSize.width, m_inputSize.height);
        return true;

    } catch (const Ort::Exception& e) {
        spdlog::error("Failed to load model '{}': {}", modelPath, e.what());
        m_ready = false;
        return false;
    }
}

std::vector<Detection> InferenceEngine::detect(const cv::Mat& frame,
                                                 float confidenceThreshold,
                                                 float nmsThreshold)
{
    if (!m_ready || !m_session) return {};

    auto startTime = std::chrono::high_resolution_clock::now();

    try {
        // Preprocess
        auto inputTensor = preprocess(frame);

        // Create input tensor
        std::array<int64_t, 4> inputShape = {1, 3, m_inputSize.height, m_inputSize.width};
        auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        Ort::Value inputOrtTensor = Ort::Value::CreateTensor<float>(
            memoryInfo, inputTensor.data(), inputTensor.size(),
            inputShape.data(), inputShape.size()
        );

        // Run inference
        auto outputs = m_session->Run(
            Ort::RunOptions{nullptr},
            m_inputNamePtrs.data(), &inputOrtTensor, 1,
            m_outputNamePtrs.data(), m_outputNamePtrs.size()
        );

        // Postprocess
        auto detections = postprocess(outputs, frame.size(), confidenceThreshold, nmsThreshold);

        auto endTime = std::chrono::high_resolution_clock::now();
        m_lastInferenceMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        return detections;

    } catch (const Ort::Exception& e) {
        spdlog::error("Inference failed: {}", e.what());
        return {};
    }
}

std::vector<float> InferenceEngine::preprocess(const cv::Mat& frame)
{
    cv::Mat resized, blob;

    // Resize to model input size with letterboxing
    cv::resize(frame, resized, m_inputSize);

    // Convert BGR -> RGB, normalize to [0, 1]
    resized.convertTo(resized, CV_32F, 1.0 / 255.0);
    cv::cvtColor(resized, resized, cv::COLOR_BGR2RGB);

    // HWC -> CHW layout
    int channels = 3;
    int height = m_inputSize.height;
    int width = m_inputSize.width;
    std::vector<float> tensor(channels * height * width);

    std::vector<cv::Mat> chw(channels);
    for (int c = 0; c < channels; ++c) {
        chw[c] = cv::Mat(height, width, CV_32FC1, tensor.data() + c * height * width);
    }
    cv::split(resized, chw);

    return tensor;
}

std::vector<Detection> InferenceEngine::postprocess(const std::vector<Ort::Value>& outputs,
                                                      const cv::Size& originalSize,
                                                      float confThreshold,
                                                      float nmsThreshold)
{
    // YOLOv8 output format: [1, num_classes + 4, num_detections]
    // Transpose to [num_detections, num_classes + 4]

    if (outputs.empty()) return {};

    auto& output = outputs[0];
    auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
    const float* data = output.GetTensorData<float>();

    if (shape.size() < 3) return {};

    int rows = static_cast<int>(shape[1]);
    int cols = static_cast<int>(shape[2]);

    // Scale factors
    float scaleX = static_cast<float>(originalSize.width)  / m_inputSize.width;
    float scaleY = static_cast<float>(originalSize.height) / m_inputSize.height;

    std::vector<cv::Rect2f> boxes;
    std::vector<float> confidences;
    std::vector<int> classIds;

    // Parse detections (YOLOv8 format: transposed)
    for (int i = 0; i < cols; ++i) {
        // First 4 values: cx, cy, w, h
        float cx = data[0 * cols + i];
        float cy = data[1 * cols + i];
        float w  = data[2 * cols + i];
        float h  = data[3 * cols + i];

        // Find best class
        float maxConf = 0.0f;
        int maxIdx = -1;
        for (int c = 4; c < rows; ++c) {
            float conf = data[c * cols + i];
            if (conf > maxConf) {
                maxConf = conf;
                maxIdx = c - 4;
            }
        }

        if (maxConf >= confThreshold) {
            float x = (cx - w / 2.0f) * scaleX;
            float y = (cy - h / 2.0f) * scaleY;
            boxes.push_back(cv::Rect2f(x, y, w * scaleX, h * scaleY));
            confidences.push_back(maxConf);
            classIds.push_back(maxIdx);
        }
    }

    // Apply NMS
    std::vector<int> indices;
    std::vector<cv::Rect> intBoxes;
    for (auto& b : boxes) {
        intBoxes.push_back(cv::Rect(
            static_cast<int>(b.x), static_cast<int>(b.y),
            static_cast<int>(b.width), static_cast<int>(b.height)
        ));
    }
    cv::dnn::NMSBoxes(intBoxes, confidences, confThreshold, nmsThreshold, indices);

    // Build result
    std::vector<Detection> detections;
    for (int idx : indices) {
        Detection det;
        det.classId    = classIds[idx];
        det.confidence = confidences[idx];
        det.bbox       = boxes[idx];
        det.className  = m_modelManager.className(det.classId);
        detections.push_back(det);
    }

    return detections;
}

std::vector<std::string> InferenceEngine::availableProviders()
{
    return Ort::GetAvailableProviders();
}

} // namespace ibom::ai
