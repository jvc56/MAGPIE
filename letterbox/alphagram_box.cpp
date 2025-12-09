#include "alphagram_box.h"
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>
#include <QGraphicsDropShadowEffect>
#include <QMouseEvent>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextTable>
#include <QTextFrame>
#include <QRegularExpression>
#include <QTimer>
#include <QDebug>

AlphagramBox::AlphagramBox(QWidget *parent)
    : QWidget(parent), tableLabel(nullptr),
      hasAnyFrontHooks(false), hasAnyBackHooks(false)
{
    layout = new QVBoxLayout(this);
    // Add margins to keep table content inside the painted border
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(0);
    setLayout(layout);

    // TEMPORARILY DISABLED: Drop shadow effect interferes with mouse tracking
    // QGraphicsDropShadowEffect* shadow = new QGraphicsDropShadowEffect(this);
    // shadow->setBlurRadius(8);
    // shadow->setXOffset(0);
    // shadow->setYOffset(2);
    // shadow->setColor(QColor(0, 0, 0, 120));
    // setGraphicsEffect(shadow);

    // Enable mouse tracking for hover events
    setMouseTracking(true);
}

void AlphagramBox::addWord(const QString& word, const QString& frontHooks, const QString& backHooks,
                           const QString& frontExtensions, const QString& backExtensions,
                           bool isPlaceholder, bool isMissed, int computeTimeMicros)
{
    words.push_back({word, frontHooks, backHooks, frontExtensions, backExtensions, isPlaceholder, isMissed, computeTimeMicros});
}

void AlphagramBox::finalize(int wordSize, int hookSize, int extensionSize, bool showComputeTime)
{
    if (words.empty()) {
        return;
    }

    // Check if any word has hooks or extensions
    hasAnyFrontHooks = false;
    hasAnyBackHooks = false;
    for (const auto& wordData : words) {
        if (!wordData.frontHooks.isEmpty() || !wordData.frontExtensions.isEmpty()) hasAnyFrontHooks = true;
        if (!wordData.backHooks.isEmpty() || !wordData.backExtensions.isEmpty()) hasAnyBackHooks = true;
    }

    // Create a single table for all words
    tableLabel = new QLabel(this);
    tableLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);  // Let mouse events pass through to parent

    QString html = "<table style='width: 100%; border-collapse: collapse;'>";

    for (const auto& wordData : words) {
        html += "<tr>";

        // Always add front hooks cell if any word in the group has front hooks
        if (hasAnyFrontHooks) {
            QString cellContent;

            // Add spaces between hook letters
            QString spacedFrontHooks = wordData.frontHooks;
            if (!spacedFrontHooks.isEmpty()) {
                QString spaced;
                for (int i = 0; i < spacedFrontHooks.length(); i++) {
                    if (i > 0) spaced += " ";
                    spaced += spacedFrontHooks[i];
                }
                spacedFrontHooks = spaced;
            }

            // Build content: hooks on top, extensions below
            // TODO: Add tooltip with full word for each hook
            cellContent = QString("<div style='font-size: %1px; font-weight: 500;'>%2</div>").arg(hookSize).arg(spacedFrontHooks);

            // Add extensions (one line per length)
            if (!wordData.frontExtensions.isEmpty()) {
                QStringList extLines = wordData.frontExtensions.split('\n', Qt::SkipEmptyParts);
                for (const QString& line : extLines) {
                    cellContent += QString("<div style='font-size: %1px; font-weight: 400;'>%2</div>").arg(extensionSize).arg(line);
                }
            }

            html += QString("<td style='font-family: \"Jost\", sans-serif; color: #fff; padding: 8px 4px; text-align: right; border-right: 1px solid #666; vertical-align: top;'>%1</td>")
                    .arg(cellContent);
        }

        // Word (Jost Semibold for revealed, Regular gray for placeholders, lighter gray for missed)
        QString wordBorder = hasAnyBackHooks ? "border-right: 1px solid #666;" : "";
        QString color;
        int fontWeight;

        if (wordData.isMissed) {
            color = "rgb(196, 196, 196)";  // Lighter gray for missed words
            fontWeight = 600;
        } else if (wordData.isPlaceholder) {
            color = "#888";
            fontWeight = 400;
        } else {
            color = "#fff";
            fontWeight = 600;
        }

        QString wordCellContent = wordData.word;

        // Add red X emoji for missed words
        if (wordData.isMissed) {
            wordCellContent += " ❌";
        }

        if (showComputeTime && !wordData.isPlaceholder && wordData.computeTimeMicros > 0) {
            wordCellContent += QString("<div style='font-size: 10px; color: #666; margin-top: 2px;'>%1μs</div>")
                    .arg(wordData.computeTimeMicros);
        }

        html += QString("<td style='font-family: \"Jost\", sans-serif; font-size: %1px; font-weight: %2; letter-spacing: 1px; color: %3; text-align: center; padding: 8px 4px; %4'>%5</td>")
                .arg(wordSize)
                .arg(fontWeight)
                .arg(color)
                .arg(wordBorder)
                .arg(wordCellContent);

        // Always add back hooks cell if any word in the group has back hooks
        if (hasAnyBackHooks) {
            QString cellContent;

            // Add spaces between hook letters
            QString spacedBackHooks = wordData.backHooks;
            if (!spacedBackHooks.isEmpty()) {
                QString spaced;
                for (int i = 0; i < spacedBackHooks.length(); i++) {
                    if (i > 0) spaced += " ";
                    spaced += spacedBackHooks[i];
                }
                spacedBackHooks = spaced;
            }

            // Build content: hooks on top, extensions below
            cellContent = QString("<div style='font-size: %1px; font-weight: 500;'>%2</div>").arg(hookSize).arg(spacedBackHooks);

            // Add extensions (one line per length)
            if (!wordData.backExtensions.isEmpty()) {
                QStringList extLines = wordData.backExtensions.split('\n', Qt::SkipEmptyParts);
                for (const QString& line : extLines) {
                    cellContent += QString("<div style='font-size: %1px; font-weight: 400;'>%2</div>").arg(extensionSize).arg(line);
                }
            }

            html += QString("<td style='font-family: \"Jost\", sans-serif; color: #fff; padding: 8px 4px; text-align: left; vertical-align: top;'>%1</td>")
                    .arg(cellContent);
        }

        html += "</tr>";
    }

    html += "</table>";

    tableLabel->setText(html);
    tableLabel->setAlignment(Qt::AlignCenter);
    tableLabel->setTextFormat(Qt::RichText);

    layout->addWidget(tableLabel);

    // Calculate bounding boxes after the label is added and sized
    QTimer::singleShot(0, this, [this]() {
        calculateBoundingBoxes();
    });
}

void AlphagramBox::clear()
{
    if (tableLabel) {
        layout->removeWidget(tableLabel);
        delete tableLabel;
        tableLabel = nullptr;
    }
    words.clear();
    boundingBoxes.clear();
}

void AlphagramBox::calculateBoundingBoxes()
{
    boundingBoxes.clear();

    if (!tableLabel || words.empty()) {
        return;
    }

    // Get the QLabel's actual document for accurate positioning
    // QLabel internally uses a QTextDocument for rich text rendering
    QTextDocument* doc = nullptr;

    // Access the label's internal document if it's using rich text
    if (tableLabel->textFormat() == Qt::RichText) {
        // We need to create our own document that matches the label's rendering
        doc = new QTextDocument();
        doc->setHtml(tableLabel->text());
        doc->setDefaultFont(tableLabel->font());
        doc->setTextWidth(tableLabel->width());
        doc->setDocumentMargin(0);
    } else {
        qDebug() << "calculateBoundingBoxes: label is not using RichText format!";
        return;
    }

    // Get the tableLabel's position within the widget
    QPoint labelOffset = tableLabel->pos();

    // Parse the document structure to find text positions
    // Move cursor to the beginning and search for the table
    QTextCursor cursor(doc);
    cursor.movePosition(QTextCursor::Start);

    // Find the table by iterating through the document
    QTextTable* table = nullptr;
    QTextFrame* rootFrame = doc->rootFrame();
    for (QTextFrame::iterator it = rootFrame->begin(); !it.atEnd(); ++it) {
        QTextFrame* childFrame = it.currentFrame();
        if (childFrame) {
            table = qobject_cast<QTextTable*>(childFrame);
            if (table) {
                break;
            }
        }
    }

    if (!table) {
        qDebug() << "calculateBoundingBoxes: no table found in document!";
        qDebug() << "  HTML:" << tableLabel->text().left(200);
        delete doc;
        return;
    }

    qDebug() << "calculateBoundingBoxes: found table with" << table->rows() << "rows," << table->columns() << "cols, processing" << words.size() << "words";
    qDebug() << "  labelOffset:" << labelOffset << "labelSize:" << tableLabel->size() << "docWidth:" << doc->textWidth();

    for (size_t row = 0; row < words.size() && row < static_cast<size_t>(table->rows()); row++) {
        const WordData& wordData = words[row];

        // Process front hooks/extensions (column 0 if present)
        int frontCol = -1;
        int wordCol = -1;
        int backCol = -1;

        if (hasAnyFrontHooks && hasAnyBackHooks) {
            frontCol = 0;
            wordCol = 1;
            backCol = 2;
        } else if (hasAnyFrontHooks) {
            frontCol = 0;
            wordCol = 1;
        } else if (hasAnyBackHooks) {
            wordCol = 0;
            backCol = 1;
        } else {
            wordCol = 0;
        }

        // Process main word column
        if (wordCol >= 0) {
            QTextTableCell cell = table->cellAt(row, wordCol);
            if (cell.isValid()) {
                processCellForBoundingBoxes(*doc, table, cell, wordData, row, false, labelOffset, true);
            }
        }

        // Process front column (only if this word actually has front hooks/extensions)
        if (frontCol >= 0 && (!wordData.frontHooks.isEmpty() || !wordData.frontExtensions.isEmpty())) {
            QTextTableCell cell = table->cellAt(row, frontCol);
            if (cell.isValid()) {
                processCellForBoundingBoxes(*doc, table, cell, wordData, row, true, labelOffset, false);
            }
        }

        // Process back column (only if this word actually has back hooks/extensions)
        if (backCol >= 0 && (!wordData.backHooks.isEmpty() || !wordData.backExtensions.isEmpty())) {
            QTextTableCell cell = table->cellAt(row, backCol);
            if (cell.isValid()) {
                processCellForBoundingBoxes(*doc, table, cell, wordData, row, false, labelOffset, false);
            }
        }
    }

    // Debug: output bounding box count and details
    qDebug() << "calculateBoundingBoxes: calculated" << boundingBoxes.size() << "boxes";
    for (size_t i = 0; i < boundingBoxes.size() && i < 10; i++) {
        const auto& bbox = boundingBoxes[i];
        qDebug() << "  bbox[" << i << "]:" << bbox.text << "->" << bbox.fullWord
                 << "rect=(" << bbox.rect.x() << "," << bbox.rect.y() << ","
                 << bbox.rect.width() << "x" << bbox.rect.height() << ")";
    }

    delete doc;
}

void AlphagramBox::processCellForBoundingBoxes(QTextDocument& doc, QTextTable* table, QTextTableCell& cell,
                                                const WordData& wordData, int row,
                                                bool isFront, const QPoint& labelOffset, bool isMainWord)
{
    qDebug() << "processCellForBoundingBoxes: row=" << row << "col=" << cell.column() << "isFront=" << isFront << "isMainWord=" << isMainWord << "wordData.word=" << wordData.word;

    // Get cell's bounding rect
    QTextCursor cellCursor = cell.firstCursorPosition();
    QRectF cellRect = doc.documentLayout()->blockBoundingRect(cellCursor.block());

    // Get all the text blocks (divs) in this cell
    QTextBlock block = cellCursor.block();
    int cellStart = cell.firstPosition();
    int cellEnd = cell.lastPosition();

    qDebug() << "  Starting block iteration, block.isValid()=" << block.isValid() << "cellStart=" << cellStart << "cellEnd=" << cellEnd;
    int blockCount = 0;
    while (block.isValid() && block.position() >= cellStart && block.position() < cellEnd) {
        blockCount++;
        QString blockText = block.text().trimmed();
        qDebug() << "  Block" << blockCount << "text:" << blockText;

        if (!blockText.isEmpty()) {
            QRectF blockRect = doc.documentLayout()->blockBoundingRect(block);

            qDebug() << "  blockRect:" << blockRect << "labelOffset:" << labelOffset;

            // Split by spaces to get individual words/extensions
            QStringList words = blockText.split(' ', Qt::SkipEmptyParts);

            // Use font metrics to calculate individual word positions
            QFont font = block.charFormat().font();
            if (font.family().isEmpty()) {
                font = doc.defaultFont();
            }
            QFontMetrics fm(font);

            // Check if text is right-aligned (front hooks/extensions)
            // For right-aligned text, we need to start from the right edge and work backwards
            Qt::Alignment alignment = block.blockFormat().alignment();
            bool isRightAligned = (alignment & Qt::AlignRight) || isFront;

            // Calculate total width of all text in this block
            int totalTextWidth = fm.horizontalAdvance(blockText);

            qDebug() << "  alignment:" << alignment << "isRightAligned:" << isRightAligned << "totalTextWidth:" << totalTextWidth;

            // Calculate x offset based on cell alignment
            qreal currentX;
            if (isRightAligned) {
                // Start from right edge of block minus total text width
                currentX = blockRect.right() - totalTextWidth;
            } else {
                // Start from left edge of block
                currentX = blockRect.x();
            }

            qDebug() << "  currentX:" << currentX;

            // For each word in the block, calculate its individual bounding box
            for (int i = 0; i < words.size(); i++) {
                QString word = words[i];
                QString cleanWord = word;
                cleanWord.remove("…");

                if (cleanWord.isEmpty()) {
                    continue;
                }

                // Skip if this is a hook/extension cell and the word matches the main word
                // (The main word appears in hook/extension cells but shouldn't be clickable there)
                if (!isMainWord && cleanWord.compare(wordData.word, Qt::CaseInsensitive) == 0) {
                    currentX += fm.horizontalAdvance((i < words.size() - 1) ? word + " " : word);
                    continue;
                }

                // Calculate width for this specific word (including the space after it)
                QString wordWithSpace = (i < words.size() - 1) ? word + " " : word;
                int wordWidth = fm.horizontalAdvance(wordWithSpace);
                int cleanWordWidth = fm.horizontalAdvance(cleanWord);

                TextBoundingBox bbox;
                bbox.rect = QRectF(
                    currentX + labelOffset.x(),
                    blockRect.y() + labelOffset.y(),
                    cleanWordWidth,
                    blockRect.height()
                );
                bbox.text = cleanWord;
                bbox.isMainWord = isMainWord;

                // For main words, fullWord is just the word itself
                // For hooks/extensions, combine with the base word
                if (isMainWord) {
                    bbox.fullWord = cleanWord.toUpper();
                } else {
                    bbox.fullWord = isFront ?
                        cleanWord.toUpper() + wordData.word.toUpper() :
                        wordData.word.toUpper() + cleanWord.toUpper();
                }

                bbox.row = row;
                bbox.isFront = isFront;
                boundingBoxes.push_back(bbox);

                // Move x position for next word
                currentX += wordWidth;
            }
        }

        block = block.next();
    }
}

void AlphagramBox::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Fill background - dark gray (50, 50, 50)
    painter.fillRect(rect(), QColor(50, 50, 50));

    // Draw border - lighter gray (90, 90, 90)
    QPen pen(QColor(90, 90, 90), 1);
    painter.setPen(pen);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));

    // Draw debug overlays if enabled
    if (showHoverDebug && !boundingBoxes.empty()) {
        QList<QColor> colors = {
            QColor(255, 0, 0, 80),     // Red
            QColor(0, 255, 0, 80),     // Green
            QColor(0, 0, 255, 80),     // Blue
            QColor(255, 255, 0, 80),   // Yellow
            QColor(255, 0, 255, 80),   // Magenta
            QColor(0, 255, 255, 80),   // Cyan
        };

        for (size_t i = 0; i < boundingBoxes.size(); i++) {
            const TextBoundingBox& bbox = boundingBoxes[i];
            QColor color = colors[i % colors.size()];

            // Highlight the hovered box differently
            if (static_cast<int>(i) == hoveredBoxIndex) {
                color.setAlpha(150);  // Make it more opaque
            }

            painter.fillRect(bbox.rect, color);
            painter.setPen(Qt::white);
            painter.drawRect(bbox.rect);
        }
    }
}

void AlphagramBox::mouseMoveEvent(QMouseEvent *event)
{
    // Always emit debug to show we're receiving events
    emit hoverDebug(QString("mouseMoveEvent: pos(%1,%2) hasLabel=%3 bboxCount=%4")
                    .arg(event->pos().x())
                    .arg(event->pos().y())
                    .arg(tableLabel != nullptr ? "yes" : "no")
                    .arg(boundingBoxes.size()));

    if (!tableLabel) {
        return;
    }

    updateHover(event->pos());
}

void AlphagramBox::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    emit hoverLeft();
}

void AlphagramBox::updateHover(const QPoint& pos)
{
    // Simple bounding-box based hover detection
    hoveredBoxIndex = -1;

    if (boundingBoxes.empty()) {
        emit hoverLeft();
        return;
    }

    // Check which bounding box contains the mouse
    for (size_t i = 0; i < boundingBoxes.size(); i++) {
        if (boundingBoxes[i].rect.contains(pos)) {
            hoveredBoxIndex = i;
            const TextBoundingBox& bbox = boundingBoxes[i];

            emit hoverDebug(QString("bbox[%1]: '%2' -> '%3' row=%4 front=%5 main=%6 pos=(%7,%8)")
                .arg(i)
                .arg(bbox.text)
                .arg(bbox.fullWord)
                .arg(bbox.row)
                .arg(bbox.isFront ? "yes" : "no")
                .arg(bbox.isMainWord ? "yes" : "no")
                .arg(pos.x())
                .arg(pos.y()));

            bool alignLeft = bbox.isFront;
            bool isHookOrExtension = !bbox.isMainWord;
            emit wordHovered(bbox.fullWord, alignLeft, isHookOrExtension);
            update();  // Trigger repaint to show highlighted box
            return;
        }
    }

    // No box contains mouse
    emit hoverDebug(QString("No bbox hit: pos=(%1,%2) totalBoxes=%3")
                    .arg(pos.x())
                    .arg(pos.y())
                    .arg(boundingBoxes.size()));
    emit hoverLeft();
    update();
}
