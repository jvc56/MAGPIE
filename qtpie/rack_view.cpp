#include "rack_view.h"
#include "tile_renderer.h"
#include <QPainter>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QDrag>
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDebug>
#include <QHBoxLayout>
#include <QRandomGenerator>
#include <QSvgRenderer>
#include <QIcon>
#include <QGuiApplication>
#include <algorithm>

RackView::RackView(QWidget *parent)
    : QWidget(parent)
{
    setAcceptDrops(true);
    // Create alphabetize button with SVG icon
    m_alphabetizeButton = new QPushButton(this);
    m_alphabetizeButton->setToolTip("Sort alphabetically");
    m_alphabetizeButton->setFixedSize(40, 40);

    // SVG for sort/alphabetize icon (bars from short to long)
    QString alphaSvg =
        "<svg viewBox='0 0 24 24' xmlns='http://www.w3.org/2000/svg'>"
        "<rect x='4' y='16' width='4' height='4' fill='white'/>"
        "<rect x='10' y='12' width='4' height='8' fill='white'/>"
        "<rect x='16' y='8' width='4' height='12' fill='white'/>"
        "</svg>";

    QPixmap alphaPixmap(24, 24);
    alphaPixmap.fill(Qt::transparent);
    QPainter alphaPainter(&alphaPixmap);
    QSvgRenderer alphaRenderer(alphaSvg.toUtf8());
    alphaRenderer.render(&alphaPainter);
    m_alphabetizeButton->setIcon(QIcon(alphaPixmap));
    m_alphabetizeButton->setIconSize(QSize(24, 24));

    m_alphabetizeButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #6496C8;"
        "  border: none;"
        "  border-radius: 20px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #5080B0;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #4070A0;"
        "}"
    );
    connect(m_alphabetizeButton, &QPushButton::clicked, this, &RackView::alphabetizeRack);

    // Create shuffle button with SVG icon
    m_shuffleButton = new QPushButton(this);
    m_shuffleButton->setToolTip("Shuffle");
    m_shuffleButton->setFixedSize(40, 40);

    // SVG for shuffle icon (crossed arrows with 20% thicker strokes)
    QString shuffleSvg =
        "<svg viewBox='0 0 24 24' xmlns='http://www.w3.org/2000/svg'>"
        "<path d='M10.59 9.17L5.41 4 4 5.41l5.17 5.17 1.42-1.41zM14.5 4l2.04 2.04L4 18.59 5.41 20 17.96 7.46 20 9.5V4h-5.5zm.33 9.41l-1.41 1.41 3.13 3.13L14.5 20H20v-5.5l-2.04 2.04-3.13-3.13z' fill='white' stroke='white' stroke-width='0.8' stroke-linejoin='round'/>"
        "</svg>";

    QPixmap shufflePixmap(24, 24);
    shufflePixmap.fill(Qt::transparent);
    QPainter shufflePainter(&shufflePixmap);
    QSvgRenderer shuffleRenderer(shuffleSvg.toUtf8());
    shuffleRenderer.render(&shufflePainter);
    m_shuffleButton->setIcon(QIcon(shufflePixmap));
    m_shuffleButton->setIconSize(QSize(24, 24));

    m_shuffleButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #6496C8;"
        "  border: none;"
        "  border-radius: 20px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #5080B0;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #4070A0;"
        "}"
    );
    connect(m_shuffleButton, &QPushButton::clicked, this, &RackView::shuffleRack);
}

void RackView::setRack(const QString& rack) {
    emit debugMessage(QString("RackView::setRack called with: '%1'").arg(rack));
    // Always maintain rack as exactly 7 characters, padding with spaces if needed
    m_rack = rack.leftJustified(7, ' ');
    emit debugMessage(QString("m_rack is now: '%1', isEmpty: %2, length: %3").arg(m_rack).arg(m_rack.isEmpty()).arg(m_rack.length()));
    update();
}

QSize RackView::sizeHint() const {
    return QSize(400, 60);
}

void RackView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);

    // Tile size is the widget height
    m_tileSize = event->size().height();

    // Position buttons
    int buttonY = (height() - 40) / 2;  // Center buttons vertically
    m_alphabetizeButton->move(10, buttonY);  // Left side with margin
    m_shuffleButton->move(width() - 50, buttonY);  // Right side with margin

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

    // Center the rack horizontally
    int startX = getStartX();

    emit debugMessage(QString("  Paint: startX=%1, tileSize=%2, tileCount=%3").arg(startX).arg(m_tileSize).arg(tileCount));

    // Create tile renderer with current tile size (using Rack style for green tiles)
    TileRenderer renderer(m_tileSize, TileRenderer::TileStyle::Rack);

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
            emit debugMessage(QString("  Skipping slot %1: char='%2' (space or invalid)").arg(i).arg(c));
            continue;  // Skip invalid characters
        }

        int x = startX + i * m_tileSize;
        emit debugMessage(QString("  Drawing slot %1: char='%2' at x=%3").arg(i).arg(c).arg(x));

        // Dim the tile if it's being dragged
        if (m_draggedTileIndex == i) {
            painter.setOpacity(0.4);
            painter.drawPixmap(x, 0, tilePixmap);
            painter.setOpacity(1.0);
        } else {
            painter.drawPixmap(x, 0, tilePixmap);
        }
    }

    // Draw drop indicator (insertion caret) if we're in a drag operation
    if (m_dropIndicatorPosition >= 0) {
            int caretX;
            if (m_dropIndicatorPosition == 0) {
                // Before first tile
                caretX = startX;
            } else if (m_dropIndicatorPosition >= tileCount) {
                // After last tile
                caretX = startX + tileCount * m_tileSize;
            } else {
                // Between tiles
                caretX = startX + m_dropIndicatorPosition * m_tileSize;
            }

            // Draw I-beam insertion indicator
            QColor caretColor(100, 150, 255);  // Blue

            // Vertical line (stem of the I)
            // Draw within widget bounds with small margin
            painter.setPen(QPen(caretColor, 3));
            int caretTop = 2;
            int caretBottom = m_tileSize - 2;
            painter.drawLine(caretX, caretTop, caretX, caretBottom);

            // Horizontal caps (top and bottom of the I)
            // Shorter caps - about half the previous size
            int capWidth = m_tileSize * 0.08;  // 8% of tile size on each side
            painter.setPen(QPen(caretColor, 3, Qt::SolidLine, Qt::FlatCap));

            // Top cap
            painter.drawLine(caretX - capWidth, caretTop, caretX + capWidth, caretTop);

            // Bottom cap
            painter.drawLine(caretX - capWidth, caretBottom, caretX + capWidth, caretBottom);
    }
}

void RackView::alphabetizeRack() {
    if (m_rack.isEmpty()) {
        return;
    }

    emit debugMessage("Alphabetizing rack");

    // Convert to uppercase for sorting, preserving case info
    QString sorted = m_rack;
    std::sort(sorted.begin(), sorted.end(), [](QChar a, QChar b) {
        // Compare uppercase versions, but if equal, lowercase comes first (blanks first)
        QChar aUpper = a.toUpper();
        QChar bUpper = b.toUpper();
        if (aUpper != bUpper) {
            return aUpper < bUpper;
        }
        // If same letter, put lowercase (blank) before uppercase
        return a.isLower() && b.isUpper();
    });

    m_rack = sorted;
    emit rackChanged(m_rack);
    update();
}

void RackView::shuffleRack() {
    if (m_rack.isEmpty()) {
        return;
    }

    emit debugMessage("Shuffling rack");

    // Fisher-Yates shuffle
    QString shuffled = m_rack;
    for (int i = shuffled.length() - 1; i > 0; --i) {
        int j = QRandomGenerator::global()->bounded(i + 1);
        QChar temp = shuffled[i];
        shuffled[i] = shuffled[j];
        shuffled[j] = temp;
    }

    m_rack = shuffled;
    emit rackChanged(m_rack);
    update();
}

int RackView::getStartX() const {
    int tileCount = qMin(m_rack.length(), 7);
    int totalWidth = tileCount * m_tileSize;
    return (width() - totalWidth) / 2;
}

int RackView::getTileIndexAtPosition(const QPoint &pos) const {
    if (m_rack.isEmpty() || m_tileSize <= 0) {
        return -1;
    }

    int startX = getStartX();
    int tileCount = qMin(m_rack.length(), 7);

    // Check if position is within rack bounds
    if (pos.y() < 0 || pos.y() > m_tileSize) {
        return -1;
    }

    for (int i = 0; i < tileCount; ++i) {
        int tileX = startX + i * m_tileSize;
        if (pos.x() >= tileX && pos.x() < tileX + m_tileSize) {
            return i;
        }
    }

    return -1;
}

void RackView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        int tileIndex = getTileIndexAtPosition(event->pos());
        if (tileIndex >= 0) {
            m_draggedTileIndex = tileIndex;
            m_dragStartPos = event->pos();
            emit debugMessage(QString("Pressed tile %1").arg(tileIndex));
        }
    }
    QWidget::mousePressEvent(event);
}

void RackView::mouseMoveEvent(QMouseEvent *event) {
    if (m_draggedTileIndex >= 0) {
        // Start dragging if moved more than a few pixels
        if ((event->pos() - m_dragStartPos).manhattanLength() > 5) {
            emit debugMessage(QString("Started dragging tile %1").arg(m_draggedTileIndex));

            // Get the tile character
            QChar c = m_rack[m_draggedTileIndex];
            m_draggedTileChar = c;

            // Immediately repaint to show the ghost tile
            update();
            QCoreApplication::processEvents();  // Force immediate repaint

            // Create tile renderer and get the tile pixmap (using Rack style for green tiles)
            TileRenderer renderer(m_tileSize, TileRenderer::TileStyle::Rack);
            QPixmap tilePixmap;

            if (c == '?') {
                tilePixmap = renderer.getBlankTile('A');
            } else if (c.isLower() && c >= 'a' && c <= 'z') {
                tilePixmap = renderer.getBlankTile(c.toLatin1());
            } else if (c.isUpper() && c >= 'A' && c <= 'Z') {
                tilePixmap = renderer.getLetterTile(c.toLatin1());
            }

            // Start Qt drag operation
            QDrag *drag = new QDrag(this);
            QMimeData *mimeData = new QMimeData;

            // Store the tile index and character
            mimeData->setText(QString("%1:%2").arg(m_draggedTileIndex).arg(c));
            drag->setMimeData(mimeData);
            // Use a 1x1 transparent pixmap to hide the default drag image and tooltip
            QPixmap invisiblePixmap(1, 1);
            invisiblePixmap.fill(Qt::transparent);
            drag->setPixmap(invisiblePixmap);
            drag->setHotSpot(QPoint(0, 0));

            emit debugMessage("Starting drag with MoveAction | IgnoreAction");

            // Execute the drag - support both Move and Ignore actions
            // Use IgnoreAction as default to disable snap-back animation on rejected drops
            Qt::DropAction dropAction = drag->exec(Qt::MoveAction | Qt::IgnoreAction, Qt::IgnoreAction);

            QString actionName;
            if (dropAction == Qt::MoveAction) actionName = "MoveAction";
            else if (dropAction == Qt::IgnoreAction) actionName = "IgnoreAction";
            else if (dropAction == Qt::CopyAction) actionName = "CopyAction";
            else actionName = QString("Unknown(%1)").arg((int)dropAction);

            emit debugMessage(QString("Drag ended with action: %1").arg(actionName));

            // Notify that drag ended so preview can be hidden or animated
            emit dragEnded(dropAction);

            // Reset drag state and clean up any override cursor
            m_draggedTileIndex = -1;
            while (QGuiApplication::overrideCursor()) {
                QGuiApplication::restoreOverrideCursor();
            }
            update();

            return; // Don't call parent handler
        }
    }
    QWidget::mouseMoveEvent(event);
}

void RackView::mouseReleaseEvent(QMouseEvent *event) {
    // Reset drag state
    m_draggedTileIndex = -1;
    m_dropIndicatorPosition = -1;
    update();

    QWidget::mouseReleaseEvent(event);
}

void RackView::dragEnterEvent(QDragEnterEvent *event) {
    // Accept drags from ourselves or from board
    if (event->mimeData()->hasText()) {
        QString mimeText = event->mimeData()->text();

        // Check if this is a rack drag or board drag
        if (event->source() == this || mimeText.startsWith("board:")) {
            emit debugMessage("dragEnter - accepting with MoveAction, restoring cursor");
            event->setDropAction(Qt::MoveAction);
            event->accept();

            // Restore cursor when re-entering the rack
            while (QGuiApplication::overrideCursor()) {
                QGuiApplication::restoreOverrideCursor();
            }

            // Clear any existing drop indicator when entering from board
            // (board drags don't show carets)
            if (mimeText.startsWith("board:") && m_dropIndicatorPosition != -1) {
                m_dropIndicatorPosition = -1;
                update();
            }
        }
    }
}

void RackView::dragMoveEvent(QDragMoveEvent *event) {
    // Check if this is a board-to-rack drag
    QString mimeText = event->mimeData()->text();
    bool isBoardDrag = mimeText.startsWith("board:");

    if (event->source() != this && !isBoardDrag) {
        event->setDropAction(Qt::IgnoreAction);
        event->accept();
        return;
    }

    // Check if the drag is within the rack's bounds (both horizontal and vertical)
    bool inRackArea = (event->position().y() >= 0 && event->position().y() <= height() &&
                       event->position().x() >= 0 && event->position().x() <= width());

    // Always emit drag position for preview overlay (even when outside rack)
    emit dragPositionChanged(mapToParent(event->position().toPoint()), m_draggedTileChar);

    if (!inRackArea) {
        // Outside rack area - set action to Ignore (forbidden cursor will show)
        event->setDropAction(Qt::IgnoreAction);
        event->accept();

        if (m_dropIndicatorPosition != -1) {
            m_dropIndicatorPosition = -1;
            update();
        }
        return;
    }

    // Inside rack area - accept the drop with MoveAction
    event->setDropAction(Qt::MoveAction);
    event->accept();

    // Calculate what the new rack order would be if we dropped here
    // Based on sorting by X coordinate of tile centers
    int startX = getStartX();
    int tileCount = qMin(m_rack.length(), 7);
    int dragX = event->position().x();

    if (isBoardDrag) {
        // Board-to-rack: Don't show caret for board drags (we're filling slots, not inserting)
        if (m_dropIndicatorPosition != -1) {
            m_dropIndicatorPosition = -1;
            update();
        }
    } else {
        // Rack-to-rack: Calculate what position the dragged tile would be in after sorting by X
        // Find which slot we're hovering over
        int dropIndex = -1;
        for (int i = 0; i < tileCount; ++i) {
            int tileX = startX + i * m_tileSize;
            int tileEndX = tileX + m_tileSize;
            if (dragX >= tileX && dragX < tileEndX) {
                dropIndex = i;
                break;
            }
        }

        // Check if the drop spot is empty
        bool dropOnEmpty = (dropIndex >= 0 && dropIndex < m_rack.length() && m_rack[dropIndex] == ' ');

        if (dropOnEmpty) {
            // Dropping on an empty spot - no tiles will be pushed, so no caret
            if (m_dropIndicatorPosition != -1) {
                m_dropIndicatorPosition = -1;
                emit debugMessage("Hiding caret - dropping on empty spot");
                update();
            }
        } else {
            // Dropping on a tile - calculate new position based on sort order
            int newPosition = m_draggedTileIndex;
            for (int i = 0; i < tileCount; ++i) {
                if (i == m_draggedTileIndex) continue;

                int tileCenterX = startX + i * m_tileSize + m_tileSize / 2;

                if (dragX < tileCenterX && i < m_draggedTileIndex) {
                    // Dragged tile is left of this tile's center, and this tile is currently before us
                    newPosition = i;
                    break;
                } else if (dragX > tileCenterX && i > m_draggedTileIndex) {
                    // Dragged tile is right of this tile's center, and this tile is currently after us
                    newPosition = i;
                }
            }

            emit debugMessage(QString("dragX=%1, draggedTile=%2, newPosition=%3")
                             .arg(dragX).arg(m_draggedTileIndex).arg(newPosition));

            // Show caret only if the ordering would change
            if (newPosition != m_draggedTileIndex) {
                // Calculate caret position based on where tile would be inserted
                int caretPos = (newPosition < m_draggedTileIndex) ? newPosition : newPosition + 1;

                if (m_dropIndicatorPosition != caretPos) {
                    m_dropIndicatorPosition = caretPos;
                    emit debugMessage(QString("Showing caret at position: %1").arg(caretPos));
                    update();
                }
            } else {
                // No change in ordering - hide caret
                if (m_dropIndicatorPosition != -1) {
                    m_dropIndicatorPosition = -1;
                    emit debugMessage("Hiding caret - no position change");
                    update();
                }
            }
        }
    }
}

void RackView::dropEvent(QDropEvent *event) {
    QString text = event->mimeData()->text();
    QStringList parts = text.split(':');

    // Handle board-to-rack drops
    if (text.startsWith("board:")) {
        // Format: "board:row:col:char"
        if (parts.size() >= 4) {
            int srcRow = parts[1].toInt();
            int srcCol = parts[2].toInt();
            QChar draggedChar = parts[3][0];

            // Find which tile position was dropped on based on X coordinate
            int startX = getStartX();
            int tileCount = qMin(m_rack.length(), 7);
            int dragX = event->position().x();

            emit debugMessage(QString("Board-to-rack drop: dragX=%1, startX=%2, tileSize=%3, tileCount=%4")
                            .arg(dragX).arg(startX).arg(m_tileSize).arg(tileCount));
            emit debugMessage(QString("  Current rack before drop: '%1' (length=%2)").arg(m_rack).arg(m_rack.length()));

            int dropIndex = -1;
            for (int i = 0; i < tileCount; ++i) {
                int tileX = startX + i * m_tileSize;
                int tileEndX = tileX + m_tileSize;
                emit debugMessage(QString("  Slot %1: tileX=%2 to %3, char='%4'")
                                .arg(i).arg(tileX).arg(tileEndX).arg(m_rack[i]));
                if (dragX >= tileX && dragX < tileEndX) {
                    dropIndex = i;
                    emit debugMessage(QString("  -> Drop hit slot %1!").arg(i));
                    break;
                }
            }

            if (dropIndex >= 0 && dropIndex < m_rack.length()) {
                QChar oldChar = m_rack[dropIndex];
                // Replace the tile at this position (could be empty or another tile)
                m_rack[dropIndex] = draggedChar;
                emit rackChanged(m_rack);
                emit debugMessage(QString("Returned tile '%1' from board to rack position %2 (was '%3')")
                                .arg(draggedChar).arg(dropIndex).arg(oldChar));
                emit debugMessage(QString("  Rack after drop: '%1'").arg(m_rack));
            } else {
                emit debugMessage(QString("Drop outside valid rack positions (dragX=%1, dropIndex=%2)").arg(dragX).arg(dropIndex));
            }

            event->setDropAction(Qt::MoveAction);
            event->accept();

            // Notify that we need to remove the tile from the board
            emit boardTileReturned(srcRow, srcCol);

            // Emit dragEnded so the preview gets hidden
            emit dragEnded(Qt::MoveAction);
        }

        // Clear drop indicator (in case it was set during drag)
        m_dropIndicatorPosition = -1;
        update();
        return;
    }

    // Handle rack-to-rack reordering
    if (event->source() != this) {
        return;
    }

    if (parts.size() != 2) {
        return;
    }

    int sourceIndex = parts[0].toInt();
    QChar draggedChar = parts[1][0];

    int startX = getStartX();
    int tileCount = qMin(m_rack.length(), 7);
    int dragX = event->position().x();

    emit debugMessage(QString("Dropped tile %1 ('%2') at x=%3")
                     .arg(sourceIndex)
                     .arg(draggedChar)
                     .arg(dragX));

    // Use the same sorting logic as dragMoveEvent
    int newPosition = sourceIndex;
    for (int i = 0; i < tileCount; ++i) {
        if (i == sourceIndex) continue;

        int tileCenterX = startX + i * m_tileSize + m_tileSize / 2;

        if (dragX < tileCenterX && i < sourceIndex) {
            newPosition = i;
            break;
        } else if (dragX > tileCenterX && i > sourceIndex) {
            newPosition = i;
        }
    }

    if (newPosition != sourceIndex) {
        // Reorder the rack
        QString newRack = m_rack;
        newRack.remove(sourceIndex, 1);
        newRack.insert(newPosition, draggedChar);

        m_rack = newRack;
        emit rackChanged(m_rack);
        emit debugMessage(QString("Moved tile from %1 to %2").arg(sourceIndex).arg(newPosition));
    } else {
        emit debugMessage("Tile dropped in same position");
    }

    event->acceptProposedAction();

    // Clear drop indicator
    m_dropIndicatorPosition = -1;
    update();
}

void RackView::dragLeaveEvent(QDragLeaveEvent *event) {
    // Clear drop indicator when drag leaves the widget
    emit debugMessage("dragLeave - left the widget");
    m_dropIndicatorPosition = -1;
    update();
    QWidget::dragLeaveEvent(event);
}

void RackView::removeTileAtIndex(int index) {
    if (index >= 0 && index < m_rack.length()) {
        // Replace tile with space to leave a gap instead of removing
        m_rack[index] = ' ';
        emit rackChanged(m_rack);
        update();
    }
}
