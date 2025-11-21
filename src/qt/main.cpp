#include <QDebug>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QtCore/QString>
#include <QFontDatabase>
#include <QQuickStyle>
#include "models/GameHistoryModel.h"
#include "models/BoardSquare.h"
#include "models/HistoryItem.h"
#include "models/ScoreLineItem.h"

using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("QtPie");
    app.setApplicationName("QtPie");
    QQuickStyle::setStyle("Basic");

    // Register custom fonts
    QString fontPath = QCoreApplication::applicationDirPath() + "/../Resources/fonts/";
    QFontDatabase::addApplicationFont(fontPath + "ClearSans-Bold.ttf");
    QFontDatabase::addApplicationFont(fontPath + "Consolas.ttf");
    QFontDatabase::addApplicationFont(fontPath + "Roboto-Bold.ttf");

    qmlRegisterType<GameHistoryModel>("QtPie", 1, 0, "GameHistoryModel");
    qmlRegisterType<BoardSquare>("QtPie", 1, 0, "BoardSquare");
    qmlRegisterType<HistoryItem>("QtPie", 1, 0, "HistoryItem");
    qmlRegisterType<ScoreLineItem>("QtPie", 1, 0, "ScoreLineItem");

    QQmlApplicationEngine engine;
    // URL matches the URI defined in CMakeLists.txt + the file path
    const QUrl url(u"qrc:/QtPie/src/qt/views/Main.qml"_s);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreated,
        &app,
        [url](QObject *obj, const QUrl &objUrl) {
            if (!obj && url == objUrl)
                QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);
    engine.load(url);

    return app.exec();
}
