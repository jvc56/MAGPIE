#include "ScoreLineItem.h"

ScoreLineItem::ScoreLineItem(const QString &text, const QString &scoreText, int type, QObject *parent)
    : QObject(parent),
      m_text(text),
      m_scoreText(scoreText),
      m_type(type)
{
}

QString ScoreLineItem::text() const
{
    return m_text;
}

QString ScoreLineItem::scoreText() const
{
    return m_scoreText;
}

int ScoreLineItem::type() const
{
    return m_type;
}