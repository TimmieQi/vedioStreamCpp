#pragma once

#include <QtWidgets/QMainWindow>
#include <QtWidgets>
#include <QThread>
#include <QTimer>
#include "VideoWidget.h"
// 前向声明
class MasterClock;
class NetworkMonitor;
class JitterBuffer;
class DecodedFrameBuffer;
class ClientWorker;
class VideoDecoder;
class AudioPlayer;
class DebugWindow;
class RIFEInterpolator; // 前向声明

class VideoStreamClient : public QMainWindow
{
    Q_OBJECT

public:
    VideoStreamClient(QWidget* parent = nullptr);
    ~VideoStreamClient();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void initUI();
    void initConnections();
    void initWorkerThread();
    void initMediaThreads();
    void resetPlaybackUI();
    // [新增] 辅助函数，用于根据状态更新按钮的文本和样式
    void updateRIFEButtonState(bool enabled);

    QThread* m_videoDecodeThread = nullptr;
    VideoDecoder* m_videoDecoder = nullptr;
    QThread* m_audioPlayThread = nullptr;
    AudioPlayer* m_audioPlayer = nullptr;

    QTimer* m_renderTimer = nullptr;

    std::unique_ptr<MasterClock> m_masterClock;
    std::unique_ptr<NetworkMonitor> m_networkMonitor;
    std::unique_ptr<JitterBuffer> m_videoJitterBuffer;
    std::unique_ptr<JitterBuffer> m_audioJitterBuffer;
    std::unique_ptr<DecodedFrameBuffer> m_decodedFrameBuffer;
    std::unique_ptr<RIFEInterpolator> m_rife_interpolator;

    QThread* m_workerThread;
    ClientWorker* m_worker;

    QWidget* m_leftPanelWidget = nullptr;
    QLineEdit* m_ipEntry = nullptr;
    QPushButton* m_connectBtn = nullptr;
    QListWidget* m_videoList = nullptr;
    QPushButton* m_playBtn = nullptr;
    QPushButton* m_debugBtn = nullptr;
    // [修改] 将按钮定义为 QPushButton*
    QPushButton* m_rifeSwitchButton = nullptr;
    QLabel* m_latencyIndicatorLabel = nullptr;
    QLabel* m_fpsLabel = nullptr; // [新增] 用于显示帧率的标签

    QWidget* m_videoPlayerContainer = nullptr;
    VideoWidget* m_videoWidget = nullptr;

    QWidget* m_controlsWidget = nullptr;
    QSlider* m_progressSlider = nullptr;
    QPushButton* m_playPauseBtn = nullptr;
    QLabel* m_timeLabel = nullptr;
    QSlider* m_volumeSlider = nullptr;
    QPushButton* m_fullscreenBtn = nullptr;

    QHBoxLayout* m_mainLayout = nullptr;
    QPushButton* m_toggleButton = nullptr;

    double m_currentDurationSec = 0.0;
    bool m_isLeftPanelCollapsed = false;
    int m_leftPanelLastWidth = 320;

    QTimer* m_animationTimer = nullptr;
    qint64 m_animationStartTime = 0;
    int m_animationStartWidth = 0;
    int m_animationEndWidth = 0;
    int m_windowStartWidth = 0;
    const int m_animationDuration = 300;

    DebugWindow* m_debugWindow = nullptr;
    QTimer* m_statusUpdateTimer = nullptr;
    double m_currentFps = 0.0;
    double m_renderedFps = 0.0; // [新增] 用于存储渲染帧率
    int m_frameCount = 0;
    int m_renderedFrameCount = 0; // [新增] 用于渲染帧的计数器
    qint64 m_lastFpsUpdateTime = 0;
    std::atomic<double> m_currentLatencyMs;

private slots:
    void toggleFullScreen();
    void onConnectBtnClicked();
    void handleConnectionSuccess(const QList<QString>& videoList);
    void handleConnectionFailed(const QString& reason);
    void onPlayBtnClicked();
    void handlePlayInfo(double duration);
    void onRenderTimerTimeout();
    void onVolumeChanged(int value);
    void onPlayPauseBtnClicked();
    void onSliderReleased();
    void showDebugWindow();
    void onDebugWindowClosed();
    void updateStatus();
    void toggleLeftPanel();
    void onAnimationStep();
    void onLatencyUpdated(double latencyMs);
};