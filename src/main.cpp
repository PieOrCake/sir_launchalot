#include <QApplication>

#include "ui/MainWindow.h"
#include "core/WineManager.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("Sir Launchalot");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("sir-launchalot");

    MainWindow window;
    window.show();

    return app.exec();
}
