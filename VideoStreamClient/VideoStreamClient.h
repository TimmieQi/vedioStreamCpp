#pragma once

#include <QtWidgets/QMainWindow>
#include <QtWidgets> // 引入所有Qt Widgets类的头文件，图方便
#include <QThread>
#include<QTimer>
// 前向声明，避免在头文件中包含不必要的类
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
    // 重写键盘和关闭事件处理器
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;


private:
    // 私有方法，用于初始化
    void initUI();
    void initConnections(); // 用于连接信号槽的函数
    void initWorkerThread(); // 用于初始化工作线程的函数
    void initMediaThreads(); // 用于初始化媒体线程的函数
    void resetPlaybackUI();

    // --- 媒体处理线程 ---
    QThread* m_videoDecodeThread = nullptr;
    VideoDecoder* m_videoDecoder = nullptr;

    QThread* m_audioPlayThread = nullptr;
    AudioPlayer* m_audioPlayer = nullptr;

    // --- 渲染和UI更新 ---
    QTimer* m_renderTimer = nullptr;
    // 用于将YUV->RGB转换的FFmpeg上下文
    struct SwsContext* m_swsContext = nullptr;
    std::vector<uint8_t> m_rgbBuffer;

    // --- 核心数据结构 ---
    // 使用智能指针管理，确保正确释放内存
    std::unique_ptr<MasterClock> m_masterClock;
    std::unique_ptr<NetworkMonitor> m_networkMonitor;
    std::unique_ptr<JitterBuffer> m_videoJitterBuffer;
    std::unique_ptr<JitterBuffer> m_audioJitterBuffer;
    std::unique_ptr<DecodedFrameBuffer> m_decodedFrameBuffer;


    // --- 工作线程和 Worker 对象 ---
    QThread* m_workerThread;
    ClientWorker* m_worker;


    // --- UI 控件指针 ---

    // 左侧面板控件
    QWidget* m_leftPanelWidget = nullptr;
    QLineEdit* m_ipEntry = nullptr;
    QPushButton* m_connectBtn = nullptr;
    QListWidget* m_videoList = nullptr;
    QPushButton* m_playBtn = nullptr;
    QPushButton* m_debugBtn = nullptr;
    QLabel* m_latencyIndicatorLabel = nullptr;

    // 右侧视频播放器区域控件
    QWidget* m_videoPlayerContainer = nullptr;
    QLabel* m_videoLabel = nullptr;

    // 底部控制条控件
    QWidget* m_controlsWidget = nullptr;
    QSlider* m_progressSlider = nullptr;
    QPushButton* m_playPauseBtn = nullptr;
    QLabel* m_timeLabel = nullptr;
    QSlider* m_volumeSlider = nullptr;
    QPushButton* m_fullscreenBtn = nullptr;

    // 布局
    QHBoxLayout* m_mainLayout = nullptr;

    // 状态和几何信息
    bool m_isFullScreen = false;
    QRect m_originalGeometry;
    double m_currentDurationSec = 0.0; // 保存当前视频总时长

    // 调试窗口成员 
    DebugWindow* m_debugWindow = nullptr;
    QTimer* m_statusUpdateTimer = nullptr; // 用于定期更新图表和状态标签

    // 图表的数据
    double m_currentFps = 0.0;
    int m_frameCount = 0;
    qint64 m_lastFpsUpdateTime = 0;
    std::atomic<double> m_currentLatencyMs;
private slots:
    // 声明槽函数，用于响应信号
    void toggleFullScreen();
    void onConnectBtnClicked();
    // --- 处理 Worker 信号的槽函数 ---
    void handleConnectionSuccess(const QList<QString>& videoList);
    void handleConnectionFailed(const QString& reason);
    void onPlayBtnClicked(); 
    void handlePlayInfo(double duration); 
    void onRenderTimerTimeout(); // 渲染定时器的槽函数
    void onVolumeChanged(int value);
    void onPlayPauseBtnClicked(); // 按下暂停键
    void onSliderReleased(); // 释放进度条

    void showDebugWindow();
    void onDebugWindowClosed();
    void updateStatus();
    // 更多槽函数将在这里添加...
};