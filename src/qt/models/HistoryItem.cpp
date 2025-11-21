#include "HistoryItem.h"
#include "ScoreLineItem.h" // Renamed from MoveLineItem

HistoryItem::HistoryItem(int playerIndex, int type, const QList<QObject*> &scoreLines, const QString &rackString, int score, int cumulativeScore, int eventIndex, QObject *parent)
    : QObject(parent),
      m_playerIndex(playerIndex),
      m_type(type),
      m_scoreLines(scoreLines),
      m_rackString(rackString),
      m_score(score),
      m_cumulativeScore(cumulativeScore),
      m_eventIndex(eventIndex)
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
