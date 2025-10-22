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
    ~BoardPanelView();

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
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

signals:
    void debugMessage(const QString &msg);
    void boardChanged();  // Emitted when MAGPIE board is updated
    void updateDragPreview(const QPixmap &tilePixmap, const QPoint &globalPos);  // Update drag preview at global position
    void hideDragPreview();  // Hide drag preview

private slots:
    void onCgpTextChanged();

private:
    void updateDragTilePreview(const QPoint &pos, QChar tileChar);
    void animatePreviewBackToRack();
    QPixmap renderTilePreview(QChar tileChar, int size);
    void renderCursorOverlay(QPainter &painter);
    int calculateDragTileSize(const QPoint &pos);  // Calculate tile size at given position

    BoardView *boardView;
    RackView *rackView;
    QTextEdit *cgpInput;
    QTextEdit *debugOutput;
    Game *game;

    // Drag state
    QPoint dragStartPosition;  // Original position of tile in rack (in BoardPanelView coordinates)
    QChar m_currentDragChar;  // Character being dragged

    // Board-to-board drag tracking
    int m_dragSourceRow = -1;  // Source row for board-to-board drags (-1 if not board drag)
    int m_dragSourceCol = -1;  // Source col for board-to-board drags (-1 if not board drag)

    // Track last hover square to avoid redundant cursor changes
    int m_lastHoverRow = -1;
    int m_lastHoverCol = -1;

    // Cached tile renderer for drag preview
    TileRenderer *m_dragTileRenderer = nullptr;
    int m_lastDragTileSize = 0;
};

#endif // BOARD_PANEL_VIEW_H
