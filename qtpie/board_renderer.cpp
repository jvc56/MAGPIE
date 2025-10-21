#include "board_renderer.h"
#include <QPainter>

BoardRenderer::BoardRenderer(int squareSize, Board *board)
    : m_squareSize(squareSize)
    , m_tileRenderer(squareSize)
    , m_board(board)
{
}

QPixmap BoardRenderer::renderEmptyBoard() const {
    QVector<QVector<QString>> emptyPosition(BOARD_DIM, QVector<QString>(BOARD_DIM, ""));
    return renderBoard(emptyPosition);
}

QPixmap BoardRenderer::renderBoard(const QVector<QVector<QString>>& position) const {
    // Board is exactly 15x15 squares (no additional margins)
    int boardPixelSize = BOARD_DIM * m_squareSize;
    QPixmap board(boardPixelSize, boardPixelSize);

    // Support retina/HiDPI displays - render at 2x and set device pixel ratio
    qreal dpr = 2.0; // Render at 2x for retina
    board = QPixmap(boardPixelSize * dpr, boardPixelSize * dpr);
    board.setDevicePixelRatio(dpr);

    board.fill(QColor(230, 230, 240)); // Background color (light-theme)

    QPainter painter(&board);

    // Draw each square
    for (int row = 0; row < BOARD_DIM; ++row) {
        for (int col = 0; col < BOARD_DIM; ++col) {
            QString letter = (row < position.size() && col < position[row].size())
                             ? position[row][col] : "";

            const QPixmap* tile;

            if (!letter.isEmpty()) {
                // There's a letter on this square
                char letterChar = letter[0].toLatin1();
                bool isBlank = letter[0].isLower();

                if (isBlank) {
                    tile = &m_tileRenderer.getBlankTile(letterChar);
                } else {
                    tile = &m_tileRenderer.getLetterTile(letterChar);
                }
            } else {
                // Empty square - show premium square or empty
                PremiumSquare premium = getPremiumSquare(row, col);

                if (premium == PremiumSquare::None) {
                    tile = &m_tileRenderer.getEmptySquare();
                } else {
                    tile = &m_tileRenderer.getPremiumSquare(premium);
                }
            }

            // Draw tile at exact position
            int x = col * m_squareSize;
            int y = row * m_squareSize;
            painter.drawPixmap(x, y, *tile);
        }
    }

    painter.end();
    return board;
}

QSize BoardRenderer::boardSize() const {
    int size = BOARD_DIM * m_squareSize;
    return QSize(size, size);
}

PremiumSquare BoardRenderer::getPremiumSquare(int row, int col) const {
    // If we have a MAGPIE board, get the bonus square from it
    if (m_board) {
        MagpieBonusSquare bonus = magpie_get_bonus_square(m_board, row, col);

        // Map MAGPIE bonus square types to our PremiumSquare enum
        switch (bonus) {
            case MAGPIE_DOUBLE_LETTER_SCORE:
                return PremiumSquare::DoubleLetter;
            case MAGPIE_TRIPLE_LETTER_SCORE:
                return PremiumSquare::TripleLetter;
            case MAGPIE_DOUBLE_WORD_SCORE:
                return PremiumSquare::DoubleWord;
            case MAGPIE_TRIPLE_WORD_SCORE:
                return PremiumSquare::TripleWord;
            case MAGPIE_BONUS_NONE:
            default:
                // Center square (7, 7) should show a star
                if (row == 7 && col == 7) {
                    return PremiumSquare::Star;
                }
                return PremiumSquare::None;
        }
    }

    // Fallback: if no board, return empty square
    return PremiumSquare::None;
}
