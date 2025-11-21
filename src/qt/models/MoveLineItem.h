#ifndef MOVELINEITEM_H
#define MOVELINEITEM_H

#include <QObject>
#include <QString>

class MoveLineItem : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString text READ text CONSTANT)
    Q_PROPERTY(QString scoreText READ scoreText CONSTANT)

public:
    explicit MoveLineItem(const QString &text, const QString &scoreText, QObject *parent = nullptr);

    QString text() const;
    QString scoreText() const;

private:
    QString m_text;
    QString m_scoreText;
};

#endif // MOVELINEITEM_H
