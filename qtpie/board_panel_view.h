#ifndef BOARD_PANEL_VIEW_H
#define BOARD_PANEL_VIEW_H

#include <QWidget>
#include <QTextEdit>
#include <QLabel>
#include "magpie_wrapper.h"
#include "board_view.h"
#include "rack_view.h"

class BoardPanelView : public QWidget {
    Q_OBJECT
public:
    explicit BoardPanelView(QWidget *parent = nullptr);

    void setGame(Game *game);
    Game* getGame() const { return game; }
    QSize minimumSizeHint() const override;

    // Getter for debug info
    BoardView* getBoardView() const { return boardView; }

    // Set the debug output widget (owned by main window)
    void setDebugOutput(QTextEdit *debugOutput) { this->debugOutput = debugOutput; }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

signals:
    void debugMessage(const QString &msg);
    void boardChanged();  // Emitted when MAGPIE board is updated

private slots:
    void onCgpTextChanged();

private:
    void updateDragTilePreview(const QPoint &pos, QChar tileChar);
    void animatePreviewBackToRack();

    BoardView *boardView;
    RackView *rackView;
    QTextEdit *cgpInput;
    QTextEdit *debugOutput;
    Game *game;

    // Drag preview overlay
    QLabel *dragTilePreview;
    QPoint dragStartPosition;  // Original position of tile in rack (in BoardPanelView coordinates)
};

#endif // BOARD_PANEL_VIEW_H
