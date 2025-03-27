#ifndef BOARD_PANEL_VIEW_H
#define BOARD_PANEL_VIEW_H

#include <QWidget>

class BoardPanelView : public QWidget {
    Q_OBJECT
public:
    explicit BoardPanelView(QWidget *parent = nullptr);
};

#endif // BOARD_PANEL_VIEW_H
