#ifndef ALPHAGRAM_BOX_H
#define ALPHAGRAM_BOX_H

#include <QWidget>
#include <QPainter>
#include <QVBoxLayout>
#include <QLabel>
#include <vector>
#include <string>

struct WordData {
    QString word;
    QString frontHooks;
    QString backHooks;
};

class AlphagramBox : public QWidget {
    Q_OBJECT

public:
    explicit AlphagramBox(QWidget *parent = nullptr);

    void addWord(const QString& word, const QString& frontHooks, const QString& backHooks);
    void finalize();
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVBoxLayout* layout;
    QLabel* tableLabel;
    std::vector<WordData> words;
};

#endif // ALPHAGRAM_BOX_H
