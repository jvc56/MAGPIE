#include "board_view.h"
#include "board_renderer.h"
#include <QPainter>
#include <QResizeEvent>

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

bool BoardView::hasHeightForWidth() const {
    return true;
}

int BoardView::heightForWidth(int w) const {
    // Board must be square
    return w;
}

QSize BoardView::sizeHint() const {
    return QSize(750, 750);
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
    }
}
