#include "VideoStreamClient.h"
#include "shared_config.h"
#include "MasterClock.h"
#include "NetworkMonitor.h"
#include "JitterBuffer.h"
#include "DecodedFrameBuffer.h"
#include "ClientWorker.h"
#include "VideoDecoder.h"
#include "AudioPlayer.h"
#include "ClickableSlider.h"
#include "DebugWindow.h"
#include "ChartWidget.h" 

#include <QDebug>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QMessageBox>


extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
// 构造函数
VideoStreamClient::VideoStreamClient(QWidget* parent)
    : QMainWindow(parent),
    m_workerThread(nullptr),
    m_worker(nullptr),
    m_isFullScreen(false),
    m_currentLatencyMs(0.0) 
{
    // --- 1. 初始化核心数据结构 (使用智能指针) ---
    m_masterClock = std::make_unique<MasterClock>();
    m_networkMonitor = std::make_unique<NetworkMonitor>();
    m_videoJitterBuffer = std::make_unique<JitterBuffer>();
    m_audioJitterBuffer = std::make_unique<JitterBuffer>();
    m_decodedFrameBuffer = std::make_unique<DecodedFrameBuffer>();

    // --- 2. 初始化UI和工作线程 ---
    initUI();
    initWorkerThread();
    initMediaThreads();
    initConnections();

    // 启动渲染定时器
    m_renderTimer = new QTimer(this);
    connect(m_renderTimer, &QTimer::timeout, this, &VideoStreamClient::onRenderTimerTimeout);
    m_renderTimer->start(33); // 大约 30 FPS

    m_lastFpsUpdateTime = QDateTime::currentMSecsSinceEpoch();
    m_statusUpdateTimer = new QTimer(this);
    connect(m_statusUpdateTimer, &QTimer::timeout, this, &VideoStreamClient::updateStatus);
    m_statusUpdateTimer->start(1000); // 每秒更新一次
    qDebug() << "客户端UI和工作线程已成功初始化。";
}

// 析构函数
VideoStreamClient::~VideoStreamClient()
{
    // 停止线程
    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->quit(); // 请求线程事件循环退出
        m_workerThread->wait(); // 等待线程完全结束
    }
    if (m_videoDecodeThread && m_videoDecodeThread->isRunning()) {
        QMetaObject::invokeMethod(m_videoDecoder, "stopDecoding", Qt::QueuedConnection);
        m_videoDecodeThread->quit();
        m_videoDecodeThread->wait();
    }
    if (m_audioPlayThread && m_audioPlayThread->isRunning()) {
        QMetaObject::invokeMethod(m_audioPlayer, "stopPlaying", Qt::QueuedConnection);
        m_audioPlayThread->quit();
        m_audioPlayThread->wait();
    }
    qDebug() << "客户端主窗口已销毁。";
}

// 初始化UI布局
void VideoStreamClient::initUI()
{
    // --- 窗口基本设置 ---
    setWindowTitle("高级视频流客户端 (C++ H.265版)");
    setGeometry(100, 100, 1000, 800);

    // --- 主窗口和主布局 ---
    QWidget* mainWidget = new QWidget(this);
    setCentralWidget(mainWidget);
    m_mainLayout = new QHBoxLayout(mainWidget);

    // ==========================================================
    // 构建左侧面板
    // ==========================================================
    m_leftPanelWidget = new QWidget(this);
    QVBoxLayout* leftLayout = new QVBoxLayout(m_leftPanelWidget);

    QWidget* connGroup = new QWidget(m_leftPanelWidget);
    QHBoxLayout* connLayout = new QHBoxLayout(connGroup);
    connLayout->addWidget(new QLabel("服务器IP:", connGroup));
    m_ipEntry = new QLineEdit("127.0.0.1", connGroup);
    connLayout->addWidget(m_ipEntry);
    m_connectBtn = new QPushButton("连接", connGroup);
    connLayout->addWidget(m_connectBtn);
    leftLayout->addWidget(connGroup);

    leftLayout->addWidget(new QLabel("播放列表:", m_leftPanelWidget));
    m_videoList = new QListWidget(m_leftPanelWidget);
    leftLayout->addWidget(m_videoList);

    m_playBtn = new QPushButton("播放选中项", m_leftPanelWidget);
    m_playBtn->setEnabled(false);
    leftLayout->addWidget(m_playBtn);

    m_debugBtn = new QPushButton("高级调试 (图表)", m_leftPanelWidget);
    leftLayout->addWidget(m_debugBtn);

    m_latencyIndicatorLabel = new QLabel("时延状态: 未知", m_leftPanelWidget);
    m_latencyIndicatorLabel->setAlignment(Qt::AlignCenter);
    m_latencyIndicatorLabel->setMinimumSize(120, 30);
    m_latencyIndicatorLabel->setStyleSheet(R"(
        QLabel {
            background-color: gray;
            color: white;
            padding: 5px;
            border: 2px solid white;
            border-radius: 5px;
            font-weight: bold;
        }
    )");
    leftLayout->addWidget(m_latencyIndicatorLabel);
    leftLayout->addStretch();

    // ==========================================================
    // 构建右侧视频播放器区域
    // ==========================================================
    m_videoPlayerContainer = new QWidget(this);
    QVBoxLayout* videoPlayerContainerLayout = new QVBoxLayout(m_videoPlayerContainer);
    videoPlayerContainerLayout->setContentsMargins(0, 0, 0, 0);

    m_videoLabel = new QLabel("请连接服务器并选择一个视频源", m_videoPlayerContainer);
    m_videoLabel->setAlignment(Qt::AlignCenter);
    m_videoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_videoLabel->setStyleSheet("background-color: black; color: white; font-size: 16px;");
    videoPlayerContainerLayout->addWidget(m_videoLabel, 1);

    // ==========================================================
    // 构建底部控制条
    // ==========================================================
    m_controlsWidget = new QWidget(m_videoPlayerContainer);
    QVBoxLayout* controlsLayout = new QVBoxLayout(m_controlsWidget);

    m_progressSlider = new ClickableSlider(Qt::Horizontal, m_controlsWidget);
    m_progressSlider->setEnabled(false);
    controlsLayout->addWidget(m_progressSlider);

    QHBoxLayout* bottomBar = new QHBoxLayout();

    m_playPauseBtn = new QPushButton(m_controlsWidget);
    m_playPauseBtn->setCheckable(true);
    m_playPauseBtn->setChecked(false);
    m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_playPauseBtn->setEnabled(false);
    bottomBar->addWidget(m_playPauseBtn);

    m_timeLabel = new QLabel("00:00 / 00:00", m_controlsWidget);
    bottomBar->addWidget(m_timeLabel);
    bottomBar->addStretch();

    bottomBar->addWidget(new QLabel("音量:", m_controlsWidget));
    m_volumeSlider = new ClickableSlider(Qt::Horizontal, m_controlsWidget);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setMaximumWidth(150);
    bottomBar->addWidget(m_volumeSlider);


    m_fullscreenBtn = new QPushButton(m_controlsWidget);
    m_fullscreenBtn->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton));
    bottomBar->addWidget(m_fullscreenBtn);

    controlsLayout->addLayout(bottomBar);
    videoPlayerContainerLayout->addWidget(m_controlsWidget);

    // ==========================================================
    // 组合主布局
    // ==========================================================
    m_mainLayout->addWidget(m_leftPanelWidget, 1);
    m_mainLayout->addWidget(m_videoPlayerContainer, 3);

    // ==========================================================
    // 状态栏
    // ==========================================================
    statusBar()->showMessage("状态: 未连接");
}

// 初始化工作线程
void VideoStreamClient::initWorkerThread()
{
    m_workerThread = new QThread(this);

    // 创建 Worker 实例，并将核心数据结构的引用传递给它
    m_worker = new ClientWorker(
        *m_networkMonitor,
        *m_videoJitterBuffer,
        *m_audioJitterBuffer
    );

    // 将 worker 移动到新线程。此后 worker 的所有槽函数都会在新线程中执行。
    m_worker->moveToThread(m_workerThread);

    // 当线程结束时，自动删除 worker 对象
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    // 启动线程的事件循环
    m_workerThread->start();

    qDebug() << "[Main] 工作线程已启动。";
}

void VideoStreamClient::initMediaThreads()
{
    // --- 视频解码线程 ---
    m_videoDecodeThread = new QThread(this);
    m_videoDecoder = new VideoDecoder(*m_videoJitterBuffer, *m_decodedFrameBuffer);
    m_videoDecoder->moveToThread(m_videoDecodeThread);
    connect(m_videoDecodeThread, &QThread::finished, m_videoDecoder, &QObject::deleteLater);
    m_videoDecodeThread->start();

    // --- 音频播放线程 ---
    m_audioPlayThread = new QThread(this);
    m_audioPlayer = new AudioPlayer(*m_audioJitterBuffer, *m_masterClock);
    m_audioPlayer->moveToThread(m_audioPlayThread);
    connect(m_audioPlayThread, &QThread::finished, m_audioPlayer, &QObject::deleteLater);
    m_audioPlayThread->start();
}

// 初始化所有信号槽连接
void VideoStreamClient::initConnections()
{
    // --- UI 控件的信号槽 ---
    connect(m_fullscreenBtn, &QPushButton::clicked, this, &VideoStreamClient::toggleFullScreen);
    connect(m_connectBtn, &QPushButton::clicked, this, &VideoStreamClient::onConnectBtnClicked);
    connect(m_playBtn, &QPushButton::clicked, this, &VideoStreamClient::onPlayBtnClicked);

    connect(m_playPauseBtn, &QPushButton::clicked, this, &VideoStreamClient::onPlayPauseBtnClicked);
    connect(m_progressSlider, &QSlider::sliderReleased, this, &VideoStreamClient::onSliderReleased);
    connect(dynamic_cast<ClickableSlider*>(m_progressSlider), &ClickableSlider::sliderClicked, this, &VideoStreamClient::onSliderReleased);

    connect(m_debugBtn, &QPushButton::clicked, this, &VideoStreamClient::showDebugWindow);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &VideoStreamClient::onVolumeChanged);
    // --- 主线程与工作线程之间的信号槽 ---
    connect(m_worker, &ClientWorker::connectionSuccess, this, &VideoStreamClient::handleConnectionSuccess);
    connect(m_worker, &ClientWorker::connectionFailed, this, &VideoStreamClient::handleConnectionFailed);
    connect(m_worker, &ClientWorker::playInfoReceived, this, &VideoStreamClient::handlePlayInfo);


}

void VideoStreamClient::resetPlaybackUI()
{
    statusBar()->showMessage("状态: 未连接");
    m_connectBtn->setText("连接");
    m_connectBtn->setEnabled(true);

    m_playBtn->setEnabled(false);
    m_videoList->clear();

    m_videoLabel->clear();

    m_videoLabel->setText("请连接服务器并选择一个视频源");

    m_videoLabel->setStyleSheet("background-color: black; color: white; font-size: 16px;");

    m_progressSlider->setEnabled(false);
    m_progressSlider->setValue(0);

    m_playPauseBtn->setEnabled(false);
    m_playPauseBtn->setChecked(false); // 确保是“播放”图标
    m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));

    m_timeLabel->setText("00:00 / 00:00");

    m_latencyIndicatorLabel->setText("时延状态: 未知");
    m_latencyIndicatorLabel->setStyleSheet(R"(
        QLabel { background-color: gray; color: white; /* ... */ }
    )");

    m_currentDurationSec = 0.0;

    if (m_debugWindow) {
        m_debugWindow->bitrateChart()->clearChart();
        m_debugWindow->fpsChart()->clearChart();
        m_debugWindow->latencyChart()->clearChart();
    }
    m_currentFps = 0.0;
    m_frameCount = 0;
    m_lastFpsUpdateTime = QDateTime::currentMSecsSinceEpoch();
    m_currentLatencyMs.store(0.0);
}

// "连接"按钮的槽函数
void VideoStreamClient::onConnectBtnClicked()
{
    if (m_connectBtn->text() == "连接")
    {
        // --- 执行连接逻辑 ---
        QString ip = m_ipEntry->text();
        if (ip.isEmpty()) {
            QMessageBox::warning(this, "错误", "请输入服务器IP地址。");
            return;
        }

        statusBar()->showMessage("状态: 正在连接 " + ip + "...");
        m_connectBtn->setEnabled(false);

        QMetaObject::invokeMethod(m_worker, "connectToServer", Qt::QueuedConnection,
            Q_ARG(QString, ip),
            Q_ARG(quint16, AppConfig::CONTROL_PORT));
    }
    else
    {
        // --- 执行断开逻辑 ---
        qDebug() << "[Main] 用户请求断开连接。";
        // 安全地请求工作线程执行断开操作
        QMetaObject::invokeMethod(m_worker, "disconnectFromServer", Qt::QueuedConnection);

        // 立即重置UI状态
        resetPlaybackUI();
    }
}


void VideoStreamClient::onPlayBtnClicked()
{
    // 获取当前选中的那一项的指针
    QListWidgetItem* currentItem = m_videoList->currentItem();

    // 检查指针是否有效
    if (!currentItem) {
        // 如果没有选中任何项，currentItem 会是 nullptr
        QMessageBox::warning(this, "提示", "请先选择一个播放项。");
        return;
    }

    // 直接从有效的指针获取文本
    QString source = currentItem->text();

    statusBar()->showMessage("状态: 正在请求播放 " + source + "...");
    // 在请求播放前，先清空所有缓冲区
    m_masterClock->reset();
    m_videoJitterBuffer->reset();
    m_audioJitterBuffer->reset();
    m_decodedFrameBuffer->reset();

    // 启动解码和播放循环
    QMetaObject::invokeMethod(m_videoDecoder, "startDecoding", Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_audioPlayer, "startPlaying", Qt::QueuedConnection);
    // 安全地调用工作线程中的方法
    QMetaObject::invokeMethod(m_worker, "requestPlay", Qt::QueuedConnection, Q_ARG(QString, source));
}

// 处理连接成功的槽函数
void VideoStreamClient::handleConnectionSuccess(const QList<QString>& videoList)
{
    statusBar()->showMessage("状态: 连接成功，请选择播放项。");
    m_connectBtn->setText("断开");
    m_connectBtn->setEnabled(true);
    m_playBtn->setEnabled(true);

    m_videoList->clear();
    m_videoList->addItems(videoList);
}

// 处理连接失败的槽函数
void VideoStreamClient::handleConnectionFailed(const QString& reason)
{
    statusBar()->showMessage("状态: 连接失败 - " + reason);
    m_connectBtn->setText("连接");
    m_connectBtn->setEnabled(true);
    m_playBtn->setEnabled(false);

    QMessageBox::critical(this, "连接失败", reason);
    resetPlaybackUI(); // 使用新函数来重置UI
}

// 处理收到播放信息的槽函数
void VideoStreamClient::handlePlayInfo(double duration)
{
    statusBar()->showMessage("状态: 正在播放...");
    // 在这里我们可以根据 duration 来设置进度条等UI

    if (duration > 0) {
        m_timeLabel->setText(QString("00:00 / %1").arg(QTime(0, 0).addSecs(static_cast<int>(duration)).toString("mm:ss")));
        m_progressSlider->setEnabled(true);
        m_progressSlider->setRange(0, 1000);
        m_progressSlider->setValue(0); // 播放开始时，重置为0

        m_playPauseBtn->setEnabled(true); // 启用播放/暂停按钮
        m_playPauseBtn->setChecked(true); // 默认是播放状态，所以按钮是“已选中”(显示暂停图标)
        m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
        m_currentDurationSec = duration; // 保存总时长
    }
    else {
        m_timeLabel->setText("直播");
        m_progressSlider->setEnabled(false);
        m_progressSlider->setRange(0, 0); // 直播时禁用范围
    }
    qDebug() << "[Main] 收到播放信息，视频时长:" << duration << "秒";
}

// 全屏切换的槽函数
void VideoStreamClient::toggleFullScreen()
{
    if (m_isFullScreen) {
        // --- 退出全屏 ---
        showNormal();

        m_leftPanelWidget->show();
        statusBar()->show();

        m_mainLayout->setStretchFactor(m_leftPanelWidget, 1);
        m_mainLayout->setStretchFactor(m_videoPlayerContainer, 3);

        m_fullscreenBtn->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton));

        m_isFullScreen = false;
    }
    else {
        // --- 进入全屏 ---
        m_originalGeometry = this->geometry();

        m_leftPanelWidget->hide();
        statusBar()->hide();

        m_mainLayout->setStretchFactor(m_leftPanelWidget, 0);
        m_mainLayout->setStretchFactor(m_videoPlayerContainer, 1);

        showFullScreen();

        m_fullscreenBtn->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton));

        m_isFullScreen = true;
    }
}

// 音量改变的槽函数
void VideoStreamClient::onVolumeChanged(int value)
{
    double volume = value / 100.0;
    if (m_audioPlayer) {
        QMetaObject::invokeMethod(m_audioPlayer, "setVolume", Qt::QueuedConnection, Q_ARG(double, volume));
    }
}

// 键盘事件处理器
void VideoStreamClient::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && m_isFullScreen) {
        toggleFullScreen();
    }
    else {
        QMainWindow::keyPressEvent(event);
    }
}

// 窗口关闭事件处理器
void VideoStreamClient::closeEvent(QCloseEvent* event)
{
    qDebug() << "窗口关闭事件触发，执行清理...";

    // 安全地请求断开连接并停止所有后台活动
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "disconnectFromServer", Qt::QueuedConnection);
    }

    // 调用基类的 closeEvent 来真正关闭窗口
    QMainWindow::closeEvent(event);
}

void VideoStreamClient::onPlayPauseBtnClicked()
{
    if (!m_masterClock) return;

    if (m_masterClock->is_paused()) {
        m_masterClock->resume();
        m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
        qDebug() << "[Main] 播放已恢复。";
    }
    else {
        m_masterClock->pause();
        m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        qDebug() << "[Main] 播放已暂停。";
    }
}

void VideoStreamClient::onSliderReleased()
{
    if (m_currentDurationSec <= 0) return; // 直播或无效时长，不支持seek

    // 计算目标时间
    double position = m_progressSlider->value() / 1000.0;
    double targetSec = position * m_currentDurationSec;

    qDebug() << "[Main] 用户请求跳转到" << targetSec << "秒";

    // --- 【关键】重置客户端状态 ---
    // 1. 清空所有缓冲区
    m_videoJitterBuffer->reset();
    m_audioJitterBuffer->reset();
    m_decodedFrameBuffer->reset();
    // 2. 重置时钟
    m_masterClock->reset();
    // 3. （可选）显示一个“加载中”的提示
    m_videoLabel->setText("正在跳转...");

    // 4. 向工作线程发送 seek 请求
    QMetaObject::invokeMethod(m_worker, "requestSeek", Qt::QueuedConnection, Q_ARG(double, targetSec));

    // 如果当前是暂停状态，恢复播放
    if (m_masterClock->is_paused()) {
        onPlayPauseBtnClicked();
    }
}

void VideoStreamClient::showDebugWindow()
{
    if (!m_debugWindow) {
        m_debugWindow = new DebugWindow(this);
        // 连接关闭信号
        connect(m_debugWindow, &DebugWindow::closed, this, &VideoStreamClient::onDebugWindowClosed);
    }
    m_debugWindow->show();
    m_debugWindow->activateWindow();
}

void VideoStreamClient::onDebugWindowClosed()
{
    // 当调试窗口关闭时，将指针置空
    m_debugWindow = nullptr;
}


void VideoStreamClient::updateStatus()
{
    //计算 FPS
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 timeDiff = now - m_lastFpsUpdateTime;
    if (timeDiff > 0) {
        m_currentFps = (m_frameCount * 1000.0) / timeDiff;
        m_frameCount = 0;
        m_lastFpsUpdateTime = now;
    }

    // 获取网络统计数据 
    NetworkStats stats = m_networkMonitor->get_statistics();
    double currentBitrateKbps = stats.bitrate_bps / 1000.0;



    double latency = m_currentLatencyMs.load();
    // 更新图表 (如果调试窗口存在) 
    if (m_debugWindow) {
        // 只有在播放时才更新图表
        if (m_masterClock->get_time_ms() >= 0 && !m_masterClock->is_paused()) {
            m_debugWindow->bitrateChart()->updateChart(currentBitrateKbps);
            m_debugWindow->fpsChart()->updateChart(m_currentFps);
            m_debugWindow->latencyChart()->updateChart(latency);
        }
    }

    // 更新时延状态标签
    if (m_masterClock->get_time_ms() >= 0 && !m_masterClock->is_paused()) {
        QString styleSheet;
        if (latency < 80) { // 优秀
            styleSheet = "background-color: green; color: white; padding: 5px; border-radius: 5px; font-weight: bold;";
        }
        else if (latency < 200) { // 一般
            styleSheet = "background-color: orange; color: black; padding: 5px; border-radius: 5px; font-weight: bold;";
        }
        else { // 较差
            styleSheet = "background-color: red; color: white; padding: 5px; border-radius: 5px; font-weight: bold;";
        }
        m_latencyIndicatorLabel->setStyleSheet(styleSheet);
        m_latencyIndicatorLabel->setText(QString("时延: %1 ms").arg(static_cast<int>(latency)));
    }
}


void VideoStreamClient::onRenderTimerTimeout()
{
    // 从主时钟获取当前的播放时间戳
    int64_t target_pts = m_masterClock->get_time_ms();
    if (target_pts < 0) {
        // 时钟还未启动，直接返回
        return;
    }

    // 根据时间戳从解码帧缓冲中获取最匹配的帧
    // get_frame 现在返回一个包裹着 AVFrame 的 unique_ptr<DecodedFrame>
    auto decoded_frame_wrapper = m_decodedFrameBuffer->get_frame(target_pts);
    if (!decoded_frame_wrapper) {
        // 缓冲中没有合适的帧（可能缓冲为空，或者所有帧都太新了）
        return;
    }

    // 从包装器中获取底层的 AVFrame 指针
    // .get() 方法返回原始指针，但所有权仍在 unique_ptr 手中
    AVFrame* frame_to_render = decoded_frame_wrapper->frame.get();

    if (frame_to_render) {

        double latency = static_cast<double>(target_pts - frame_to_render->pts);
        // 时延不应该是负数
        m_currentLatencyMs.store(std::max(0.0, latency));
    }
    // --- 新增结束 ---

    if (!frame_to_render || !frame_to_render->data[0]) {
        // 确保帧和其数据是有效的
        return;
    }

    // 初始化或获取缓存的 SwsContext，用于颜色空间转换
    //    这个上下文会被 FFmpeg 内部缓存，避免每次都重新计算转换参数，提高效率。
    m_swsContext = sws_getCachedContext(
        m_swsContext,                          // 传入现有的 context，如果没有则为 nullptr
        frame_to_render->width,                // 输入宽度
        frame_to_render->height,               // 输入高度
        (AVPixelFormat)frame_to_render->format,// 输入像素格式 (例如 AV_PIX_FMT_YUV420P)
        frame_to_render->width,                // 输出宽度 (保持不变)
        frame_to_render->height,               // 输出高度 (保持不变)
        AV_PIX_FMT_RGB24,                      // 输出像素格式 (QImage 常用)
        SWS_BILINEAR,                          // 缩放算法 (这里不缩放，但仍需指定)
        nullptr,                               // 输入滤镜
        nullptr,                               // 输出滤镜
        nullptr                                // 算法参数
    );

    if (!m_swsContext) {
        qDebug() << "[Render] 无法创建或获取 sws_scale 上下文。";
        return;
    }

    // 准备输出缓冲区 (用于存放转换后的 RGB 数据)
    int rgb_buffer_size = av_image_get_buffer_size(
        AV_PIX_FMT_RGB24,
        frame_to_render->width,
        frame_to_render->height,
        1 // 字节对齐，1表示不需要特殊对齐
    );
    if (m_rgbBuffer.size() < static_cast<size_t>(rgb_buffer_size)) {
        m_rgbBuffer.resize(rgb_buffer_size);
    }

    // 创建一个指向输出缓冲区的指针数组
    uint8_t* dest_data[1] = { m_rgbBuffer.data() };
    // 创建一个描述输出数据每行字节数的数组
    int dest_linesize[1] = { frame_to_render->width * 3 }; // RGB24 = 3字节/像素

    // 执行颜色空间转换！
    // 这是最关键的一步。我们直接使用 AVFrame 中的 data 和 linesize 数组。
    sws_scale(
        m_swsContext,
        (const uint8_t* const*)frame_to_render->data, // 输入的 Y, U, V 数据指针数组
        frame_to_render->linesize,                    // 输入的 Y, U, V 各自的 linesize
        0,                                            // 起始扫描线
        frame_to_render->height,                      // 处理的扫描线数量
        dest_data,                                    // 输出的数据指针数组
        dest_linesize                                 // 输出的 linesize 数组
    );

    // 将转换后的 RGB 数据包装成 QImage
    // 注意：这里的 QImage 只是引用了 m_rgbBuffer 的内存，没有进行拷贝。
    QImage image(
        m_rgbBuffer.data(),
        frame_to_render->width,
        frame_to_render->height,
        dest_linesize[0], // 使用计算出的 linesize
        QImage::Format_RGB888
    );



    QPixmap pixmap = QPixmap::fromImage(image.copy());

    QPixmap scaled_pixmap = pixmap.scaled(m_videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

    m_videoLabel->setPixmap(scaled_pixmap);
    m_frameCount++;
    // 更新UI上的时间显示和进度条
      if (m_currentDurationSec > 0 && !m_progressSlider->isSliderDown())
    {
        double current_pos_sec = target_pts / 1000.0;
        
        // 更新进度条 (范围 0-1000)
        int slider_value = static_cast<int>((current_pos_sec / m_currentDurationSec) * 1000.0);
        m_progressSlider->setValue(slider_value);
        
        // 更新时间标签
        QTime current_time(0, 0);
        current_time = current_time.addSecs(static_cast<int>(current_pos_sec));
        
        QTime total_time(0, 0);
        total_time = total_time.addSecs(static_cast<int>(m_currentDurationSec));

        m_timeLabel->setText(QString("%1 / %2")
            .arg(current_time.toString("mm:ss"))
            .arg(total_time.toString("mm:ss")));
    }
}