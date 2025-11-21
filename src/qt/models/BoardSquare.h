#ifndef BOARDSQUARE_H
#define BOARDSQUARE_H

#include <QObject>
#include <QString>

class BoardSquare : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString letter READ letter CONSTANT)
    Q_PROPERTY(int score READ score CONSTANT)
    Q_PROPERTY(bool isBlank READ isBlank CONSTANT)
    Q_PROPERTY(int letterMultiplier READ letterMultiplier CONSTANT)
    Q_PROPERTY(int wordMultiplier READ wordMultiplier CONSTANT)
    Q_PROPERTY(bool isLastMove READ isLastMove CONSTANT)

public:
    explicit BoardSquare(const QString &letter, int score, bool isBlank, int letterMultiplier, int wordMultiplier, bool isLastMove, QObject *parent = nullptr);

    QString letter() const;
    int score() const;
    bool isBlank() const;
    int letterMultiplier() const;
    int wordMultiplier() const;
    bool isLastMove() const;

private:
    QString m_letter;
    int m_score;
    bool m_isBlank;
    int m_letterMultiplier;
    int m_wordMultiplier;
    bool m_isLastMove;
};

#endif // BOARDSQUARE_H
