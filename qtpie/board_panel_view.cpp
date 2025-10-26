#include "board_panel_view.h"
#include "board_view.h"
#include "tile_renderer.h"
#include "magpie_wrapper.h"
#include "blank_designation_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
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
#include <QApplication>
#include <QCursor>
#include <QTime>
#include <QTimer>
#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <algorithm>

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

// Helper to convert UCGI move notation to uppercase for display
// UCGI may use lowercase letters (e.g., "8h.WORD"), but for display we want uppercase (e.g., "8H.WORD")
static QString toUppercaseNotation(const QString &notation) {
    QString result = notation;
    // Find the position coordinate part (before the dot)
    int dotPos = notation.indexOf('.');
    if (dotPos > 0) {
        // Convert only the position part to uppercase
        for (int i = 0; i < dotPos; ++i) {
            result[i] = result[i].toUpper();
        }
    }
    return result;
}

BoardPanelView::BoardPanelView(QWidget *parent)
    : QWidget(parent)
    , game(nullptr)
    , m_dragTileRenderer(nullptr)
    , m_lastDragTileSize(0)
{
    // Accept drops to act as a catch-all for drags
    setAcceptDrops(true);

    // Enable keyboard focus for keyboard entry mode
    setFocusPolicy(Qt::StrongFocus);

    // Don't clip child widgets so BoardView cursor at position 15 can extend beyond bounds
    setAttribute(Qt::WA_NoSystemBackground, false);
    setAutoFillBackground(false);

    // Enforce minimum size to prevent container from shrinking smaller than board
    // Board minimum: 20px * 15 + margins = 322px
    // Add space for CGP input, rack, controls
    constexpr int MIN_BOARD_SIZE = 322;
    constexpr int MIN_WIDTH = MIN_BOARD_SIZE;
    constexpr int MIN_HEIGHT = MIN_BOARD_SIZE + 200;  // board + other elements
    setMinimumSize(MIN_WIDTH, MIN_HEIGHT);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

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
    cgpInput->setReadOnly(true);  // Readonly in gameplay mode
    cgpInput->setPlainText("");  // Start with empty CGP
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
    connect(rackView, &RackView::dragPositionChanged, this, [this](const QPoint &pos, QChar tileChar, const QPoint &clickOffset) {
        // Store the start position and char when drag first begins
        if (m_currentDragChar.isNull()) {
            dragStartPosition = pos;
            m_currentDragChar = tileChar;
            m_dragClickOffset = clickOffset;
        }
        updateDragTilePreview(pos, tileChar);
    });

    // Connect rack drag end to hide preview
    connect(rackView, &RackView::dragEnded, this, [this](Qt::DropAction result) {
        m_currentDragChar = QChar();  // Reset drag char
        emit hideDragPreview();
    });

    // Connect board drag start to show preview
    connect(boardView, &BoardView::tileDragStarted, this, [this](const QPoint &pos, QChar tileChar) {
        dragStartPosition = pos;
        m_currentDragChar = tileChar;
        updateDragTilePreview(pos, tileChar);
    });

    // Connect board drag end to clean up state
    connect(boardView, &BoardView::tileDragEnded, this, [this](Qt::DropAction result) {
        // Clear drag source tracking
        m_dragSourceRow = -1;
        m_dragSourceCol = -1;
        m_currentDragChar = QChar();

        // Hide preview
        emit hideDragPreview();
    });

    // Connect rack accepting board tile to remove it from board
    connect(rackView, &RackView::boardTileReturned, this, [this](int row, int col) {
        boardView->removeUncommittedTile(row, col);
    });

    // Forward debug log from BoardView
    connect(boardView, &BoardView::debugLog, this, &BoardPanelView::debugMessage);

    // Connect board square click for keyboard entry
    connect(boardView, &BoardView::squareClicked, this, [this](int row, int col) {
        // Check if clicking on the current cursor position - if so, toggle direction
        if (boardView->isKeyboardEntryActive()) {
            int currentRow, currentCol;
            BoardView::Direction currentDir;
            boardView->getKeyboardEntry(currentRow, currentCol, currentDir);

            if (row == currentRow && col == currentCol) {
                // Clicking on current square - toggle direction
                BoardView::Direction newDir = (currentDir == BoardView::Horizontal) ? BoardView::Vertical : BoardView::Horizontal;
                boardView->setKeyboardEntry(row, col, newDir);
                emit debugMessage(QString("Direction toggled to %1").arg(newDir == BoardView::Horizontal ? "Horizontal" : "Vertical"));
                setFocus();  // Keep keyboard focus
                return;
            }
        }

        // Clicking on a different square - enter keyboard mode
        emit debugMessage(QString("Square clicked: (%1, %2) - entering keyboard mode").arg(row).arg(col));
        boardView->setKeyboardEntry(row, col, BoardView::Horizontal);
        setFocus();  // Take keyboard focus to receive key events
    });

    // Connect uncommitted tiles changed signal to validation
    connect(boardView, &BoardView::uncommittedTilesChanged, this, &BoardPanelView::validateAndLogUncommittedTiles);

    // Controls panel with gameplay buttons
    QWidget *controlsPanel = new QWidget(this);
    controlsPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QHBoxLayout *controlsLayout = new QHBoxLayout(controlsPanel);
    controlsLayout->setContentsMargins(5, 10, 5, 10);
    controlsLayout->setSpacing(10);

    // Create buttons with matching light theme style
    QString buttonStyle =
        "QPushButton {"
        "  background-color: #E8E8F0;"
        "  color: #333333;"
        "  border: 1px solid #C0C0D0;"
        "  border-radius: 4px;"
        "  padding: 8px 16px;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #D8D8E8;"
        "  border: 1px solid #A0A0C0;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #C8C8D8;"
        "}";

    passButton = new QPushButton("Pass", controlsPanel);
    passButton->setCursor(Qt::PointingHandCursor);
    connect(passButton, &QPushButton::clicked, this, &BoardPanelView::onPassClicked);

    exchangeButton = new QPushButton("Exchange", controlsPanel);
    exchangeButton->setCursor(Qt::PointingHandCursor);
    connect(exchangeButton, &QPushButton::clicked, this, &BoardPanelView::onExchangeClicked);

    playButton = new QPushButton("Play", controlsPanel);
    playButton->setCursor(Qt::PointingHandCursor);
    connect(playButton, &QPushButton::clicked, this, &BoardPanelView::onPlayClicked);

    // Initialize button states (Pass always enabled, Exchange enabled, Play disabled until valid move)
    setButtonEnabled(passButton, true);
    setButtonEnabled(exchangeButton, true);
    setButtonEnabled(playButton, false);

    // Add buttons to layout with stretch to keep them together
    controlsLayout->addStretch(1);
    controlsLayout->addWidget(passButton);
    controlsLayout->addWidget(exchangeButton);
    controlsLayout->addWidget(playButton);
    controlsLayout->addStretch(1);

    mainLayout->addWidget(boardView, 0);
    mainLayout->addWidget(cgpWidget, 0);
    mainLayout->addWidget(rackView, 1);
    mainLayout->addWidget(controlsPanel, 0);
}

BoardPanelView::~BoardPanelView() {
    delete m_dragTileRenderer;
}

void BoardPanelView::setGame(Game *game) {
    this->game = game;
    Board *board = magpie_get_board_from_game(game);
    boardView->setBoard(board);

    // Start player 0's timer (game begins with player 0's turn)
    emit playerTurnChanged(0);

    // Don't trigger CGP load - game is already set up
    // onCgpTextChanged();
}

void BoardPanelView::updateCgpDisplay() {
    if (!game || !cgpInput) {
        return;
    }

    // Get CGP string from game
    char *cgpString = magpie_get_cgp(game);
    if (cgpString) {
        QString cgpQString = QString::fromUtf8(cgpString);
        free(cgpString);

        // Hide the opponent's rack for privacy
        // CGP format: "board rack1/rack2 scores turns"
        // We want to show: "board rack1/ scores turns"
        QStringList parts = cgpQString.split(' ', Qt::KeepEmptyParts);
        if (parts.size() >= 3) {
            // parts[0] = board
            // parts[1] = rack1/rack2
            // parts[2] = scores (e.g., "0/0")
            // parts[3] = consecutive zeros

            QString racksPart = parts[1];
            QStringList racks = racksPart.split('/', Qt::KeepEmptyParts);
            if (racks.size() >= 2) {
                // Keep only the first rack (current player's rack) and hide the second
                QString hiddenRacksCgp = racks[0] + "/";
                parts[1] = hiddenRacksCgp;
                cgpQString = parts.join(' ');
            }
        }

        // Temporarily disconnect the textChanged signal to avoid triggering onCgpTextChanged
        disconnect(cgpInput, &QTextEdit::textChanged, this, &BoardPanelView::onCgpTextChanged);

        // Update the CGP text
        cgpInput->setPlainText(cgpQString);

        // Update the board view to render the new position
        if (boardView) {
            boardView->setCgpPosition(cgpQString);
        }

        // Reconnect the signal
        connect(cgpInput, &QTextEdit::textChanged, this, &BoardPanelView::onCgpTextChanged);
    }
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

    // Check if this is a tile drag from RackView or BoardView
    if (event->mimeData()->hasText()) {
        emit debugMessage("BoardPanelView::dragEnter - accepting drag");

        // Ghost the keyboard cursor during drag
        boardView->setDragActive(true);

        // Accept the drag
        event->accept();

        // Show drag preview - extract tile character from mime data
        QPoint globalPos = event->position().toPoint();
        QString mimeText = event->mimeData()->text();
        QStringList parts = mimeText.split(':');

        QChar tileChar;
        if (mimeText.startsWith("board:")) {
            // Board drag format: "board:row:col:char"
            if (parts.size() >= 4) {
                tileChar = parts[3][0];
                // Set ghost tile at original board position
                int row = parts[1].toInt();
                int col = parts[2].toInt();
                boardView->setGhostTile(row, col, tileChar);

                // Track source position for board-to-board drags
                // Only set it if not already set (don't update on re-entry)
                if (m_dragSourceRow == -1 && m_dragSourceCol == -1) {
                    m_dragSourceRow = row;
                    m_dragSourceCol = col;
                    emit debugMessage(QString("  -> Set drag source to (%1,%2)").arg(row).arg(col));
                } else {
                    emit debugMessage(QString("  -> Drag source already set to (%1,%2), keeping it")
                                     .arg(m_dragSourceRow).arg(m_dragSourceCol));
                }
            }
        } else {
            // Rack drag format: "index:char"
            if (parts.size() >= 2) {
                tileChar = parts[1][0];
            }
            // Clear source position for rack drags (only if not already cleared)
            if (m_dragSourceRow != -1 || m_dragSourceCol != -1) {
                m_dragSourceRow = -1;
                m_dragSourceCol = -1;
                emit debugMessage("  -> Cleared drag source (rack drag)");
            }
        }

        if (!tileChar.isNull()) {
            updateDragTilePreview(globalPos, tileChar);
        }
    } else {
        // Not our drag - let it propagate
        QWidget::dragEnterEvent(event);
    }
}

void BoardPanelView::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasText()) {
        // Use actual cursor position, not event position (which can be offset by hotspot)
        QPoint cursorGlobalPos = QCursor::pos();
        QPoint cursorInPanel = mapFromGlobal(cursorGlobalPos);

        QPoint eventPos = event->position().toPoint();
        QPoint globalPos = mapToGlobal(eventPos);

        emit debugMessage(QString("dragMove COMPARE: cursor global=(%1,%2) inPanel=(%3,%4)")
                         .arg(cursorGlobalPos.x()).arg(cursorGlobalPos.y())
                         .arg(cursorInPanel.x()).arg(cursorInPanel.y()));
        emit debugMessage(QString("                 event  eventPos=(%1,%2) global=(%3,%4)")
                         .arg(eventPos.x()).arg(eventPos.y())
                         .arg(globalPos.x()).arg(globalPos.y()));

        QWidget *mainWidget = window();
        // USE CURSOR POSITION instead of event position to avoid hotspot offset issues
        QPoint panelPos = cursorInPanel;

        // Calculate tile size for this position
        int tileSize = calculateDragTileSize(panelPos);

        // Calculate tile center by subtracting click offset from cursor position
        QPoint tileCenterGlobal = cursorGlobalPos - m_dragClickOffset;
        QPoint tileCenterInPanel = mapFromGlobal(tileCenterGlobal);

        // DEBUG: Verify preview positioning
        // Preview top-left = tile center - (size/2, size/2)
        // Mouse rel to preview TL = cursor - preview_top_left = cursor - (tile_center - size/2) = offset + size/2
        QPoint expectedMouseRelToPreviewTL = m_dragClickOffset + QPoint(tileSize/2, tileSize/2);
        emit debugMessage(QString("PREVIEW: cursor=(%1,%2) offset=(%3,%4) => mouse should be at (%5,%6) rel to preview TL")
                         .arg(cursorGlobalPos.x()).arg(cursorGlobalPos.y())
                         .arg(m_dragClickOffset.x()).arg(m_dragClickOffset.y())
                         .arg(expectedMouseRelToPreviewTL.x()).arg(expectedMouseRelToPreviewTL.y()));

        // Convert tile center from panel coordinates to MainWidget coordinates
        QPoint pixmapCenterInMain = mapTo(mainWidget, tileCenterInPanel);
        QPoint pixmapCenterGlobal = mainWidget->mapToGlobal(pixmapCenterInMain);
        QPoint pixmapCenterInBoard = boardView->mapFromGlobal(pixmapCenterGlobal);

        // Use overlap-based square detection (same as drop)
        int pixmapLeft = pixmapCenterInMain.x() - tileSize / 2;
        int pixmapRight = pixmapCenterInMain.x() + tileSize / 2;
        int pixmapTop = pixmapCenterInMain.y() - tileSize / 2;
        int pixmapBottom = pixmapCenterInMain.y() + tileSize / 2;

        QPoint pixmapLeftTopGlobal = mainWidget->mapToGlobal(QPoint(pixmapLeft, pixmapTop));
        QPoint pixmapRightBottomGlobal = mainWidget->mapToGlobal(QPoint(pixmapRight, pixmapBottom));
        QPoint pixmapLeftTopInBoard = boardView->mapFromGlobal(pixmapLeftTopGlobal);
        QPoint pixmapRightBottomInBoard = boardView->mapFromGlobal(pixmapRightBottomGlobal);

        int pixmapLeftInBoard = pixmapLeftTopInBoard.x();
        int pixmapRightInBoard = pixmapRightBottomInBoard.x();
        int pixmapTopInBoard = pixmapLeftTopInBoard.y();
        int pixmapBottomInBoard = pixmapRightBottomInBoard.y();

        int marginX = boardView->getMarginX();
        int marginY = boardView->getMarginY();
        int squareSize = boardView->getSquareSize();

        // Find which squares the pixmap overlaps
        int leftCol = std::max(0, (pixmapLeftInBoard - marginX) / squareSize);
        int rightCol = std::min(14, (pixmapRightInBoard - marginX) / squareSize);
        int topRow = std::max(0, (pixmapTopInBoard - marginY) / squareSize);
        int bottomRow = std::min(14, (pixmapBottomInBoard - marginY) / squareSize);

        // Find square with maximum overlap
        int row = -1, col = -1;
        int maxOverlap = 0;

        for (int r = topRow; r <= bottomRow; r++) {
            for (int c = leftCol; c <= rightCol; c++) {
                int sqLeft = marginX + c * squareSize;
                int sqRight = sqLeft + squareSize;
                int sqTop = marginY + r * squareSize;
                int sqBottom = sqTop + squareSize;

                int overlapLeft = std::max(pixmapLeftInBoard, sqLeft);
                int overlapRight = std::min(pixmapRightInBoard, sqRight);
                int overlapTop = std::max(pixmapTopInBoard, sqTop);
                int overlapBottom = std::min(pixmapBottomInBoard, sqBottom);

                int overlapWidth = std::max(0, overlapRight - overlapLeft);
                int overlapHeight = std::max(0, overlapBottom - overlapTop);
                int overlap = overlapWidth * overlapHeight;

                if (overlap > maxOverlap) {
                    maxOverlap = overlap;
                    row = r;
                    col = c;
                }
            }
        }

        // Check if this is the source square (where we picked up the tile from)
        bool isSourceSquare = (row >= 0 && col >= 0 &&
                              row == m_dragSourceRow && col == m_dragSourceCol);
        bool isEmpty = (row >= 0 && col >= 0 && boardView->isSquareEmpty(row, col));

        // Only process cursor changes if we've moved to a different square
        bool squareChanged = (row != m_lastHoverRow || col != m_lastHoverCol);

        emit debugMessage(QString("dragMove: eventPos=(%1,%2) globalPos=(%3,%4)")
                         .arg(eventPos.x()).arg(eventPos.y())
                         .arg(globalPos.x()).arg(globalPos.y()));
        emit debugMessage(QString("  tileSize=%1 pixmapCenter in Main=(%2,%3)")
                         .arg(tileSize)
                         .arg(pixmapCenterInMain.x()).arg(pixmapCenterInMain.y()));
        emit debugMessage(QString("  pixmap bounds in Board: [%1,%2] to [%3,%4]")
                         .arg(pixmapLeftInBoard).arg(pixmapTopInBoard)
                         .arg(pixmapRightInBoard).arg(pixmapBottomInBoard));
        emit debugMessage(QString("  calculated: row=%1 col=%2 (isEmpty=%3 isSource=%4 changed=%5)")
                         .arg(row).arg(col).arg(isEmpty).arg(isSourceSquare).arg(squareChanged));

        // ALWAYS accept with MoveAction to avoid macOS rejection animation delay
        // We'll validate in dropEvent instead
        event->setDropAction(Qt::MoveAction);
        event->accept();

        if (row >= 0 && col >= 0 && (isEmpty || isSourceSquare)) {
            if (squareChanged) {
                emit debugMessage("  -> Valid drop: showing green outline");
            }

            // Update board hover square to show green outline
            boardView->setHoverSquare(row, col);
        } else {
            // Clear board hover square (no visual feedback for invalid drops)
            boardView->setHoverSquare(-1, -1);
        }

        // Update last hover position
        m_lastHoverRow = row;
        m_lastHoverCol = col;

        // Update drag tile preview with size interpolation
        // Extract the tile character from mime data
        QString mimeText = event->mimeData()->text();
        QStringList parts = mimeText.split(':');

        QChar tileChar;
        if (mimeText.startsWith("board:")) {
            // Board drag format: "board:row:col:char"
            if (parts.size() >= 4) {
                tileChar = parts[3][0];
            }
        } else {
            // Rack drag format: "index:char"
            if (parts.size() >= 2) {
                tileChar = parts[1][0];
            }
        }

        if (!tileChar.isNull()) {
            QPoint eventPos = event->position().toPoint();
            updateDragTilePreview(eventPos, tileChar);
        }
    } else {
        // Not our drag - let it propagate
        QWidget::dragMoveEvent(event);
    }
}

void BoardPanelView::dragLeaveEvent(QDragLeaveEvent *event) {
    // Clear hover square when drag leaves
    boardView->setHoverSquare(-1, -1);

    // Restore keyboard cursor to full opacity
    boardView->setDragActive(false);

    // Don't clear ghost tile here - it should stay visible until drop completes
    // This allows dragging over other panels while keeping the source ghosted

    // Clear hover tracking
    m_lastHoverRow = -1;
    m_lastHoverCol = -1;

    // Restore cursor to default
    unsetCursor();

    // Note: Don't hide preview here - MainWidget will keep it visible
    // as long as we're still in the window
    QWidget::dragLeaveEvent(event);
}

void BoardPanelView::dropEvent(QDropEvent *event) {
    // The drop position should be based on where the preview was ACTUALLY rendered
    // Use the last preview position that was emitted during dragMove
    QPoint globalPos = m_lastPreviewGlobalPos;
    QWidget *mainWidget = window();
    QPoint panelPos = mapFromGlobal(globalPos);
    QPoint boardLocalPos = boardView->mapFromParent(panelPos);

    // Get board geometry info for debugging
    int marginX = boardView->getMarginX();
    int marginY = boardView->getMarginY();
    int squareSize = boardView->getSquareSize();

    // Calculate the tile size at this position
    int tileSize = calculateDragTileSize(panelPos);

    // The pixmap is rendered centered at globalPos in MainWidget coordinates
    QPoint pixmapCenterInMain = mainWidget->mapFromGlobal(globalPos);
    int pixmapLeft = pixmapCenterInMain.x() - tileSize / 2;
    int pixmapRight = pixmapCenterInMain.x() + tileSize / 2;
    int pixmapTop = pixmapCenterInMain.y() - tileSize / 2;
    int pixmapBottom = pixmapCenterInMain.y() + tileSize / 2;

    // Map pixmap center from MainWidget coordinates to BoardView coordinates
    QPoint pixmapCenterGlobal = mainWidget->mapToGlobal(pixmapCenterInMain);
    QPoint pixmapCenterInBoard = boardView->mapFromGlobal(pixmapCenterGlobal);

    // Map pixmap bounds to BoardView coordinates
    QPoint pixmapLeftTopGlobal = mainWidget->mapToGlobal(QPoint(pixmapLeft, pixmapTop));
    QPoint pixmapRightBottomGlobal = mainWidget->mapToGlobal(QPoint(pixmapRight, pixmapBottom));
    QPoint pixmapLeftTopInBoard = boardView->mapFromGlobal(pixmapLeftTopGlobal);
    QPoint pixmapRightBottomInBoard = boardView->mapFromGlobal(pixmapRightBottomGlobal);

    // Find which square has the most overlap with the pixmap
    // Calculate overlap with each square the pixmap touches
    int pixmapLeftInBoard = pixmapLeftTopInBoard.x();
    int pixmapRightInBoard = pixmapRightBottomInBoard.x();
    int pixmapTopInBoard = pixmapLeftTopInBoard.y();
    int pixmapBottomInBoard = pixmapRightBottomInBoard.y();

    // Determine which squares the pixmap overlaps
    int leftCol = std::max(0, (pixmapLeftInBoard - marginX) / squareSize);
    int rightCol = std::min(14, (pixmapRightInBoard - marginX) / squareSize);
    int topRow = std::max(0, (pixmapTopInBoard - marginY) / squareSize);
    int bottomRow = std::min(14, (pixmapBottomInBoard - marginY) / squareSize);

    // Find the square with maximum overlap
    int bestRow = -1, bestCol = -1;
    int maxOverlap = 0;

    // Store overlap info for debugging
    QString overlapDebug;

    for (int r = topRow; r <= bottomRow; r++) {
        for (int c = leftCol; c <= rightCol; c++) {
            // Calculate square bounds
            int sqLeft = marginX + c * squareSize;
            int sqRight = sqLeft + squareSize;
            int sqTop = marginY + r * squareSize;
            int sqBottom = sqTop + squareSize;

            // Calculate overlap area
            int overlapLeft = std::max(pixmapLeftInBoard, sqLeft);
            int overlapRight = std::min(pixmapRightInBoard, sqRight);
            int overlapTop = std::max(pixmapTopInBoard, sqTop);
            int overlapBottom = std::min(pixmapBottomInBoard, sqBottom);

            int overlapWidth = std::max(0, overlapRight - overlapLeft);
            int overlapHeight = std::max(0, overlapBottom - overlapTop);
            int overlap = overlapWidth * overlapHeight;

            // Debug info for this square
            if (overlap > 0) {
                overlapDebug += QString("    [%1,%2]: %3px² (bounds: [%4,%5] to [%6,%7])\n")
                    .arg(r).arg(c).arg(overlap)
                    .arg(sqLeft).arg(sqTop).arg(sqRight).arg(sqBottom);
            }

            if (overlap > maxOverlap) {
                maxOverlap = overlap;
                bestRow = r;
                bestCol = c;
            }
        }
    }

    int row = bestRow;
    int col = bestCol;

    // Calculate what the manual calculation would give us
    int boardRelX = boardLocalPos.x() - marginX;
    int boardRelY = boardLocalPos.y() - marginY;
    int manualCol = boardRelX / squareSize;
    int manualRow = boardRelY / squareSize;

    // Also calculate the column boundaries
    int col6Start = marginX + 6 * squareSize;
    int col7Start = marginX + 7 * squareSize;
    int col8Start = marginX + 8 * squareSize;

    emit debugMessage(QString("=== Drop Analysis ==="));
    emit debugMessage(QString("Drop event global: (%1,%2)").arg(globalPos.x()).arg(globalPos.y()));
    emit debugMessage(QString("Pixmap size: %1px").arg(tileSize));
    emit debugMessage(QString(""));
    emit debugMessage(QString("Pixmap in MainWidget coords:"));
    emit debugMessage(QString("  Center: (%1,%2)").arg(pixmapCenterInMain.x()).arg(pixmapCenterInMain.y()));
    emit debugMessage(QString("  Bounds: [%1,%2] to [%3,%4]").arg(pixmapLeft).arg(pixmapTop).arg(pixmapRight).arg(pixmapBottom));
    emit debugMessage(QString(""));
    emit debugMessage(QString("Pixmap in BoardView coords:"));
    emit debugMessage(QString("  Center: (%1,%2)").arg(pixmapCenterInBoard.x()).arg(pixmapCenterInBoard.y()));
    emit debugMessage(QString("  Bounds: [%1,%2] to [%3,%4]").arg(pixmapLeftInBoard).arg(pixmapTopInBoard).arg(pixmapRightInBoard).arg(pixmapBottomInBoard));
    emit debugMessage(QString(""));
    emit debugMessage(QString("Board geometry:"));
    emit debugMessage(QString("  Margin: (%1,%2)").arg(marginX).arg(marginY));
    emit debugMessage(QString("  Square size: %1px").arg(squareSize));
    emit debugMessage(QString("  Column boundaries: col6=%1 col7=%2 col8=%3").arg(col6Start).arg(col7Start).arg(col8Start));
    emit debugMessage(QString(""));
    emit debugMessage(QString("Overlap analysis:"));
    emit debugMessage(overlapDebug);
    emit debugMessage(QString("  Max overlap: %1px² → Best square: [%2,%3]").arg(maxOverlap).arg(row).arg(col));

    if (row >= 0 && col >= 0 && boardView->isSquareEmpty(row, col)) {
        // Valid drop on empty square
        QString mimeText = event->mimeData()->text();
        QStringList parts = mimeText.split(':');

        // Handle board-to-board drops
        if (mimeText.startsWith("board:")) {
            // Format: "board:row:col:char"
            if (parts.size() >= 4) {
                int srcRow = parts[1].toInt();
                int srcCol = parts[2].toInt();
                QChar tileChar = parts[3][0];

                emit debugMessage(QString("Moved tile '%1' from (%2,%3) to (%4,%5)")
                                .arg(tileChar).arg(srcRow).arg(srcCol).arg(row).arg(col));

                // Remove from source position
                boardView->removeUncommittedTile(srcRow, srcCol);

                // If it's a blank tile (? or lowercase letter), place it first then show designation dialog
                if (tileChar == '?' || (tileChar.isLower() && tileChar >= 'a' && tileChar <= 'z')) {
                    // Place the blank tile temporarily as undesignated
                    boardView->placeUncommittedTile(row, col, '?');

                    // Clear ghost tile immediately (before dialog appears)
                    boardView->clearGhostTile();

                    // Hide the drag preview
                    emit hideDragPreview();

                    // Show designation dialog
                    BlankDesignationDialog dialog(this);
                    if (dialog.exec() == QDialog::Accepted) {
                        QChar selectedLetter = dialog.getSelectedLetter();
                        if (!selectedLetter.isNull()) {
                            // Update to designated blank (lowercase)
                            tileChar = selectedLetter.toLower();
                            boardView->placeUncommittedTile(row, col, tileChar);
                            emit debugMessage(QString("Designated blank as '%1'").arg(selectedLetter));
                        } else {
                            // Dialog was cancelled or no selection - remove the tile and return to rack
                            boardView->removeUncommittedTile(row, col);
                            rackView->addTile('?');
                            emit debugMessage("Blank designation cancelled - returned to rack");
                            event->setDropAction(Qt::IgnoreAction);
                            event->accept();
                            return;
                        }
                    } else {
                        // Dialog was cancelled - remove the tile and return to rack
                        boardView->removeUncommittedTile(row, col);
                        rackView->addTile('?');
                        emit debugMessage("Blank designation cancelled - returned to rack");
                        event->setDropAction(Qt::IgnoreAction);
                        event->accept();
                        return;
                    }
                } else {
                    // Place regular (non-blank) tile at destination
                    boardView->placeUncommittedTile(row, col, tileChar);
                }

                // If dropped on the keyboard cursor position, advance cursor to next empty square
                if (boardView->isKeyboardEntryActive()) {
                    int cursorRow, cursorCol;
                    BoardView::Direction cursorDir;
                    boardView->getKeyboardEntry(cursorRow, cursorCol, cursorDir);

                    if (row == cursorRow && col == cursorCol) {
                        // Dropped on cursor - advance to next empty square
                        int nextRow = row;
                        int nextCol = col;
                        bool foundEmpty = false;

                        // Keep advancing until we find an empty square or go past the board edge
                        for (int step = 1; step <= 15; ++step) {
                            if (cursorDir == BoardView::Horizontal) {
                                nextCol = col + step;
                            } else {
                                nextRow = row + step;
                            }

                            // If we've gone past position 15, stop
                            if (nextRow > 15 || nextCol > 15) {
                                nextRow = qMin(nextRow, 15);
                                nextCol = qMin(nextCol, 15);
                                break;
                            }

                            // If we're at position 15 (off board edge), stop there with caret
                            if (nextRow == 15 || nextCol == 15) {
                                foundEmpty = true;
                                break;
                            }

                            // If this square is empty, we found our target
                            if (boardView->isSquareEmpty(nextRow, nextCol)) {
                                foundEmpty = true;
                                break;
                            }
                        }

                        // Move cursor to the next position
                        boardView->setKeyboardEntry(nextRow, nextCol, cursorDir);
                        if (foundEmpty) {
                            emit debugMessage(QString("Advanced cursor to next empty square (%1, %2)").arg(nextRow).arg(nextCol));
                        } else {
                            emit debugMessage(QString("Advanced cursor to position (%1, %2)").arg(nextRow).arg(nextCol));
                        }
                    }
                }

                // Accept the drop
                event->setDropAction(Qt::MoveAction);
                event->accept();
            }
        }
        // Handle rack-to-board drops
        else if (parts.size() >= 2) {
            int tileIndex = parts[0].toInt();
            QChar tileChar = parts[1][0];

            emit debugMessage(QString("Dropped tile '%1' (index %2) at board position (%3, %4)")
                            .arg(tileChar).arg(tileIndex).arg(row).arg(col));

            // If it's a blank tile, place it first then show designation dialog
            if (tileChar == '?') {
                // Place the blank tile temporarily as undesignated
                boardView->placeUncommittedTile(row, col, '?');
                emit debugMessage(QString("Locked blank '?' at (%1, %2) before dialog").arg(row).arg(col));

                // Remove the tile from the rack immediately
                rackView->removeTileAtIndex(tileIndex);

                // Clear ghost tile immediately (before dialog appears)
                boardView->clearGhostTile();

                // Hide the drag preview
                emit hideDragPreview();

                // Show designation dialog
                BlankDesignationDialog dialog(this);
                if (dialog.exec() == QDialog::Accepted) {
                    QChar selectedLetter = dialog.getSelectedLetter();
                    if (!selectedLetter.isNull()) {
                        // Update to designated blank (lowercase)
                        tileChar = selectedLetter.toLower();
                        boardView->placeUncommittedTile(row, col, tileChar);
                        emit debugMessage(QString("Designated blank as '%1'").arg(selectedLetter));
                    } else {
                        // Dialog was cancelled - remove the tile and return to rack
                        boardView->removeUncommittedTile(row, col);
                        rackView->addTile('?');
                        emit debugMessage("Blank designation cancelled");
                        event->setDropAction(Qt::IgnoreAction);
                        event->accept();
                        return;
                    }
                } else {
                    // Dialog was cancelled - remove the tile and return to rack
                    boardView->removeUncommittedTile(row, col);
                    rackView->addTile('?');
                    emit debugMessage("Blank designation cancelled");
                    event->setDropAction(Qt::IgnoreAction);
                    event->accept();
                    return;
                }
            } else {
                // Place regular (non-blank) tile on the board
                boardView->placeUncommittedTile(row, col, tileChar);

                // Remove the tile from the rack
                rackView->removeTileAtIndex(tileIndex);
            }

            // If dropped on the keyboard cursor position, advance cursor to next empty square
            if (boardView->isKeyboardEntryActive()) {
                int cursorRow, cursorCol;
                BoardView::Direction cursorDir;
                boardView->getKeyboardEntry(cursorRow, cursorCol, cursorDir);

                if (row == cursorRow && col == cursorCol) {
                    // Dropped on cursor - advance to next empty square
                    int nextRow = row;
                    int nextCol = col;
                    bool foundEmpty = false;

                    // Keep advancing until we find an empty square or go past the board edge
                    for (int step = 1; step <= 15; ++step) {
                        if (cursorDir == BoardView::Horizontal) {
                            nextCol = col + step;
                        } else {
                            nextRow = row + step;
                        }

                        // If we've gone past position 15, stop
                        if (nextRow > 15 || nextCol > 15) {
                            nextRow = qMin(nextRow, 15);
                            nextCol = qMin(nextCol, 15);
                            break;
                        }

                        // If we're at position 15 (off board edge), stop there with caret
                        if (nextRow == 15 || nextCol == 15) {
                            foundEmpty = true;
                            break;
                        }

                        // If this square is empty, we found our target
                        if (boardView->isSquareEmpty(nextRow, nextCol)) {
                            foundEmpty = true;
                            break;
                        }
                    }

                    // Move cursor to the next position
                    boardView->setKeyboardEntry(nextRow, nextCol, cursorDir);
                    if (foundEmpty) {
                        emit debugMessage(QString("Advanced cursor to next empty square (%1, %2)").arg(nextRow).arg(nextCol));
                    } else {
                        emit debugMessage(QString("Advanced cursor to position (%1, %2)").arg(nextRow).arg(nextCol));
                    }
                }
            }

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

    // Restore keyboard cursor to full opacity
    boardView->setDragActive(false);

    // Clear ghost tile
    boardView->clearGhostTile();

    // Clear drag source tracking
    m_dragSourceRow = -1;
    m_dragSourceCol = -1;

    // Clear hover tracking
    m_lastHoverRow = -1;
    m_lastHoverCol = -1;

    // Restore cursor to default
    unsetCursor();

    // Reset drag char
    m_currentDragChar = QChar();

    // Hide drag preview
    emit hideDragPreview();
}

QPixmap BoardPanelView::renderTilePreview(QChar tileChar, int size) {
    // Recreate renderer if size changed
    if (!m_dragTileRenderer || m_lastDragTileSize != size) {
        delete m_dragTileRenderer;
        m_dragTileRenderer = new TileRenderer(size, TileRenderer::TileStyle::Rack);
        m_lastDragTileSize = size;
    }

    // Render the tile - blanks always show as undesignated ('?' with 0) during drag
    if (tileChar.isLower() && tileChar >= 'a' && tileChar <= 'z') {
        // Designated blank from board - show as undesignated during drag
        return m_dragTileRenderer->getUndesignatedBlank();
    } else if (tileChar.isUpper() && tileChar >= 'A' && tileChar <= 'Z') {
        return m_dragTileRenderer->getLetterTile(tileChar.toLatin1());
    } else if (tileChar == '?') {
        // Undesignated blank from rack - show as undesignated
        return m_dragTileRenderer->getUndesignatedBlank();
    }
    return QPixmap();
}

int BoardPanelView::calculateDragTileSize(const QPoint &pos) {
    if (!rackView || !boardView) {
        return 0;
    }

    // Get rack and board geometries in this widget's coordinates
    QRect rackRect = rackView->geometry();
    QRect boardRect = boardView->geometry();

    // Get sizes
    int rackTileSize = rackView->height();  // Rack tiles are square, size = rack height
    int boardSquareSize = boardView->getSquareSize();

    if (rackTileSize <= 0 || boardSquareSize <= 0) {
        return 0;
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

    return interpolatedSize;
}

void BoardPanelView::updateDragTilePreview(const QPoint &pos, QChar tileChar) {
    if (!rackView || !boardView) {
        return;
    }

    // Calculate tile size at this position
    int interpolatedSize = calculateDragTileSize(pos);
    if (interpolatedSize <= 0) {
        return;
    }

    // Render the actual tile at the interpolated size
    QPixmap tilePixmap = renderTilePreview(tileChar, interpolatedSize);

    // Use the cursor's actual global position
    QWidget *mainWidget = window();
    QPoint cursorGlobalPos = QCursor::pos();
    QPoint cursorInMainBeforeOffset = mainWidget->mapFromGlobal(cursorGlobalPos);

    // Apply click offset in global coords, then convert final position
    QPoint tileCenterGlobal = cursorGlobalPos - m_dragClickOffset;
    QPoint pixmapCenterInMain = mainWidget->mapFromGlobal(tileCenterGlobal);

    // Store the cursor global position (with offset applied) for later use in drop calculation
    m_lastPreviewGlobalPos = cursorGlobalPos - m_dragClickOffset;

    // BUGFIX: Apply horizontal offset correction of tileSize/2
    // This compensates for a coordinate system mismatch between how rack tiles
    // are positioned and how the preview is rendered
    QPoint correctedPosition = pixmapCenterInMain + QPoint(interpolatedSize/2, 0);

    // Emit signal to update preview at top level with MainWidget coordinates
    emit updateDragPreview(tilePixmap, correctedPosition);
}

void BoardPanelView::animatePreviewBackToRack() {
    // Just hide the preview - animation could be added later
    emit hideDragPreview();
}

void BoardPanelView::keyPressEvent(QKeyEvent *event) {
    emit debugMessage(QString("keyPressEvent: key=%1, keyboardActive=%2")
                     .arg(event->key()).arg(boardView->isKeyboardEntryActive()));

    // Handle Enter/Return key to submit play if enabled
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (playButton && playButton->isEnabled()) {
            emit debugMessage("Enter key pressed - triggering play button");
            onPlayClicked();
            event->accept();
            return;
        }
    }

    // Block keyboard input if not allowed (not player's turn or debug window focused)
    if (!shouldAllowKeyboardInput()) {
        QWidget::keyPressEvent(event);
        return;
    }

    if (!boardView->isKeyboardEntryActive()) {
        QWidget::keyPressEvent(event);
        return;
    }

    int row, col;
    BoardView::Direction dir;
    boardView->getKeyboardEntry(row, col, dir);

    // Handle spacebar to toggle direction
    if (event->key() == Qt::Key_Space) {
        BoardView::Direction newDir = (dir == BoardView::Horizontal) ? BoardView::Vertical : BoardView::Horizontal;
        boardView->setKeyboardEntry(row, col, newDir);
        emit debugMessage(QString("Direction toggled to %1").arg(newDir == BoardView::Horizontal ? "Horizontal" : "Vertical"));
        event->accept();
        return;
    }

    // Handle arrow keys to move to next empty square
    if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Left ||
        event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {

        int newRow = row;
        int newCol = col;
        int deltaRow = 0;
        int deltaCol = 0;

        // Determine movement direction based on arrow key
        if (event->key() == Qt::Key_Right) {
            deltaCol = 1;
        } else if (event->key() == Qt::Key_Left) {
            deltaCol = -1;
        } else if (event->key() == Qt::Key_Down) {
            deltaRow = 1;
        } else if (event->key() == Qt::Key_Up) {
            deltaRow = -1;
        }

        // Move in the arrow direction until we find an empty square or hit the edge
        bool foundEmpty = false;
        for (int step = 1; step <= 15; ++step) {
            newRow = row + deltaRow * step;
            newCol = col + deltaCol * step;

            // Stop if we've gone off the board (past position 15)
            if (newRow < 0 || newCol < 0 || newRow > 15 || newCol > 15) {
                break;
            }

            // If we're at position 15 (off board edge), stop there
            if (newRow == 15 || newCol == 15) {
                foundEmpty = true;
                break;
            }

            // If this square is empty, we found our target
            if (boardView->isSquareEmpty(newRow, newCol)) {
                foundEmpty = true;
                break;
            }
        }

        if (foundEmpty) {
            boardView->setKeyboardEntry(newRow, newCol, dir);
            emit debugMessage(QString("Moved cursor to (%1, %2)").arg(newRow).arg(newCol));
        } else {
            emit debugMessage("No empty square found in that direction");
        }

        event->accept();
        return;
    }

    // Handle Escape to cancel keyboard entry
    if (event->key() == Qt::Key_Escape) {
        boardView->clearKeyboardEntry();
        emit debugMessage("Keyboard entry cancelled");
        event->accept();
        return;
    }

    // Handle backspace to move backwards and delete uncommitted tile if present
    if (event->key() == Qt::Key_Backspace) {
        int searchRow = row;
        int searchCol = col;

        // If we're at or past position 15, start from 14
        if (searchRow >= 15 || searchCol >= 15) {
            searchRow = qMin(searchRow, 14);
            searchCol = qMin(searchCol, 14);
        } else {
            // Move back one position to start searching
            if (dir == BoardView::Horizontal) {
                searchCol--;
            } else {
                searchRow--;
            }
        }

        // Search backwards for the previous non-permanent-tile square
        while (searchRow >= 0 && searchCol >= 0) {
            // Check if this square is empty or has an uncommitted tile
            // (i.e., not occupied by a permanent tile)
            if (boardView->isSquareEmpty(searchRow, searchCol) ||
                boardView->hasUncommittedTile(searchRow, searchCol)) {

                // If there's an uncommitted tile here, remove it
                if (boardView->hasUncommittedTile(searchRow, searchCol)) {
                    QChar tileChar;
                    const auto& tiles = boardView->getUncommittedTiles();
                    for (const auto& tile : tiles) {
                        if (tile.row == searchRow && tile.col == searchCol) {
                            tileChar = tile.letter;
                            break;
                        }
                    }

                    // Remove from board
                    boardView->removeUncommittedTile(searchRow, searchCol);

                    // Return to rack
                    if (!tileChar.isNull()) {
                        rackView->addTile(tileChar);
                        emit debugMessage(QString("Removed tile '%1' at (%2, %3) and returned to rack").arg(tileChar).arg(searchRow).arg(searchCol));
                    }
                }

                // Move cursor to this position
                boardView->setKeyboardEntry(searchRow, searchCol, dir);
                event->accept();
                return;
            }

            // This square has a permanent tile - keep searching backwards
            if (dir == BoardView::Horizontal) {
                searchCol--;
            } else {
                searchRow--;
            }
        }

        // Reached the edge of the board - do nothing
        emit debugMessage("Reached start of board");
        event->accept();
        return;
    }

    // Handle letter keys (A-Z)
    QString text = event->text().toUpper();
    if (text.length() == 1) {
        QChar ch = text[0];
        if (ch >= 'A' && ch <= 'Z') {
            // Don't allow typing if we're at position 15 (off the board)
            if (row >= 15 || col >= 15) {
                emit debugMessage("Cannot place tile at position 15 (off board)");
                event->accept();
                return;
            }

            // Check if shift key is held (forces blank usage if available)
            bool forceBlank = (event->modifiers() & Qt::ShiftModifier);

            // Determine which tile to use
            if (forceBlank) {
                // Shift held: prefer blank, fallback to natural
                if (rackView->hasBlank()) {
                    if (rackView->removeBlank()) {
                        boardView->placeUncommittedTile(row, col, ch.toLower());
                        emit debugMessage(QString("Placed blank as '%1' at (%2, %3) [shift]").arg(ch).arg(row).arg(col));
                    } else {
                        emit debugMessage(QString("Failed to remove blank from rack"));
                        event->accept();
                        return;
                    }
                } else if (rackView->hasNaturalLetter(ch)) {
                    if (rackView->removeNaturalLetter(ch)) {
                        boardView->placeUncommittedTile(row, col, ch);
                        emit debugMessage(QString("Placed natural '%1' at (%2, %3) [shift, no blank available]").arg(ch).arg(row).arg(col));
                    } else {
                        emit debugMessage(QString("Failed to remove natural '%1' from rack").arg(ch));
                        event->accept();
                        return;
                    }
                } else {
                    emit debugMessage(QString("Tile '%1' not available in rack").arg(ch));
                    event->accept();
                    return;
                }
            } else {
                // No shift: prefer natural, fallback to blank
                if (rackView->hasNaturalLetter(ch)) {
                    if (rackView->removeNaturalLetter(ch)) {
                        boardView->placeUncommittedTile(row, col, ch);
                        emit debugMessage(QString("Placed natural '%1' at (%2, %3)").arg(ch).arg(row).arg(col));
                    } else {
                        emit debugMessage(QString("Failed to remove natural '%1' from rack").arg(ch));
                        event->accept();
                        return;
                    }
                } else if (rackView->hasBlank()) {
                    if (rackView->removeBlank()) {
                        boardView->placeUncommittedTile(row, col, ch.toLower());
                        emit debugMessage(QString("Placed blank as '%1' at (%2, %3)").arg(ch).arg(row).arg(col));
                    } else {
                        emit debugMessage(QString("Failed to remove blank from rack"));
                        event->accept();
                        return;
                    }
                } else {
                    emit debugMessage(QString("Tile '%1' not available in rack").arg(ch));
                    event->accept();
                    return;
                }
            }

            // Move to next empty square (skip over already-placed tiles)
            int nextRow = row;
            int nextCol = col;
            bool foundEmpty = false;

            // Keep advancing until we find an empty square or go past the board edge
            while (true) {
                if (dir == BoardView::Horizontal) {
                    nextCol++;
                } else {
                    nextRow++;
                }

                // If we've gone past position 15, stop at 15
                if (nextRow > 15 || nextCol > 15) {
                    nextRow = qMin(nextRow, 15);
                    nextCol = qMin(nextCol, 15);
                    break;
                }

                // If we're at position 15 (off board edge), stop there with caret
                if (nextRow == 15 || nextCol == 15) {
                    break;
                }

                // If this square is empty, we found our target
                if (boardView->isSquareEmpty(nextRow, nextCol)) {
                    foundEmpty = true;
                    break;
                }

                // Otherwise, this square has a tile - keep looking
            }

            // Move cursor to the next position (can be 0-15)
            boardView->setKeyboardEntry(nextRow, nextCol, dir);

            if (foundEmpty) {
                emit debugMessage(QString("Advanced to next empty square (%1, %2)").arg(nextRow).arg(nextCol));
            } else if (nextRow == 15 || nextCol == 15) {
                emit debugMessage(QString("Advanced to position 15 - insertion caret at edge").arg(nextRow).arg(nextCol));
            } else {
                emit debugMessage(QString("Advanced to position (%1, %2) - no empty squares").arg(nextRow).arg(nextCol));
            }
            event->accept();
            return;
        }
    }

    QWidget::keyPressEvent(event);
}

void BoardPanelView::paintEvent(QPaintEvent *event) {
    // Call base class paintEvent first
    QWidget::paintEvent(event);

    // Draw keyboard cursor overlay if at position 15 (for insertion caret at edge)
    if (boardView && boardView->isKeyboardEntryActive()) {
        int row, col;
        BoardView::Direction dir;
        boardView->getKeyboardEntry(row, col, dir);

        // Only draw overlay if cursor is at position 15 (caret extends beyond BoardView bounds)
        if (row == 15 || col == 15) {
            QPainter painter(this);
            renderCursorOverlay(painter);
        }
    }
}

void BoardPanelView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);

    // Maintain 3:4 aspect ratio (width = 3/4 * height)
    // Calculate desired width based on available height
    int availableHeight = event->size().height();
    int desiredWidth = (availableHeight * 3) / 4;

    // Constrain to minimum width
    constexpr int MIN_WIDTH = 322;
    if (desiredWidth < MIN_WIDTH) {
        desiredWidth = MIN_WIDTH;
    }

    // Set fixed width to maintain aspect ratio
    setFixedWidth(desiredWidth);
}

void BoardPanelView::renderCursorOverlay(QPainter &painter) {
    if (!boardView) return;

    int row, col;
    BoardView::Direction dir;
    boardView->getKeyboardEntry(row, col, dir);

    int squareSize = boardView->getSquareSize();
    if (squareSize <= 0) return;

    // Calculate position in BoardPanelView coordinates
    QPoint boardPos = boardView->pos();
    int marginX = boardView->getMarginX();
    int marginY = boardView->getMarginY();

    int x = boardPos.x() + marginX + col * squareSize;
    int y = boardPos.y() + marginY + row * squareSize;

    // Draw insertion caret for position 15
    painter.setRenderHint(QPainter::Antialiasing);
    QPen caretPen(QColor(0, 200, 0, 200), 3);
    caretPen.setCapStyle(Qt::RoundCap);
    painter.setPen(caretPen);

    if (dir == BoardView::Horizontal && col == 15) {
        // Vertical bar for horizontal direction at right edge
        int centerX = x;
        int topY = y + 2;
        int bottomY = y + squareSize - 2;

        // Main vertical line
        painter.drawLine(centerX, topY, centerX, bottomY);

        // Top horizontal cap (narrower)
        int capWidth = 4;
        painter.drawLine(centerX - capWidth/2, topY, centerX + capWidth/2, topY);

        // Bottom horizontal cap (narrower)
        painter.drawLine(centerX - capWidth/2, bottomY, centerX + capWidth/2, bottomY);
    } else if (dir == BoardView::Vertical && row == 15) {
        // Horizontal bar for vertical direction at bottom edge
        int centerY = y;
        int leftX = x + 2;
        int rightX = x + squareSize - 2;

        // Main horizontal line
        painter.drawLine(leftX, centerY, rightX, centerY);

        // Left vertical cap (narrower)
        int capHeight = 4;
        painter.drawLine(leftX, centerY - capHeight/2, leftX, centerY + capHeight/2);

        // Right vertical cap (narrower)
        painter.drawLine(rightX, centerY - capHeight/2, rightX, centerY + capHeight/2);
    }
}

void BoardPanelView::validateAndLogUncommittedTiles() {
    if (!boardView || !game) {
        return;
    }

    // Generate move notation from uncommitted tiles
    QString notation = boardView->generateMoveNotation(game);

    // If notation is empty, tiles can't form valid notation
    if (notation.isEmpty()) {
        const auto& tiles = boardView->getUncommittedTiles();
        if (tiles.isEmpty()) {
            emit validationMessage("Move validation: No tiles placed");
        } else if (tiles.size() == 1) {
            // Check for undesignated blank
            if (tiles[0].letter == '?') {
                emit validationMessage("Move validation: Cannot validate - undesignated blank");
            } else {
                emit validationMessage("Move validation: Single tile (need playthrough or more tiles)");
            }
        } else {
            // Check for undesignated blank
            bool hasUndesignatedBlank = false;
            for (const auto& tile : tiles) {
                if (tile.letter == '?') {
                    hasUndesignatedBlank = true;
                    break;
                }
            }

            if (hasUndesignatedBlank) {
                emit validationMessage("Move validation: Cannot validate - undesignated blank");
            } else {
                emit validationMessage("Move validation: Tiles not in a line or have gaps");
            }
        }
        m_moveIsValid = false;
        updateButtonStates();
        emit uncommittedMoveChanged();  // Notify game history
        return;
    }

    // We have valid notation - attempt to validate it
    emit validationMessage(QString("Move notation: %1").arg(notation));

    // Call MAGPIE validation (player 0 for now)
    char *errorMsg = magpie_validate_move(game, 0, notation.toUtf8().constData());

    if (errorMsg) {
        // Validation failed - log the error
        QString errorStr = QString::fromUtf8(errorMsg);
        // Extract just the error type from the full message
        // Format is typically: "ERROR_STATUS_...: description"
        QString errorType = errorStr.section(':', 0, 0).trimmed();

        // Make error type human-readable
        errorType.replace("ERROR_STATUS_MOVE_VALIDATION_", "");
        errorType.replace('_', ' ');
        errorType = errorType.toLower();

        emit validationMessage(QString("Validation failed: %1").arg(errorType));
        free(errorMsg);
        m_moveIsValid = false;
    } else {
        // Validation succeeded!
        emit validationMessage("Validation: OK - move is well-formed and connected");
        m_moveIsValid = true;
    }

    updateButtonStates();
    emit uncommittedMoveChanged();  // Notify game history
}

void BoardPanelView::setButtonEnabled(QPushButton *button, bool enabled) {
    if (!button) return;

    button->setEnabled(enabled);

    // Update styling based on state
    if (enabled) {
        // Blue enabled state
        button->setStyleSheet(
            "QPushButton {"
            "  background-color: #6B9BD1;"  // Blue
            "  color: white;"
            "  border: 1px solid #5A8AC0;"
            "  border-radius: 4px;"
            "  padding: 8px 16px;"
            "  font-weight: bold;"
            "  font-size: 12px;"
            "}"
            "QPushButton:hover {"
            "  background-color: #5A8AC0;"
            "  border: 1px solid #4A7AB0;"
            "}"
            "QPushButton:pressed {"
            "  background-color: #4A7AB0;"
            "}"
        );
    } else {
        // Grey disabled state
        button->setStyleSheet(
            "QPushButton {"
            "  background-color: #D0D0D0;"  // Grey
            "  color: #888888;"
            "  border: 1px solid #C0C0C0;"
            "  border-radius: 4px;"
            "  padding: 8px 16px;"
            "  font-weight: bold;"
            "  font-size: 12px;"
            "}"
        );
    }
}

bool BoardPanelView::shouldAllowKeyboardInput() const {
    // Disable keyboard input if it's not the player's turn or if debug window has focus
    return m_isPlayerTurn && !m_debugWindowHasFocus;
}

void BoardPanelView::setPlayerTurn(bool isPlayerTurn) {
    m_isPlayerTurn = isPlayerTurn;
    updateButtonStates();

    // Clear keyboard entry mode when not player's turn
    if (!isPlayerTurn && boardView) {
        boardView->clearKeyboardEntry();
    }

    // Emit turn changed signal (0 = player, 1 = computer)
    int playerIndex = isPlayerTurn ? 0 : 1;
    emit playerTurnChanged(playerIndex);

    emit debugMessage(QString("Turn changed: %1").arg(isPlayerTurn ? "Player" : "Computer"));
}

void BoardPanelView::setDebugWindowFocused(bool focused) {
    m_debugWindowHasFocus = focused;
    emit debugMessage(QString("Debug window focus: %1").arg(focused ? "true" : "false"));
}

void BoardPanelView::updateButtonStates() {
    // All buttons disabled when not player's turn
    if (!m_isPlayerTurn) {
        setButtonEnabled(passButton, false);
        setButtonEnabled(exchangeButton, false);
        setButtonEnabled(playButton, false);
        playButton->setText("Play");
        return;
    }

    // Pass: always enabled in gameplay mode when it's player's turn
    setButtonEnabled(passButton, true);

    // Exchange: enabled if exchanges are allowed (assume always for now)
    setButtonEnabled(exchangeButton, true);

    // Play: enabled only if move is valid
    setButtonEnabled(playButton, m_moveIsValid);

    // Update button text to show ⏎ symbol when enabled
    if (m_moveIsValid) {
        playButton->setText("Play \u23ce");
    } else {
        playButton->setText("Play");
    }
}

void BoardPanelView::onPassClicked() {
    emit debugMessage("Pass button clicked");
    // TODO: Implement pass logic
}

void BoardPanelView::onExchangeClicked() {
    emit debugMessage("Exchange button clicked");
    // TODO: Implement exchange logic
}

void BoardPanelView::onPlayClicked() {
    if (!game || !boardView || !m_moveIsValid) {
        return;
    }

    emit debugMessage("Play button clicked - submitting move");

    // Get the move notation
    QString notation = boardView->generateMoveNotation(game);
    if (notation.isEmpty()) {
        emit debugMessage("ERROR: No valid move notation");
        return;
    }

    emit debugMessage(QString("Submitting move: %1").arg(notation));

    // Get score before the move
    int prevScore = magpie_get_player_score(game, 0);

    // Get the move score
    int playScore = magpie_get_move_score(game, 0, notation.toUtf8().constData());

    // Submit the player's move (player 0)
    char *errorMsg = magpie_play_move(game, 0, notation.toUtf8().constData());
    if (errorMsg) {
        emit debugMessage(QString("ERROR: Failed to play move: %1").arg(QString::fromUtf8(errorMsg)));
        free(errorMsg);
        return;
    }

    // Get score after the move
    int newScore = magpie_get_player_score(game, 0);

    emit debugMessage(QString("Move played successfully: %1 pts (%2 + %3 = %4)")
                     .arg(playScore).arg(prevScore).arg(playScore).arg(newScore));

    // Update the board display FIRST (before clearing uncommitted)
    Board *board = magpie_get_board_from_game(game);
    boardView->setBoard(board);
    emit debugMessage(QString("Board updated after player move (board ptr: %1)").arg((quintptr)board, 0, 16));

    // Clear uncommitted tiles from board (now that committed tiles are on the board)
    boardView->clearUncommittedTiles();

    // Update CGP display
    updateCgpDisplay();

    // Update rack with new tiles
    char *rackStr = magpie_get_rack(game, 0);
    QString rackQString;
    if (rackStr) {
        rackQString = QString::fromUtf8(rackStr);
        rackView->setRack(rackQString);
        free(rackStr);
    }

    // Emit move committed signal for game history to freeze this turn and create new one
    emit moveCommitted(0, prevScore, playScore, newScore, notation, rackQString);

    // Reset validation state
    m_moveIsValid = false;

    // Switch to computer's turn
    setPlayerTurn(false);

    // Emit board changed signal (this will create computer's placeholder turn entry)
    emit boardChanged();

    // Make computer move immediately (delay happens inside)
    makeComputerMove();
}

void BoardPanelView::makeComputerMove() {
    if (!game) {
        return;
    }

    emit debugMessage("Computer is thinking...");

    // Delay for 3 seconds to simulate thinking, then make the move
    QTimer::singleShot(3000, this, [this]() {
        if (!game) {
            return;
        }

        // Get top equity move for player 1 (computer)
        char *moveNotation = magpie_get_top_equity_move(game, 1);
        if (!moveNotation) {
            emit debugMessage("ERROR: Computer failed to generate move");
            return;
        }

        QString computerMove = toUppercaseNotation(QString::fromUtf8(moveNotation));
        emit debugMessage(QString("Computer plays: %1").arg(computerMove));

        // Get score before the move
        int prevScore = magpie_get_player_score(game, 1);

        // Get the move score
        int playScore = magpie_get_move_score(game, 1, moveNotation);

        // Play the computer's move
        char *errorMsg = magpie_play_move(game, 1, moveNotation);
        free(moveNotation);

        if (errorMsg) {
            emit debugMessage(QString("ERROR: Computer move failed: %1").arg(QString::fromUtf8(errorMsg)));
            free(errorMsg);
            return;
        }

        // Get score after the move
        int newScore = magpie_get_player_score(game, 1);

        emit debugMessage(QString("Computer move played successfully: %1 pts (%2 + %3 = %4)")
                         .arg(playScore).arg(prevScore).arg(playScore).arg(newScore));

        // Update the board display
        Board *board = magpie_get_board_from_game(game);
        boardView->setBoard(board);
        emit debugMessage(QString("Board updated after computer move (board ptr: %1)").arg((quintptr)board, 0, 16));

        // Update CGP display
        updateCgpDisplay();

        // Get computer's rack for history (but don't display it)
        char *computerRackStr = magpie_get_rack(game, 1);
        QString computerRack = computerRackStr ? QString::fromUtf8(computerRackStr) : QString();
        if (computerRackStr) {
            free(computerRackStr);
        }

        // Emit move committed signal for computer's move
        emit moveCommitted(1, prevScore, playScore, newScore, computerMove, computerRack);

        // Update player's rack (player 0, not computer's rack)
        char *rackStr = magpie_get_rack(game, 0);
        if (rackStr) {
            rackView->setRack(QString::fromUtf8(rackStr));
            free(rackStr);
        }

        // Emit board changed signal to update game history
        emit boardChanged();

        // Switch back to player's turn (this will start player's timer)
        setPlayerTurn(true);

        emit debugMessage("Player's turn");
    });
}