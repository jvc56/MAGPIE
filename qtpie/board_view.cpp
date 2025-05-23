#include "board_view.h"
#include <QPainter>

BoardView::BoardView(QWidget *parent)
    : QWidget(parent)
{
}

bool BoardView::hasHeightForWidth() const {
    return true;
}

int BoardView::heightForWidth(int w) const {
    return w;
}

QSize BoardView::sizeHint() const {
    return QSize(300, 300);
}

void BoardView::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::gray);
    //painter.drawText(rect(), Qt::AlignCenter, "Board");
}
