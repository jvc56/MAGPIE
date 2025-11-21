#ifndef SCORELANDITEM_H
#define SCORELANDITEM_H

#include <QObject>
#include <QString>

class ScoreLineItem : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString text READ text CONSTANT)
    Q_PROPERTY(QString scoreText READ scoreText CONSTANT)
    Q_PROPERTY(int type READ type CONSTANT) // Original game_event_t type

public:
    explicit ScoreLineItem(const QString &text, const QString &scoreText, int type, QObject *parent = nullptr);

    QString text() const;
    QString scoreText() const;
    int type() const;

private:
    QString m_text;
    QString m_scoreText;
    int m_type;
};

#endif // SCORELANDITEM_H
