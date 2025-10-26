#include "alphagram_box.h"
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>

AlphagramBox::AlphagramBox(QWidget *parent)
    : QWidget(parent)
{
    layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 15, 20, 15);
    layout->setSpacing(5);
    setLayout(layout);
}

void AlphagramBox::addWord(const QString& word, const QString& frontHooks, const QString& backHooks)
{
    QLabel* label = new QLabel(this);

    // Create HTML with hooks and word using Jost Bold
    QString html = "<div style='white-space: nowrap;'>";

    // Front hooks (smaller, gray)
    html += QString("<span style='font-size: 12px; color: #666; margin-right: 8px;'>%1</span>")
            .arg(frontHooks);

    // Word (Jost Bold, larger, black)
    html += QString("<span style='font-family: \"Jost\", sans-serif; font-size: 18px; font-weight: bold; letter-spacing: 1px; color: #000;'>%1</span>")
            .arg(word);

    // Back hooks (smaller, gray)
    html += QString("<span style='font-size: 12px; color: #666; margin-left: 8px;'>%1</span>")
            .arg(backHooks);

    html += "</div>";

    label->setText(html);
    label->setAlignment(Qt::AlignCenter);
    label->setTextFormat(Qt::RichText);

    layout->addWidget(label);
    wordLabels.push_back(label);
}

void AlphagramBox::clear()
{
    for (auto label : wordLabels) {
        layout->removeWidget(label);
        delete label;
    }
    wordLabels.clear();
}

void AlphagramBox::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Draw rounded rectangle border
    QPainterPath path;
    path.addRoundedRect(rect().adjusted(2, 2, -2, -2), 10, 10);

    // Fill background
    painter.fillPath(path, QColor(248, 248, 248));

    // Draw border
    QPen pen(Qt::black, 3);
    painter.setPen(pen);
    painter.drawPath(path);
}
