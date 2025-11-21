#ifndef HISTORYITEM_H
#define HISTORYITEM_H

#include <QObject>
#include <QString>
#include <QList>
#include "ScoreLineItem.h" // Renamed from MoveLineItem

class HistoryItem : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int playerIndex READ playerIndex CONSTANT)
    Q_PROPERTY(int type READ type CONSTANT)
    Q_PROPERTY(QString rackString READ rackString CONSTANT)
    Q_PROPERTY(int score READ score CONSTANT) // This is the calculated total turn score for this item
    Q_PROPERTY(int cumulativeScore READ cumulativeScore CONSTANT)
    Q_PROPERTY(int eventIndex READ eventIndex CONSTANT)
    Q_PROPERTY(QList<QObject*> scoreLines READ scoreLines CONSTANT) // Renamed from moveLines
    Q_PROPERTY(QString unseenTiles READ unseenTiles CONSTANT)
    Q_PROPERTY(int bagCount READ bagCount CONSTANT)
    Q_PROPERTY(int vowelCount READ vowelCount CONSTANT)
    Q_PROPERTY(int consonantCount READ consonantCount CONSTANT)

public:
    explicit HistoryItem(int playerIndex, int type, const QList<QObject*> &scoreLines, const QString &rackString, int score, int cumulativeScore, int eventIndex,
                         const QString &unseenTiles, int bagCount, int vowelCount, int consonantCount,
                         QObject *parent = nullptr);

    int playerIndex() const;
    int type() const;
    QList<QObject*> scoreLines() const; // Renamed from moveLines()
    QString rackString() const;
    int score() const;
    int cumulativeScore() const;
    int eventIndex() const;
    QString unseenTiles() const;
    int bagCount() const;
    int vowelCount() const;
    int consonantCount() const;

private:
    int m_playerIndex;
    int m_type;
    QList<QObject*> m_scoreLines; // Renamed from m_moveLines
    QString m_rackString;
    int m_score;
    int m_cumulativeScore;
    int m_eventIndex;
    QString m_unseenTiles;
    int m_bagCount;
    int m_vowelCount;
    int m_consonantCount;
};

#endif // HISTORYITEM_H
