#include "turn_entry_widget.h"
#include <QFont>

TurnEntryWidget::TurnEntryWidget(QWidget *parent)
    : QWidget(parent)
    , m_isCommitted(false)
    , m_isValidated(false)
{
    setObjectName("turnEntry");  // Must have object name for ID selector
    setFixedHeight(80);  // Match header height
    setAttribute(Qt::WA_StyledBackground, true);  // Force Qt to respect stylesheet background

    // ID selector + WA_StyledBackground (Test 2 approach - confirmed working)
    setStyleSheet(
        "#turnEntry {"
        "  background-color: #FFF9E6;"
        "  border: 2px solid #FFE082;"
        "  border-radius: 8px;"
        "}"
    );

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(6);

    // Row 1: Move notation (left) and Score arithmetic (right)
    QHBoxLayout *topRow = new QHBoxLayout();
    topRow->setSpacing(8);

    m_notationLabel = new QLabel("", this);
    // 80% of header score size (24pt) = 19pt
    QFont notationFont("Consolas", 19);
    notationFont.setBold(true);
    m_notationLabel->setFont(notationFont);
    m_notationLabel->setAlignment(Qt::AlignLeft);

    // Score arithmetic container
    QWidget *scoreWidget = new QWidget(this);
    QHBoxLayout *scoreLayout = new QHBoxLayout(scoreWidget);
    scoreLayout->setContentsMargins(0, 0, 0, 0);
    scoreLayout->setSpacing(8);

    m_prevScoreLabel = new QLabel("", scoreWidget);
    // 80% of header score size (24pt) = 19pt
    QFont scoreFont("Consolas", 19);
    m_prevScoreLabel->setFont(scoreFont);
    m_prevScoreLabel->setAlignment(Qt::AlignRight);

    m_playScoreLabel = new QLabel("", scoreWidget);
    m_playScoreLabel->setFont(scoreFont);
    m_playScoreLabel->setAlignment(Qt::AlignRight);

    scoreLayout->addWidget(m_prevScoreLabel);
    scoreLayout->addWidget(m_playScoreLabel);

    topRow->addWidget(m_notationLabel, 1, Qt::AlignLeft);
    topRow->addStretch();
    topRow->addWidget(scoreWidget, 0, Qt::AlignRight);

    // Row 2: Time and rack (left), empty (right)
    QHBoxLayout *bottomRow = new QHBoxLayout();
    bottomRow->setSpacing(8);

    QWidget *timeRackWidget = new QWidget(this);
    QHBoxLayout *timeRackLayout = new QHBoxLayout(timeRackWidget);
    timeRackLayout->setContentsMargins(0, 0, 0, 0);
    timeRackLayout->setSpacing(8);

    m_timeLabel = new QLabel("", timeRackWidget);
    // 60% of header score size (24pt) = 14pt
    QFont monoFont("Consolas", 14);
    m_timeLabel->setFont(monoFont);
    m_timeLabel->setAlignment(Qt::AlignLeft);

    m_rackLabel = new QLabel("", timeRackWidget);
    m_rackLabel->setFont(monoFont);
    m_rackLabel->setAlignment(Qt::AlignLeft);

    timeRackLayout->addWidget(m_timeLabel);
    timeRackLayout->addWidget(m_rackLabel);

    // Cumulative score label (hidden for now, kept for compatibility)
    m_cumulativeLabel = new QLabel("", this);
    m_cumulativeLabel->hide();

    bottomRow->addWidget(timeRackWidget, 1, Qt::AlignLeft);
    bottomRow->addStretch();

    mainLayout->addLayout(topRow);
    mainLayout->addLayout(bottomRow);
}

void TurnEntryWidget::setCommittedMove(int prevScore, int playScore, int cumulativeScore,
                                        const QString &notation, const QString &timeStr, const QString &rack)
{
    Q_UNUSED(cumulativeScore);
    m_isCommitted = true;
    m_isValidated = true;

    m_prevScoreLabel->setText(QString::number(prevScore));
    m_playScoreLabel->setText(QString("+%1").arg(playScore));
    m_notationLabel->setText(convertToStandardNotation(notation));
    m_timeLabel->setText(timeStr);
    m_rackLabel->setText(rack);

    // Committed moves have white background
    setStyleSheet(
        "#turnEntry {"
        "  background-color: #FFFFFF;"
        "  border: 2px solid #E0E0E0;"
        "  border-radius: 8px;"
        "}"
    );
}

void TurnEntryWidget::setValidatedMove(int prevScore, int playScore,
                                       const QString &notation, const QString &timeStr, const QString &rack)
{
    m_isCommitted = false;
    m_isValidated = true;

    m_prevScoreLabel->setText(QString::number(prevScore));
    m_playScoreLabel->setText(QString("+%1").arg(playScore));
    m_notationLabel->setText(convertToStandardNotation(notation));
    m_timeLabel->setText(timeStr);
    m_rackLabel->setText(rack);

    // Validated but uncommitted: green background
    setStyleSheet(
        "#turnEntry {"
        "  background-color: #E8F5E9;"
        "  border: 2px solid #A5D6A7;"
        "  border-radius: 8px;"
        "}"
    );
}

void TurnEntryWidget::setUnvalidatedMove(int prevScore, const QString &notation, const QString &timeStr, const QString &rack)
{
    m_isCommitted = false;
    m_isValidated = false;

    m_prevScoreLabel->setText(QString::number(prevScore));
    m_playScoreLabel->setText("");  // No play score for unvalidated moves
    m_notationLabel->setText(convertToStandardNotation(notation));
    m_timeLabel->setText(timeStr);
    m_rackLabel->setText(rack);

    // Unvalidated: yellow background
    setStyleSheet(
        "#turnEntry {"
        "  background-color: #FFF9E6;"
        "  border: 2px solid #FFE082;"
        "  border-radius: 8px;"
        "}"
    );
}

QString TurnEntryWidget::convertToStandardNotation(const QString &ucgiNotation)
{
    // Convert from UCGI format (8D.FEVER) to standard format (8D FEVER)
    if (ucgiNotation.isEmpty()) {
        return "";
    }

    QString result = ucgiNotation;
    // Replace the dot with a space
    result.replace('.', ' ');
    return result;
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
