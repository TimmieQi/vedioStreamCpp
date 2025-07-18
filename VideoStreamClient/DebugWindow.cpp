#include "DebugWindow.h"
#include "ChartWidget.h"
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

DebugWindow::DebugWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("高级调试 - 实时图表");
    setGeometry(150, 150, 800, 700);

    // 创建三个图表实例
    m_bitrateChart = new ChartWidget("码率 (kbps)", "kbps", this);
    m_fpsChart = new ChartWidget("帧率 (FPS)", "FPS", this);
    m_latencyChart = new ChartWidget("时延 (ms)", "ms", this);

    // 设置中心窗口和布局
    QWidget* centralWidget = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(centralWidget);
    layout->addWidget(m_bitrateChart);
    layout->addWidget(m_fpsChart);
    layout->addWidget(m_latencyChart);
    setCentralWidget(centralWidget);
}

DebugWindow::~DebugWindow()
{
}

// 实现三个 getter 函数
ChartWidget* DebugWindow::bitrateChart() const { return m_bitrateChart; }
ChartWidget* DebugWindow::fpsChart() const { return m_fpsChart; }
ChartWidget* DebugWindow::latencyChart() const { return m_latencyChart; }

// 当调试窗口关闭时，需要通知主窗口
void DebugWindow::closeEvent(QCloseEvent* event)
{
    // 这个信号/槽机制我们将在主窗口中实现
    emit closed();
    QMainWindow::closeEvent(event);
}