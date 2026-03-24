#include <QApplication>

#include "ui/MainWindow.h"
#include "core/WineManager.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("Sir Launchalot");
    app.setApplicationVersion("0.1.0");

    bool devMode = app.arguments().contains("--dev");
    app.setOrganizationName(devMode ? "sir-launchalot-dev" : "sir-launchalot");

    MainWindow window(devMode);
    window.show();

    return app.exec();
}
