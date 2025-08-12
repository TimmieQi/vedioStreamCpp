// ClickableSlider.cpp
#include "ClickableSlider.h"
#include <QStyle>

ClickableSlider::ClickableSlider(QWidget* parent)
    : QSlider(parent)
{
}

ClickableSlider::ClickableSlider(Qt::Orientation orientation, QWidget* parent)
    : QSlider(orientation, parent)
{
}

void ClickableSlider::mousePressEvent(QMouseEvent* event)
{
    // 调用基类的实现，确保滑块的正常拖动功能不受影响
    QSlider::mousePressEvent(event);

    // 如果是鼠标左键点击
    if (event->button() == Qt::LeftButton)
    {
        // 使用 QStyle 来计算点击位置对应的滑块值
        // 这是一种跨平台、不受样式影响的健壮方法
        int value = QStyle::sliderValueFromPosition(minimum(), maximum(), event->pos().x(), width());

        // 设置滑块到新值
        setValue(value);

        // 发出自定义信号，通知外部值已因点击而改变
        emit sliderClicked();
    }
}