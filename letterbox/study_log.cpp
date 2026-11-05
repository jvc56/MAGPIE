#include "study_log.h"
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>

namespace studylog {

namespace {

const QString& sessionId()
{
    static const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    return id;
}

}  // namespace

QString logDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
}

void write(const QString& mode, const QString& event, QJsonObject fields)
{
    fields["ts"] = QDateTime::currentMSecsSinceEpoch();
    fields["app"] = sessionId();
    fields["mode"] = mode;
    fields["e"] = event;

    const QString dir = logDirectory();
    QDir().mkpath(dir);
    QFile file(dir + "/" + QDate::currentDate().toString("yyyy-MM-dd") + ".jsonl");
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        file.write(QJsonDocument(fields).toJson(QJsonDocument::Compact));
        file.write("\n");
    }
}

QJsonObject keyFields(const QKeyEvent* event)
{
    QJsonObject fields;
    const QString text = event->text();
    if (!text.isEmpty() && text.at(0).isPrint()) {
        fields["k"] = text.toUpper();
    } else {
        fields["k"] = QKeySequence(event->key()).toString(QKeySequence::PortableText);
    }

    QStringList mods;
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    if (modifiers & Qt::ShiftModifier) mods << "shift";
    if (modifiers & Qt::ControlModifier) mods << "cmd";  // Command on macOS
    if (modifiers & Qt::AltModifier) mods << "alt";
    if (modifiers & Qt::MetaModifier) mods << "ctrl";    // Control on macOS
    if (!mods.isEmpty()) {
        fields["mods"] = mods.join("+");
    }
    if (event->isAutoRepeat()) {
        fields["repeat"] = true;
    }
    return fields;
}

}  // namespace studylog
