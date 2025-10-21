#include "rack_view.h"
#include "tile_renderer.h"
#include <QPainter>
#include <QResizeEvent>
#include <QDebug>

RackView::RackView(QWidget *parent)
    : QWidget(parent)
{
}

void RackView::setRack(const QString& rack) {
    emit debugMessage(QString("RackView::setRack called with: '%1'").arg(rack));
    m_rack = rack;
    emit debugMessage(QString("m_rack is now: '%1', isEmpty: %2").arg(m_rack).arg(m_rack.isEmpty()));
    update();
}

QSize RackView::sizeHint() const {
    return QSize(400, 60);
}

void RackView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);

    // Tile size is the widget height
    m_tileSize = event->size().height();

    update();
}

void RackView::paintEvent(QPaintEvent *) {
    emit debugMessage(QString("RackView::paintEvent - m_rack: '%1', m_tileSize: %2").arg(m_rack).arg(m_tileSize));

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    // Fill background (matching board background)
    painter.fillRect(rect(), QColor(230, 230, 240));

    if (m_rack.isEmpty() || m_tileSize <= 0) {
        // Show "Empty rack" text
        QFont font("Arial", 12);
        painter.setFont(font);
        painter.setPen(QColor(120, 120, 120));  // Gray text
        painter.drawText(rect(), Qt::AlignCenter, "Empty rack");
        emit debugMessage("Showing empty rack text");
        return;
    }

    emit debugMessage(QString("Drawing %1 tiles").arg(m_rack.length()));

    // Calculate number of tiles (max 7)
    int tileCount = qMin(m_rack.length(), 7);

    // Calculate total width needed
    int totalWidth = tileCount * m_tileSize;

    // Center the rack horizontally
    int startX = (width() - totalWidth) / 2;

    // Create tile renderer with current tile size
    TileRenderer renderer(m_tileSize);

    // Draw each tile
    for (int i = 0; i < tileCount; ++i) {
        QChar c = m_rack[i];

        QPixmap tilePixmap;
        if (c == '?') {
            // Blank tile (use any letter, e.g. 'A')
            tilePixmap = renderer.getBlankTile('A');
        } else if (c.isLower() && c >= 'a' && c <= 'z') {
            // Lowercase = blank tile showing this letter
            tilePixmap = renderer.getBlankTile(c.toLatin1());
        } else if (c.isUpper() && c >= 'A' && c <= 'Z') {
            // Regular letter tile
            tilePixmap = renderer.getLetterTile(c.toLatin1());
        } else {
            continue;  // Skip invalid characters
        }

        int x = startX + i * m_tileSize;
        painter.drawPixmap(x, 0, tilePixmap);
    }
}
