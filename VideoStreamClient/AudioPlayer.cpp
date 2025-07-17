#include "AudioPlayer.h"
#include "JitterBuffer.h"
#include "MasterClock.h"
#include "shared_config.h"
#include <QDebug>
#include <vector>
#include <qcoreapplication.h>

AudioPlayer::AudioPlayer(JitterBuffer& inputBuffer, MasterClock& clock, QObject* parent)
    : QObject(parent),
    m_isPlaying(false),
    m_volume(1.0),
    m_inputBuffer(inputBuffer),
    m_clock(clock)
{
    // 创建一个静音块，用于填充丢失的包
    m_silenceBuffer.resize(AppConfig::AUDIO_CHUNK_SAMPLES * AppConfig::AUDIO_CHANNELS, 0);
}

AudioPlayer::~AudioPlayer()
{
    stopPlaying();
    cleanupPortAudio();
}

bool AudioPlayer::initPortAudio()
{
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        qDebug() << "[AudioPlayer] PortAudio 初始化错误:" << Pa_GetErrorText(err);
        return false;
    }

    err = Pa_OpenDefaultStream(
        &m_stream,
        0, // no input channels
        AppConfig::AUDIO_CHANNELS,
        paInt16, // 16 bit integer
        AppConfig::AUDIO_RATE,
        AppConfig::AUDIO_CHUNK_SAMPLES,
        nullptr, // no callback, use blocking API
        nullptr  // no user data
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

    if (!initPortAudio()) {
        return;
    }

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
        if (m_clock.is_paused()) {
            QThread::msleep(10);
            continue;
        }

        // 从 JitterBuffer 获取一个音频包
        auto mediaPacket = m_inputBuffer.get_packet();

        const int16_t* audio_data_ptr;
        size_t data_size;

        if (mediaPacket && !mediaPacket->payload.empty()) {
            // 成功获取到包
            m_clock.start(mediaPacket->ts); // 尝试用第一个收到的包启动时钟
            m_clock.update_time(mediaPacket->ts);
            audio_data_ptr = reinterpret_cast<const int16_t*>(mediaPacket->payload.data());
            data_size = mediaPacket->payload.size() / sizeof(int16_t);
        }
        else {
            // 包丢失或缓冲区为空，播放静音
            audio_data_ptr = m_silenceBuffer.data();
            data_size = m_silenceBuffer.size();
        }

        // 处理音量
        double current_volume = m_volume.load();
        if (data_size > 0)
        {
            double current_volume = m_volume.load();

            // 当音量不等于1.0时，才需要创建临时缓冲区并修改
            if (std::abs(current_volume - 1.0) > 1e-6) { // 使用浮点数比较
                std::vector<int16_t> temp_buffer(data_size);
                for (size_t i = 0; i < data_size; ++i) {
                    temp_buffer[i] = static_cast<int16_t>(audio_data_ptr[i] * current_volume);
                }
                Pa_WriteStream(m_stream, temp_buffer.data(), data_size);
            }
            else { // 音量为1.0，直接写
                Pa_WriteStream(m_stream, audio_data_ptr, data_size);
            }
        }
    }

    Pa_StopStream(m_stream);
    qDebug() << "[AudioPlayer] 音频播放循环结束。";
}