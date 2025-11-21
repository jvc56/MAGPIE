#include "MoveLineItem.h"

MoveLineItem::MoveLineItem(const QString &text, const QString &scoreText, QObject *parent)
    : QObject(parent),
      m_text(text),
      m_scoreText(scoreText)
{
}

QString MoveLineItem::text() const
{
    return m_text;
}

QString MoveLineItem::scoreText() const
{
    return m_scoreText;
}