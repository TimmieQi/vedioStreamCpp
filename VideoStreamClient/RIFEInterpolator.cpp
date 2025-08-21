#include "RIFEInterpolator.h"
#include <stdexcept>
#include <vector>
#include <opencv2/opencv.hpp>
#include <QDebug>

// ONNX Runtime C API 头文件
#include <onnxruntime_c_api.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// 辅助函数 (保持不变)
namespace {
    bool avframe_to_mat_bgr(const AVFrame* av_frame, cv::Mat& out_mat) {
        if (!av_frame || av_frame->format != AV_PIX_FMT_YUV420P) return false;
        SwsContext* sws_ctx = sws_getContext(av_frame->width, av_frame->height, (AVPixelFormat)av_frame->format, av_frame->width, av_frame->height, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx) return false;
        out_mat = cv::Mat(av_frame->height, av_frame->width, CV_8UC3);
        int dst_stride = static_cast<int>(out_mat.step);
        uint8_t* dst_data = out_mat.data;
        sws_scale(sws_ctx, av_frame->data, av_frame->linesize, 0, av_frame->height, &dst_data, &dst_stride);
        sws_freeContext(sws_ctx);
        return true;
    }

    AVFrame* mat_bgr_to_avframe(const cv::Mat& mat) {
        AVFrame* frame = av_frame_alloc();
        if (!frame) return nullptr;
        frame->width = mat.cols;
        frame->height = mat.rows;
        frame->format = AV_PIX_FMT_YUV420P;
        if (av_frame_get_buffer(frame, 32) < 0) { av_frame_free(&frame); return nullptr; }
        SwsContext* sws_ctx = sws_getContext(mat.cols, mat.rows, AV_PIX_FMT_BGR24, mat.cols, mat.rows, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx) { av_frame_free(&frame); return nullptr; }
        int src_stride = static_cast<int>(mat.step);
        const uint8_t* src_data = mat.data;
        sws_scale(sws_ctx, &src_data, &src_stride, 0, mat.rows, frame->data, frame->linesize);
        sws_freeContext(sws_ctx);
        return frame;
    }
}

// Pimpl (Pointer to Implementation) 结构体定义
struct RIFEInterpolator::Impl {
    const OrtApi* ort_api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* session_options = nullptr;
    OrtSession* session = nullptr;
    OrtAllocator* allocator = nullptr;

    std::vector<float> input1_tensor_values;
    std::vector<float> input2_tensor_values;

    std::vector<std::string> input_node_name_strings;
    std::vector<std::string> output_node_name_strings;
    std::vector<const char*> input_node_names;
    std::vector<const char*> output_node_names;

    bool initialized = false;
    int inpWidth = 0;
    int inpHeight = 0;

    void CheckStatus(OrtStatus* status) const {
        if (status != nullptr) {
            const char* msg = ort_api->GetErrorMessage(status);
            std::string error_message = "ONNX Runtime Error: ";
            error_message += msg;
            ort_api->ReleaseStatus(status);
            throw std::runtime_error(error_message);
        }
    }

    // 构造函数现在是空的
    Impl() = default;

    ~Impl() {
        if (ort_api) {
            if (session) ort_api->ReleaseSession(session);
            if (session_options) ort_api->ReleaseSessionOptions(session_options);
            if (env) ort_api->ReleaseEnv(env);
        }
    }

    // 所有初始化逻辑都在这里
    bool initialize(const std::string& model_path, std::string& error_message) {
        ort_api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        if (!ort_api) {
            error_message = "Fatal: Could not get ONNX Runtime API base.";
            return false;
        }

        try {
            CheckStatus(ort_api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "RIFE-Interpolator", &env));
            CheckStatus(ort_api->CreateSessionOptions(&session_options));

            // ========== START: MODIFIED FOR GPU ACCELERATION ==========
            OrtCUDAProviderOptions cuda_options{};
            OrtStatus* cuda_status = ort_api->SessionOptionsAppendExecutionProvider_CUDA(session_options, &cuda_options);

            if (cuda_status != nullptr) {
                const char* msg = ort_api->GetErrorMessage(cuda_status);
                qWarning() << "[RIFE] Could not enable CUDA execution provider. Reason:" << msg;
                ort_api->ReleaseStatus(cuda_status);
                qWarning() << "[RIFE] Falling back to CPU. Performance will be limited.";
            }
            else {
                qInfo() << "[RIFE] CUDA execution provider enabled successfully. Using GPU for inference.";
            }
            // ========== END: MODIFIED FOR GPU ACCELERATION ==========

            std::wstring model_path_w(model_path.begin(), model_path.end());
            CheckStatus(ort_api->CreateSession(env, model_path_w.c_str(), session_options, &session));

            CheckStatus(ort_api->GetAllocatorWithDefaultOptions(&allocator));

            size_t num_input_nodes, num_output_nodes;
            CheckStatus(ort_api->SessionGetInputCount(session, &num_input_nodes));
            CheckStatus(ort_api->SessionGetOutputCount(session, &num_output_nodes));

            if (num_input_nodes != 2) {
                throw std::runtime_error("Invalid ONNX model. Expected 2 inputs.");
            }

            for (size_t i = 0; i < num_input_nodes; ++i) {
                char* input_name_ptr = nullptr;
                CheckStatus(ort_api->SessionGetInputName(session, i, allocator, &input_name_ptr));
                if (input_name_ptr) {
                    input_node_name_strings.emplace_back(input_name_ptr);
                    allocator->Free(allocator, input_name_ptr);
                }
                else {
                    throw std::runtime_error("Failed to get input name for node index " + std::to_string(i));
                }
            }
            for (const auto& name : input_node_name_strings) {
                input_node_names.push_back(name.c_str());
            }

            for (size_t i = 0; i < num_output_nodes; ++i) {
                char* output_name_ptr = nullptr;
                CheckStatus(ort_api->SessionGetOutputName(session, i, allocator, &output_name_ptr));
                if (output_name_ptr) {
                    output_node_name_strings.emplace_back(output_name_ptr);
                    allocator->Free(allocator, output_name_ptr);
                }
                else {
                    throw std::runtime_error("Failed to get output name for node index " + std::to_string(i));
                }
            }
            for (const auto& name : output_node_name_strings) {
                output_node_names.push_back(name.c_str());
            }

            initialized = true;
            return true;

        }
        catch (const std::runtime_error& e) {
            error_message = e.what();
            initialized = false;
            return false;
        }
    }

    void preprocess(const cv::Mat& img, std::vector<float>& input_tensor) {
        cv::Mat rgbimg;
        cv::cvtColor(img, rgbimg, cv::COLOR_BGR2RGB);

        const int h = rgbimg.rows;
        const int w = rgbimg.cols;
        const int align = 32;
        int ph = h, pw = w;

        if (h % align != 0 || w % align != 0) {
            ph = (int((h - 1) / align) + 1) * align;
            pw = (int((w - 1) / align) + 1) * align;
            cv::copyMakeBorder(rgbimg, rgbimg, 0, ph - h, 0, pw - w, cv::BORDER_CONSTANT, 0);
        }

        inpHeight = rgbimg.rows;
        inpWidth = rgbimg.cols;

        rgbimg.convertTo(rgbimg, CV_32FC3, 1.0 / 255.0);

        const int image_area = inpHeight * inpWidth;
        input_tensor.resize(static_cast<size_t>(3) * image_area);

        std::vector<cv::Mat> rgbChannels(3);
        cv::split(rgbimg, rgbChannels);

        memcpy(input_tensor.data(), (float*)rgbChannels[0].data, image_area * sizeof(float));
        memcpy(input_tensor.data() + image_area, (float*)rgbChannels[1].data, image_area * sizeof(float));
        memcpy(input_tensor.data() + 2 * image_area, (float*)rgbChannels[2].data, image_area * sizeof(float));
    }

    cv::Mat postprocess(OrtValue* output_tensor) {
        float* pdata;
        CheckStatus(ort_api->GetTensorMutableData(output_tensor, (void**)&pdata));

        OrtTensorTypeAndShapeInfo* shape_info;
        CheckStatus(ort_api->GetTensorTypeAndShape(output_tensor, &shape_info));
        std::vector<int64_t> output_shape;
        size_t num_dims;
        CheckStatus(ort_api->GetDimensionsCount(shape_info, &num_dims));
        output_shape.resize(num_dims);
        CheckStatus(ort_api->GetDimensions(shape_info, output_shape.data(), num_dims));

        const int out_h = static_cast<int>(output_shape[2]);
        const int out_w = static_cast<int>(output_shape[3]);
        const int channel_step = out_h * out_w;

        cv::Mat rmat(out_h, out_w, CV_32FC1, pdata);
        cv::Mat gmat(out_h, out_w, CV_32FC1, pdata + channel_step);
        cv::Mat bmat(out_h, out_w, CV_32FC1, pdata + 2 * channel_step);

        rmat *= 255.f; gmat *= 255.f; bmat *= 255.f;
        cv::threshold(rmat, rmat, 255, 255, cv::THRESH_TRUNC); cv::threshold(rmat, rmat, 0, 0, cv::THRESH_TOZERO);
        cv::threshold(gmat, gmat, 255, 255, cv::THRESH_TRUNC); cv::threshold(gmat, gmat, 0, 0, cv::THRESH_TOZERO);
        cv::threshold(bmat, bmat, 255, 255, cv::THRESH_TRUNC); cv::threshold(bmat, bmat, 0, 0, cv::THRESH_TOZERO);

        std::vector<cv::Mat> channel_mats(3);
        channel_mats[0] = bmat; channel_mats[1] = gmat; channel_mats[2] = rmat;

        cv::Mat dstimg;
        cv::merge(channel_mats, dstimg);
        dstimg.convertTo(dstimg, CV_8UC3);

        if (ort_api && shape_info) ort_api->ReleaseTensorTypeAndShapeInfo(shape_info);
        return dstimg;
    }
};

// --- RIFEInterpolator 类的公共接口实现 ---

RIFEInterpolator::RIFEInterpolator()
    : pimpl(std::make_unique<Impl>()) {
}

RIFEInterpolator::~RIFEInterpolator() = default;
RIFEInterpolator::RIFEInterpolator(RIFEInterpolator&&) noexcept = default;
RIFEInterpolator& RIFEInterpolator::operator=(RIFEInterpolator&&) noexcept = default;

bool RIFEInterpolator::initialize(const std::string& model_path, std::string& error_message) {
    return pimpl->initialize(model_path, error_message);
}

bool RIFEInterpolator::is_initialized() const {
    return pimpl && pimpl->initialized;
}

AVFrame* RIFEInterpolator::interpolate(const AVFrame* prev, const AVFrame* next, double /*factor*/) {
    if (!is_initialized()) return nullptr;

    cv::Mat mat1, mat2;
    if (!avframe_to_mat_bgr(prev, mat1) || !avframe_to_mat_bgr(next, mat2)) return nullptr;

    const int srch = mat1.rows;
    const int srcw = mat1.cols;

    pimpl->preprocess(mat1, pimpl->input1_tensor_values);
    pimpl->preprocess(mat2, pimpl->input2_tensor_values);

    std::vector<int64_t> input_img_shape = { 1, 3, pimpl->inpHeight, pimpl->inpWidth };

    OrtValue* input_tensors[2] = { nullptr, nullptr };
    OrtValue* output_tensor = nullptr;
    AVFrame* result_frame = nullptr;

    try {
        OrtMemoryInfo* memory_info;
        pimpl->CheckStatus(pimpl->ort_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info));

        pimpl->CheckStatus(pimpl->ort_api->CreateTensorWithDataAsOrtValue(memory_info, pimpl->input1_tensor_values.data(), pimpl->input1_tensor_values.size() * sizeof(float), input_img_shape.data(), input_img_shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensors[0]));
        pimpl->CheckStatus(pimpl->ort_api->CreateTensorWithDataAsOrtValue(memory_info, pimpl->input2_tensor_values.data(), pimpl->input2_tensor_values.size() * sizeof(float), input_img_shape.data(), input_img_shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensors[1]));

        pimpl->ort_api->ReleaseMemoryInfo(memory_info);

        pimpl->CheckStatus(pimpl->ort_api->Run(
            pimpl->session,
            nullptr,
            pimpl->input_node_names.data(),
            (const OrtValue* const*)input_tensors,
            pimpl->input_node_names.size(),
            pimpl->output_node_names.data(),
            pimpl->output_node_names.size(),
            &output_tensor));

        cv::Mat interpolated_bgr = pimpl->postprocess(output_tensor);
        cv::Mat final_bgr = interpolated_bgr(cv::Rect{ 0, 0, srcw, srch });
        result_frame = mat_bgr_to_avframe(final_bgr);

    }
    catch (const std::runtime_error& e) {
        qCritical() << "[RIFE] Interpolation failed:" << e.what();
        result_frame = nullptr;
    }

    if (input_tensors[0]) pimpl->ort_api->ReleaseValue(input_tensors[0]);
    if (input_tensors[1]) pimpl->ort_api->ReleaseValue(input_tensors[1]);
    if (output_tensor) pimpl->ort_api->ReleaseValue(output_tensor);

    return result_frame;
}