#ifndef ALPHAGRAM_BOX_H
#define ALPHAGRAM_BOX_H

#include <QWidget>
#include <QPainter>
#include <QVBoxLayout>
#include <QLabel>
#include <QRectF>
#include <QTextTableCell>
#include <QTextDocument>
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

// Bounding box for a clickable text element (hook, extension, or main word)
struct TextBoundingBox {
    QRectF rect;
    QString text;        // The actual text (e.g., "INN", "RENO")
    QString fullWord;    // The full word when combined with base (e.g., "INNOVATE")
    int row;             // Which word row this belongs to
    bool isFront;        // true = front hook/extension, false = back
    bool isMainWord;     // true = main word, false = hook/extension
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
    void setShowHoverDebug(bool show) { showHoverDebug = show; update(); }

signals:
    void wordHovered(const QString& word, bool alignLeft, bool isHookOrExtension);
    void hoverLeft();
    void hoverDebug(const QString& debugInfo);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void updateHover(const QPoint& pos);
    void calculateBoundingBoxes();
    void processCellForBoundingBoxes(QTextDocument& doc, QTextTable* table, QTextTableCell& cell,
                                     const WordData& wordData, int row,
                                     bool isFront, const QPoint& labelOffset, bool isMainWord = false);

    QVBoxLayout* layout;
    QLabel* tableLabel;
    std::vector<WordData> words;
    bool hasAnyFrontHooks;
    bool hasAnyBackHooks;

    // Bounding boxes for hover detection
    std::vector<TextBoundingBox> boundingBoxes;
    bool showHoverDebug = false;
    int hoveredBoxIndex = -1;  // Index of currently hovered box for highlighting
};

#endif // ALPHAGRAM_BOX_H
