#include "alphagram_box.h"
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>

AlphagramBox::AlphagramBox(QWidget *parent)
    : QWidget(parent), tableLabel(nullptr)
{
    layout = new QVBoxLayout(this);
    // Add margins to keep table content inside the painted border
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(0);
    setLayout(layout);
}

void AlphagramBox::addWord(const QString& word, const QString& frontHooks, const QString& backHooks)
{
    words.push_back({word, frontHooks, backHooks});
}

void AlphagramBox::finalize()
{
    if (words.empty()) {
        return;
    }

    // Check if any word has hooks
    bool hasAnyFrontHooks = false;
    bool hasAnyBackHooks = false;
    for (const auto& wordData : words) {
        if (!wordData.frontHooks.isEmpty()) hasAnyFrontHooks = true;
        if (!wordData.backHooks.isEmpty()) hasAnyBackHooks = true;
    }

    // Create a single table for all words
    tableLabel = new QLabel(this);

    QString html = "<table style='width: 100%; border-collapse: collapse;'>";

    for (const auto& wordData : words) {
        html += "<tr>";

        // Always add front hooks cell if any word in the group has front hooks
        if (hasAnyFrontHooks) {
            html += QString("<td style='font-size: 12px; color: #666; padding: 8px 4px; text-align: right; border-right: 1px solid #000;'>%1</td>")
                    .arg(wordData.frontHooks);
        }

        // Word (Jost Bold, larger, black)
        QString wordBorder = hasAnyBackHooks ? "border-right: 1px solid #000;" : "";
        html += QString("<td style='font-family: \"Jost\", sans-serif; font-size: 18px; font-weight: bold; letter-spacing: 1px; color: #000; text-align: center; padding: 8px 4px; %1'>%2</td>")
                .arg(wordBorder)
                .arg(wordData.word);

        // Always add back hooks cell if any word in the group has back hooks
        if (hasAnyBackHooks) {
            html += QString("<td style='font-size: 12px; color: #666; padding: 8px 4px; text-align: left;'>%1</td>")
                    .arg(wordData.backHooks);
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

    // Fill background
    painter.fillRect(rect(), QColor(248, 248, 248));

    // Draw border
    QPen pen(Qt::black, 1);
    painter.setPen(pen);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}
