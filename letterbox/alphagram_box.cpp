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
}

void AlphagramBox::clear()
{
    if (tableLabel) {
        layout->removeWidget(tableLabel);
        delete tableLabel;
        tableLabel = nullptr;
    }
    words.clear();
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
}

void AlphagramBox::mouseMoveEvent(QMouseEvent *event)
{
    // Always emit debug to show we're receiving events
    emit hoverDebug(QString("mouseMoveEvent: pos(%1,%2) hasLabel=%3")
                    .arg(event->pos().x())
                    .arg(event->pos().y())
                    .arg(tableLabel != nullptr ? "yes" : "no"));

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
    if (words.empty() || !tableLabel) {
        emit hoverDebug("No words or label");
        return;
    }

    // Get the table label's geometry
    QRect labelRect = tableLabel->geometry();

    // Check if mouse is within the label
    if (!labelRect.contains(pos)) {
        emit hoverLeft();
        return;
    }

    // Calculate relative position within the label
    QPoint relPos = pos - labelRect.topLeft();

    // Create a QTextDocument from the label's HTML to do hit-testing
    QTextDocument doc;
    doc.setHtml(tableLabel->text());
    doc.setTextWidth(tableLabel->width());

    // Hit test to find cursor position at mouse coordinates
    int cursorPos = doc.documentLayout()->hitTest(relPos, Qt::FuzzyHit);

    if (cursorPos < 0) {
        emit hoverLeft();
        return;
    }

    // Get the cursor and find which table cell we're in
    QTextCursor cursor(&doc);
    cursor.setPosition(cursorPos);

    // Get the table at this position
    QTextTable* table = cursor.currentTable();
    if (!table) {
        emit hoverDebug("Not in table");
        emit hoverLeft();
        return;
    }

    // Get the cell at the cursor position
    QTextTableCell cell = table->cellAt(cursor);
    if (!cell.isValid()) {
        emit hoverDebug("Invalid cell");
        emit hoverLeft();
        return;
    }

    int row = cell.row();
    int col = cell.column();
    int numCols = table->columns();

    // Get cell content
    QTextCursor cellCursor = cell.firstCursorPosition();
    cellCursor.movePosition(QTextCursor::NextCell, QTextCursor::KeepAnchor);
    QString cellText = cellCursor.selectedText().trimmed();

    // Get the word/text block under the cursor for hook/extension detection
    cursor.setPosition(cursorPos);
    QString wordAtPos;
    if (!cursor.atEnd()) {
        int savedPos = cursor.position();
        cursor.select(QTextCursor::WordUnderCursor);
        wordAtPos = cursor.selectedText().trimmed();
        cursor.setPosition(savedPos);
    }

    // Find which word this row belongs to
    const WordData* matchedWord = nullptr;
    if (row < static_cast<int>(words.size())) {
        matchedWord = &words[row];
    }

    if (!matchedWord) {
        emit hoverDebug(QString("No word for row %1").arg(row));
        emit hoverLeft();
        return;
    }

    QString baseWord = matchedWord->word;
    QString hoverResult;
    bool alignLeft = false;
    QString column;

    // Determine column type based on table structure
    // If we have 3 columns: 0=front, 1=word, 2=back
    // If we have 2 columns with front hooks: 0=front, 1=word
    // If we have 2 columns with back hooks: 0=word, 1=back
    // If we have 1 column: 0=word

    if (numCols == 3) {
        if (col == 0) {
            column = "FRONT";
            alignLeft = true;
        } else if (col == 1) {
            column = "CENTER";
            alignLeft = false;
        } else {
            column = "BACK";
            alignLeft = false;
        }
    } else if (numCols == 2) {
        if (hasAnyFrontHooks && !hasAnyBackHooks) {
            column = (col == 0) ? "FRONT" : "CENTER";
            alignLeft = (col == 0);
        } else if (!hasAnyFrontHooks && hasAnyBackHooks) {
            column = (col == 0) ? "CENTER" : "BACK";
            alignLeft = false;
        } else {
            column = "CENTER";
            alignLeft = false;
        }
    } else {
        column = "CENTER";
        alignLeft = false;
    }

    // Now determine what to show based on column and content
    if (column == "FRONT") {
        // Front hooks/extensions column
        // Check if wordAtPos is a single-letter hook (must be in frontHooks)
        if (wordAtPos.length() == 1 && wordAtPos[0].isLetter() &&
            matchedWord->frontHooks.contains(wordAtPos, Qt::CaseInsensitive)) {
            hoverResult = wordAtPos.toUpper() + baseWord;  // Hook + word
        } else {
            // Try to match as extension - exact match on the word under cursor
            QStringList extLines = matchedWord->frontExtensions.split('\n', Qt::SkipEmptyParts);
            for (const QString& line : extLines) {
                QString ext = line.trimmed();
                // Exact match on the extension word
                if (ext.compare(wordAtPos, Qt::CaseInsensitive) == 0) {
                    // Front extension: add extension before base word
                    hoverResult = ext + baseWord;
                    break;
                }
            }
        }
    } else if (column == "BACK") {
        // Back hooks/extensions column
        // Check if wordAtPos is a single-letter hook (must be in backHooks)
        if (wordAtPos.length() == 1 && wordAtPos[0].isLetter() &&
            matchedWord->backHooks.contains(wordAtPos, Qt::CaseInsensitive)) {
            hoverResult = baseWord + wordAtPos.toUpper();  // Word + hook
        } else {
            // Try to match as extension - exact match on the word under cursor
            QStringList extLines = matchedWord->backExtensions.split('\n', Qt::SkipEmptyParts);
            for (const QString& line : extLines) {
                QString ext = line.trimmed();
                // Exact match on the extension word
                if (ext.compare(wordAtPos, Qt::CaseInsensitive) == 0) {
                    // Back extension: add base word before extension
                    hoverResult = baseWord + ext;
                    break;
                }
            }
        }
    } else {
        // Center column - just show the word
        hoverResult = baseWord;
    }

    // Emit debug info
    emit hoverDebug(QString("row=%1 col=%2/%3 type=%4 word='%5' result='%6'")
                    .arg(row)
                    .arg(col)
                    .arg(numCols)
                    .arg(column)
                    .arg(wordAtPos)
                    .arg(hoverResult));

    if (!hoverResult.isEmpty()) {
        emit wordHovered(hoverResult, alignLeft);
    } else {
        emit hoverLeft();
    }
}
