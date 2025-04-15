#ifndef BOARD_PANEL_VIEW_H
#define BOARD_PANEL_VIEW_H

#include <QWidget>

extern "C" {
#include "../src/ent/game.h"
}

#include "board_view.h"

class BoardPanelView : public QWidget {
    Q_OBJECT
public:
    explicit BoardPanelView(QWidget *parent = nullptr);

    void setGame(Game *game);
  private:
    BoardView *boardView;
    Game *game;
};

#endif // BOARD_PANEL_VIEW_H
