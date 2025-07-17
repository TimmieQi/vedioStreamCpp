#include "VideoStreamClient.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    VideoStreamClient w;
    w.show();
    return a.exec();
}
