#ifndef BOARD_VIEW_H
#define BOARD_VIEW_H

#include <QWidget>
#include <QPixmap>
#include "magpie_wrapper.h"

class BoardView : public QWidget {
    Q_OBJECT
public:
    explicit BoardView(QWidget *parent = nullptr);
    bool hasHeightForWidth() const override;
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void setBoard(Board *board) {
        this->board = board;
        update();
    }

    void setCgpPosition(const QString& cgp);

    // Getters for debug info
    int getSquareSize() const { return m_squareSize; }
    int getLabelFontSize() const { return m_labelFontSize; }
    int getMarginX() const { return m_marginX; }
    int getMarginY() const { return m_marginY; }

    // Convert widget coordinates to board row/col (-1 if outside board)
    void getBoardCoordinates(const QPoint &pos, int &row, int &col) const;

    // Check if a board square is empty
    bool isSquareEmpty(int row, int col) const;

    // Set hover square for drop preview (-1, -1 to clear)
    void setHoverSquare(int row, int col);

    // Place an uncommitted tile on the board
    void placeUncommittedTile(int row, int col, QChar letter);

    // Remove an uncommitted tile from the board
    void removeUncommittedTile(int row, int col);

    // Clear all uncommitted tiles
    void clearUncommittedTiles();

    // Check if a square has an uncommitted tile
    bool hasUncommittedTile(int row, int col) const;

    // Set ghost tile position (shows dimmed tile during drag) - (-1, -1) to clear
    void setGhostTile(int row, int col, QChar letter);
    void clearGhostTile();

signals:
    void tileDragStarted(const QPoint &globalPos, QChar tileChar);
    void tileDragEnded(Qt::DropAction result);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void renderBoard();
    QString parseCgpBoard(const QString& cgp);

    // Structure to represent an uncommitted tile placed on the board
    struct UncommittedTile {
        int row;
        int col;
        QChar letter;  // Uppercase for normal, lowercase for blank
    };

    Board *board;
    QPixmap m_boardPixmap;
    QString m_cgpPosition;  // Current CGP position
    int m_squareSize = 0;
    int m_labelFontSize = 0;  // Current label font size
    int m_marginX = 0;
    int m_marginY = 0;
    int m_hoverRow = -1;  // Row of square being hovered over during drag (-1 = none)
    int m_hoverCol = -1;  // Column of square being hovered over during drag (-1 = none)
    QVector<UncommittedTile> m_uncommittedTiles;  // Tiles placed but not committed

    // Drag state for dragging from board
    int m_draggedRow = -1;
    int m_draggedCol = -1;
    QPoint m_dragStartPos;

    // Ghost tile state (shows dimmed tile at original position during drag)
    int m_ghostRow = -1;
    int m_ghostCol = -1;
    QChar m_ghostLetter;
};

#endif // BOARD_VIEW_H
