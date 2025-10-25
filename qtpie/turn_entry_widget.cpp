#include "turn_entry_widget.h"
#include <QFont>
#include <QPalette>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QDebug>
#include <cstdio>

TurnEntryWidget::TurnEntryWidget(QWidget *parent, RenderMode mode, int variant)
    : QWidget(parent)
    , m_renderMode(mode)
    , m_variant(variant % 6)
    , m_isCommitted(false)
    , m_isValidated(false)
    , m_showCumulative(false)
{
    setObjectName("turnEntry");
    setFixedHeight(80);

    // No child widgets needed
    m_notationLabel = nullptr;
    m_prevScoreLabel = nullptr;
    m_playScoreLabel = nullptr;
    m_cumulativeLabel = nullptr;
    m_timeLabel = nullptr;
    m_rackLabel = nullptr;

    // Start with yellow placeholder background (will change on commit)
    setStyleSheet(
        "#turnEntry {"
        "  background-color: #FFF9E6;"
        "  border: 2px solid #FFE082;"
        "  border-radius: 8px;"
        "}"
    );
    setAttribute(Qt::WA_StyledBackground, true);
}

void TurnEntryWidget::paintEvent(QPaintEvent *event)
{
    // CRITICAL: Call base paintEvent FIRST - draws stylesheet background
    QWidget::paintEvent(event);

    // Now paint text on top
    QPainter painter(this);
    painter.setClipRect(rect());
    painter.setClipping(true);

    // Use Consolas font - monospaced for Scrabble
    QFont notationFont("Consolas", 19, QFont::Bold);
    painter.setFont(notationFont);
    painter.setPen(QColor(0, 0, 0));  // Black text

    // Draw move notation (e.g., "8D FEVER")
    painter.drawText(15, 35, m_paintNotation);

    // Draw scores on right side
    QFont scoreFont("Consolas", 19);
    painter.setFont(scoreFont);
    painter.drawText(width() - 140, 35, m_paintPrevScore);
    painter.drawText(width() - 70, 35, m_paintPlayScore);

    // Draw cumulative score
    if (m_showCumulative) {
        painter.setFont(scoreFont);
        painter.drawText(width() - 70, 60, m_paintCumulative);
    }
}

void TurnEntryWidget::setCommittedMove(int prevScore, int playScore, int cumulativeScore,
                                        const QString &notation, const QString &timeStr, const QString &rack)
{
    m_isCommitted = true;
    m_isValidated = true;
    m_showCumulative = true;

    m_paintNotation = convertToStandardNotation(notation);
    m_paintPrevScore = QString::number(prevScore);
    m_paintPlayScore = QString("+%1").arg(playScore);
    m_paintCumulative = QString::number(cumulativeScore);
    m_paintTime = timeStr;
    m_paintRack = rack;

    // Change to white background via stylesheet
    setStyleSheet(
        "#turnEntry {"
        "  background-color: #FFFFFF;"
        "  border: 2px solid #E0E0E0;"
        "  border-radius: 8px;"
        "}"
    );

    update();  // Trigger repaint
}

void TurnEntryWidget::setValidatedMove(int prevScore, int playScore,
                                       const QString &notation, const QString &timeStr, const QString &rack)
{
    m_isCommitted = false;
    m_isValidated = true;
    m_showCumulative = false;

    m_paintNotation = convertToStandardNotation(notation);
    m_paintPrevScore = QString::number(prevScore);
    m_paintPlayScore = QString("+%1").arg(playScore);
    m_paintTime = timeStr;
    m_paintRack = rack;

    // Green background for validated
    setStyleSheet(
        "#turnEntry {"
        "  background-color: #E8F5E9;"
        "  border: 2px solid #A5D6A7;"
        "  border-radius: 8px;"
        "}"
    );

    update();
}

void TurnEntryWidget::setUnvalidatedMove(int prevScore, const QString &notation, const QString &timeStr, const QString &rack)
{
    m_isCommitted = false;
    m_isValidated = false;
    m_showCumulative = false;

    m_paintNotation = convertToStandardNotation(notation);
    m_paintPrevScore = QString::number(prevScore);
    m_paintPlayScore = "";  // No play score for unvalidated
    m_paintTime = timeStr;
    m_paintRack = rack;

    // Yellow background for unvalidated placeholder
    setStyleSheet(
        "#turnEntry {"
        "  background-color: #FFF9E6;"
        "  border: 2px solid #FFE082;"
        "  border-radius: 8px;"
        "}"
    );

    update();
}

void TurnEntryWidget::updateTime(const QString &timeStr)
{
    m_paintTime = timeStr;
    update();
}

void TurnEntryWidget::clear()
{
    m_paintNotation = "";
    m_paintPrevScore = "";
    m_paintPlayScore = "";
    m_paintCumulative = "";
    m_paintTime = "";
    m_paintRack = "";
    m_showCumulative = false;
    update();
}

QString TurnEntryWidget::convertToStandardNotation(const QString &ucgiNotation)
{
    if (ucgiNotation.isEmpty()) {
        return "";
    }
    QString result = ucgiNotation;
    result.replace('.', ' ');
    return result;
}
