#include "FSRCNNUpscaler.h"
#include <stdexcept>
#include <vector>
#include <opencv2/opencv.hpp>
#include <QDebug>
#include <algorithm>
#include <cstdint>

#include <onnxruntime_c_api.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {
    // 辅助函数 #1: float32 -> float16
    uint16_t float32_to_float16(float f) {
        uint32_t x;
        memcpy(&x, &f, sizeof(float));
        uint32_t sign = (x >> 16) & 0x8000;
        int32_t exponent = ((x >> 23) & 0xff) - 127;
        uint32_t mantissa = x & 0x7fffff;
        if (exponent > 15) return sign | 0x7c00;
        if (exponent < -14) return sign;
        exponent += 15;
        mantissa >>= 13;
        return sign | (exponent << 10) | mantissa;
    }

    // 辅助函数 #2: float16 -> float32
    float float16_to_float32(uint16_t h) {
        uint32_t sign = (h >> 15) & 1;
        uint32_t exponent = (h >> 10) & 0x1f;
        uint32_t mantissa = h & 0x3ff;
        uint32_t f;
        if (exponent == 0) {
            if (mantissa == 0) {
                f = sign << 31;
            }
            else {
                exponent = 127 - 14;
                mantissa <<= 1;
                while ((mantissa & 0x400) == 0) {
                    mantissa <<= 1;
                    exponent--;
                }
                mantissa &= 0x3ff;
                f = (sign << 31) | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
            }
        }
        else if (exponent == 0x1f) {
            f = (sign << 31) | 0x7f800000 | (mantissa << 13);
        }
        else {
            f = (sign << 31) | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
        }
        float val;
        memcpy(&val, &f, sizeof(float));
        return val;
    }
}

struct FSRCNNUpscaler::Impl {
    const OrtApi* ort_api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* session_options = nullptr;
    OrtSession* session = nullptr;
    OrtAllocator* allocator = nullptr;
    std::vector<uint16_t> input_tensor_values;
    std::vector<std::string> input_node_name_strings;
    std::vector<std::string> output_node_name_strings;
    std::vector<const char*> input_node_names;
    std::vector<const char*> output_node_names;
    bool initialized = false;

    void CheckStatus(OrtStatus* status) const {
        if (status != nullptr) {
            const char* msg = ort_api->GetErrorMessage(status);
            std::string error_message = "ONNX Runtime Error (FSRCNN): ";
            error_message += msg;
            ort_api->ReleaseStatus(status);
            throw std::runtime_error(error_message);
        }
    }

    Impl() = default;
    ~Impl() {
        if (ort_api) {
            if (session) ort_api->ReleaseSession(session);
            if (session_options) ort_api->ReleaseSessionOptions(session_options);
            if (env) ort_api->ReleaseEnv(env);
        }
    }

    bool initialize(const std::string& model_path, std::string& error_message) {
        ort_api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        if (!ort_api) {
            error_message = "Fatal: Could not get ONNX Runtime API base.";
            return false;
        }
        try {
            CheckStatus(ort_api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "FSRCNN-Upscaler", &env));
            CheckStatus(ort_api->CreateSessionOptions(&session_options));
            qInfo() << "[FSRCNN] Using CPU for inference.";
            std::wstring model_path_w(model_path.begin(), model_path.end());
            CheckStatus(ort_api->CreateSession(env, model_path_w.c_str(), session_options, &session));
            CheckStatus(ort_api->GetAllocatorWithDefaultOptions(&allocator));
            size_t num_input_nodes, num_output_nodes;
            CheckStatus(ort_api->SessionGetInputCount(session, &num_input_nodes));
            CheckStatus(ort_api->SessionGetOutputCount(session, &num_output_nodes));
            if (num_input_nodes != 1) throw std::runtime_error("Invalid ONNX model. Expected 1 input for FSRCNN.");
            char* input_name_ptr = nullptr;
            CheckStatus(ort_api->SessionGetInputName(session, 0, allocator, &input_name_ptr));
            input_node_name_strings.emplace_back(input_name_ptr);
            allocator->Free(allocator, input_name_ptr);
            input_node_names.push_back(input_node_name_strings[0].c_str());
            char* output_name_ptr = nullptr;
            CheckStatus(ort_api->SessionGetOutputName(session, 0, allocator, &output_name_ptr));
            output_node_name_strings.emplace_back(output_name_ptr);
            allocator->Free(allocator, output_name_ptr);
            output_node_names.push_back(output_node_name_strings[0].c_str());
            initialized = true;
            return true;
        }
        catch (const std::runtime_error& e) {
            error_message = e.what();
            initialized = false;
            return false;
        }
    }

    void preprocess(const cv::Mat& img_y, std::vector<uint16_t>& input_tensor) {
        cv::Mat float_img;
        img_y.convertTo(float_img, CV_32FC1, 1.0 / 255.0);
        const size_t pixel_count = (size_t)float_img.rows * float_img.cols;
        input_tensor.resize(pixel_count);
        const float* float_data = (float*)float_img.data;
        for (size_t i = 0; i < pixel_count; ++i) {
            input_tensor[i] = float32_to_float16(float_data[i]);
        }
    }

    cv::Mat postprocess(OrtValue* output_tensor, int out_h, int out_w) {
        uint16_t* pdata_16;
        CheckStatus(ort_api->GetTensorMutableData(output_tensor, (void**)&pdata_16));

        cv::Mat result_mat(out_h, out_w, CV_32FC1);
        for (int i = 0; i < out_h * out_w; ++i) {
            result_mat.at<float>(i) = float16_to_float32(pdata_16[i]);
        }

        // 【核心修改】移除归一化，直接将模型输出（通常在0-1范围）转换为0-255范围
        result_mat *= 255.0;

        cv::Mat final_img;
        result_mat.convertTo(final_img, CV_8UC1);
        return final_img;
    }

    cv::Mat run_inference_y_channel(const cv::Mat& input_y_mat) {
        preprocess(input_y_mat, input_tensor_values);
        std::vector<int64_t> input_shape = { 1, 1, (int64_t)input_y_mat.rows, (int64_t)input_y_mat.cols };
        OrtValue* input_tensor = nullptr;
        OrtValue* output_tensor = nullptr;
        try {
            OrtMemoryInfo* memory_info;
            CheckStatus(ort_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info));
            CheckStatus(ort_api->CreateTensorWithDataAsOrtValue(
                memory_info, input_tensor_values.data(), input_tensor_values.size() * sizeof(uint16_t),
                input_shape.data(), input_shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, &input_tensor));
            ort_api->ReleaseMemoryInfo(memory_info);
            CheckStatus(ort_api->Run(
                session, nullptr,
                input_node_names.data(), (const OrtValue* const*)&input_tensor, 1,
                output_node_names.data(), output_node_names.size(),
                &output_tensor));
            OrtTensorTypeAndShapeInfo* shape_info;
            CheckStatus(ort_api->GetTensorTypeAndShape(output_tensor, &shape_info));
            std::vector<int64_t> output_shape;
            size_t num_dims;
            CheckStatus(ort_api->GetDimensionsCount(shape_info, &num_dims));
            output_shape.resize(num_dims);
            CheckStatus(ort_api->GetDimensions(shape_info, output_shape.data(), num_dims));
            const int out_h = static_cast<int>(output_shape[2]);
            const int out_w = static_cast<int>(output_shape[3]);
            cv::Mat result_y = postprocess(output_tensor, out_h, out_w);
            if (shape_info) ort_api->ReleaseTensorTypeAndShapeInfo(shape_info);
            if (input_tensor) ort_api->ReleaseValue(input_tensor);
            if (output_tensor) ort_api->ReleaseValue(output_tensor);
            return result_y;
        }
        catch (const std::runtime_error& e) {
            qCritical() << "[FSRCNN] Inference failed:" << e.what();
            if (input_tensor) ort_api->ReleaseValue(input_tensor);
            if (output_tensor) ort_api->ReleaseValue(output_tensor);
            return {};
        }
    }
};

FSRCNNUpscaler::FSRCNNUpscaler() : pimpl(std::make_unique<Impl>()) {}
FSRCNNUpscaler::~FSRCNNUpscaler() = default;
FSRCNNUpscaler::FSRCNNUpscaler(FSRCNNUpscaler&&) noexcept = default;
FSRCNNUpscaler& FSRCNNUpscaler::operator=(FSRCNNUpscaler&&) noexcept = default;

bool FSRCNNUpscaler::initialize(const std::string& model_path, std::string& error_message) {
    return pimpl->initialize(model_path, error_message);
}

bool FSRCNNUpscaler::is_initialized() const {
    return pimpl && pimpl->initialized;
}

AVFrame* FSRCNNUpscaler::upscale(const AVFrame* input_frame) {
    if (!is_initialized() || !input_frame || input_frame->format != AV_PIX_FMT_YUV420P) {
        qWarning() << "[FSRCNN] Invalid input: Uninitialized or non-YUV420P frame";
        return nullptr;
    }

    // 步骤 1: 直接从AVFrame的Y通道数据创建一个cv::Mat，无需转换色彩空间
    cv::Mat input_y_channel(input_frame->height, input_frame->width, CV_8UC1, input_frame->data[0], input_frame->linesize[0]);

    // 步骤 2: 对Y通道进行超分
    // 注意：需要clone()来确保内存连续，因为原始AVFrame的内存布局可能有填充
    cv::Mat upscaled_y_channel = pimpl->run_inference_y_channel(input_y_channel.clone());
    if (upscaled_y_channel.empty()) {
        qCritical() << "[FSRCNN] Y-channel inference failed.";
        return nullptr;
    }
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(0.2, cv::Size(16, 16));
    clahe->apply(upscaled_y_channel, upscaled_y_channel);
    // 步骤 3: 同样地，直接包装U和V通道数据，并使用双三次插值法(CUBIC)将其放大
    cv::Mat u_channel(input_frame->height / 2, input_frame->width / 2, CV_8UC1, input_frame->data[1], input_frame->linesize[1]);
    cv::Mat v_channel(input_frame->height / 2, input_frame->width / 2, CV_8UC1, input_frame->data[2], input_frame->linesize[2]);

    const int target_width = upscaled_y_channel.cols;
    const int target_height = upscaled_y_channel.rows;
    cv::Mat upscaled_u_channel, upscaled_v_channel;

    // YUV420P格式的色度通道尺寸是亮度通道的一半
    cv::resize(u_channel, upscaled_u_channel, cv::Size(target_width / 2, target_height / 2), 0, 0, cv::INTER_CUBIC);
    cv::resize(v_channel, upscaled_v_channel, cv::Size(target_width / 2, target_height / 2), 0, 0, cv::INTER_CUBIC);

    // 步骤 4: 创建一个新的AVFrame，并将超分后的Y、U、V通道数据拷贝进去
    AVFrame* result_frame = av_frame_alloc();
    if (!result_frame) {
        qCritical() << "[FSRCNN] Failed to allocate result frame.";
        return nullptr;
    }

    result_frame->width = target_width;
    result_frame->height = target_height;
    result_frame->format = AV_PIX_FMT_YUV420P;
    result_frame->pts = input_frame->pts; // 保持原始时间戳

    if (av_frame_get_buffer(result_frame, 32) < 0) {
        qCritical() << "[FSRCNN] Failed to allocate buffer for result frame.";
        av_frame_free(&result_frame);
        return nullptr;
    }

    // 逐行拷贝数据，以正确处理内存对齐（stride/linesize）
    // 拷贝Y通道
    for (int y = 0; y < result_frame->height; ++y) {
        memcpy(result_frame->data[0] + y * result_frame->linesize[0], upscaled_y_channel.data + y * upscaled_y_channel.step, result_frame->width);
    }
    // 拷贝U通道
    for (int y = 0; y < result_frame->height / 2; ++y) {
        memcpy(result_frame->data[1] + y * result_frame->linesize[1], upscaled_u_channel.data + y * upscaled_u_channel.step, result_frame->width / 2);
    }
    // 拷贝V通道
    for (int y = 0; y < result_frame->height / 2; ++y) {
        memcpy(result_frame->data[2] + y * result_frame->linesize[2], upscaled_v_channel.data + y * upscaled_v_channel.step, result_frame->width / 2);
    }

    return result_frame;
}