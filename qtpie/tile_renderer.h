#ifndef TILE_RENDERER_H
#define TILE_RENDERER_H

#include <QPixmap>
#include <QMap>
#include <QString>
#include <QPainter>

// Premium square types
enum class PremiumSquare {
    None,
    DoubleLetter,
    TripleLetter,
    DoubleWord,
    TripleWord,
    Star
};

/**
 * TileRenderer pre-renders all possible tile types at a given resolution.
 *
 * Similar to the Python implementation, this class:
 * - Pre-renders all letter tiles (A-Z)
 * - Pre-renders blank tiles (a-z displayed as lowercase)
 * - Pre-renders premium squares (DL, TL, DW, TW, STAR)
 * - Caches everything as QPixmaps for fast compositing
 *
 * All tiles are rendered at exactly the same pixel size with rounded corners,
 * gradients, and proper letter/value positioning.
 */
class TileRenderer {
public:
    enum class TileStyle {
        Board,  // Beige tiles for board
        Rack    // Green tiles for rack
    };

    explicit TileRenderer(int tileSize, TileStyle style = TileStyle::Board);

    // Get pre-rendered tiles
    const QPixmap& getLetterTile(char letter) const;
    const QPixmap& getBlankTile(char letter) const;
    const QPixmap& getPremiumSquare(PremiumSquare type) const;
    const QPixmap& getPremiumSquareNoLabel(PremiumSquare type) const;
    const QPixmap& getEmptySquare() const;

    int tileSize() const { return m_tileSize; }

private:
    void renderAllTiles();

    // Render individual tile types
    QPixmap renderLetterTile(char letter, bool isBlank);
    QPixmap renderPremiumSquare(PremiumSquare type, bool includeLabel = true);
    QPixmap renderEmptySquare();

    // Helper drawing functions
    void drawRoundedRect(QPainter& painter, const QRectF& rect, double cornerRadius);
    void applyGradient(QImage& image, const QRectF& rect, double intensity = 0.18);

    int m_tileSize;
    TileStyle m_style;
    QString m_letterFontFamily;  // ClearSans-Bold
    QString m_valueFontFamily;   // Roboto-Bold
    QMap<char, QPixmap> m_letterTiles;    // A-Z
    QMap<char, QPixmap> m_blankTiles;     // a-z
    QMap<PremiumSquare, QPixmap> m_premiumSquares;
    QMap<PremiumSquare, QPixmap> m_premiumSquaresNoLabel;  // Premium squares without labels
    QPixmap m_emptySquare;
};

#endif // TILE_RENDERER_H
