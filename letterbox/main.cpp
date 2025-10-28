#include "letterbox_window.h"
#include <QApplication>
#include <QFontDatabase>
#include <QDebug>

#ifdef Q_OS_MACOS
extern "C" {
    void setDarkModeAppearance();
}
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

#ifdef Q_OS_MACOS
    setDarkModeAppearance();
#endif

    // Load Jost fonts
    int fontIdBold = QFontDatabase::addApplicationFont(":/fonts/Jost/static/Jost-Bold.ttf");
    if (fontIdBold == -1) {
        qWarning() << "Failed to load Jost-Bold.ttf font";
    } else {
        qDebug() << "Loaded Jost Bold font";
    }

    int fontIdRegular = QFontDatabase::addApplicationFont(":/fonts/Jost/static/Jost-Regular.ttf");
    if (fontIdRegular == -1) {
        qWarning() << "Failed to load Jost-Regular.ttf font";
    } else {
        qDebug() << "Loaded Jost Regular font";
    }

    LetterboxWindow window;
    window.show();

    return app.exec();
}
