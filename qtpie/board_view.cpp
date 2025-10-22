#include "board_view.h"
#include "board_renderer.h"
#include "tile_renderer.h"
#include <QPainter>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QDrag>
#include <QMimeData>

BoardView::BoardView(QWidget *parent)
    : QWidget(parent)
    , board(nullptr)
{
    // We'll render the board when we get resized
    // Minimum size: 15 squares * 20px/square (300) + margins for labels (100) = 400px
    constexpr int MIN_SQUARE_SIZE = 20;
    constexpr int BOARD_DIM = 15;
    // Add 1/8 for margins as in resizeEvent
    int minSize = MIN_SQUARE_SIZE * BOARD_DIM * 9 / 8;  // * 9/8 accounts for margin
    setMinimumSize(minSize, minSize);
}

BoardView::~BoardView() {
    delete m_tileRenderer;
    delete m_boardRenderer;
}

bool BoardView::hasHeightForWidth() const {
    return true;
}

int BoardView::heightForWidth(int w) const {
    // Board must be square
    return w;
}

QSize BoardView::sizeHint() const {
    // Add extra space for position 15 cursor (one additional square size)
    int extraSpace = m_squareSize > 0 ? m_squareSize : 50;
    return QSize(750 + extraSpace, 750 + extraSpace);
}

QSize BoardView::minimumSizeHint() const {
    // Ensure minimum size is respected in layouts
    // Same calculation as in constructor
    constexpr int MIN_SQUARE_SIZE = 20;
    constexpr int BOARD_DIM = 15;
    int minSize = MIN_SQUARE_SIZE * BOARD_DIM * 9 / 8;  // 337.5 -> 337 (but panel enforces 330)
    return QSize(minSize, minSize);
}

void BoardView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);

    constexpr int BOARD_DIM = 15;
    constexpr int EFFECTIVE_DIM = 16;  // Include space for position 15 cursor
    constexpr int MIN_SQUARE_SIZE = 20;  // Minimum readable square size
    constexpr int BOARD_PADDING = 5;     // Fixed padding around board squares (right/bottom)

    int availableWidth = event->size().width();
    int availableHeight = event->size().height();

    // We need to reserve space for labels and padding
    // Start with a rough estimate for label space based on minimum square size
    int estimatedFontSize = qBound(8, MIN_SQUARE_SIZE / 3, 24);
    int estimatedLabelMargin = MIN_SQUARE_SIZE / 8;
    int estimatedLabelSpace = estimatedFontSize * 2 + estimatedLabelMargin;

    // Calculate available space for the board itself
    int widthForBoard = availableWidth - std::max(BOARD_PADDING, estimatedLabelSpace) - BOARD_PADDING;
    int heightForBoard = availableHeight - std::max(BOARD_PADDING, estimatedLabelSpace) - BOARD_PADDING;
    int availableSize = std::min(widthForBoard, heightForBoard);

    // Calculate square size
    m_squareSize = availableSize / BOARD_DIM;

    // Enforce minimum square size - board stays readable
    if (m_squareSize < MIN_SQUARE_SIZE) {
        m_squareSize = MIN_SQUARE_SIZE;
    }

    // Now calculate actual space needed for labels based on final square size
    m_labelFontSize = qBound(8, m_squareSize / 3, 24);
    constexpr int labelMarginTop = 1;   // Fixed 1px margin above board for column labels
    constexpr int labelMarginLeft = 2;  // Fixed 2px margin left of board for row labels
    int labelSpaceTop = m_labelFontSize * 2 + labelMarginTop;   // Space needed above board for column labels
    int labelSpaceLeft = m_labelFontSize * 2 + labelMarginLeft; // Space needed left of board for row labels

    // Position board with label space on top/left, fixed padding on right/bottom
    // Reduce top/left margins by 2px for tighter spacing
    m_marginX = std::max(BOARD_PADDING, labelSpaceLeft) - 2;
    m_marginY = std::max(BOARD_PADDING, labelSpaceTop) - 2;

    // CRITICAL FIX: Don't call setMinimumSize in resizeEvent - causes infinite resize loop!
    // Each resize creates a new TileRenderer, causing memory explosion
    // Removed setMinimumSize call to prevent infinite loop

    // CRITICAL: Recreate tile renderers when size changes to avoid memory leak in paintEvent
    // Only create if size actually changed to avoid unnecessary allocations
    static int lastSquareSize = 0;
    if (m_squareSize != lastSquareSize) {
        delete m_tileRenderer;
        delete m_boardRenderer;
        m_tileRenderer = new TileRenderer(m_squareSize, TileRenderer::TileStyle::Rack);
        m_boardRenderer = new TileRenderer(m_squareSize, TileRenderer::TileStyle::Board);
        lastSquareSize = m_squareSize;
    }

    // Re-render the board at the new size
    renderBoard();

    update();
}

QString BoardView::parseCgpBoard(const QString& cgp) {
    // Extract just the board part (before the first space or /)
    QString boardPart = cgp.split(' ').first().trimmed();
    return boardPart;
}

void BoardView::renderBoard() {
    if (m_squareSize <= 0) {
        return;
    }

    // Create board renderer with exact square size
    BoardRenderer renderer(m_squareSize, board);

    // Parse CGP position if we have one
    if (!m_cgpPosition.isEmpty()) {
        QString boardPart = parseCgpBoard(m_cgpPosition);
        QStringList rows = boardPart.split('/');

        // Create 15x15 grid
        constexpr int BOARD_DIM = 15;
        QVector<QVector<QString>> position(BOARD_DIM, QVector<QString>(BOARD_DIM, ""));

        for (int row = 0; row < BOARD_DIM && row < rows.size(); ++row) {
            QString rowStr = rows[row];
            int col = 0;
            int i = 0;

            while (i < rowStr.length() && col < BOARD_DIM) {
                QChar c = rowStr[i];

                if (c.isDigit()) {
                    // Number indicates empty squares
                    QString numStr;
                    while (i < rowStr.length() && rowStr[i].isDigit()) {
                        numStr += rowStr[i];
                        ++i;
                    }
                    int emptyCount = numStr.toInt();
                    col += emptyCount;
                    continue;
                } else if (c.isLetter()) {
                    // Letter (uppercase or lowercase for blank)
                    position[row][col] = QString(c);
                    ++col;
                }
                ++i;
            }
        }

        m_boardPixmap = renderer.renderBoard(position);
    } else {
        // Render empty board
        m_boardPixmap = renderer.renderEmptyBoard();
    }
}

void BoardView::setCgpPosition(const QString& cgp) {
    m_cgpPosition = cgp;
    renderBoard();
    update();
}

void BoardView::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    // Fill background (matching light-theme)
    painter.fillRect(rect(), QColor(230, 230, 240));

    // Draw the board pixmap if we have one
    if (!m_boardPixmap.isNull()) {
        painter.drawPixmap(m_marginX, m_marginY, m_boardPixmap);
    }

    // Draw row and column labels
    if (m_squareSize > 0) {
        constexpr int BOARD_DIM = 15;

        // Label color - darker than background
        QColor labelColor(60, 60, 60);

        // Font for labels - use calculated font size
        QFont labelFont("Arial", m_labelFontSize, QFont::Bold);
        painter.setFont(labelFont);
        painter.setPen(labelColor);

        // Margin between labels and board - different for top and left
        constexpr int labelMarginTop = 1;   // Fixed 1px margin above board for column labels
        constexpr int labelMarginLeft = 2;  // Fixed 2px margin left of board for row labels

        // Column labels (A-O) at top - close to board edge
        // Position labels so their bottom is labelMarginTop pixels above the board
        for (int col = 0; col < BOARD_DIM; ++col) {
            QString label = QString(QChar('A' + col));
            int x = m_marginX + col * m_squareSize + m_squareSize / 2;

            // Draw text with bottom edge at labelMarginTop above board
            // Use a tall rect to ensure text bottom aligns exactly
            int rectTop = m_marginY - labelMarginTop - m_labelFontSize * 2;
            QRectF rect(x - m_squareSize / 2, rectTop,
                        m_squareSize, m_labelFontSize * 2);
            painter.drawText(rect, Qt::AlignCenter | Qt::AlignBottom, label);
        }

        // Row labels (1-15) at left - right justified with labelMarginLeft
        int labelWidth = m_labelFontSize * 2;  // Width for label text
        for (int row = 0; row < BOARD_DIM; ++row) {
            QString label = QString::number(row + 1);
            int y = m_marginY + row * m_squareSize + m_squareSize / 2;

            // Right-justify the label with labelMarginLeft to the left of the board
            QRectF rect(m_marginX - labelMarginLeft - labelWidth, y - m_labelFontSize,
                        labelWidth, m_labelFontSize * 2);
            painter.drawText(rect, Qt::AlignRight | Qt::AlignVCenter, label);
        }

        // Draw uncommitted tiles (placed but not committed) in green
        if (!m_uncommittedTiles.isEmpty() && m_tileRenderer) {
            // Use cached tile renderer to avoid memory leak
            for (const UncommittedTile &tile : m_uncommittedTiles) {
                // Skip this tile if it's the ghost position (being dragged)
                if (tile.row == m_ghostRow && tile.col == m_ghostCol) {
                    continue;
                }

                int x = m_marginX + tile.col * m_squareSize;
                int y = m_marginY + tile.row * m_squareSize;

                QPixmap tilePixmap;
                if (tile.letter == '?') {
                    // Blank tile - need to show designated letter
                    tilePixmap = m_tileRenderer->getBlankTile('A');  // Default to 'A' for now
                } else if (tile.letter.isLower() && tile.letter >= 'a' && tile.letter <= 'z') {
                    // Lowercase = blank tile with designated letter
                    tilePixmap = m_tileRenderer->getBlankTile(tile.letter.toUpper().toLatin1());
                } else if (tile.letter.isUpper() && tile.letter >= 'A' && tile.letter <= 'Z') {
                    // Normal tile
                    tilePixmap = m_tileRenderer->getLetterTile(tile.letter.toLatin1());
                }

                painter.drawPixmap(x, y, tilePixmap);
            }
        }

        // Draw ghost tile (dimmed version at original position during drag)
        if (m_ghostRow >= 0 && m_ghostCol >= 0 && !m_ghostLetter.isNull() && m_tileRenderer) {
            int x = m_marginX + m_ghostCol * m_squareSize;
            int y = m_marginY + m_ghostRow * m_squareSize;

            QPixmap tilePixmap;
            if (m_ghostLetter.isLower() && m_ghostLetter >= 'a' && m_ghostLetter <= 'z') {
                // Blank tile with designated letter
                tilePixmap = m_tileRenderer->getBlankTile(m_ghostLetter.toUpper().toLatin1());
            } else if (m_ghostLetter.isUpper() && m_ghostLetter >= 'A' && m_ghostLetter <= 'Z') {
                // Normal tile
                tilePixmap = m_tileRenderer->getLetterTile(m_ghostLetter.toLatin1());
            }

            // Draw with 45% opacity to make it ghosted/dimmed
            painter.setOpacity(0.45);
            painter.drawPixmap(x, y, tilePixmap);
            painter.setOpacity(1.0);  // Restore full opacity
        }

        // Draw green hover outline for valid drop target
        if (m_hoverRow >= 0 && m_hoverCol >= 0) {
            int x = m_marginX + m_hoverCol * m_squareSize;
            int y = m_marginY + m_hoverRow * m_squareSize;

            // Green outline: thicker (4px), semi-transparent
            painter.setPen(QPen(QColor(0, 200, 0, 180), 4));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(x, y, m_squareSize, m_squareSize);
        }

        // Draw keyboard entry indicator
        if (m_keyboardRow >= 0 && m_keyboardCol >= 0 && m_keyboardRow <= 15 && m_keyboardCol <= 15) {
            int x = m_marginX + m_keyboardCol * m_squareSize;
            int y = m_marginY + m_keyboardRow * m_squareSize;

            // Check if we're at position 15 (off board edge) - use insertion caret
            if (m_keyboardRow == 15 || m_keyboardCol == 15) {
                painter.setRenderHint(QPainter::Antialiasing);

                // Draw insertion caret (green bar with caps)
                QPen caretPen(QColor(0, 200, 0, 220), 3);
                caretPen.setCapStyle(Qt::RoundCap);
                painter.setPen(caretPen);

                // Disable clipping so caret at edge doesn't get cropped
                painter.setClipping(false);

                if (m_keyboardDir == Horizontal && m_keyboardCol == 15) {
                    // Vertical bar for horizontal direction at right edge
                    int centerX = x;
                    int topY = y + 2;
                    int bottomY = y + m_squareSize - 2;

                    // Main vertical line
                    painter.drawLine(centerX, topY, centerX, bottomY);

                    // Top horizontal cap (narrower)
                    int capWidth = 4;
                    painter.drawLine(centerX - capWidth/2, topY, centerX + capWidth/2, topY);

                    // Bottom horizontal cap (narrower)
                    painter.drawLine(centerX - capWidth/2, bottomY, centerX + capWidth/2, bottomY);
                } else if (m_keyboardDir == Vertical && m_keyboardRow == 15) {
                    // Horizontal bar for vertical direction at bottom edge
                    int centerY = y;
                    int leftX = x + 2;
                    int rightX = x + m_squareSize - 2;

                    // Main horizontal line
                    painter.drawLine(leftX, centerY, rightX, centerY);

                    // Left vertical cap (narrower)
                    int capHeight = 4;
                    painter.drawLine(leftX, centerY - capHeight/2, leftX, centerY + capHeight/2);

                    // Right vertical cap (narrower)
                    painter.drawLine(rightX, centerY - capHeight/2, rightX, centerY + capHeight/2);
                }

                // Re-enable clipping
                painter.setClipping(true);
            } else {
                // Normal position (0-14): Draw square with arrow
                // Draw premium square without label (if on empty premium square and we have cached renderer)
                if (board && m_boardRenderer) {
                    // Check if square is empty (no tile on board and no uncommitted tile)
                    bool hasUncommittedTile = false;
                    for (const UncommittedTile &tile : m_uncommittedTiles) {
                        if (tile.row == m_keyboardRow && tile.col == m_keyboardCol) {
                            hasUncommittedTile = true;
                            break;
                        }
                    }

                    bool isEmpty = isSquareEmpty(m_keyboardRow, m_keyboardCol) && !hasUncommittedTile;

                    if (isEmpty) {
                        MagpieBonusSquare bonus = magpie_get_bonus_square(board, m_keyboardRow, m_keyboardCol);
                        PremiumSquare premiumType = PremiumSquare::None;

                        switch (bonus) {
                            case MAGPIE_DOUBLE_LETTER_SCORE:
                                premiumType = PremiumSquare::DoubleLetter;
                                break;
                            case MAGPIE_TRIPLE_LETTER_SCORE:
                                premiumType = PremiumSquare::TripleLetter;
                                break;
                            case MAGPIE_DOUBLE_WORD_SCORE:
                                premiumType = PremiumSquare::DoubleWord;
                                break;
                            case MAGPIE_TRIPLE_WORD_SCORE:
                                premiumType = PremiumSquare::TripleWord;
                                break;
                            default:
                                premiumType = PremiumSquare::None;
                                break;
                        }

                        // Draw premium square without label using cached board renderer
                        if (premiumType != PremiumSquare::None) {
                            const QPixmap& premiumSquare = m_boardRenderer->getPremiumSquareNoLabel(premiumType);
                            painter.drawPixmap(x, y, premiumSquare);
                        }
                    }
                }

                // Draw green square outline
                painter.setPen(QPen(QColor(0, 200, 0, 180), 4));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(x, y, m_squareSize, m_squareSize);

                // Render arrow at 4x scale for supersampling
                qreal supersample = 4.0;
                qreal dpr = 2.0;
                int renderSize = m_squareSize * supersample;

                // Create image with alpha channel for the arrow
                QImage arrowImage(renderSize, renderSize, QImage::Format_ARGB32);
                arrowImage.fill(Qt::transparent);

                QPainter arrowPainter(&arrowImage);
                arrowPainter.setRenderHint(QPainter::Antialiasing);

                // Draw semi-transparent green arrow at 4x scale
                arrowPainter.setPen(QPen(QColor(0, 200, 0, 200), 3 * supersample));
                arrowPainter.setBrush(QColor(0, 200, 0, 150));

                int centerX = renderSize / 2;
                int centerY = renderSize / 2;
                int arrowSize = (m_squareSize * supersample) / 3;

                if (m_keyboardDir == Horizontal) {
                    // Right arrow
                    QPolygon arrow;
                    arrow << QPoint(centerX - arrowSize/2, centerY - arrowSize/2)
                          << QPoint(centerX + arrowSize/2, centerY)
                          << QPoint(centerX - arrowSize/2, centerY + arrowSize/2);
                    arrowPainter.drawPolygon(arrow);
                } else {
                    // Down arrow
                    QPolygon arrow;
                    arrow << QPoint(centerX - arrowSize/2, centerY - arrowSize/2)
                          << QPoint(centerX, centerY + arrowSize/2)
                          << QPoint(centerX + arrowSize/2, centerY - arrowSize/2);
                    arrowPainter.drawPolygon(arrow);
                }

                arrowPainter.end();

                // Downsample from 4x to final size using high-quality smooth scaling
                QImage scaledArrowImage = arrowImage.scaled(
                    m_squareSize * dpr, m_squareSize * dpr,
                    Qt::IgnoreAspectRatio,
                    Qt::SmoothTransformation
                );

                QPixmap arrowPixmap = QPixmap::fromImage(scaledArrowImage);
                arrowPixmap.setDevicePixelRatio(dpr);

                // Draw the supersampled arrow
                painter.drawPixmap(x, y, arrowPixmap);
            }
        }
    }
}

void BoardView::getBoardCoordinates(const QPoint &pos, int &row, int &col) const {
    row = -1;
    col = -1;

    if (m_squareSize <= 0) {
        return;
    }

    // Convert widget coordinates to board coordinates
    int x = pos.x() - m_marginX;
    int y = pos.y() - m_marginY;

    // Check if within board bounds
    constexpr int BOARD_DIM = 15;
    if (x >= 0 && y >= 0 && x < BOARD_DIM * m_squareSize && y < BOARD_DIM * m_squareSize) {
        col = x / m_squareSize;
        row = y / m_squareSize;
    }
}

bool BoardView::isSquareEmpty(int row, int col) const {
    if (!board || row < 0 || col < 0 || row >= 15 || col >= 15) {
        return false;
    }

    // Check if there's an uncommitted tile here
    if (hasUncommittedTile(row, col)) {
        return false;
    }

    return magpie_board_is_square_empty(board, row, col) != 0;
}

void BoardView::setHoverSquare(int row, int col) {
    if (m_hoverRow != row || m_hoverCol != col) {
        m_hoverRow = row;
        m_hoverCol = col;
        update();  // Trigger repaint to show hover outline
    }
}

void BoardView::placeUncommittedTile(int row, int col, QChar letter) {
    // Remove any existing uncommitted tile at this position
    removeUncommittedTile(row, col);

    // Add the new tile
    UncommittedTile tile;
    tile.row = row;
    tile.col = col;
    tile.letter = letter;
    m_uncommittedTiles.append(tile);

    update();  // Trigger repaint to show the new tile
}

void BoardView::removeUncommittedTile(int row, int col) {
    for (int i = 0; i < m_uncommittedTiles.size(); ++i) {
        if (m_uncommittedTiles[i].row == row && m_uncommittedTiles[i].col == col) {
            m_uncommittedTiles.removeAt(i);
            update();
            return;
        }
    }
}

void BoardView::clearUncommittedTiles() {
    if (!m_uncommittedTiles.isEmpty()) {
        m_uncommittedTiles.clear();
        update();
    }
}

bool BoardView::hasUncommittedTile(int row, int col) const {
    for (const UncommittedTile &tile : m_uncommittedTiles) {
        if (tile.row == row && tile.col == col) {
            return true;
        }
    }
    return false;
}

void BoardView::setGhostTile(int row, int col, QChar letter) {
    m_ghostRow = row;
    m_ghostCol = col;
    m_ghostLetter = letter;
    update();
}

void BoardView::clearGhostTile() {
    m_ghostRow = -1;
    m_ghostCol = -1;
    update();
}

void BoardView::setKeyboardEntry(int row, int col, Direction dir) {
    m_keyboardRow = row;
    m_keyboardCol = col;
    m_keyboardDir = dir;

    update();

    // If cursor is at position 15, also update parent to draw overlay
    if ((row == 15 || col == 15) && parentWidget()) {
        parentWidget()->update();
    }
}

void BoardView::clearKeyboardEntry() {
    m_keyboardRow = -1;
    m_keyboardCol = -1;
    update();

    // Also update parent to clear overlay
    if (parentWidget()) {
        parentWidget()->update();
    }
}

void BoardView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        int row, col;
        getBoardCoordinates(event->pos(), row, col);

        if (row >= 0 && col >= 0) {
            // Check if clicking on an uncommitted tile
            if (hasUncommittedTile(row, col)) {
                m_draggedRow = row;
                m_draggedCol = col;
                m_dragStartPos = event->pos();
            }
            // Check if clicking on empty square (for keyboard entry)
            else if (isSquareEmpty(row, col)) {
                emit squareClicked(row, col);
            }
        }
    }
    QWidget::mousePressEvent(event);
}

void BoardView::mouseMoveEvent(QMouseEvent *event) {
    if (m_draggedRow >= 0 && m_draggedCol >= 0) {
        // Start dragging if moved more than a few pixels
        if ((event->pos() - m_dragStartPos).manhattanLength() > 5) {
            // Find the tile being dragged
            QChar tileChar;
            for (const UncommittedTile &tile : m_uncommittedTiles) {
                if (tile.row == m_draggedRow && tile.col == m_draggedCol) {
                    tileChar = tile.letter;
                    break;
                }
            }

            if (!tileChar.isNull()) {
                // Create drag operation
                QDrag *drag = new QDrag(this);
                QMimeData *mimeData = new QMimeData;

                // Store board position and character in format "board:row:col:char"
                mimeData->setText(QString("board:%1:%2:%3")
                                .arg(m_draggedRow).arg(m_draggedCol).arg(tileChar));
                drag->setMimeData(mimeData);

                // Use invisible pixmap (preview will be shown by overlay)
                QPixmap invisiblePixmap(1, 1);
                invisiblePixmap.fill(Qt::transparent);
                drag->setPixmap(invisiblePixmap);
                drag->setHotSpot(QPoint(0, 0));

                // Emit signal so BoardPanelView can show preview
                emit tileDragStarted(mapToParent(event->pos()), tileChar);

                // Execute the drag
                // Use IgnoreAction as default to disable snap-back animation on rejected drops
                Qt::DropAction dropAction = drag->exec(Qt::MoveAction | Qt::IgnoreAction, Qt::IgnoreAction);

                // If drop succeeded, the tile was already removed by the drop handler
                // If drop failed, tile stays where it is

                // Clear ghost tile now that drag is complete
                clearGhostTile();

                // Notify that drag ended
                emit tileDragEnded(dropAction);

                // Reset drag state
                m_draggedRow = -1;
                m_draggedCol = -1;
                update();

                return;
            }
        }
    }
    QWidget::mouseMoveEvent(event);
}

void BoardView::mouseReleaseEvent(QMouseEvent *event) {
    // Reset drag state
    m_draggedRow = -1;
    m_draggedCol = -1;
    QWidget::mouseReleaseEvent(event);
}
