#pragma once

#include <QtWidgets/QMainWindow>
#include <QtWidgets>
#include <QThread>
#include <QTimer>

// 前向声明
class MasterClock;
class NetworkMonitor;
class JitterBuffer;
class DecodedFrameBuffer;
class ClientWorker;
class VideoDecoder;
class AudioPlayer;
class DebugWindow;

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

    // --- 媒体处理线程 ---
    QThread* m_videoDecodeThread = nullptr;
    VideoDecoder* m_videoDecoder = nullptr;
    QThread* m_audioPlayThread = nullptr;
    AudioPlayer* m_audioPlayer = nullptr;

    // --- 渲染和UI更新 ---
    QTimer* m_renderTimer = nullptr;
    struct SwsContext* m_swsContext = nullptr;
    std::vector<uint8_t> m_rgbBuffer;

    // --- 核心数据结构 ---
    std::unique_ptr<MasterClock> m_masterClock;
    std::unique_ptr<NetworkMonitor> m_networkMonitor;
    std::unique_ptr<JitterBuffer> m_videoJitterBuffer;
    std::unique_ptr<JitterBuffer> m_audioJitterBuffer;
    std::unique_ptr<DecodedFrameBuffer> m_decodedFrameBuffer;

    // --- 工作线程 ---
    QThread* m_workerThread;
    ClientWorker* m_worker;

    // --- UI 控件指针 ---
    QWidget* m_leftPanelWidget = nullptr;
    QLineEdit* m_ipEntry = nullptr;
    QPushButton* m_connectBtn = nullptr;
    QListWidget* m_videoList = nullptr;
    QPushButton* m_playBtn = nullptr;
    QPushButton* m_debugBtn = nullptr;
    QLabel* m_latencyIndicatorLabel = nullptr;

    QWidget* m_videoPlayerContainer = nullptr;
    QLabel* m_videoLabel = nullptr;

    QWidget* m_controlsWidget = nullptr;
    QSlider* m_progressSlider = nullptr;
    QPushButton* m_playPauseBtn = nullptr;
    QLabel* m_timeLabel = nullptr;
    QSlider* m_volumeSlider = nullptr;
    QPushButton* m_fullscreenBtn = nullptr;

    // --- 布局与动画 ---
    QHBoxLayout* m_mainLayout = nullptr; // 主布局
    QPushButton* m_toggleButton = nullptr; // 切换按钮

    // --- 状态和几何信息 ---
    bool m_isFullScreen = false;
    QRect m_originalGeometry;
    double m_currentDurationSec = 0.0;
    bool m_isLeftPanelCollapsed = false;
    int m_leftPanelLastWidth = 320;

    // --- 手动动画成员 ---
    QTimer* m_animationTimer = nullptr;
    qint64 m_animationStartTime = 0;
    int m_animationStartWidth = 0;
    int m_animationEndWidth = 0;
    int m_windowStartWidth = 0; // 新增：记录动画开始时窗口的宽度
    const int m_animationDuration = 300;

    // --- 调试 ---
    DebugWindow* m_debugWindow = nullptr;
    QTimer* m_statusUpdateTimer = nullptr;
    double m_currentFps = 0.0;
    int m_frameCount = 0;
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