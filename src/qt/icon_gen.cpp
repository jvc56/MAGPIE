#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QFontDatabase>
#include <QDir>
#include <QDebug>
#include <QStandardPaths>

void drawIcon(int size, const QString &outputPath, const QString &fontFamily) {
    QImage image(size, size, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    double cellSize = size;
    // double margin = 0; // No margin for the icon itself, fill the space? 
    // Usually icons have a slight padding, but for a "tile" style, filling closely is good. 
    // Let's stick to the QML style: margin is small or handled by the radius.
    
    QRectF rect(0, 0, cellSize, cellSize);
    
    // Scale radius. In QML it was 6 for a ~60px cell.
    double radius = cellSize * 0.15; // 10% radius is standardish for macOS rounded squares, but let's match the "tile" look.

    // Draw Background
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#F9E2AF"));
    p.drawRoundedRect(rect, radius, radius);

    // Draw Letter M
    QFont font(fontFamily);
    font.setPixelSize(cellSize * 0.55); // Slightly larger than 0.5 to fill space well
    font.setWeight(QFont::Black); // 900
    p.setFont(font);
    p.setPen(QColor("#1E1E2E"));

    // Center align
    p.drawText(rect, Qt::AlignCenter, "M");

    // Draw Score 3
    QFont scoreFont(fontFamily);
    scoreFont.setPixelSize(cellSize * 0.25);
    scoreFont.setWeight(QFont::Bold);
    p.setFont(scoreFont);
    
    // Bottom right positioning
    // Using simple text flags for alignment
    // Adjust rect for padding
    double padding = cellSize * 0.1;
    QRectF scoreRect = rect.adjusted(0, 0, -padding, -padding * 0.5);
    p.drawText(scoreRect, Qt::AlignBottom | Qt::AlignRight, "3");

    p.end();
    image.save(outputPath);
}

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);

    if (argc < 2) {
        qWarning() << "Usage: icon_gen <path_to_font>";
        return 1;
    }

    QString fontPath = argv[1];
    int fontId = QFontDatabase::addApplicationFont(fontPath);
    if (fontId == -1) {
        qWarning() << "Failed to load font from" << fontPath;
        return 1;
    }
    QString fontFamily = QFontDatabase::applicationFontFamilies(fontId).at(0);

    QDir().mkdir("AppIcon.iconset");

    struct Size { int size; QString name; };
    Size sizes[] = {
        {16, "icon_16x16.png"},
        {32, "icon_16x16@2x.png"},
        {32, "icon_32x32.png"},
        {64, "icon_32x32@2x.png"},
        {128, "icon_128x128.png"},
        {256, "icon_128x128@2x.png"},
        {256, "icon_256x256.png"},
        {512, "icon_256x256@2x.png"},
        {512, "icon_512x512.png"},
        {1024, "icon_512x512@2x.png"}
    };

    for (const auto &s : sizes) {
        drawIcon(s.size, "AppIcon.iconset/" + s.name, fontFamily);
    }

    return 0;
}
