#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>
#include <memory>
#include <onnxruntime_cxx_api.h>

namespace ibom::ai {

class ModelManager;

/// Result of a single object detection.
struct Detection {
    int    classId = -1;
    std::string className;
    float  confidence = 0.0f;
    cv::Rect2f bbox;          // Bounding box in image coords
    float  angle = 0.0f;      // Estimated rotation (if available)
};

/**
 * @brief ONNX Runtime inference engine with TensorRT acceleration.
 *
 * Handles model loading, preprocessing, inference, and postprocessing
 * for all AI tasks (component detection, solder inspection, OCR).
 */
class InferenceEngine {
public:
    explicit InferenceEngine(ModelManager& modelManager);
    ~InferenceEngine();

    InferenceEngine(const InferenceEngine&) = delete;
    InferenceEngine& operator=(const InferenceEngine&) = delete;

    /// Initialize ONNX Runtime session (call once).
    bool initialize(bool useTensorRT = true, int gpuDeviceId = 0);

    /// Whether the engine is ready for inference.
    bool isReady() const { return m_ready; }

    /// Load a specific ONNX model.
    bool loadModel(const std::string& modelPath);

    /// Run inference on a frame and return detections.
    std::vector<Detection> detect(const cv::Mat& frame,
                                   float confidenceThreshold = 0.5f,
                                   float nmsThreshold = 0.45f);

    /// Get inference time of last detect() call in milliseconds.
    float lastInferenceTimeMs() const { return m_lastInferenceMs; }

    /// Get the model input size.
    cv::Size inputSize() const { return m_inputSize; }

    /// Get available execution providers.
    static std::vector<std::string> availableProviders();

private:
    /// Preprocess frame for model input.
    std::vector<float> preprocess(const cv::Mat& frame);

    /// Run NMS (Non-Maximum Suppression) on raw detections.
    std::vector<Detection> postprocess(const std::vector<Ort::Value>& outputs,
                                        const cv::Size& originalSize,
                                        float confThreshold,
                                        float nmsThreshold);

    ModelManager& m_modelManager;

    std::unique_ptr<Ort::Env>             m_env;
    std::unique_ptr<Ort::SessionOptions>  m_sessionOptions;
    std::unique_ptr<Ort::Session>         m_session;

    std::vector<std::string> m_inputNames;
    std::vector<std::string> m_outputNames;
    std::vector<const char*> m_inputNamePtrs;
    std::vector<const char*> m_outputNamePtrs;

    cv::Size m_inputSize{640, 640}; // Default YOLO input size
    int      m_numClasses = 0;
    bool     m_ready = false;
    float    m_lastInferenceMs = 0.0f;
};

} // namespace ibom::ai
