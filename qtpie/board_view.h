#ifndef BOARD_VIEW_H
#define BOARD_VIEW_H

#include <QWidget>

class BoardView : public QWidget {
    Q_OBJECT
public:
    explicit BoardView(QWidget *parent = nullptr);
    bool hasHeightForWidth() const override;
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
};

#endif // BOARD_VIEW_H
