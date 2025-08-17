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
#include <libavutil/error.h> // For av_strerror
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
    m_control_block->running = false;

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
    AVPacket* demux_packet = av_packet_alloc();
    if (!demux_packet) return;

    auto stream_start_time = std::chrono::steady_clock::now();
    int64_t sync_start_pts_ms = 0; // 同步时间起点

    auto pause_start_time = std::chrono::steady_clock::now();
    bool was_paused = false;

    while (m_control_block->running) {

        if (m_control_block->paused) {
            if (!was_paused) {
                // 记录进入暂停状态的时刻
                pause_start_time = std::chrono::steady_clock::now();
                was_paused = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // 从暂停状态恢复
        if (was_paused) {
            // 将 stream_start_time 向后推移暂停所花费的时间
            auto pause_duration = std::chrono::steady_clock::now() - pause_start_time;
            stream_start_time += pause_duration;
            was_paused = false;
        }


        double seek_time = m_control_block->seek_to.load();
        if (seek_time >= 0) {
            m_control_block->seek_to = -1.0;

            int64_t seek_ts = av_rescale_q(static_cast<int64_t>(seek_time * 1000), { 1, 1000 }, m_video_stream->time_base);

            if (av_seek_frame(m_format_ctx, m_video_stream_index, seek_ts, AVSEEK_FLAG_BACKWARD) >= 0) {
                if (m_video_decoder_ctx) avcodec_flush_buffers(m_video_decoder_ctx);
                if (m_audio_decoder_ctx) avcodec_flush_buffers(m_audio_decoder_ctx);
                if (m_video_encoder_ctx) avcodec_flush_buffers(m_video_encoder_ctx);

                // 【核心修正】进入“寻帧同步”模式
                bool sync_point_found = false;
                while (m_control_block->running && !sync_point_found) {
                    if (av_read_frame(m_format_ctx, demux_packet) < 0) {
                        m_control_block->running = false;
                        break;
                    }
                    if (demux_packet->stream_index == m_video_stream_index) {
                        // 找到了第一个视频包，用它的时间戳作为新的同步起点
                        sync_start_pts_ms = av_rescale_q(demux_packet->pts, m_video_stream->time_base, { 1, 1000 });
                        stream_start_time = std::chrono::steady_clock::now();
                        sync_point_found = true;
                        std::cout << "[文件推流] Seek同步点找到，新的起始媒体时间: " << sync_start_pts_ms / 1000.0 << "s" << std::endl;

                        // 这个包需要被处理，所以我们不能丢弃它
                        // 通过goto跳出循环，并继续处理这个包
                        goto process_packet;
                    }
                    else {
                        // 在找到第一个视频包之前，丢弃所有其他包（主要是音频包）
                        av_packet_unref(demux_packet);
                    }
                }
            }
        }

        if (av_read_frame(m_format_ctx, demux_packet) < 0) {
            m_control_block->running = false;
            break;
        }

    process_packet:
        AVStream* packet_stream = m_format_ctx->streams[demux_packet->stream_index];
        AVCodecContext* current_decoder = (demux_packet->stream_index == m_video_stream_index) ? m_video_decoder_ctx : m_audio_decoder_ctx;

        if (!current_decoder) {
            av_packet_unref(demux_packet);
            continue;
        }

        // 丢弃所有在新的同步点之前的包
        int64_t packet_pts_ms = av_rescale_q(demux_packet->pts, packet_stream->time_base, { 1, 1000 });
        if (packet_pts_ms < sync_start_pts_ms) {
            av_packet_unref(demux_packet);
            continue;
        }

        if (avcodec_send_packet(current_decoder, demux_packet) == 0) {
            while (m_control_block->running && avcodec_receive_frame(current_decoder, m_decoded_frame) == 0) {

                int64_t current_pts_ms = av_rescale_q(m_decoded_frame->pts, packet_stream->time_base, { 1, 1000 });

                auto media_elapsed = std::chrono::milliseconds(current_pts_ms - sync_start_pts_ms);
                auto real_elapsed = std::chrono::steady_clock::now() - stream_start_time;

                if (real_elapsed < media_elapsed) {
                    std::this_thread::sleep_for(media_elapsed - real_elapsed);
                }

                m_decoded_frame->pts = current_pts_ms;

                if (m_decoded_frame->width > 0) {
                    // ... 视频帧处理逻辑不变 ...
                    if (!m_yuv_frame->data[0] || m_yuv_frame->width != m_decoded_frame->width || m_yuv_frame->height != m_decoded_frame->height) {
                        av_frame_unref(m_yuv_frame);
                        m_yuv_frame->format = AV_PIX_FMT_YUV420P;
                        m_yuv_frame->width = m_decoded_frame->width;
                        m_yuv_frame->height = m_decoded_frame->height;
                        av_frame_get_buffer(m_yuv_frame, 0);
                    }
                    m_sws_ctx_video = sws_getCachedContext(m_sws_ctx_video, m_decoded_frame->width, m_decoded_frame->height, (AVPixelFormat)m_decoded_frame->format, m_yuv_frame->width, m_yuv_frame->height, (AVPixelFormat)m_yuv_frame->format, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    sws_scale(m_sws_ctx_video, (const uint8_t* const*)m_decoded_frame->data, m_decoded_frame->linesize, 0, m_decoded_frame->height, m_yuv_frame->data, m_yuv_frame->linesize);
                    m_yuv_frame->pts = m_decoded_frame->pts;
                    encode_and_send_video(m_yuv_frame);
                }
                else if (m_decoded_frame->nb_samples > 0) {
                    resample_and_send_audio(m_decoded_frame);
                }
            }
        }
        av_packet_unref(demux_packet);
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
        send_quic_data(AppConfig::PacketType::Audio, output_buffer_array[0], data_size, frame->pts);
    }
    if (output_buffer_array) {
        av_freep(&output_buffer_array[0]);
        av_freep(&output_buffer_array);
    }
}