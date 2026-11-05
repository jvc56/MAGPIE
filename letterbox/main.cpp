#include "letterbox_window.h"
#include <QApplication>
#include <QFontDatabase>
#include <QDebug>
#include <QScreen>
#include <QWindow>

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

    // Load Consolas font
    int fontIdConsolas = QFontDatabase::addApplicationFont(":/fonts/Consolas.ttf");
    if (fontIdConsolas == -1) {
        qWarning() << "Failed to load Consolas.ttf font";
    } else {
        qDebug() << "Loaded Consolas font";
    }

    LetterboxWindow window;

    // Fill the screen's usable area (below the menu bar, beside the dock) with
    // the title bar showing, like Rectangle's Maximize. The native window is
    // created first so its frame (title bar) size is known.
    window.winId();
    if (QScreen* screen = window.screen()) {
        QMargins frame = window.windowHandle()->frameMargins();
        if (frame.isNull()) {
            frame = QMargins(0, 28, 0, 0);  // Typical macOS title bar
        }
        window.setGeometry(screen->availableGeometry().marginsRemoved(frame));
    }
    window.show();

    return app.exec();
}
