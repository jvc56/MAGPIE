#include "../src/qt/models/AnalysisModel.h"
#include "../src/qt/models/BoardSquare.h"
#include "../src/qt/models/GameHistoryModel.h"
#include "../src/qt/models/HistoryItem.h"
#include "../src/qt/models/ScoreLineItem.h"
#include <QCoreApplication>
#include <QDir>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQuickTest>

class Setup : public QObject {
  Q_OBJECT
public:
  Setup() {}

public slots:
  void qmlEngineAvailable(QQmlEngine *engine) {
    qmlRegisterType<GameHistoryModel>("QtPie", 1, 0, "GameHistoryModel");
    qmlRegisterType<AnalysisModel>("QtPie", 1, 0, "AnalysisModel");
    qmlRegisterType<BoardSquare>("QtPie", 1, 0, "BoardSquare");
    qmlRegisterType<HistoryItem>("QtPie", 1, 0, "HistoryItem");
    qmlRegisterType<ScoreLineItem>("QtPie", 1, 0, "ScoreLineItem");

    // Add source directory as import path so tests can find QML files
    QString srcPath = QDir(QCoreApplication::applicationDirPath())
                          .absoluteFilePath("../src/qt/views");
    engine->addImportPath(srcPath);

    // Also add the source root for relative imports from test QML files
    QString sourceRoot =
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("..");
    engine->addImportPath(sourceRoot);
  }
};

QUICK_TEST_MAIN_WITH_SETUP(tst_ui, Setup)

#include "tst_ui.moc"
