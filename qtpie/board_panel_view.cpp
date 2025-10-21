#include "board_panel_view.h"
#include "board_view.h"
#include "tile_renderer.h"
#include "magpie_wrapper.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QFont>
#include <QFontMetrics>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QGuiApplication>
#include <QPropertyAnimation>

// Helper to create placeholder widgets with light theme.
static QWidget* createPlaceholder(const QString &text, const QColor &bgColor = QColor(255, 255, 255)) {
    QWidget *widget = new QWidget;
    // Light theme styling
    widget->setStyleSheet(
        QString("background-color: %1; border: 1px solid #C0C0D0; border-radius: 8px;")
        .arg(bgColor.name())
    );

    QLabel *label = new QLabel(text, widget);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("color: #333333; font-weight: bold; font-size: 14px; border: none;");

    QVBoxLayout *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->addWidget(label);
    return widget;
}

BoardPanelView::BoardPanelView(QWidget *parent)
    : QWidget(parent)
    , game(nullptr)
    , dragTilePreview(nullptr)
{
    // Accept drops to act as a catch-all for drags
    setAcceptDrops(true);

    // Create drag tile preview overlay (initially hidden)
    dragTilePreview = new QLabel(this);
    dragTilePreview->setVisible(false);
    dragTilePreview->setAttribute(Qt::WA_TransparentForMouseEvents);  // Don't interfere with drag events
    dragTilePreview->raise();  // Always on top

    // Enforce minimum size to prevent container from shrinking smaller than board
    // Board minimum: 20px * 15 + margins = 322px
    // Add space for CGP input, rack, controls
    constexpr int MIN_BOARD_SIZE = 322;
    constexpr int MIN_WIDTH = MIN_BOARD_SIZE;
    constexpr int MIN_HEIGHT = MIN_BOARD_SIZE + 200;  // board + other elements
    setMinimumSize(MIN_WIDTH, MIN_HEIGHT);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(5);

    // BoardView is the square canvas displaying board contents.
    boardView = new BoardView(this);
    boardView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // CGP input section (label + text field)
    QWidget *cgpWidget = new QWidget(this);
    QHBoxLayout *cgpLayout = new QHBoxLayout(cgpWidget);
    cgpLayout->setContentsMargins(5, 5, 5, 5);
    cgpLayout->setSpacing(10);

    QLabel *cgpLabel = new QLabel("CGP:", cgpWidget);
    QFont labelFont = cgpLabel->font();
    labelFont.setBold(true);
    cgpLabel->setFont(labelFont);
    cgpLabel->setStyleSheet("color: #333333;");

    cgpInput = new QTextEdit(cgpWidget);
    cgpInput->setAcceptRichText(false);  // Only accept plain text
    cgpInput->setAcceptDrops(false);  // Don't accept tile drops
    cgpInput->setPlainText("4AUREOLED3/11O3/11Z3/10FY3/10A4/10C4/10I4/7THANX3/10GUV2/15/15/15/15/15/15 AHMPRTU/ 177/44 0");
    cgpInput->setPlaceholderText("Enter CGP position (e.g., 15/15/15/... / 0/0 0)");

    QFont monoFont("Courier", 11);
    cgpInput->setFont(monoFont);

    // Set height for 3 lines of text
    QFontMetrics fm(monoFont);
    int lineHeight = fm.lineSpacing();
    int textEditHeight = lineHeight * 3 + 12;  // 3 lines + padding
    cgpInput->setFixedHeight(textEditHeight);

    // Light theme styling for input field
    cgpInput->setStyleSheet(
        "QTextEdit {"
        "  background-color: white;"
        "  color: #333333;"
        "  border: 1px solid #C0C0D0;"
        "  border-radius: 4px;"
        "  padding: 4px 8px;"
        "}"
        "QTextEdit:focus {"
        "  border: 1px solid #6496C8;"
        "}"
    );

    cgpLayout->addWidget(cgpLabel);
    cgpLayout->addWidget(cgpInput, 1);

    // Connect to update board on text change
    connect(cgpInput, &QTextEdit::textChanged, this, &BoardPanelView::onCgpTextChanged);

    // Rack view for displaying tiles
    rackView = new RackView(this);
    rackView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Connect rack debug messages to forward through this widget
    connect(rackView, &RackView::debugMessage, this, &BoardPanelView::debugMessage);

    // Connect rack drag position updates to show preview overlay
    connect(rackView, &RackView::dragPositionChanged, this, [this](const QPoint &pos, QChar tileChar) {
        // Store the start position when drag first begins (when preview becomes visible)
        if (!dragTilePreview->isVisible()) {
            dragStartPosition = pos;
        }
        updateDragTilePreview(pos, tileChar);
    });

    // Connect rack drag end to animate or hide preview
    connect(rackView, &RackView::dragEnded, this, [this](Qt::DropAction result) {
        if (dragTilePreview && dragTilePreview->isVisible()) {
            if (result == Qt::IgnoreAction) {
                // Failed drop - animate back to original position
                animatePreviewBackToRack();
            } else {
                // Successful drop - hide immediately
                QRect previewRect = dragTilePreview->geometry();
                dragTilePreview->setVisible(false);
                update(previewRect);
            }
        }
    });

    // Placeholder for controls beneath the rack
    QWidget *controlsPlaceholder = createPlaceholder("Controls");
    controlsPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    mainLayout->addWidget(boardView, 0);
    mainLayout->addWidget(cgpWidget, 0);
    mainLayout->addWidget(rackView, 1);
    mainLayout->addWidget(controlsPlaceholder, 1);
}

void BoardPanelView::setGame(Game *game) {
    this->game = game;
    Board *board = magpie_get_board_from_game(game);
    boardView->setBoard(board);

    // Trigger initial CGP load
    onCgpTextChanged();
}

void BoardPanelView::onCgpTextChanged() {
    // Update the board view with the new CGP position
    QString text = cgpInput->toPlainText();
    boardView->setCgpPosition(text);

    emit debugMessage("=== CGP text changed ===");
    emit debugMessage("Text: " + text);

    // If we have a MAGPIE game, load the CGP into it
    if (game) {
        QByteArray cgpBytes = text.toUtf8();
        magpie_load_cgp(game, cgpBytes.constData());
        emit boardChanged();  // Signal that the board has been updated
    }

    // Parse and update rack from CGP
    // CGP format: "board rack/ / scores consecutive_zeros"
    // Example: "15/15/.../15 AEINRST/ / 0/0 0"
    // The board section contains 14 slashes (between 15 rows)
    // After board, there's a space, then the rack, then a slash

    // Split by space to separate board from rack/scores
    QStringList parts = text.split(' ', Qt::SkipEmptyParts);

    emit debugMessage(QString("Split into %1 parts").arg(parts.size()));

    if (parts.size() >= 2) {
        // parts[0] is the board (15/15/15...)
        // parts[1] is the rack with trailing slash (AEINRST/)
        QString rackPart = parts[1];
        emit debugMessage("Rack part: '" + rackPart + "'");

        // Remove trailing slash if present
        if (rackPart.endsWith('/')) {
            rackPart.chop(1);
        }

        emit debugMessage("Parsed rack: '" + rackPart + "'");
        rackView->setRack(rackPart);
    } else {
        emit debugMessage("Not enough parts - setting empty rack");
        rackView->setRack("");
    }
    emit debugMessage("");  // blank line
}

QSize BoardPanelView::minimumSizeHint() const {
    // Must match the minimum size set in constructor
    constexpr int MIN_BOARD_SIZE = 322;  // 20px * 15 + margins
    constexpr int MIN_WIDTH = MIN_BOARD_SIZE;
    constexpr int MIN_HEIGHT = MIN_BOARD_SIZE + 200;  // board + other elements
    return QSize(MIN_WIDTH, MIN_HEIGHT);
}

void BoardPanelView::dragEnterEvent(QDragEnterEvent *event) {
    emit debugMessage(QString("BoardPanelView::dragEnter - hasText=%1 source=%2")
                     .arg(event->mimeData()->hasText())
                     .arg(event->source() ? event->source()->objectName() : "null"));

    // Check if this is a tile drag from RackView
    if (event->mimeData()->hasText()) {
        emit debugMessage("BoardPanelView::dragEnter - accepting with IgnoreAction (forbidden cursor)");

        // Accept with IgnoreAction to receive dragMoveEvents but show forbidden cursor
        event->setDropAction(Qt::IgnoreAction);
        event->accept();

        // Set system forbidden cursor immediately on enter
        QGuiApplication::setOverrideCursor(Qt::ForbiddenCursor);
        emit debugMessage("Set override cursor to ForbiddenCursor on dragEnter");

        // Show drag preview - extract tile character from mime data
        QPoint globalPos = event->position().toPoint();
        QString mimeText = event->mimeData()->text();
        QStringList parts = mimeText.split(':');
        if (parts.size() >= 2) {
            updateDragTilePreview(globalPos, parts[1][0]);
        }
    } else {
        // Not our drag - let it propagate
        QWidget::dragEnterEvent(event);
    }
}

void BoardPanelView::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasText()) {
        QPoint globalPos = event->position().toPoint();

        // Check if drag is over the board and if so, which square
        QPoint boardLocalPos = boardView->mapFromParent(globalPos);
        int row, col;
        boardView->getBoardCoordinates(boardLocalPos, row, col);

        bool overValidSquare = false;
        if (row >= 0 && col >= 0 && boardView->isSquareEmpty(row, col)) {
            // Dragging over an empty square on the board - allow drop
            overValidSquare = true;
            event->setDropAction(Qt::MoveAction);
            event->accept();

            // Show normal cursor
            while (QGuiApplication::overrideCursor()) {
                QGuiApplication::restoreOverrideCursor();
            }

            // Update board hover square to show green outline
            boardView->setHoverSquare(row, col);
        } else {
            // Not over a valid square - forbidden cursor
            event->setDropAction(Qt::IgnoreAction);
            event->accept();

            // Set forbidden cursor
            if (!QGuiApplication::overrideCursor() ||
                QGuiApplication::overrideCursor()->shape() != Qt::ForbiddenCursor) {
                QGuiApplication::setOverrideCursor(Qt::ForbiddenCursor);
            }

            // Clear board hover square
            boardView->setHoverSquare(-1, -1);
        }

        // Update drag tile preview with size interpolation
        // Extract the tile character from mime data
        QString mimeText = event->mimeData()->text();
        QStringList parts = mimeText.split(':');
        if (parts.size() >= 2) {
            updateDragTilePreview(globalPos, parts[1][0]);
        }
    } else {
        // Not our drag - let it propagate
        QWidget::dragMoveEvent(event);
    }
}

void BoardPanelView::dragLeaveEvent(QDragLeaveEvent *event) {
    // Clear hover square when drag leaves
    boardView->setHoverSquare(-1, -1);

    // Hide drag preview when leaving
    if (dragTilePreview && dragTilePreview->isVisible()) {
        QRect previewRect = dragTilePreview->geometry();
        dragTilePreview->setVisible(false);
        update(previewRect);
    }
    QWidget::dragLeaveEvent(event);
}

void BoardPanelView::dropEvent(QDropEvent *event) {
    // Check if dropping on a valid board square
    QPoint globalPos = event->position().toPoint();
    QPoint boardLocalPos = boardView->mapFromParent(globalPos);
    int row, col;
    boardView->getBoardCoordinates(boardLocalPos, row, col);

    if (row >= 0 && col >= 0 && boardView->isSquareEmpty(row, col)) {
        // Valid drop on empty square
        QString mimeText = event->mimeData()->text();
        QStringList parts = mimeText.split(':');
        if (parts.size() >= 2) {
            int tileIndex = parts[0].toInt();
            QChar tileChar = parts[1][0];

            emit debugMessage(QString("Dropped tile '%1' (index %2) at board position (%3, %4)")
                            .arg(tileChar).arg(tileIndex).arg(row).arg(col));

            // Place the tile on the board as uncommitted
            boardView->placeUncommittedTile(row, col, tileChar);

            // Remove the tile from the rack
            rackView->removeTileAtIndex(tileIndex);

            // Accept the drop
            event->setDropAction(Qt::MoveAction);
            event->accept();

        } else {
            event->ignore();
        }
    } else {
        // Invalid drop location
        event->ignore();
    }

    // Clear hover square
    boardView->setHoverSquare(-1, -1);

    // Hide drag preview
    if (dragTilePreview && dragTilePreview->isVisible()) {
        QRect previewRect = dragTilePreview->geometry();
        dragTilePreview->setVisible(false);
        update(previewRect);
    }
}

void BoardPanelView::updateDragTilePreview(const QPoint &pos, QChar tileChar) {
    if (!dragTilePreview || !rackView || !boardView) {
        return;
    }

    // Get rack and board geometries in this widget's coordinates
    QRect rackRect = rackView->geometry();
    QRect boardRect = boardView->geometry();

    // Get sizes
    int rackTileSize = rackView->height();  // Rack tiles are square, size = rack height
    int boardSquareSize = boardView->getSquareSize();

    if (rackTileSize <= 0 || boardSquareSize <= 0) {
        return;
    }

    // Calculate what percentage of the tile is over the board vs rack
    // Define the blend zone as the tile size itself
    int blendZoneSize = qMax(rackTileSize, boardSquareSize);

    // Calculate bounds for discrete zones
    int boardTop = boardRect.top();
    int rackTop = rackRect.top();

    double factor = 0.0;  // 0 = rack size, 1 = board size
    int interpolatedSize;

    // Check if fully in board area
    if (pos.y() + blendZoneSize/2 < boardRect.bottom() &&
        pos.x() >= boardRect.left() && pos.x() <= boardRect.right() &&
        pos.y() >= boardRect.top()) {
        // Fully in board
        factor = 1.0;
        interpolatedSize = boardSquareSize;
    }
    // Check if fully in rack area
    else if (pos.y() - blendZoneSize/2 > rackTop &&
             pos.x() >= rackRect.left() && pos.x() <= rackRect.right() &&
             pos.y() <= rackRect.bottom()) {
        // Fully in rack
        factor = 0.0;
        interpolatedSize = rackTileSize;
    }
    // Blending zone - interpolate based on how much overlaps each area
    else if (pos.y() >= boardTop - blendZoneSize && pos.y() <= rackTop + blendZoneSize) {
        // Calculate center of blend zone
        int blendCenter = (boardRect.bottom() + rackTop) / 2;
        int blendRange = (rackTop - boardRect.bottom()) + blendZoneSize;

        if (blendRange > 0) {
            // Distance from board side (negative) to rack side (positive)
            int distFromCenter = pos.y() - blendCenter;
            factor = 0.5 - (double(distFromCenter) / double(blendRange));
            factor = qBound(0.0, factor, 1.0);
        } else {
            factor = 0.5;
        }

        interpolatedSize = int(rackTileSize + factor * (boardSquareSize - rackTileSize));
    }
    // Outside both areas - use closest size
    else if (pos.y() < boardTop) {
        interpolatedSize = boardSquareSize;
    } else {
        interpolatedSize = rackTileSize;
    }

    // Render the actual tile at the interpolated size
    TileRenderer renderer(interpolatedSize, TileRenderer::TileStyle::Rack);

    QPixmap tilePixmap;
    if (tileChar == '?') {
        tilePixmap = renderer.getBlankTile('A');
    } else if (tileChar.isLower() && tileChar >= 'a' && tileChar <= 'z') {
        tilePixmap = renderer.getBlankTile(tileChar.toLatin1());
    } else if (tileChar.isUpper() && tileChar >= 'A' && tileChar <= 'Z') {
        tilePixmap = renderer.getLetterTile(tileChar.toLatin1());
    } else {
        // Unknown character - use a placeholder
        tilePixmap = renderer.getLetterTile('?');
    }

    // Position the preview centered on cursor
    // IMPORTANT: Resize the QLabel to match the pixmap size to prevent cropping
    dragTilePreview->setPixmap(tilePixmap);
    dragTilePreview->resize(interpolatedSize, interpolatedSize);
    dragTilePreview->move(pos.x() - interpolatedSize / 2, pos.y() - interpolatedSize / 2);
    dragTilePreview->setVisible(true);
    dragTilePreview->raise();
}

void BoardPanelView::animatePreviewBackToRack() {
    if (!dragTilePreview || !dragTilePreview->isVisible()) {
        return;
    }

    // Just immediately snap back to rack position without animation
    // This is simpler and faster than trying to animate both size and position
    // while also re-rendering the tile at different sizes
    QRect previewRect = dragTilePreview->geometry();
    dragTilePreview->setVisible(false);
    update(previewRect);
}