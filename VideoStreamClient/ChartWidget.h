#pragma once

#include <QtWidgets/QWidget>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QAreaSeries>
#include <deque>
#include <QtCharts/QValueAxis>

class ChartWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ChartWidget(const QString& title, const QString& yLabel, QWidget* parent = nullptr);
    ~ChartWidget();

public slots:
    void updateChart(double value);
    void clearChart();

private:
    void initChart();

    QChartView* m_chartView;
    QChart* m_chart;
    QLineSeries* m_lineSeries;
    QAreaSeries* m_areaSeries;
    // 使用 deque 来存储历史数据，模拟 Python 的 collections.deque
    std::deque<double> m_dataHistory;
    qint64 m_startTimeMs;
    int m_maxHistorySize;
    QValueAxis* m_axisX;
    QValueAxis* m_axisY;

};