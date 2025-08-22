#include "AudioPlayer.h"
#include "JitterBuffer.h"
#include "MasterClock.h"
#include "shared_config.h"
#include <QDebug>
#include <vector>
#include <qcoreapplication.h>

constexpr int64_t AUDIO_SYNC_THRESHOLD_LATE = 80;

AudioPlayer::AudioPlayer(JitterBuffer& inputBuffer, MasterClock& clock, QObject* parent)
    : QObject(parent),
    m_isPlaying(false),
    m_volume(1.0),
    m_inputBuffer(inputBuffer),
    m_clock(clock)
{
    m_silenceBuffer.resize(AppConfig::AUDIO_CHUNK_SAMPLES * AppConfig::AUDIO_CHANNELS, 0);
}

AudioPlayer::~AudioPlayer()
{
    stopPlaying();
    cleanupPortAudio();
}

// ... initPortAudio 和 cleanupPortAudio 保持不变 ...
bool AudioPlayer::initPortAudio()
{
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        qDebug() << "[AudioPlayer] PortAudio 初始化错误:" << Pa_GetErrorText(err);
        return false;
    }

    err = Pa_OpenDefaultStream(
        &m_stream,
        0,
        AppConfig::AUDIO_CHANNELS,
        paInt16,
        AppConfig::AUDIO_RATE,
        AppConfig::AUDIO_CHUNK_SAMPLES,
        nullptr,
        nullptr
    );

    if (err != paNoError) {
        qDebug() << "[AudioPlayer] PortAudio 打开流错误:" << Pa_GetErrorText(err);
        return false;
    }

    qDebug() << "[AudioPlayer] PortAudio 初始化成功。";
    return true;
}

void AudioPlayer::cleanupPortAudio()
{
    if (m_stream) {
        Pa_CloseStream(m_stream);
        m_stream = nullptr;
    }
    Pa_Terminate();
    qDebug() << "[AudioPlayer] PortAudio 已清理。";
}


void AudioPlayer::startPlaying()
{
    if (m_isPlaying) return;
    if (!initPortAudio()) return;
    Pa_StartStream(m_stream);
    m_isPlaying = true;
    qDebug() << "[AudioPlayer] 音频播放循环启动。";
    playLoop();
}

void AudioPlayer::stopPlaying()
{
    m_isPlaying = false;
}

void AudioPlayer::setVolume(double volume)
{
    m_volume.store(volume);
}

void AudioPlayer::playLoop()
{
    while (m_isPlaying)
    {
        QCoreApplication::processEvents();
        if (m_clock.is_paused()) { // 等待时钟启动后再检查暂停
            QThread::msleep(10);
            continue;
        }

        auto mediaPacket = m_inputBuffer.get_packet();
        if (!mediaPacket) {
            // JitterBuffer 为空或检测到丢包
            if (m_clock.is_started()) {
                // 只有在时钟启动后才播放静音，避免一开始就播放
                Pa_WriteStream(m_stream, m_silenceBuffer.data(), m_silenceBuffer.size());
            }
            else {
                // 时钟还没启动，继续等待第一个包
                QThread::msleep(5);
            }
            continue;
        }

        // 【核心修改】检查并启动主时钟
        if (!m_clock.is_started()) {
            m_clock.start(mediaPacket->ts);
        }

        int64_t master_time_ms = m_clock.get_time_ms();
        int64_t packet_pts_ms = mediaPacket->ts;
        int64_t time_diff = packet_pts_ms - master_time_ms;

        if (time_diff < -AUDIO_SYNC_THRESHOLD_LATE) {
            qDebug() << "[AudioPlayer] 丢弃过时的音频包, PTS:" << packet_pts_ms << "ms, MasterClock:" << master_time_ms << "ms, Diff:" << time_diff << "ms";
            continue;
        }

        if (time_diff > 0) {
            QThread::msleep(static_cast<unsigned long>(time_diff));
        }

        const int16_t* audio_data_ptr = reinterpret_cast<const int16_t*>(mediaPacket->payload.data());
        size_t data_size = mediaPacket->payload.size() / sizeof(int16_t);
        double current_volume = m_volume.load();

        if (data_size > 0)
        {
            if (std::abs(current_volume - 1.0) > 1e-6) {
                std::vector<int16_t> temp_buffer(data_size);
                for (size_t i = 0; i < data_size; ++i) {
                    temp_buffer[i] = static_cast<int16_t>(audio_data_ptr[i] * current_volume);
                }
                Pa_WriteStream(m_stream, temp_buffer.data(), data_size);
            }
            else {
                Pa_WriteStream(m_stream, audio_data_ptr, data_size);
            }
        }
    }

    Pa_StopStream(m_stream);
    qDebug() << "[AudioPlayer] 音频播放循环结束。";
}