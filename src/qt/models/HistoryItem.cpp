#include "HistoryItem.h"
#include "ScoreLineItem.h" // Renamed from MoveLineItem

HistoryItem::HistoryItem(int playerIndex, int type, const QList<QObject*> &scoreLines, const QString &rackString, int score, int cumulativeScore, int eventIndex,
                         const QString &unseenTiles, int bagCount, int vowelCount, int consonantCount, int blankCount,
                         const QString &playedTiles, const QString &leaveString, const QString &fullRack,
                         QObject *parent)
    : QObject(parent),
      m_playerIndex(playerIndex),
      m_type(type),
      m_scoreLines(scoreLines),
      m_rackString(rackString),
      m_score(score),
      m_cumulativeScore(cumulativeScore),
      m_eventIndex(eventIndex),
      m_unseenTiles(unseenTiles),
      m_bagCount(bagCount),
      m_vowelCount(vowelCount),
      m_consonantCount(consonantCount),
      m_blankCount(blankCount),
      m_playedTiles(playedTiles),
      m_leaveString(leaveString),
      m_fullRack(fullRack)
{
    // Take ownership of score lines
    for (QObject *obj : m_scoreLines) {
        if (obj) obj->setParent(this);
    }
}

int HistoryItem::playerIndex() const
{
    return m_playerIndex;
}

int HistoryItem::type() const
{
    return m_type;
}

QList<QObject*> HistoryItem::scoreLines() const // Renamed getter
{
    return m_scoreLines;
}

QString HistoryItem::rackString() const
{
    return m_rackString;
}

int HistoryItem::score() const
{
    return m_score;
}

int HistoryItem::cumulativeScore() const
{
    return m_cumulativeScore;
}

int HistoryItem::eventIndex() const
{
    return m_eventIndex;
}

QString HistoryItem::unseenTiles() const
{
    return m_unseenTiles;
}

int HistoryItem::bagCount() const
{
    return m_bagCount;
}

int HistoryItem::vowelCount() const
{
    return m_vowelCount;
}

int HistoryItem::consonantCount() const
{
    return m_consonantCount;
}

int HistoryItem::blankCount() const
{
    return m_blankCount;
}

QString HistoryItem::playedTiles() const
{
    return m_playedTiles;
}

QString HistoryItem::leaveString() const
{
    return m_leaveString;
}

QString HistoryItem::fullRack() const
{
    return m_fullRack;
}
