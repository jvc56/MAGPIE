#ifndef STUDY_LOG_H
#define STUDY_LOG_H

// Append-only study event log shared by the Letterbox and Word Walls modes.
//
// One JSON object per line in <AppData>/logs/YYYY-MM-DD.jsonl. Every line has
//   ts    wall-clock time, ms since the Unix epoch
//   app   per-launch session id
//   mode  "letterbox" or "wordwalls"
//   e     event name
// plus event-specific fields. Logs are only appended, never rewritten, so
// analysis scripts can read them while the app is running.

#include <QJsonObject>
#include <QString>

class QKeyEvent;

namespace studylog {

void write(const QString& mode, const QString& event, QJsonObject fields = {});

// Fields describing a key press: "k" is the typed text, or a key name such as
// "Backspace" or "Return", and "mods" lists any modifiers held
QJsonObject keyFields(const QKeyEvent* event);

QString logDirectory();

}  // namespace studylog

#endif // STUDY_LOG_H
