#include "VideoStreamClient.h"
#include <QtWidgets/QApplication>
extern "C" { 
#include <libavutil/frame.h> 
}
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    qRegisterMetaType<AVFrame*>();
    VideoStreamClient w;
    w.show();
    return a.exec();
}
