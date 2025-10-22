#ifndef RACK_VIEW_H
#define RACK_VIEW_H

#include <QWidget>
#include <QString>
#include <QPushButton>
#include <QHBoxLayout>
#include <QPoint>

class TileRenderer;

class RackView : public QWidget {
    Q_OBJECT
public:
    explicit RackView(QWidget *parent = nullptr);
    ~RackView();

    void setRack(const QString& rack);
    QSize sizeHint() const override;
    QString getRack() const { return m_rack; }

    // Remove tile at specific index (for when it's placed on board)
    void removeTileAtIndex(int index);

    // Add tile back to rack (for backspace in keyboard entry)
    void addTile(QChar tile);

    // Check if rack has a natural (non-blank) letter
    bool hasNaturalLetter(QChar letter) const;

    // Check if rack has a blank
    bool hasBlank() const;

    // Remove first occurrence of natural letter from rack, return true if found
    bool removeNaturalLetter(QChar letter);

    // Remove first blank from rack, return true if found
    bool removeBlank();

    // Get center position of tile at index (for testing)
    QPoint getTileCenter(int index) const;

signals:
    void debugMessage(const QString &msg);
    void rackChanged(const QString& newRack);
    void dragPositionChanged(const QPoint &globalPos, QChar tileChar);
    void dragEnded(Qt::DropAction result);
    void boardTileReturned(int row, int col);  // Emitted when a board tile is dropped on rack

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void alphabetizeRack();
    void shuffleRack();

private:
    int getTileIndexAtPosition(const QPoint &pos) const;
    int getStartX() const;

    QString m_rack;
    int m_tileSize = 0;
    QPushButton *m_alphabetizeButton;
    QPushButton *m_shuffleButton;

    // Drag state
    int m_draggedTileIndex = -1;
    QChar m_draggedTileChar;
    QPoint m_dragStartPos;
    QPoint m_dragClickOffset;  // Offset from tile center to click position
    int m_dropIndicatorPosition = -1;  // -1 means no indicator, otherwise index where tile would be inserted

    // Cached tile renderer (created once, reused to avoid memory leak)
    TileRenderer *m_tileRenderer = nullptr;
};

#endif // RACK_VIEW_H
