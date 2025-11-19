#include "BoardSquare.h"

BoardSquare::BoardSquare(const QString &letter, int score, bool isBlank, int letterMultiplier, int wordMultiplier, QObject *parent)
    : QObject{parent},
      m_letter(letter),
      m_score(score),
      m_isBlank(isBlank),
      m_letterMultiplier(letterMultiplier),
      m_wordMultiplier(wordMultiplier)
{
}

QString BoardSquare::letter() const
{
    return m_letter;
}

int BoardSquare::score() const
{
    return m_score;
}

bool BoardSquare::isBlank() const
{
    return m_isBlank;
}

int BoardSquare::letterMultiplier() const
{
    return m_letterMultiplier;
}

int BoardSquare::wordMultiplier() const
{
    return m_wordMultiplier;
}
