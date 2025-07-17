#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_VideoStreamClient.h"

class VideoStreamClient : public QMainWindow
{
    Q_OBJECT

public:
    VideoStreamClient(QWidget *parent = nullptr);
    ~VideoStreamClient();

private:
    Ui::VideoStreamClientClass ui;
};
