#include "tile_renderer.h"
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QRadialGradient>
#include <QCoreApplication>
#include <QDir>
#include <cmath>

// Constants matching Python implementation
static const double TILE_FRACTION = 0.88;
static const double CORNER_RADIUS_FRACTION = 0.25;
static const double GRADIENT_FRACTION = 0.98;

// Colors (matching light-theme from Python implementation)
static const QColor BOARD_TILE_COLOR(245, 230, 190);     // Beige tile background for board
static const QColor BOARD_BORDER_COLOR(180, 170, 140);   // Brownish border for board tiles
static const QColor RACK_TILE_COLOR(120, 180, 115);      // Green tile background for rack
static const QColor RACK_BORDER_COLOR(90, 140, 85);      // Greenish border for rack tiles
static const QColor LETTER_COLOR(0, 0, 0);               // Black text
static const QColor BG_COLOR(230, 230, 240);             // Board background
static const QColor EMPTY_SQUARE_COLOR(195, 196, 208);   // Empty square fill

// Premium square colors (light-theme)
static const QColor DL_COLOR(150, 190, 220);         // Light blue
static const QColor TL_COLOR(80, 140, 180);          // Dark blue
static const QColor DW_COLOR(240, 160, 170);         // Pink
static const QColor TW_COLOR(220, 100, 100);         // Red
static const QColor STAR_COLOR(240, 160, 170);       // Pink (same as DW)

// Point values for Scrabble letters
static const QMap<char, int> LETTER_VALUES = {
    {'A', 1}, {'B', 3}, {'C', 3}, {'D', 2}, {'E', 1}, {'F', 4}, {'G', 2}, {'H', 4},
    {'I', 1}, {'J', 8}, {'K', 5}, {'L', 1}, {'M', 3}, {'N', 1}, {'O', 1}, {'P', 3},
    {'Q', 10}, {'R', 1}, {'S', 1}, {'T', 1}, {'U', 1}, {'V', 4}, {'W', 4}, {'X', 8},
    {'Y', 4}, {'Z', 10}
};

// Helper to load custom fonts
static QString loadFontFamily(const QString& fontPath) {
    int id = QFontDatabase::addApplicationFont(fontPath);
    if (id != -1) {
        QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) {
            return families.first();
        }
    }
    return QString();
}

TileRenderer::TileRenderer(int tileSize, TileStyle style)
    : m_tileSize(tileSize)
    , m_style(style)
{
    // Load custom fonts
    QDir fontsDir(QCoreApplication::applicationDirPath() + "/../Resources/fonts");
    if (!fontsDir.exists()) {
        // Try alternate location for development
        fontsDir.setPath(QCoreApplication::applicationDirPath() + "/fonts");
    }

    m_letterFontFamily = loadFontFamily(fontsDir.filePath("ClearSans-Bold.ttf"));
    m_valueFontFamily = loadFontFamily(fontsDir.filePath("Roboto-Bold.ttf"));

    // Fallback to system fonts if custom fonts not found
    if (m_letterFontFamily.isEmpty()) {
        m_letterFontFamily = "Arial";
    }
    if (m_valueFontFamily.isEmpty()) {
        m_valueFontFamily = "Arial";
    }

    renderAllTiles();
}

void TileRenderer::renderAllTiles() {
    // Render letter tiles (A-Z)
    for (char c = 'A'; c <= 'Z'; ++c) {
        m_letterTiles[c] = renderLetterTile(c, false);
    }

    // Render blank tiles (a-z, displayed as uppercase in outline)
    for (char c = 'A'; c <= 'Z'; ++c) {
        m_blankTiles[c] = renderLetterTile(c, true);
    }

    // Render premium squares
    m_premiumSquares[PremiumSquare::DoubleLetter] = renderPremiumSquare(PremiumSquare::DoubleLetter);
    m_premiumSquares[PremiumSquare::TripleLetter] = renderPremiumSquare(PremiumSquare::TripleLetter);
    m_premiumSquares[PremiumSquare::DoubleWord] = renderPremiumSquare(PremiumSquare::DoubleWord);
    m_premiumSquares[PremiumSquare::TripleWord] = renderPremiumSquare(PremiumSquare::TripleWord);
    m_premiumSquares[PremiumSquare::Star] = renderPremiumSquare(PremiumSquare::Star);

    // Render empty square
    m_emptySquare = renderEmptySquare();
}

QPixmap TileRenderer::renderLetterTile(char letter, bool isBlank) {
    // Render at 4x scale for supersampling, then downsample for high-quality antialiasing
    // Combined with 2x for retina = 8x total rendering resolution
    qreal supersample = 4.0;
    qreal dpr = 2.0;
    int renderSize = m_tileSize * supersample;

    // Create image with explicit alpha channel support (non-premultiplied to avoid black edge artifacts)
    QImage image(renderSize, renderSize, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Calculate tile area (centered within the square) - scaled by supersample
    int tilePixelSize = static_cast<int>(m_tileSize * supersample * TILE_FRACTION);
    int margin = (renderSize - tilePixelSize) / 2;
    QRectF tileRect(margin, margin, tilePixelSize, tilePixelSize);

    // Draw tile background with subtle border
    QColor tileColor = (m_style == TileStyle::Rack) ? RACK_TILE_COLOR : BOARD_TILE_COLOR;
    QColor borderColor = (m_style == TileStyle::Rack) ? RACK_BORDER_COLOR : BOARD_BORDER_COLOR;

    painter.setBrush(tileColor);
    QPen borderPen(borderColor, qMax(1.0, m_tileSize * supersample / 50.0));
    painter.setPen(borderPen);
    double cornerRadius = tilePixelSize * CORNER_RADIUS_FRACTION;
    drawRoundedRect(painter, tileRect, cornerRadius);

    painter.end();

    // Apply gradient effect
    int gradientSize = static_cast<int>(tilePixelSize * GRADIENT_FRACTION);
    int gradientOffset = (tilePixelSize - gradientSize) / 2;
    QRectF gradientRect(margin + gradientOffset, margin + gradientOffset,
                        gradientSize, gradientSize);
    applyGradient(image, gradientRect, 0.18);

    // Draw letter and value
    painter.begin(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // Letter font (large, bold) - use ClearSans-Bold (scaled)
    QFont letterFont(m_letterFontFamily, static_cast<int>(m_tileSize * supersample * 0.5), QFont::Bold);
    // Disable hinting for smoother rendering at any size
    letterFont.setHintingPreference(QFont::PreferNoHinting);
    letterFont.setStyleStrategy(QFont::PreferAntialias);
    painter.setFont(letterFont);
    painter.setPen(LETTER_COLOR);

    double letterOffsetUp = 0.05;
    QPointF letterCenter(margin + gradientSize / 2.0,
                         margin + gradientSize * (0.5 - letterOffsetUp));

    if (isBlank) {
        // Draw blank tile with outlined letter (scaled)
        painter.save();
        QPen outlinePen(LETTER_COLOR);
        outlinePen.setWidth(qMax(2.0, m_tileSize * supersample / 30.0));
        painter.setPen(outlinePen);
        painter.setBrush(Qt::NoBrush);

        int blankSize = static_cast<int>(gradientSize * 0.6667);
        int blankX = letterCenter.x() - blankSize / 2;
        int blankY = letterCenter.y() - blankSize / 2;
        int blankRadius = static_cast<int>(blankSize * 0.25);

        painter.drawRoundedRect(blankX, blankY, blankSize, blankSize,
                                blankRadius, blankRadius);
        painter.restore();
    }

    // Draw the letter (centered)
    QRectF letterRect(letterCenter.x() - gradientSize / 2,
                      letterCenter.y() - gradientSize / 2,
                      gradientSize, gradientSize);
    painter.drawText(letterRect, Qt::AlignCenter, QString(letter));

    // Draw point value (bottom-right corner) for non-blank tiles
    if (!isBlank) {
        int value = LETTER_VALUES.value(letter, 0);
        QString valueStr = QString::number(value);

        // Font size and position based on digit count (matching Python implementation)
        double fontScale;
        double hOffset;
        double adjustX, adjustY;

        if (valueStr.length() == 1) {
            // 1-digit values: larger font, further right
            fontScale = 0.76;  // 80% larger than 0.42 for better readability
            hOffset = 0.83;  // Moved 0.05 left from 0.88
            adjustX = -2.0;
            adjustY = -2.0;
        } else {
            // 2-digit values: smaller font, more centered
            fontScale = 0.58;  // 80% larger than 0.32 for better readability
            hOffset = 0.77;  // Moved 0.05 left from 0.82
            adjustX = 1.0;
            adjustY = -1.0;
        }

        // Calculate absolute font size (scaled by supersample)
        // Base font size at reference scale is 22, tile size is 63 at scale 1
        // So font size = (m_tileSize / 63.0) * 22 * fontScale * supersample
        int fontSize = static_cast<int>((m_tileSize / 63.0) * 22.0 * fontScale * supersample);
        QFont valueFont(m_valueFontFamily, fontSize, QFont::Bold);
        // Disable hinting for smoother rendering at any size
        valueFont.setHintingPreference(QFont::PreferNoHinting);
        valueFont.setStyleStrategy(QFont::PreferAntialias);
        painter.setFont(valueFont);

        // Position in bottom-right corner (scaled)
        double scale = m_tileSize * supersample / 63.0;
        double valueX = margin + gradientOffset + hOffset * gradientSize + adjustX * scale;
        double valueY = margin + gradientOffset + 0.80 * gradientSize + adjustY * scale;

        // Draw centered at calculated position
        QFontMetrics fm(valueFont);
        QRectF valueRect = fm.boundingRect(valueStr);
        valueRect.moveCenter(QPointF(valueX, valueY));
        painter.drawText(valueRect, Qt::AlignCenter, valueStr);
    }

    painter.end();

    // Downsample from 4x to final size using high-quality smooth scaling
    QImage scaledImage = image.scaled(
        m_tileSize * dpr, m_tileSize * dpr,
        Qt::IgnoreAspectRatio,
        Qt::SmoothTransformation  // High-quality Lanczos-like filtering
    );

    QPixmap finalPixmap = QPixmap::fromImage(scaledImage);
    finalPixmap.setDevicePixelRatio(dpr);

    return finalPixmap;
}

QPixmap TileRenderer::renderPremiumSquare(PremiumSquare type) {
    // Render at 4x scale for supersampling, then downsample
    qreal supersample = 4.0;
    qreal dpr = 2.0;
    int renderSize = m_tileSize * supersample;

    QPixmap pixmap(renderSize, renderSize);
    pixmap.fill(BG_COLOR);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // Determine color and label
    QColor color;
    QString label;
    double fontScale = 0.35;  // Slightly smaller than before

    switch (type) {
        case PremiumSquare::DoubleLetter:
            color = DL_COLOR;
            label = "DL";
            break;
        case PremiumSquare::TripleLetter:
            color = TL_COLOR;
            label = "TL";
            break;
        case PremiumSquare::DoubleWord:
            color = DW_COLOR;
            label = "DW";
            break;
        case PremiumSquare::TripleWord:
            color = TW_COLOR;
            label = "TW";
            break;
        case PremiumSquare::Star:
            color = STAR_COLOR;
            label = "★";
            fontScale = 0.55;  // Slightly smaller star
            break;
        default:
            color = BG_COLOR;
            label = "";
            break;
    }

    // Calculate tile area (scaled)
    int tilePixelSize = static_cast<int>(m_tileSize * supersample * TILE_FRACTION);
    int margin = (renderSize - tilePixelSize) / 2;
    QRectF tileRect(margin, margin, tilePixelSize, tilePixelSize);

    // Draw colored square with subtle border (scaled)
    painter.setBrush(color);
    // Slightly darker version of the square color for border
    QColor borderColor = color.darker(115);
    QPen borderPen(borderColor, qMax(1.0, m_tileSize * supersample / 50.0));
    painter.setPen(borderPen);
    double cornerRadius = tilePixelSize * CORNER_RADIUS_FRACTION;
    drawRoundedRect(painter, tileRect, cornerRadius);

    // Draw label with white text (scaled)
    QFont labelFont("Arial", static_cast<int>(m_tileSize * supersample * fontScale), QFont::Bold);
    labelFont.setHintingPreference(QFont::PreferNoHinting);
    labelFont.setStyleStrategy(QFont::PreferAntialias);
    painter.setFont(labelFont);
    painter.setPen(Qt::white);  // White text instead of black

    painter.drawText(tileRect, Qt::AlignCenter, label);

    painter.end();

    // Downsample from 4x to final size
    QPixmap finalPixmap = pixmap.scaled(
        m_tileSize * dpr, m_tileSize * dpr,
        Qt::IgnoreAspectRatio,
        Qt::SmoothTransformation
    );
    finalPixmap.setDevicePixelRatio(dpr);

    return finalPixmap;
}

QPixmap TileRenderer::renderEmptySquare() {
    // Render at 4x scale for supersampling, then downsample
    qreal supersample = 4.0;
    qreal dpr = 2.0;
    int renderSize = m_tileSize * supersample;

    QPixmap pixmap(renderSize, renderSize);
    pixmap.fill(BG_COLOR);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // Draw empty square with rounded corners and subtle border (scaled)
    int tilePixelSize = static_cast<int>(m_tileSize * supersample * TILE_FRACTION);
    int margin = (renderSize - tilePixelSize) / 2;
    QRectF tileRect(margin, margin, tilePixelSize, tilePixelSize);

    painter.setBrush(EMPTY_SQUARE_COLOR);
    // Subtle border for empty squares (scaled)
    QColor borderColor = EMPTY_SQUARE_COLOR.darker(110);
    QPen borderPen(borderColor, qMax(1.0, m_tileSize * supersample / 50.0));
    painter.setPen(borderPen);
    double cornerRadius = tilePixelSize * CORNER_RADIUS_FRACTION;
    drawRoundedRect(painter, tileRect, cornerRadius);

    painter.end();

    // Downsample from 4x to final size
    QPixmap finalPixmap = pixmap.scaled(
        m_tileSize * dpr, m_tileSize * dpr,
        Qt::IgnoreAspectRatio,
        Qt::SmoothTransformation
    );
    finalPixmap.setDevicePixelRatio(dpr);

    return finalPixmap;
}

void TileRenderer::drawRoundedRect(QPainter& painter, const QRectF& rect, double cornerRadius) {
    QPainterPath path;
    path.addRoundedRect(rect, cornerRadius, cornerRadius);
    painter.drawPath(path);
}

void TileRenderer::applyGradient(QImage& image, const QRectF& rect, double intensity) {
    if (image.isNull()) return;

    int x1 = static_cast<int>(rect.x());
    int y1 = static_cast<int>(rect.y());
    int x2 = static_cast<int>(rect.right());
    int y2 = static_cast<int>(rect.bottom());

    double width = rect.width();
    double height = rect.height();
    double centerX = x1 + width / 2.0;
    double centerY = y1 + height / 2.0;

    for (int py = y1; py < y2 && py < image.height(); ++py) {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(py));
        for (int px = x1; px < x2 && px < image.width(); ++px) {
            // Distance from center (normalized)
            double dx = (px - centerX) / (width / 2.0);
            double dy = (py - centerY) / (height / 2.0);
            double dist = std::sqrt(dx * dx + dy * dy);

            // Concave gradient (darker at edges)
            double factor = std::min(1.0, dist);
            int gradientValue = static_cast<int>(255 * (1.0 - intensity * factor));

            QRgb pixel = line[px];
            int r = qRed(pixel);
            int g = qGreen(pixel);
            int b = qBlue(pixel);
            int a = qAlpha(pixel);  // Preserve alpha channel

            double opacity = intensity;
            r = static_cast<int>(r * (1.0 - opacity) + gradientValue * opacity);
            g = static_cast<int>(g * (1.0 - opacity) + gradientValue * opacity);
            b = static_cast<int>(b * (1.0 - opacity) + gradientValue * opacity);

            line[px] = qRgba(r, g, b, a);  // Use qRgba to preserve alpha
        }
    }
}

const QPixmap& TileRenderer::getLetterTile(char letter) const {
    char upperLetter = QChar(letter).toUpper().toLatin1();
    auto it = m_letterTiles.constFind(upperLetter);
    if (it != m_letterTiles.constEnd()) {
        return it.value();
    }
    return m_emptySquare; // Fallback
}

const QPixmap& TileRenderer::getBlankTile(char letter) const {
    char upperLetter = QChar(letter).toUpper().toLatin1();
    auto it = m_blankTiles.constFind(upperLetter);
    if (it != m_blankTiles.constEnd()) {
        return it.value();
    }
    return m_emptySquare; // Fallback
}

const QPixmap& TileRenderer::getPremiumSquare(PremiumSquare type) const {
    auto it = m_premiumSquares.constFind(type);
    if (it != m_premiumSquares.constEnd()) {
        return it.value();
    }
    return m_emptySquare; // Fallback
}

const QPixmap& TileRenderer::getEmptySquare() const {
    return m_emptySquare;
}
