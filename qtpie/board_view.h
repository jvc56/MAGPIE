#ifndef BOARD_VIEW_H
#define BOARD_VIEW_H

#include <QWidget>

extern "C" {
#include "../src/ent/board.h"
}

class BoardView : public QWidget {
    Q_OBJECT
public:
    explicit BoardView(QWidget *parent = nullptr);
    bool hasHeightForWidth() const override;
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;
    void setBoard(Board *board) {
        this->board = board;
        update();
    }
protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Board *board;
};

#endif // BOARD_VIEW_H
