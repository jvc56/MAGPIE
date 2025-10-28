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
    bool isMissed;            // If true, render in lighter gray with red X
    int computeTimeMicros;    // Time to compute hooks/extensions in microseconds
};

class AlphagramBox : public QWidget {
    Q_OBJECT

public:
    explicit AlphagramBox(QWidget *parent = nullptr);

    void addWord(const QString& word, const QString& frontHooks, const QString& backHooks,
                 const QString& frontExtensions = "", const QString& backExtensions = "",
                 bool isPlaceholder = false, bool isMissed = false, int computeTimeMicros = 0);
    void finalize(int wordSize = 36, int hookSize = 24, int extensionSize = 14, bool showComputeTime = false);
    void clear();

signals:
    void wordHovered(const QString& word, bool alignLeft);
    void hoverLeft();
    void hoverDebug(const QString& debugInfo);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void updateHover(const QPoint& pos);

    QVBoxLayout* layout;
    QLabel* tableLabel;
    std::vector<WordData> words;
    bool hasAnyFrontHooks;
    bool hasAnyBackHooks;
};

#endif // ALPHAGRAM_BOX_H
