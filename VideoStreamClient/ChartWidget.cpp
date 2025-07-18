#include "ChartWidget.h"
#include <QtWidgets/QVBoxLayout>
#include <QtCharts/QValueAxis>
#include <QDateTime>
#include <QtWidgets/QGraphicsLayout>
#include<qtimer.h>
ChartWidget::ChartWidget(const QString& title, const QString& yLabel, QWidget* parent)
    : QWidget(parent),
    m_chartView(nullptr),
    m_chart(nullptr),
    m_lineSeries(nullptr),
    m_areaSeries(nullptr),
    m_startTimeMs(0),
    m_maxHistorySize(100), // 最多显示100个数据点
    m_axisX(nullptr), 
    m_axisY(nullptr)
{
    initChart();
    m_chart->setTitle(title);
    if (m_axisY) {
        m_axisY->setTitleText(yLabel);
    }
}

ChartWidget::~ChartWidget()
{
}


void ChartWidget::initChart()
{
    // --- 1. 创建核心组件 ---
    m_chart = new QChart();
    m_chartView = new QChartView(m_chart, this);
    m_lineSeries = new QLineSeries();
    m_areaSeries = new QAreaSeries(m_lineSeries);

    // --- 2. 设置图表和系列的基本样式 ---
    m_chart->setTheme(QChart::ChartThemeDark);
    m_chart->legend()->hide();
    m_chart->layout()->setContentsMargins(0, 0, 0, 0);
    m_chart->setBackgroundRoundness(0);

    QPen pen(QColor(0, 255, 255)); // Cyan
    pen.setWidth(2);
    m_lineSeries->setPen(pen);
    m_areaSeries->setPen(Qt::NoPen);
    m_areaSeries->setColor(QColor(0, 255, 255, 64));

    // 3. 先将系列添加到图表中
    m_chart->addSeries(m_areaSeries);


    // 4. 手动创建坐标轴，并将指针保存到成员变量中
    m_axisX = new QValueAxis;
    m_axisX->setLabelFormat("%.1fs");
    m_axisX->setTitleText("时间");

    m_axisY = new QValueAxis;
    m_axisY->setLabelFormat("%.1f");
    // Y轴标题在构造函数中设置

    // 5. 将坐标轴添加到图表中
    m_chart->addAxis(m_axisX, Qt::AlignBottom);
    m_chart->addAxis(m_axisY, Qt::AlignLeft);

    // 6. 将系列附加到坐标轴上
    m_areaSeries->attachAxis(m_axisX);
    m_areaSeries->attachAxis(m_axisY);


    // --- 7. 设置布局 ---
    m_chartView->setRenderHint(QPainter::Antialiasing);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_chartView);
    setLayout(layout);
}

void ChartWidget::updateChart(double value)
{
    if (m_startTimeMs == 0) {
        m_startTimeMs = QDateTime::currentMSecsSinceEpoch();
    }

    // 当前时间点 (相对于开始时间的秒数)
    double currentTimeSec = (QDateTime::currentMSecsSinceEpoch() - m_startTimeMs) / 1000.0;

    // 添加新数据
    m_lineSeries->append(currentTimeSec, value);
    m_dataHistory.push_back(value);

    // 如果数据点超过最大数量，则移除最旧的点
    if (m_lineSeries->count() > m_maxHistorySize) {
        m_lineSeries->remove(0);
        m_dataHistory.pop_front();
    }

    if (!m_axisX || !m_axisY) return;

    // X轴：显示最近10秒的数据
    m_axisX->setRange(std::max(0.0, currentTimeSec - 10.0), currentTimeSec + 1.0);

    // Y轴：根据当前显示的数据动态调整
    if (!m_dataHistory.empty()) {
        auto minmax = std::minmax_element(m_dataHistory.cbegin(), m_dataHistory.cend());
        double minY = *minmax.first;
        double maxY = *minmax.second;
        double padding = (maxY - minY) * 0.1;

        if (padding < 1.0) padding = 1.0;

        m_axisY->setRange(std::max(0.0, minY - padding), maxY + padding);
    }
    else {
        m_axisY->setRange(0, 10);
    }
}


void ChartWidget::clearChart()
{
    m_lineSeries->clear();
    m_dataHistory.clear();
    m_startTimeMs = 0;

    // 直接使用成员变量重置坐标轴
    if (m_axisX && m_axisY) {
        m_axisX->setRange(0, 10);
        m_axisY->setRange(0, 10);
    }
}
