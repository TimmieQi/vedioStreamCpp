#include "FileStreamer.h"
#include "shared_config.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

FileStreamer::FileStreamer(
    const QUIC_API_TABLE* msquic, HQUIC connection,
    std::shared_ptr<AdaptiveStreamController> controller, const std::string& video_path)
    : BaseStreamer(msquic, connection, controller), m_video_path(video_path)
{
    m_decoded_frame = av_frame_alloc();
    m_yuv_frame = av_frame_alloc();
}

FileStreamer::~FileStreamer()
{
    cleanup();
}

void FileStreamer::start()
{
    // 【核心修改】不再调用 initialize_quic_streams
    if (initialize_ffmpeg()) {
        m_control_block->running = true;
        stream_loop();
    }
    else {
        std::cerr << "[文件推流] 启动失败，初始化未完成。" << std::endl;
    }
}

void FileStreamer::cleanup()
{
    if (m_is_cleaned_up.exchange(true)) {
        return;
    }
    std::cout << "[文件推流] 开始清理文件推流特定资源..." << std::endl;

    if (m_format_ctx) { avformat_close_input(&m_format_ctx); m_format_ctx = nullptr; }
    if (m_video_decoder_ctx) { avcodec_free_context(&m_video_decoder_ctx); m_video_decoder_ctx = nullptr; }
    if (m_audio_decoder_ctx) { avcodec_free_context(&m_audio_decoder_ctx); m_audio_decoder_ctx = nullptr; }
    if (m_swr_ctx) { swr_free(&m_swr_ctx); m_swr_ctx = nullptr; }
    if (m_sws_ctx_video) { sws_freeContext(m_sws_ctx_video); m_sws_ctx_video = nullptr; }
    if (m_decoded_frame) { av_frame_free(&m_decoded_frame); m_decoded_frame = nullptr; }
    if (m_yuv_frame) { av_frame_free(&m_yuv_frame); m_yuv_frame = nullptr; }
    for (auto& pair : m_decoded_frame_buffer) { av_frame_free(&pair.second); }
    m_decoded_frame_buffer.clear();

    std::cout << "[文件推流] 文件推流特定资源已清理。" << std::endl;

    BaseStreamer::cleanup();
}

bool FileStreamer::initialize_ffmpeg() {
    // ... 此函数实现保持不变 ...
    if (avformat_open_input(&m_format_ctx, m_video_path.c_str(), nullptr, nullptr) != 0) return false;
    if (avformat_find_stream_info(m_format_ctx, nullptr) < 0) return false;
    for (unsigned int i = 0; i < m_format_ctx->nb_streams; i++) {
        AVStream* stream = m_format_ctx->streams[i];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) continue;
        AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
        if (!codec_ctx) continue;
        avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) { avcodec_free_context(&codec_ctx); continue; }
        if (decoder->type == AVMEDIA_TYPE_VIDEO && m_video_stream_index < 0) { m_video_stream_index = i; m_video_stream = stream; m_video_decoder_ctx = codec_ctx; }
        else if (decoder->type == AVMEDIA_TYPE_AUDIO && m_audio_stream_index < 0) { m_audio_stream_index = i; m_audio_stream = stream; m_audio_decoder_ctx = codec_ctx; }
        else { avcodec_free_context(&codec_ctx); }
    }
    if (m_video_stream_index < 0) return false;
    if (m_audio_stream_index >= 0) {
        AVChannelLayout in_layout;
        if (m_audio_decoder_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) { av_channel_layout_default(&in_layout, m_audio_decoder_ctx->ch_layout.nb_channels); }
        else { in_layout = m_audio_decoder_ctx->ch_layout; }
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_MONO;
        swr_alloc_set_opts2(&m_swr_ctx, &out_layout, AV_SAMPLE_FMT_S16, AppConfig::AUDIO_RATE, &in_layout, m_audio_decoder_ctx->sample_fmt, m_audio_decoder_ctx->sample_rate, 0, nullptr);
        if (!m_swr_ctx || swr_init(m_swr_ctx) < 0) return false;
    }
    std::cout << "[文件推流] FFmpeg 初始化成功。" << std::endl;
    return true;
}

void FileStreamer::stream_loop() {
    // ... 此函数大部分逻辑保持不变 ...
    AVPacket* demux_packet = av_packet_alloc();
    if (!demux_packet) return;
    bool first_frame_processed = false;
    auto start_time_perf = std::chrono::high_resolution_clock::now();
    double start_pts_sec = 0.0;
    while (m_control_block->running) {
        double seek_time = m_control_block->seek_to.load();
        if (seek_time >= 0) {
            m_control_block->seek_to = -1.0;
            int64_t seek_ts = static_cast<int64_t>(seek_time / av_q2d(m_video_stream->time_base));
            if (av_seek_frame(m_format_ctx, m_video_stream_index, seek_ts, AVSEEK_FLAG_BACKWARD) >= 0) {
                if (m_video_decoder_ctx) avcodec_flush_buffers(m_video_decoder_ctx);
                if (m_audio_decoder_ctx) avcodec_flush_buffers(m_audio_decoder_ctx);
                if (m_video_encoder_ctx) avcodec_flush_buffers(m_video_encoder_ctx);
                for (auto& pair : m_decoded_frame_buffer) av_frame_free(&pair.second);
                m_decoded_frame_buffer.clear();
                first_frame_processed = false;
            }
        }
        while (m_decoded_frame_buffer.size() < 60 && m_control_block->running) {
            if (av_read_frame(m_format_ctx, demux_packet) < 0) { m_control_block->running = false; break; }
            AVStream* packet_stream = m_format_ctx->streams[demux_packet->stream_index];
            AVCodecContext* current_decoder = (demux_packet->stream_index == m_video_stream_index) ? m_video_decoder_ctx : m_audio_decoder_ctx;
            if (!current_decoder) { av_packet_unref(demux_packet); continue; }
            if (avcodec_send_packet(current_decoder, demux_packet) == 0) {
                while (avcodec_receive_frame(current_decoder, m_decoded_frame) == 0) {
                    AVFrame* frame_clone = av_frame_clone(m_decoded_frame);
                    double pts_sec = frame_clone->pts * av_q2d(packet_stream->time_base);
                    m_decoded_frame_buffer.emplace_back(pts_sec, frame_clone);
                }
            }
            av_packet_unref(demux_packet);
        }
        if (m_decoded_frame_buffer.empty()) break;
        std::sort(m_decoded_frame_buffer.begin(), m_decoded_frame_buffer.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        auto [current_pts_sec, frame_to_process] = m_decoded_frame_buffer.front();
        m_decoded_frame_buffer.pop_front();
        if (!first_frame_processed) {
            start_pts_sec = current_pts_sec;
            start_time_perf = std::chrono::high_resolution_clock::now();
            first_frame_processed = true;
        }
        auto target_elapsed = std::chrono::duration<double>(current_pts_sec - start_pts_sec);
        auto real_elapsed = std::chrono::high_resolution_clock::now() - start_time_perf;
        if ((target_elapsed - real_elapsed).count() > 0.001) {
            std::this_thread::sleep_for(target_elapsed - real_elapsed);
        }
        frame_to_process->pts = static_cast<int64_t>(current_pts_sec * 1000);

        if (frame_to_process->width > 0) {
            if (!m_yuv_frame->data[0] || m_yuv_frame->width != frame_to_process->width || m_yuv_frame->height != frame_to_process->height) {
                av_frame_unref(m_yuv_frame);
                m_yuv_frame->format = AV_PIX_FMT_YUV420P;
                m_yuv_frame->width = frame_to_process->width;
                m_yuv_frame->height = frame_to_process->height;
                if (av_frame_get_buffer(m_yuv_frame, 0) < 0) {
                    std::cerr << "[文件推流] 错误：无法为YUV帧分配缓冲区" << std::endl;
                    av_frame_free(&frame_to_process);
                    continue;
                }
            }

            m_sws_ctx_video = sws_getCachedContext(
                m_sws_ctx_video,
                frame_to_process->width, frame_to_process->height, (AVPixelFormat)frame_to_process->format,
                m_yuv_frame->width, m_yuv_frame->height, (AVPixelFormat)m_yuv_frame->format,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
            );

            if (!m_sws_ctx_video) {
                std::cerr << "[文件推流] 错误：无法创建SWS转换上下文" << std::endl;
                av_frame_free(&frame_to_process);
                continue;
            }

            sws_scale(
                m_sws_ctx_video,
                (const uint8_t* const*)frame_to_process->data, frame_to_process->linesize,
                0, frame_to_process->height,
                m_yuv_frame->data, m_yuv_frame->linesize
            );
            m_yuv_frame->pts = frame_to_process->pts;
            encode_and_send_video(m_yuv_frame);
        }
        else if (frame_to_process->nb_samples > 0) {
            resample_and_send_audio(frame_to_process);
        }
        av_frame_free(&frame_to_process);
    }
    if (m_video_encoder_ctx) encode_and_send_video(nullptr);
    av_packet_free(&demux_packet);
    std::cout << "[文件推流] 推流循环结束。" << std::endl;
}

void FileStreamer::resample_and_send_audio(AVFrame* frame) {
    if (!m_swr_ctx || !frame) return;
    uint8_t** output_buffer_array = nullptr;
    int linesize;
    int output_samples = av_rescale_rnd(swr_get_delay(m_swr_ctx, frame->sample_rate) + frame->nb_samples, AppConfig::AUDIO_RATE, frame->sample_rate, AV_ROUND_UP);
    av_samples_alloc_array_and_samples(&output_buffer_array, &linesize, AppConfig::AUDIO_CHANNELS, output_samples, AV_SAMPLE_FMT_S16, 0);
    int samples_converted = swr_convert(m_swr_ctx, output_buffer_array, output_samples, (const uint8_t**)frame->data, frame->nb_samples);
    if (samples_converted > 0) {
        int data_size = av_samples_get_buffer_size(nullptr, AppConfig::AUDIO_CHANNELS, samples_converted, AV_SAMPLE_FMT_S16, 1);
        // 【核心修改】发送音频包时，传入音频类型
        send_quic_data(AppConfig::PacketType::Audio, output_buffer_array[0], data_size, frame->pts);
    }
    if (output_buffer_array) {
        av_freep(&output_buffer_array[0]);
        av_freep(&output_buffer_array);
    }
}