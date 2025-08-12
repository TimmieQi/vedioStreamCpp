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
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <cmath>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

// ... (构造函数和析构函数保持不变) ...
VideoStreamClient::VideoStreamClient(QWidget* parent)
    : QMainWindow(parent),
    m_workerThread(nullptr),
    m_worker(nullptr),
    m_isFullScreen(false),
    m_isLeftPanelCollapsed(false),
    m_currentLatencyMs(0.0)
{
    m_masterClock = std::make_unique<MasterClock>();
    m_networkMonitor = std::make_unique<NetworkMonitor>();
    m_videoJitterBuffer = std::make_unique<JitterBuffer>();
    m_audioJitterBuffer = std::make_unique<JitterBuffer>();
    m_decodedFrameBuffer = std::make_unique<DecodedFrameBuffer>();

    initUI();
    initWorkerThread();
    initMediaThreads();
    initConnections();

    m_animationTimer = new QTimer(this);
    connect(m_animationTimer, &QTimer::timeout, this, &VideoStreamClient::onAnimationStep);

    m_renderTimer = new QTimer(this);
    connect(m_renderTimer, &QTimer::timeout, this, &VideoStreamClient::onRenderTimerTimeout);
    m_renderTimer->start(8);

    m_lastFpsUpdateTime = QDateTime::currentMSecsSinceEpoch();
    m_statusUpdateTimer = new QTimer(this);
    connect(m_statusUpdateTimer, &QTimer::timeout, this, &VideoStreamClient::updateStatus);
    m_statusUpdateTimer->start(1000);

    qDebug() << "客户端UI和工作线程已成功初始化。";
}

VideoStreamClient::~VideoStreamClient()
{
    if (m_workerThread && m_workerThread->isRunning()) {
        QMetaObject::invokeMethod(m_worker, "disconnectFromServer", Qt::QueuedConnection);
        m_workerThread->quit();
        m_workerThread->wait();
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

// ... (initUI, initConnections, onLatencyUpdated, toggleLeftPanel, onAnimationStep, 等UI相关代码保持不变) ...
void VideoStreamClient::initUI()
{
    const QString COLOR_BACKGROUND = "#f0f2f5";
    const QString COLOR_PANEL = "#ffffff";
    const QString COLOR_PRIMARY = "#007bff";
    const QString COLOR_PRIMARY_HOVER = "#0056b3";
    const QString COLOR_TEXT_PRIMARY = "#333333";
    const QString COLOR_TEXT_SECONDARY = "#606266";
    const QString COLOR_BORDER = "#dcdfe6";
    const QString COLOR_BUTTON_HOVER = "#e9e9e9";

    setWindowTitle("高级视频流客户端 (QUIC H.265版)"); // 更新标题
    setGeometry(100, 100, 1280, 800);
    this->setStyleSheet(QString("background-color: %1;").arg(COLOR_BACKGROUND));

    QWidget* mainWidget = new QWidget(this);
    setCentralWidget(mainWidget);
    m_mainLayout = new QHBoxLayout(mainWidget);
    m_mainLayout->setSpacing(0);
    m_mainLayout->setContentsMargins(15, 15, 15, 15);

    m_toggleButton = new QPushButton(this);
    m_toggleButton->setCursor(Qt::PointingHandCursor);
    m_toggleButton->setCheckable(true);
    m_toggleButton->setChecked(true);
    m_toggleButton->setMinimumHeight(100);
    m_toggleButton->setStyleSheet(QString(
        "QPushButton { background-color: %1; border: 1px solid %2; border-right: none; padding: 10px 5px; border-top-left-radius: 8px; border-bottom-left-radius: 8px; }"
        "QPushButton:hover { background-color: #e5e5e5; }"
    ).arg(COLOR_PANEL, COLOR_BORDER));
    m_mainLayout->addWidget(m_toggleButton);

    m_leftPanelWidget = new QWidget(this);
    m_leftPanelWidget->setMinimumWidth(m_leftPanelLastWidth);
    m_leftPanelWidget->setMaximumWidth(m_leftPanelLastWidth);
    m_leftPanelWidget->setStyleSheet(QString("background-color: %1; border-radius: 0px; border: 1px solid %2;").arg(COLOR_PANEL, COLOR_BORDER));
    QVBoxLayout* leftLayout = new QVBoxLayout(m_leftPanelWidget);
    leftLayout->setSpacing(15);
    leftLayout->setContentsMargins(20, 15, 20, 20);

    QGroupBox* connGroup = new QGroupBox("服务器连接", m_leftPanelWidget);
    connGroup->setStyleSheet(QString("QGroupBox { border: 1px solid %1; border-radius: 5px; margin-top: 10px; font-size: 14px; color: %2; } QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 0 5px; left: 10px; }").arg(COLOR_BORDER, COLOR_TEXT_PRIMARY));
    QHBoxLayout* connLayout = new QHBoxLayout(connGroup);
    connLayout->setSpacing(10);
    QLabel* ipLabel = new QLabel("服务器IP:", connGroup);
    ipLabel->setStyleSheet(QString("font-size: 14px; color: %1;").arg(COLOR_TEXT_PRIMARY));
    connLayout->addWidget(ipLabel);
    m_ipEntry = new QLineEdit("127.0.0.1", connGroup);
    m_ipEntry->setStyleSheet(QString("QLineEdit { font-size: 14px; padding: 8px; border: 1px solid %1; border-radius: 5px; color: %2; } QLineEdit:focus { border: 1px solid %3; }").arg(COLOR_BORDER, COLOR_TEXT_PRIMARY, COLOR_PRIMARY));
    connLayout->addWidget(m_ipEntry);
    m_connectBtn = new QPushButton("连接", connGroup);
    m_connectBtn->setCursor(Qt::PointingHandCursor);
    m_connectBtn->setStyleSheet(QString("QPushButton { font-size: 14px; font-weight: bold; padding: 8px 18px; background-color: %1; color: white; border: none; border-radius: 5px; } QPushButton:hover { background-color: %2; }").arg(COLOR_PRIMARY, COLOR_PRIMARY_HOVER));
    connLayout->addWidget(m_connectBtn);
    leftLayout->addWidget(connGroup);
    QLabel* playlistLabel = new QLabel("播放列表:", m_leftPanelWidget);
    playlistLabel->setStyleSheet(QString("font-size: 14px; font-weight: bold; color: %1; margin-top: 10px;").arg(COLOR_TEXT_PRIMARY));
    leftLayout->addWidget(playlistLabel);
    m_videoList = new QListWidget(m_leftPanelWidget);
    m_videoList->setMinimumHeight(450);
    m_videoList->setStyleSheet(QString("QListWidget { border: 1px solid %1; border-radius: 5px; font-size: 14px; } QListWidget::item { padding: 10px; color: %2; } QListWidget::item:hover { background-color: %3; } QListWidget::item:selected { background-color: %4; color: white; }").arg(COLOR_BORDER, COLOR_TEXT_PRIMARY, COLOR_BUTTON_HOVER, COLOR_PRIMARY));
    leftLayout->addWidget(m_videoList);
    m_playBtn = new QPushButton("播放选中项", m_leftPanelWidget);
    m_playBtn->setCursor(Qt::PointingHandCursor);
    m_playBtn->setEnabled(false);
    m_playBtn->setStyleSheet(QString("QPushButton { font-size: 14px; padding: 8px 15px; background-color: white; color: %1; border: 1px solid %2; border-radius: 5px; } QPushButton:hover { background-color: %3; } QPushButton:disabled { background-color: #f9f9f9; color: #c0c4cc; border-color: #e4e7ed; }").arg(COLOR_TEXT_PRIMARY, COLOR_BORDER, COLOR_BUTTON_HOVER));
    leftLayout->addWidget(m_playBtn);
    leftLayout->addStretch();
    m_debugBtn = new QPushButton("高级调试 (图表)", m_leftPanelWidget);
    m_debugBtn->setCursor(Qt::PointingHandCursor);
    m_debugBtn->setStyleSheet(QString("QPushButton { font-size: 14px; padding: 8px 15px; background-color: white; color: %1; border: 1px solid %2; border-radius: 5px; } QPushButton:hover { background-color: %3; }").arg(COLOR_TEXT_PRIMARY, COLOR_BORDER, COLOR_BUTTON_HOVER));
    leftLayout->addWidget(m_debugBtn);
    m_latencyIndicatorLabel = new QLabel("时延状态: 未知", m_leftPanelWidget);
    m_latencyIndicatorLabel->setAlignment(Qt::AlignCenter);
    m_latencyIndicatorLabel->setMinimumSize(120, 30);
    m_latencyIndicatorLabel->setStyleSheet(QString("QLabel { background-color: #e8f0fe; color: %1; padding: 5px; border: 1px solid %2; border-radius: 5px; font-weight: bold; font-size: 14px; }").arg(COLOR_PRIMARY, COLOR_BORDER));
    leftLayout->addWidget(m_latencyIndicatorLabel, 0, Qt::AlignHCenter);

    m_videoPlayerContainer = new QWidget(this);
    m_videoPlayerContainer->setStyleSheet(QString("background-color: %1; border-radius: 8px;").arg(COLOR_PANEL));
    QVBoxLayout* videoPlayerContainerLayout = new QVBoxLayout(m_videoPlayerContainer);
    videoPlayerContainerLayout->setContentsMargins(0, 0, 0, 0);

    m_videoLabel = new QLabel("请连接服务器并选择一个视频源", m_videoPlayerContainer);
    m_videoLabel->setAlignment(Qt::AlignCenter);
    m_videoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_videoLabel->setStyleSheet(QString("color: %1; font-size: 20px; background-color: #000000; border-top-left-radius: 8px; border-top-right-radius: 8px;").arg(COLOR_TEXT_SECONDARY));
    videoPlayerContainerLayout->addWidget(m_videoLabel, 1);
    m_controlsWidget = new QWidget(m_videoPlayerContainer);
    m_controlsWidget->setStyleSheet(QString("background-color: rgba(255, 255, 255, 0.9); border-radius: 0 0 8px 8px; padding: 8px; border-top: 1px solid %1;").arg(COLOR_BORDER));
    QVBoxLayout* controlsLayout = new QVBoxLayout(m_controlsWidget);
    controlsLayout->setSpacing(5);
    m_progressSlider = new ClickableSlider(Qt::Horizontal, m_controlsWidget);
    m_progressSlider->setEnabled(false);
    m_progressSlider->setStyleSheet(QString("QSlider::groove:horizontal { background: #e0e0e0; height: 5px; border-radius: 2px; } QSlider::handle:horizontal { background: %1; width: 16px; height: 16px; border-radius: 8px; margin: -6px 0; } QSlider::handle:horizontal:hover { background: %2; } QSlider::sub-page:horizontal { background: %1; height: 5px; border-radius: 2px; }").arg(COLOR_PRIMARY, COLOR_PRIMARY_HOVER));
    controlsLayout->addWidget(m_progressSlider);
    QHBoxLayout* bottomBar = new QHBoxLayout();
    bottomBar->setSpacing(15);
    m_playPauseBtn = new QPushButton(m_controlsWidget);
    m_playPauseBtn->setCheckable(true);
    m_playPauseBtn->setChecked(false);
    m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_playPauseBtn->setIconSize(QSize(20, 20));
    m_playPauseBtn->setCursor(Qt::PointingHandCursor);
    m_playPauseBtn->setEnabled(false);
    m_playPauseBtn->setStyleSheet(QString("QPushButton { color: %1; background-color: transparent; border: none; padding: 5px; } QPushButton:hover { color: %2; }").arg(COLOR_TEXT_PRIMARY, COLOR_PRIMARY));
    bottomBar->addWidget(m_playPauseBtn);
    m_timeLabel = new QLabel("00:00 / 00:00", m_controlsWidget);
    m_timeLabel->setStyleSheet(QString("color: %1; font-size: 14px;").arg(COLOR_TEXT_PRIMARY));
    bottomBar->addWidget(m_timeLabel);
    bottomBar->addStretch();
    QLabel* volumeLabel = new QLabel(m_controlsWidget);
    volumeLabel->setPixmap(style()->standardIcon(QStyle::SP_MediaVolume).pixmap(QSize(20, 20)));
    bottomBar->addWidget(volumeLabel);
    m_volumeSlider = new ClickableSlider(Qt::Horizontal, m_controlsWidget);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setMaximumWidth(120);
    m_volumeSlider->setStyleSheet(m_progressSlider->styleSheet());
    bottomBar->addWidget(m_volumeSlider);
    m_fullscreenBtn = new QPushButton(m_controlsWidget);
    m_fullscreenBtn->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton));
    m_fullscreenBtn->setIconSize(QSize(20, 20));
    m_fullscreenBtn->setCursor(Qt::PointingHandCursor);
    m_fullscreenBtn->setStyleSheet(m_playPauseBtn->styleSheet());
    bottomBar->addWidget(m_fullscreenBtn);
    controlsLayout->addLayout(bottomBar);
    videoPlayerContainerLayout->addWidget(m_controlsWidget);

    m_mainLayout->addWidget(m_leftPanelWidget);
    m_mainLayout->addWidget(m_videoPlayerContainer);
    m_mainLayout->setStretch(1, 0);
    m_mainLayout->setStretch(2, 1);

    statusBar()->setStyleSheet(QString("background-color: %1; color: %2; font-size: 13px; border-top: 1px solid %3;").arg(COLOR_PANEL, COLOR_TEXT_SECONDARY, COLOR_BORDER));
    statusBar()->showMessage("状态: 未连接");
}

void VideoStreamClient::initConnections()
{
    connect(m_toggleButton, &QPushButton::clicked, this, &VideoStreamClient::toggleLeftPanel);
    connect(m_fullscreenBtn, &QPushButton::clicked, this, &VideoStreamClient::toggleFullScreen);
    connect(m_connectBtn, &QPushButton::clicked, this, &VideoStreamClient::onConnectBtnClicked);
    connect(m_playBtn, &QPushButton::clicked, this, &VideoStreamClient::onPlayBtnClicked);
    connect(m_playPauseBtn, &QPushButton::clicked, this, &VideoStreamClient::onPlayPauseBtnClicked);
    connect(m_progressSlider, &QSlider::sliderReleased, this, &VideoStreamClient::onSliderReleased);
    connect(dynamic_cast<ClickableSlider*>(m_progressSlider), &ClickableSlider::sliderClicked, this, &VideoStreamClient::onSliderReleased);
    connect(m_debugBtn, &QPushButton::clicked, this, &VideoStreamClient::showDebugWindow);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &VideoStreamClient::onVolumeChanged);

    // 连接 UI 和 ClientWorker 的信号槽
    connect(m_worker, &ClientWorker::connectionSuccess, this, &VideoStreamClient::handleConnectionSuccess);
    connect(m_worker, &ClientWorker::connectionFailed, this, &VideoStreamClient::handleConnectionFailed);
    connect(m_worker, &ClientWorker::playInfoReceived, this, &VideoStreamClient::handlePlayInfo);
    connect(m_worker, &ClientWorker::latencyUpdated, this, &VideoStreamClient::onLatencyUpdated);
}

void VideoStreamClient::onLatencyUpdated(double latencyMs)
{
    m_currentLatencyMs.store(latencyMs);
}

void VideoStreamClient::toggleLeftPanel()
{
    if (m_animationTimer->isActive()) m_animationTimer->stop();
    if (m_isLeftPanelCollapsed) {
        m_leftPanelWidget->show();
        m_animationStartWidth = 0;
        m_animationEndWidth = m_leftPanelLastWidth;
    }
    else {
        m_leftPanelLastWidth = m_leftPanelWidget->width();
        if (m_leftPanelLastWidth <= 0) m_leftPanelLastWidth = 320;
        m_animationStartWidth = m_leftPanelLastWidth;
        m_animationEndWidth = 0;
    }
    m_windowStartWidth = this->width();
    m_animationStartTime = QDateTime::currentMSecsSinceEpoch();
    m_animationTimer->start(16);
}

void VideoStreamClient::onAnimationStep()
{
    qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_animationStartTime;
    double progress = static_cast<double>(elapsed) / m_animationDuration;
    if (progress >= 1.0) {
        progress = 1.0;
        m_animationTimer->stop();
    }
    double eased_progress = 1.0 - std::pow(1.0 - progress, 3);
    int currentPanelWidth = m_animationStartWidth + (m_animationEndWidth - m_animationStartWidth) * eased_progress;
    m_leftPanelWidget->setFixedWidth(currentPanelWidth);
    int width_delta = m_animationStartWidth - currentPanelWidth;
    this->resize(m_windowStartWidth - width_delta, this->height());
    if (!m_animationTimer->isActive()) {
        m_isLeftPanelCollapsed = (m_animationEndWidth == 0);
        m_toggleButton->setChecked(!m_isLeftPanelCollapsed);
        if (m_isLeftPanelCollapsed) m_leftPanelWidget->hide();
    }
}

void VideoStreamClient::initWorkerThread()
{
    m_workerThread = new QThread(this);
    m_worker = new ClientWorker(*m_networkMonitor, *m_videoJitterBuffer, *m_audioJitterBuffer);
    m_worker->moveToThread(m_workerThread);
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_workerThread->start();
    qDebug() << "[Main] 工作线程已启动。";
}

void VideoStreamClient::initMediaThreads()
{
    m_videoDecodeThread = new QThread(this);
    m_videoDecoder = new VideoDecoder(*m_videoJitterBuffer, *m_decodedFrameBuffer);
    m_videoDecoder->moveToThread(m_videoDecodeThread);
    connect(m_videoDecodeThread, &QThread::finished, m_videoDecoder, &QObject::deleteLater);
    // 【重要】启动解码循环的方式
    // 当解码线程启动时，让它开始自己的事件循环。
    // 我们将通过 invokeMethod 来触发解码逻辑。
    // 如果 VideoDecoder 有一个 run() 或 decodeLoop() 的 public slot，连接会更优雅。
    m_videoDecodeThread->start();

    m_audioPlayThread = new QThread(this);
    m_audioPlayer = new AudioPlayer(*m_audioJitterBuffer, *m_masterClock);
    m_audioPlayer->moveToThread(m_audioPlayThread);
    connect(m_audioPlayThread, &QThread::finished, m_audioPlayer, &QObject::deleteLater);
    m_audioPlayThread->start();
}
// ... (其他 UI 函数 toggleFullScreen, resetPlaybackUI 等保持不变) ...

void VideoStreamClient::toggleFullScreen()
{
    if (m_isFullScreen) {
        showNormal();
        if (!m_isLeftPanelCollapsed) m_leftPanelWidget->show();
        m_toggleButton->show();
        statusBar()->show();
        m_mainLayout->setContentsMargins(15, 15, 15, 15);
        m_fullscreenBtn->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton));
        m_isFullScreen = false;
    }
    else {
        m_originalGeometry = this->geometry();
        m_leftPanelWidget->hide();
        m_toggleButton->hide();
        statusBar()->hide();
        m_mainLayout->setContentsMargins(0, 0, 0, 0);
        showFullScreen();
        m_fullscreenBtn->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton));
        m_isFullScreen = true;
    }
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
    m_progressSlider->setEnabled(false);
    m_progressSlider->setValue(0);
    m_playPauseBtn->setEnabled(false);
    m_playPauseBtn->setChecked(false);
    m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_timeLabel->setText("00:00 / 00:00");
    m_latencyIndicatorLabel->setText("时延状态: 未知");
    m_latencyIndicatorLabel->setStyleSheet("background-color: #e8f0fe; color: #007bff; padding: 5px; border: 1px solid #dcdfe6; border-radius: 5px; font-weight: bold; font-size: 14px;");
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

void VideoStreamClient::onConnectBtnClicked()
{
    if (m_connectBtn->text() == "连接") {
        QString ip = m_ipEntry->text();
        if (ip.isEmpty()) {
            QMessageBox::warning(this, "错误", "请输入服务器IP地址。");
            return;
        }
        statusBar()->showMessage("状态: 正在连接 " + ip + "...");
        m_connectBtn->setEnabled(false);
        // 调用 ClientWorker 的 connectToServer，它会把请求转发给 QuicClient
        QMetaObject::invokeMethod(m_worker, "connectToServer", Qt::QueuedConnection,
            Q_ARG(QString, ip),
            Q_ARG(quint16, AppConfig::CONTROL_PORT)); // 使用QUIC服务器监听的端口
    }
    else {
        qDebug() << "[Main] 用户请求断开连接。";
        QMetaObject::invokeMethod(m_worker, "disconnectFromServer", Qt::QueuedConnection);
        resetPlaybackUI();
    }
}

void VideoStreamClient::onPlayBtnClicked()
{
    QListWidgetItem* currentItem = m_videoList->currentItem();
    if (!currentItem) {
        QMessageBox::warning(this, "提示", "请先选择一个播放项。");
        return;
    }
    QString source = currentItem->text();
    statusBar()->showMessage("状态: 正在请求播放 " + source + "...");

    // 重置所有媒体组件
    m_masterClock->reset();
    m_videoJitterBuffer->reset();
    m_audioJitterBuffer->reset();
    m_decodedFrameBuffer->reset();

    // 确保解码器和播放器已准备好接收新数据
    QMetaObject::invokeMethod(m_videoDecoder, "startDecoding", Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_audioPlayer, "startPlaying", Qt::QueuedConnection);

    // 发送播放请求
    QMetaObject::invokeMethod(m_worker, "requestPlay", Qt::QueuedConnection, Q_ARG(QString, source));
}

// ... (handleConnectionSuccess, handleConnectionFailed, handlePlayInfo 等处理函数保持不变) ...
void VideoStreamClient::handleConnectionSuccess(const QList<QString>& videoList)
{
    statusBar()->showMessage("状态: 连接成功，请选择播放项。");
    m_connectBtn->setText("断开");
    m_connectBtn->setEnabled(true);
    m_playBtn->setEnabled(true);
    m_videoList->clear();
    m_videoList->addItems(videoList);
}

void VideoStreamClient::handleConnectionFailed(const QString& reason)
{
    statusBar()->showMessage("状态: 连接失败 - " + reason);
    m_connectBtn->setText("连接");
    m_connectBtn->setEnabled(true);
    m_playBtn->setEnabled(false);
    QMessageBox::critical(this, "连接失败", reason);
    resetPlaybackUI();
}

void VideoStreamClient::handlePlayInfo(double duration)
{
    statusBar()->showMessage("状态: 正在播放...");
    if (duration > 0) {
        m_timeLabel->setText(QString("00:00 / %1").arg(QTime(0, 0).addSecs(static_cast<int>(duration)).toString("mm:ss")));
        m_progressSlider->setEnabled(true);
        m_progressSlider->setRange(0, 1000);
        m_progressSlider->setValue(0);
        m_playPauseBtn->setEnabled(true);
        m_playPauseBtn->setChecked(true);
        m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
        m_currentDurationSec = duration;
    }
    else {
        m_timeLabel->setText("直播");
        m_progressSlider->setEnabled(false);
        m_progressSlider->setRange(0, 0);
    }
    qDebug() << "[Main] 收到播放信息，视频时长:" << duration << "秒";
}

void VideoStreamClient::onVolumeChanged(int value)
{
    double volume = value / 100.0;
    if (m_audioPlayer) {
        QMetaObject::invokeMethod(m_audioPlayer, "setVolume", Qt::QueuedConnection, Q_ARG(double, volume));
    }
}

void VideoStreamClient::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && m_isFullScreen) {
        toggleFullScreen();
    }
    else {
        QMainWindow::keyPressEvent(event);
    }
}

void VideoStreamClient::closeEvent(QCloseEvent* event)
{
    qDebug() << "窗口关闭事件触发，执行清理...";
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "disconnectFromServer", Qt::QueuedConnection);
    }
    QMainWindow::closeEvent(event);
}

void VideoStreamClient::onPlayPauseBtnClicked()
{
    if (!m_masterClock) return;
    if (m_masterClock->is_paused()) {
        m_masterClock->resume();
        m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    }
    else {
        m_masterClock->pause();
        m_playPauseBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    }
}

void VideoStreamClient::onSliderReleased()
{
    if (m_currentDurationSec <= 0) return;
    double position = m_progressSlider->value() / 1000.0;
    double targetSec = position * m_currentDurationSec;

    // 重置缓冲区和时钟，准备跳转
    m_videoJitterBuffer->reset();
    m_audioJitterBuffer->reset();
    m_decodedFrameBuffer->reset();
    m_masterClock->reset();
    m_videoLabel->setText("正在跳转...");

    // 发送跳转请求
    QMetaObject::invokeMethod(m_worker, "requestSeek", Qt::QueuedConnection, Q_ARG(double, targetSec));
    if (m_masterClock->is_paused()) {
        onPlayPauseBtnClicked(); // 如果是暂停状态，恢复播放
    }
}
// ... (showDebugWindow, onDebugWindowClosed, updateStatus, onRenderTimerTimeout 等函数保持不变) ...

void VideoStreamClient::showDebugWindow()
{
    if (!m_debugWindow) {
        m_debugWindow = new DebugWindow(this);
        connect(m_debugWindow, &DebugWindow::closed, this, &VideoStreamClient::onDebugWindowClosed);
    }
    m_debugWindow->show();
    m_debugWindow->activateWindow();
}

void VideoStreamClient::onDebugWindowClosed()
{
    m_debugWindow = nullptr;
}

void VideoStreamClient::updateStatus()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 timeDiff = now - m_lastFpsUpdateTime;
    if (timeDiff > 0) {
        m_currentFps = (m_frameCount * 1000.0) / timeDiff;
        m_frameCount = 0;
        m_lastFpsUpdateTime = now;
    }

    NetworkStats stats = m_networkMonitor->get_statistics();
    double currentBitrateKbps = stats.bitrate_bps / 1000.0;
    double latency = m_currentLatencyMs.load();

    if (m_debugWindow) {
        if (m_masterClock->get_time_ms() >= 0 && !m_masterClock->is_paused()) {
            m_debugWindow->bitrateChart()->updateChart(currentBitrateKbps);
            m_debugWindow->fpsChart()->updateChart(m_currentFps);
            m_debugWindow->latencyChart()->updateChart(latency);
        }
    }

    if (m_masterClock->get_time_ms() >= 0 && !m_masterClock->is_paused()) {
        QString styleSheet;
        if (latency < 80) styleSheet = "background-color: lightgreen; color: black; padding: 5px; border-radius: 5px; font-weight: bold;";
        else if (latency < 200) styleSheet = "background-color: orange; color: black; padding: 5px; border-radius: 5px; font-weight: bold;";
        else styleSheet = "background-color: red; color: white; padding: 5px; border-radius: 5px; font-weight: bold;";
        m_latencyIndicatorLabel->setStyleSheet(styleSheet);
        m_latencyIndicatorLabel->setText(QString("时延: %1 ms").arg(static_cast<int>(latency)));
    }
}

void VideoStreamClient::onRenderTimerTimeout()
{
    int64_t target_pts = m_masterClock->get_time_ms();
    if (target_pts < 0) return;
    auto decoded_frame_wrapper = m_decodedFrameBuffer->get_frame(target_pts);
    if (!decoded_frame_wrapper) {
        decoded_frame_wrapper = m_decodedFrameBuffer->get_interpolated_frame(target_pts);
    }
    if (!decoded_frame_wrapper) return;
    AVFrame* frame_to_render = decoded_frame_wrapper->frame.get();
    if (!frame_to_render || !frame_to_render->data[0]) return;
    m_swsContext = sws_getCachedContext(m_swsContext,
        frame_to_render->width, frame_to_render->height, (AVPixelFormat)frame_to_render->format,
        frame_to_render->width, frame_to_render->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsContext) return;
    int rgb_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, frame_to_render->width, frame_to_render->height, 1);
    if (m_rgbBuffer.size() < static_cast<size_t>(rgb_buffer_size)) {
        m_rgbBuffer.resize(rgb_buffer_size);
    }
    uint8_t* dest_data[1] = { m_rgbBuffer.data() };
    int dest_linesize[1] = { frame_to_render->width * 3 };
    sws_scale(m_swsContext, (const uint8_t* const*)frame_to_render->data, frame_to_render->linesize, 0, frame_to_render->height, dest_data, dest_linesize);
    QImage image(m_rgbBuffer.data(), frame_to_render->width, frame_to_render->height, dest_linesize[0], QImage::Format_RGB888);
    QPixmap pixmap = QPixmap::fromImage(image.copy());
    QPixmap scaled_pixmap = pixmap.scaled(m_videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_videoLabel->setPixmap(scaled_pixmap);
    m_frameCount++;
    if (m_currentDurationSec > 0 && !m_progressSlider->isSliderDown()) {
        double current_pos_sec = target_pts / 1000.0;
        int slider_value = static_cast<int>((current_pos_sec / m_currentDurationSec) * 1000.0);
        m_progressSlider->setValue(slider_value);
        QTime current_time(0, 0);
        current_time = current_time.addSecs(static_cast<int>(current_pos_sec));
        QTime total_time(0, 0);
        total_time = total_time.addSecs(static_cast<int>(m_currentDurationSec));
        m_timeLabel->setText(QString("%1 / %2").arg(current_time.toString("mm:ss")).arg(total_time.toString("mm:ss")));
    }
}