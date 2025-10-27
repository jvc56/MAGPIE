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
    QString frontExtensions;  // Multi-line, one line per length
    QString backExtensions;   // Multi-line, one line per length
    bool isPlaceholder;       // If true, render with gray color and regular weight
};

class AlphagramBox : public QWidget {
    Q_OBJECT

public:
    explicit AlphagramBox(QWidget *parent = nullptr);

    void addWord(const QString& word, const QString& frontHooks, const QString& backHooks,
                 const QString& frontExtensions = "", const QString& backExtensions = "",
                 bool isPlaceholder = false);
    void finalize(int wordSize = 36, int hookSize = 24, int extensionSize = 14);
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVBoxLayout* layout;
    QLabel* tableLabel;
    std::vector<WordData> words;
};

#endif // ALPHAGRAM_BOX_H
