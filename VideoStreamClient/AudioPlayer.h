#pragma once

#include <QObject>
#include <QThread>
#include <atomic>
#include <portaudio.h>

// 前向声明
class JitterBuffer;
class MasterClock;

class AudioPlayer : public QObject
{
    Q_OBJECT

public:
    AudioPlayer(JitterBuffer& inputBuffer, MasterClock& clock, QObject* parent = nullptr);
    ~AudioPlayer();

public slots:
    void startPlaying();
    void stopPlaying();
    void setVolume(double volume); // 0.0 - 1.0

private:
    bool initPortAudio();
    void cleanupPortAudio();
    void playLoop();

private:
    std::atomic<bool> m_isPlaying;
    std::atomic<double> m_volume;
    JitterBuffer& m_inputBuffer;
    MasterClock& m_clock;

    PaStream* m_stream = nullptr;
    std::vector<int16_t> m_silenceBuffer;
};