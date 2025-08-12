// ClickableSlider.h
#pragma once

#include <QSlider>
#include <QMouseEvent>

class ClickableSlider : public QSlider
{
    Q_OBJECT

public:
    explicit ClickableSlider(QWidget* parent = nullptr);
    explicit ClickableSlider(Qt::Orientation orientation, QWidget* parent = nullptr);

signals:
    // 自定义一个信号，在点击时发出
    void sliderClicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;
};