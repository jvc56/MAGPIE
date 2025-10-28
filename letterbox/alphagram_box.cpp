#include "alphagram_box.h"
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>
#include <QGraphicsDropShadowEffect>
#include <QMouseEvent>

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
        emit hoverDebug(QString("Outside label: pos(%1,%2) label(%3,%4 %5x%6)")
                        .arg(pos.x()).arg(pos.y())
                        .arg(labelRect.x()).arg(labelRect.y())
                        .arg(labelRect.width()).arg(labelRect.height()));
        emit hoverLeft();
        return;
    }

    // Calculate relative position within the table
    int relX = pos.x() - labelRect.x();
    int labelWidth = labelRect.width();

    // Rough column detection based on width distribution
    // This is a stub - we'll show the overlay for any hover for now
    // Left third = front hooks, middle third = word, right third = back hooks
    int columnWidth = labelWidth / 3;

    QString hoverWord;
    bool alignLeft = false;
    QString column;

    if (hasAnyFrontHooks && relX < columnWidth) {
        // Front hooks column - show on left
        hoverWord = "FRONT HOOK";  // Stub: will calculate actual extension word
        alignLeft = true;
        column = "FRONT";
    } else if (hasAnyBackHooks && relX > labelWidth - columnWidth) {
        // Back hooks column - show on right
        hoverWord = "BACK HOOK";  // Stub: will calculate actual extension word
        alignLeft = false;
        column = "BACK";
    } else {
        // Word column - show on right
        if (!words.empty()) {
            hoverWord = words[0].word;  // Stub: will detect actual word row
        }
        alignLeft = false;
        column = "WORD";
    }

    // Emit debug info
    emit hoverDebug(QString("pos(%1,%2) rel(%3/%4) col=%5 word='%6'")
                    .arg(pos.x()).arg(pos.y())
                    .arg(relX).arg(labelWidth)
                    .arg(column)
                    .arg(hoverWord));

    if (!hoverWord.isEmpty()) {
        emit wordHovered(hoverWord, alignLeft);
    } else {
        emit hoverLeft();
    }
}
