#ifndef BOARD_RENDERER_H
#define BOARD_RENDERER_H

#include <QPixmap>
#include <QSize>
#include "tile_renderer.h"
#include "magpie_wrapper.h"

/**
 * BoardRenderer manages the complete board visualization.
 *
 * It:
 * - Uses MAGPIE's board layout for premium squares
 * - Pre-renders the empty board with premium squares positioned correctly
 * - Ensures all squares are exactly the same pixel size
 * - Adjusts margins to maintain square aspect ratio
 */
class BoardRenderer {
public:
    explicit BoardRenderer(int squareSize, Board *board = nullptr);

    // Set the MAGPIE board to use for bonus squares
    void setBoard(Board *board) { m_board = board; }

    // Render the complete empty board with premium squares
    QPixmap renderEmptyBoard() const;

    // Render board with position (15x15 grid of letters, empty string for empty square)
    QPixmap renderBoard(const QVector<QVector<QString>>& position) const;

    // Get board dimensions
    QSize boardSize() const;
    int squareSize() const { return m_squareSize; }

private:
    // Get premium square from MAGPIE board layout
    PremiumSquare getPremiumSquare(int row, int col) const;

    int m_squareSize;
    TileRenderer m_tileRenderer;
    Board *m_board;

    static constexpr int BOARD_DIM = 15;
};

#endif // BOARD_RENDERER_H
