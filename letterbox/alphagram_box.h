#ifndef ALPHAGRAM_BOX_H
#define ALPHAGRAM_BOX_H

#include <QWidget>
#include <QPainter>
#include <QVBoxLayout>
#include <QLabel>
#include <vector>
#include <string>

class AlphagramBox : public QWidget {
    Q_OBJECT

public:
    explicit AlphagramBox(QWidget *parent = nullptr);

    void addWord(const QString& word, const QString& frontHooks, const QString& backHooks);
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVBoxLayout* layout;
    std::vector<QLabel*> wordLabels;
};

#endif // ALPHAGRAM_BOX_H
