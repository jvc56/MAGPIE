#include "turn_entry_widget.h"
#include <QFont>

TurnEntryWidget::TurnEntryWidget(QWidget *parent)
    : QWidget(parent)
    , m_isCommitted(false)
    , m_isValidated(false)
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 4, 6, 4);
    mainLayout->setSpacing(2);

    // Row 1: Score arithmetic [PREV SCORE] [+PLAY SCORE]
    QWidget *scoreRow1 = new QWidget(this);
    QHBoxLayout *scoreRow1Layout = new QHBoxLayout(scoreRow1);
    scoreRow1Layout->setContentsMargins(0, 0, 0, 0);
    scoreRow1Layout->setSpacing(8);

    m_prevScoreLabel = new QLabel("", scoreRow1);
    QFont scoreFont("Consolas", 11);
    m_prevScoreLabel->setFont(scoreFont);
    m_prevScoreLabel->setStyleSheet("color: #666666;");
    m_prevScoreLabel->setAlignment(Qt::AlignLeft);

    m_playScoreLabel = new QLabel("", scoreRow1);
    m_playScoreLabel->setFont(scoreFont);
    m_playScoreLabel->setStyleSheet("color: #4A90E2;");  // Blue for play score
    m_playScoreLabel->setAlignment(Qt::AlignRight);

    scoreRow1Layout->addWidget(m_prevScoreLabel, 0, Qt::AlignLeft);
    scoreRow1Layout->addStretch();
    scoreRow1Layout->addWidget(m_playScoreLabel, 0, Qt::AlignRight);

    // Row 2: Cumulative score
    m_cumulativeLabel = new QLabel("", this);
    QFont cumulativeFont("Consolas", 12);
    cumulativeFont.setBold(true);
    m_cumulativeLabel->setFont(cumulativeFont);
    m_cumulativeLabel->setStyleSheet("color: #333333;");
    m_cumulativeLabel->setAlignment(Qt::AlignLeft);

    // Row 3: Move notation
    m_notationLabel = new QLabel("", this);
    QFont notationFont = m_notationLabel->font();
    notationFont.setPointSize(10);
    m_notationLabel->setFont(notationFont);
    m_notationLabel->setStyleSheet("color: #333333;");
    m_notationLabel->setAlignment(Qt::AlignLeft);
    m_notationLabel->setWordWrap(true);

    // Row 4: Time and rack [TIME] [RACK]
    QWidget *timeRackRow = new QWidget(this);
    QHBoxLayout *timeRackLayout = new QHBoxLayout(timeRackRow);
    timeRackLayout->setContentsMargins(0, 0, 0, 0);
    timeRackLayout->setSpacing(12);

    m_timeLabel = new QLabel("", timeRackRow);
    QFont monoFont("Consolas", 9);
    m_timeLabel->setFont(monoFont);
    m_timeLabel->setStyleSheet("color: #888888;");
    m_timeLabel->setAlignment(Qt::AlignLeft);

    m_rackLabel = new QLabel("", timeRackRow);
    m_rackLabel->setFont(monoFont);
    m_rackLabel->setStyleSheet("color: #888888;");
    m_rackLabel->setAlignment(Qt::AlignLeft);

    timeRackLayout->addWidget(m_timeLabel, 0, Qt::AlignLeft);
    timeRackLayout->addWidget(m_rackLabel, 0, Qt::AlignLeft);
    timeRackLayout->addStretch();

    mainLayout->addWidget(scoreRow1);
    mainLayout->addWidget(m_cumulativeLabel);
    mainLayout->addWidget(m_notationLabel);
    mainLayout->addWidget(timeRackRow);

    setStyleSheet("QWidget { background-color: #F8F8F8; border: 1px solid #E0E0E0; border-radius: 4px; }");
}

void TurnEntryWidget::setCommittedMove(int prevScore, int playScore, int cumulativeScore,
                                        const QString &notation, const QString &timeStr, const QString &rack)
{
    m_isCommitted = true;
    m_isValidated = true;

    m_prevScoreLabel->setText(QString::number(prevScore));
    m_playScoreLabel->setText(QString("+%1").arg(playScore));
    m_cumulativeLabel->setText(QString::number(cumulativeScore));
    m_notationLabel->setText(notation);
    m_timeLabel->setText(timeStr);
    m_rackLabel->setText(rack);

    // Committed moves have normal appearance
    setStyleSheet("QWidget { background-color: #F8F8F8; border: 1px solid #E0E0E0; border-radius: 4px; }");
}

void TurnEntryWidget::setValidatedMove(int prevScore, int playScore,
                                       const QString &notation, const QString &timeStr, const QString &rack)
{
    m_isCommitted = false;
    m_isValidated = true;

    m_prevScoreLabel->setText(QString::number(prevScore));
    m_playScoreLabel->setText(QString("+%1").arg(playScore));
    m_cumulativeLabel->setText("?");  // Unknown until committed
    m_notationLabel->setText(notation);
    m_timeLabel->setText(timeStr);
    m_rackLabel->setText(rack);

    // Validated but uncommitted: light green tint
    setStyleSheet("QWidget { background-color: #E8F5E9; border: 1px solid #A5D6A7; border-radius: 4px; }");
}

void TurnEntryWidget::setUnvalidatedMove(const QString &notation, const QString &timeStr, const QString &rack)
{
    m_isCommitted = false;
    m_isValidated = false;

    m_prevScoreLabel->setText("");
    m_playScoreLabel->setText("");
    m_cumulativeLabel->setText("");
    m_notationLabel->setText(notation);
    m_timeLabel->setText(timeStr);
    m_rackLabel->setText(rack);

    // Unvalidated: light yellow tint
    setStyleSheet("QWidget { background-color: #FFF9E6; border: 1px solid #FFE082; border-radius: 4px; }");
}

void TurnEntryWidget::updateTime(const QString &timeStr)
{
    m_timeLabel->setText(timeStr);
}

void TurnEntryWidget::clear()
{
    m_prevScoreLabel->clear();
    m_playScoreLabel->clear();
    m_cumulativeLabel->clear();
    m_notationLabel->clear();
    m_timeLabel->clear();
    m_rackLabel->clear();
    m_isCommitted = false;
    m_isValidated = false;
}
