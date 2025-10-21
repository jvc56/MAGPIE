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

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void renderBoard();
    QString parseCgpBoard(const QString& cgp);

    Board *board;
    QPixmap m_boardPixmap;
    QString m_cgpPosition;  // Current CGP position
    int m_squareSize = 0;
    int m_labelFontSize = 0;  // Current label font size
    int m_marginX = 0;
    int m_marginY = 0;
};

#endif // BOARD_VIEW_H
