#include "game_history_panel.h"
#include <QScrollArea>
#include <QFrame>
#include <QSvgRenderer>
#include <QPainter>
#include <QPixmap>
#include <QFontDatabase>
#include <QDir>
#include <QCoreApplication>

// PlayerHistoryColumn implementation
PlayerHistoryColumn::PlayerHistoryColumn(const QString &playerName, QWidget *parent)
    : QWidget(parent)
    , m_playerName(playerName)
    , m_timeSeconds(25 * 60)  // Start at 25:00
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // Header section with player info in a horizontal layout
    QWidget *headerWidget = new QWidget(this);
    QHBoxLayout *headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);

    // Left side: Player name and score vertically stacked
    QWidget *nameScoreWidget = new QWidget(headerWidget);
    QVBoxLayout *nameScoreLayout = new QVBoxLayout(nameScoreWidget);
    nameScoreLayout->setContentsMargins(0, 0, 0, 0);
    nameScoreLayout->setSpacing(2);

    // Player name (bold, larger)
    m_nameLabel = new QLabel(playerName, nameScoreWidget);
    QFont nameFont = m_nameLabel->font();
    nameFont.setBold(true);
    nameFont.setPointSize(14);
    m_nameLabel->setFont(nameFont);
    m_nameLabel->setStyleSheet("color: #333333;");

    // Score label
    m_scoreLabel = new QLabel("Score: 0", nameScoreWidget);
    m_scoreLabel->setStyleSheet("color: #555555; font-size: 11px;");

    nameScoreLayout->addWidget(m_nameLabel);
    nameScoreLayout->addWidget(m_scoreLabel);

    // Right side: Timer
    m_timerLabel = new QLabel("25:00", headerWidget);
    QFont timerFont = m_timerLabel->font();
    timerFont.setFamily("Courier");
    timerFont.setPointSize(14);
    timerFont.setBold(true);
    m_timerLabel->setFont(timerFont);
    m_timerLabel->setStyleSheet("color: #333333;");
    m_timerLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    headerLayout->addWidget(nameScoreWidget, 1);
    headerLayout->addWidget(m_timerLabel, 0);

    mainLayout->addWidget(headerWidget);

    // Separator line
    QFrame *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    separator->setStyleSheet("color: #C0C0D0;");
    mainLayout->addWidget(separator);

    // Moves container
    QWidget *movesWidget = new QWidget(this);
    m_movesLayout = new QVBoxLayout(movesWidget);
    m_movesLayout->setContentsMargins(0, 0, 0, 0);
    m_movesLayout->setSpacing(2);
    m_movesLayout->addStretch();

    QScrollArea *scrollArea = new QScrollArea(this);
    scrollArea->setWidget(movesWidget);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setStyleSheet("QScrollArea { background-color: transparent; }");

    mainLayout->addWidget(scrollArea, 1);

    setStyleSheet("QWidget { background-color: #FFFFFF; border: 1px solid #C0C0D0; border-radius: 8px; }");
}

void PlayerHistoryColumn::setScore(int score) {
    m_scoreLabel->setText(QString::number(score));
}

void PlayerHistoryColumn::setTimeRemaining(int seconds) {
    m_timeSeconds = seconds;
    int minutes = seconds / 60;
    int secs = seconds % 60;
    m_timerLabel->setText(QString("%1:%2").arg(minutes).arg(secs, 2, 10, QChar('0')));
}

void PlayerHistoryColumn::addMoveEntry(const QString &moveText) {
    QLabel *moveLabel = new QLabel(moveText, this);
    moveLabel->setStyleSheet("color: #333333; padding: 2px;");
    moveLabel->setWordWrap(true);
    
    // Insert before the stretch
    m_movesLayout->insertWidget(m_movesLayout->count() - 1, moveLabel);
}

void PlayerHistoryColumn::clearHistory() {
    // Remove all move labels except the stretch
    while (m_movesLayout->count() > 1) {
        QLayoutItem *item = m_movesLayout->takeAt(0);
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
}

// GameHistoryPanel implementation
GameHistoryPanel::GameHistoryPanel(QWidget *parent)
    : QWidget(parent)
    , m_timerRunning(false)
    , m_currentTimerPlayer(0)
    , m_player1TimeSeconds(25 * 60)
    , m_player2TimeSeconds(25 * 60)
    , m_useTwoColumns(false)
{
    // Load Consolas font using same pattern as tile_renderer
    QDir fontsDir(QCoreApplication::applicationDirPath() + "/../Resources/fonts");
    if (!fontsDir.exists()) {
        // Try alternate location for development
        fontsDir.setPath(QCoreApplication::applicationDirPath() + "/fonts");
    }

    int fontId = QFontDatabase::addApplicationFont(fontsDir.filePath("Consolas.ttf"));
    if (fontId == -1) {
        qWarning("Failed to load Consolas font");
    }

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(5);

    // Container for player headers
    QWidget *headerContainer = new QWidget(this);
    QHBoxLayout *headerLayout = new QHBoxLayout(headerContainer);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(5);

    // Left: Player 1 (olaugh)
    QWidget *player1Widget = new QWidget(headerContainer);
    player1Widget->setFixedHeight(80);
    player1Widget->setStyleSheet("background-color: #FFFFFF; border-radius: 8px;");

    QVBoxLayout *p1Layout = new QVBoxLayout(player1Widget);
    p1Layout->setContentsMargins(12, 12, 12, 12);
    p1Layout->setSpacing(6);

    // Row 1: Player name
    QLabel *p1Name = new QLabel("olaugh", player1Widget);
    QFont nameFont = p1Name->font();
    nameFont.setBold(true);
    nameFont.setPointSize(16);
    p1Name->setFont(nameFont);
    p1Name->setStyleSheet("color: #333333;");
    p1Name->setAlignment(Qt::AlignLeft);

    // Row 2: Score (left) and Timer + Button (right)
    QWidget *p1Row2 = new QWidget(player1Widget);
    p1Row2->setMinimumHeight(36);  // Ensure button isn't cropped
    QHBoxLayout *p1Row2Layout = new QHBoxLayout(p1Row2);
    p1Row2Layout->setContentsMargins(0, 0, 0, 0);
    p1Row2Layout->setSpacing(0);

    m_player1Column = new PlayerHistoryColumn("olaugh", this);
    m_player1Column->hide();  // Hide the unused widget
    m_player1Column->m_scoreLabel = new QLabel("0", p1Row2);
    QFont scoreFont("Consolas", 24);
    scoreFont.setBold(true);
    m_player1Column->m_scoreLabel->setFont(scoreFont);
    m_player1Column->m_scoreLabel->setStyleSheet("color: #333333;");
    m_player1Column->m_scoreLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Timer + Button container (right side)
    QWidget *timerContainer = new QWidget(p1Row2);
    QHBoxLayout *timerLayout = new QHBoxLayout(timerContainer);
    timerLayout->setContentsMargins(0, 0, 0, 0);
    timerLayout->setSpacing(8);

    // Create circular blue pause button with SVG icon
    m_timerButton = new QPushButton(timerContainer);
    m_timerButton->setText("");  // No text!

    // Create pause SVG icon (two vertical bars)
    QSvgRenderer *pauseRenderer = new QSvgRenderer(this);
    QString pauseSvg =
        "<svg width='16' height='16' viewBox='0 0 16 16' xmlns='http://www.w3.org/2000/svg'>"
        "<rect x='5' y='4' width='2' height='8' fill='white'/>"
        "<rect x='9' y='4' width='2' height='8' fill='white'/>"
        "</svg>";
    pauseRenderer->load(pauseSvg.toUtf8());

    QPixmap pausePixmap(16, 16);
    pausePixmap.fill(Qt::transparent);
    QPainter painter(&pausePixmap);
    pauseRenderer->render(&painter);
    painter.end();

    m_timerButton->setIcon(QIcon(pausePixmap));
    m_timerButton->setIconSize(QSize(16, 16));
    m_timerButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #4A90E2;"
        "  border: none;"
        "  border-radius: 16px;"
        "  padding: 0px;"
        "  text-align: center;"
        "}"
        "QPushButton:hover {"
        "  background-color: #357ABD;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #2868A8;"
        "}"
    );
    m_timerButton->setCursor(Qt::PointingHandCursor);
    m_timerButton->setFixedSize(32, 32);
    connect(m_timerButton, &QPushButton::clicked, this, &GameHistoryPanel::onToggleTimer);

    m_player1Column->m_timerLabel = new QLabel("25:00", timerContainer);
    QFont timerFont("Consolas", 16);
    timerFont.setBold(true);
    m_player1Column->m_timerLabel->setFont(timerFont);
    m_player1Column->m_timerLabel->setStyleSheet("color: #333333;");
    m_player1Column->m_timerLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    timerLayout->addWidget(m_timerButton);
    timerLayout->addWidget(m_player1Column->m_timerLabel);

    p1Row2Layout->addWidget(m_player1Column->m_scoreLabel, 0, Qt::AlignLeft);
    p1Row2Layout->addStretch();
    p1Row2Layout->addWidget(timerContainer, 0, Qt::AlignRight);

    p1Layout->addWidget(p1Name);
    p1Layout->addWidget(p1Row2);

    // Right: Player 2 (magpie)
    QWidget *player2Widget = new QWidget(headerContainer);
    player2Widget->setFixedHeight(80);
    player2Widget->setStyleSheet("background-color: #FFFFFF; border-radius: 8px;");

    QVBoxLayout *p2Layout = new QVBoxLayout(player2Widget);
    p2Layout->setContentsMargins(12, 12, 12, 12);
    p2Layout->setSpacing(6);

    // Row 1: Player name
    QLabel *p2Name = new QLabel("magpie", player2Widget);
    p2Name->setFont(nameFont);
    p2Name->setStyleSheet("color: #333333;");
    p2Name->setAlignment(Qt::AlignLeft);

    // Row 2: Score (left) and Timer (right)
    QWidget *p2Row2 = new QWidget(player2Widget);
    p2Row2->setMinimumHeight(36);  // Consistent height with player 1
    QHBoxLayout *p2Row2Layout = new QHBoxLayout(p2Row2);
    p2Row2Layout->setContentsMargins(0, 0, 0, 0);
    p2Row2Layout->setSpacing(0);

    m_player2Column = new PlayerHistoryColumn("magpie", this);
    m_player2Column->hide();  // Hide the unused widget
    m_player2Column->m_scoreLabel = new QLabel("0", p2Row2);
    m_player2Column->m_scoreLabel->setFont(scoreFont);
    m_player2Column->m_scoreLabel->setStyleSheet("color: #333333;");
    m_player2Column->m_scoreLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Timer container (right side) - with invisible spacer for button alignment
    QWidget *p2TimerContainer = new QWidget(p2Row2);
    QHBoxLayout *p2TimerLayout = new QHBoxLayout(p2TimerContainer);
    p2TimerLayout->setContentsMargins(0, 0, 0, 0);
    p2TimerLayout->setSpacing(8);

    // Invisible spacer to match button size (32px + 8px spacing)
    QWidget *buttonSpacer = new QWidget(p2TimerContainer);
    buttonSpacer->setFixedSize(32, 32);

    m_player2Column->m_timerLabel = new QLabel("25:00", p2TimerContainer);
    m_player2Column->m_timerLabel->setFont(timerFont);
    m_player2Column->m_timerLabel->setStyleSheet("color: #333333;");
    m_player2Column->m_timerLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    p2TimerLayout->addWidget(buttonSpacer);
    p2TimerLayout->addWidget(m_player2Column->m_timerLabel);

    p2Row2Layout->addWidget(m_player2Column->m_scoreLabel, 0, Qt::AlignLeft);
    p2Row2Layout->addStretch();
    p2Row2Layout->addWidget(p2TimerContainer, 0, Qt::AlignRight);

    p2Layout->addWidget(p2Name);
    p2Layout->addWidget(p2Row2);

    headerLayout->addWidget(player1Widget, 1);
    headerLayout->addWidget(player2Widget, 1);

    mainLayout->addWidget(headerContainer, 0);

    // Move history area (will be added later)
    QWidget *historyWidget = new QWidget(this);
    historyWidget->setStyleSheet("background-color: #FFFFFF; border: 1px solid #C0C0D0; border-radius: 8px;");
    mainLayout->addWidget(historyWidget, 1);

    // Create game timer
    m_gameTimer = new QTimer(this);
    m_gameTimer->setInterval(1000);  // 1 second
    connect(m_gameTimer, &QTimer::timeout, this, &GameHistoryPanel::onTimerTick);

    // Start timer for player 1 automatically
    startTimer(0);
}

void GameHistoryPanel::setPlayerNames(const QString &player1, const QString &player2) {
    m_player1Column->findChild<QLabel*>()->setText(player1);
    m_player2Column->findChild<QLabel*>()->setText(player2);
}

void GameHistoryPanel::setPlayerScore(int playerIndex, int score) {
    if (playerIndex == 0) {
        m_player1Column->setScore(score);
    } else if (playerIndex == 1) {
        m_player2Column->setScore(score);
    }
}

void GameHistoryPanel::addMove(int playerIndex, const QString &moveText) {
    if (playerIndex == 0) {
        m_player1Column->addMoveEntry(moveText);
    } else if (playerIndex == 1) {
        m_player2Column->addMoveEntry(moveText);
    }
}

void GameHistoryPanel::clearHistory() {
    m_player1Column->clearHistory();
    m_player2Column->clearHistory();
    m_player1TimeSeconds = 25 * 60;
    m_player2TimeSeconds = 25 * 60;
    m_player1Column->setTimeRemaining(m_player1TimeSeconds);
    m_player2Column->setTimeRemaining(m_player2TimeSeconds);
}

void GameHistoryPanel::startTimer(int playerIndex) {
    m_currentTimerPlayer = playerIndex;
    m_timerRunning = true;
    m_gameTimer->start();

    // Update button icon to pause
    QSvgRenderer *pauseRenderer = new QSvgRenderer(this);
    QString pauseSvg =
        "<svg width='16' height='16' viewBox='0 0 16 16' xmlns='http://www.w3.org/2000/svg'>"
        "<rect x='5' y='4' width='2' height='8' fill='white'/>"
        "<rect x='9' y='4' width='2' height='8' fill='white'/>"
        "</svg>";
    pauseRenderer->load(pauseSvg.toUtf8());
    QPixmap pausePixmap(16, 16);
    pausePixmap.fill(Qt::transparent);
    QPainter painter(&pausePixmap);
    pauseRenderer->render(&painter);
    painter.end();
    m_timerButton->setIcon(QIcon(pausePixmap));

    emit timerToggled(true);
}

void GameHistoryPanel::pauseTimer() {
    m_timerRunning = false;
    m_gameTimer->stop();

    // Update button icon to play
    QSvgRenderer *playRenderer = new QSvgRenderer(this);
    QString playSvg =
        "<svg width='16' height='16' viewBox='0 0 16 16' xmlns='http://www.w3.org/2000/svg'>"
        "<path d='M5 3 L5 13 L13 8 Z' fill='white'/>"
        "</svg>";
    playRenderer->load(playSvg.toUtf8());
    QPixmap playPixmap(16, 16);
    playPixmap.fill(Qt::transparent);
    QPainter painter(&playPixmap);
    playRenderer->render(&painter);
    painter.end();
    m_timerButton->setIcon(QIcon(playPixmap));

    emit timerToggled(false);
}

void GameHistoryPanel::onTimerTick() {
    if (!m_timerRunning) return;
    
    if (m_currentTimerPlayer == 0) {
        m_player1TimeSeconds--;
        if (m_player1TimeSeconds < 0) m_player1TimeSeconds = 0;
        m_player1Column->setTimeRemaining(m_player1TimeSeconds);
    } else {
        m_player2TimeSeconds--;
        if (m_player2TimeSeconds < 0) m_player2TimeSeconds = 0;
        m_player2Column->setTimeRemaining(m_player2TimeSeconds);
    }
}

void GameHistoryPanel::onToggleTimer() {
    if (m_timerRunning) {
        pauseTimer();
    } else {
        startTimer(m_currentTimerPlayer);
    }
}


QString GameHistoryPanel::formatTime(int seconds) const {
    int minutes = seconds / 60;
    int secs = seconds % 60;
    return QString("%1:%2").arg(minutes).arg(secs, 2, 10, QChar('0'));
}
