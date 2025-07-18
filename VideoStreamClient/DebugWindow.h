#pragma once

#include <QtWidgets/QMainWindow>

class ChartWidget; // 前向声明

class DebugWindow : public QMainWindow
{
    Q_OBJECT
signals: void closed();
public:
    explicit DebugWindow(QWidget* parent = nullptr);
    ~DebugWindow();

    // 提供公共接口，让主窗口可以更新图表
    ChartWidget* bitrateChart() const;
    ChartWidget* fpsChart() const;
    ChartWidget* latencyChart() const;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    ChartWidget* m_bitrateChart;
    ChartWidget* m_fpsChart;
    ChartWidget* m_latencyChart;
};