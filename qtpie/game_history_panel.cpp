#include "game_history_panel.h"
#include <QScrollArea>
#include <QScrollBar>
#include <QFrame>
#include <QSvgRenderer>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QFontDatabase>
#include <QDir>
#include <QCoreApplication>
#include <QPalette>

// PlayerHistoryColumn implementation - just a container for turn entries
PlayerHistoryColumn::PlayerHistoryColumn(const QString &playerName, TurnEntryWidget::RenderMode renderMode, QWidget *parent)
    : QWidget(parent)
    , m_playerName(playerName)
    , m_renderMode(renderMode)
    , m_timeSeconds(25 * 60)  // Start at 25:00
    , m_currentTurnEntry(nullptr)
{
    setObjectName("playerHistoryColumn");

    // Set size policy - minimum vertical size, don't expand
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    // Simple vertical layout for turn entries
    m_movesLayout = new QVBoxLayout(this);
    m_movesLayout->setContentsMargins(8, 8, 8, 8);
    m_movesLayout->setSpacing(4);
    m_movesLayout->setSizeConstraint(QLayout::SetMinimumSize);
    // Set alignment to top - no stretch needed since scroll area handles overflow
    m_movesLayout->setAlignment(Qt::AlignTop);

    // Create dummy labels for compatibility (will be reassigned by GameHistoryPanel)
    m_nameLabel = new QLabel(playerName, this);
    m_scoreLabel = new QLabel("0", this);
    m_timerLabel = new QLabel("25:00", this);
    m_nameLabel->hide();
    m_scoreLabel->hide();
    m_timerLabel->hide();
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

    // Add to the end of the layout
    m_movesLayout->addWidget(moveLabel);
}

void PlayerHistoryColumn::clearHistory() {
    // Remove all turn entry widgets
    while (m_movesLayout->count() > 0) {
        QLayoutItem *item = m_movesLayout->takeAt(0);
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
    m_currentTurnEntry = nullptr;
}

TurnEntryWidget* PlayerHistoryColumn::addTurnEntry() {
    // Use entry count as variant number for testing (cycles through 0-5)
    int variant = getTurnEntryCount();
    TurnEntryWidget *entry = new TurnEntryWidget(this, m_renderMode, variant);

    // Add to the end of the layout
    // This makes entries appear top-to-bottom in chronological order
    m_movesLayout->addWidget(entry);
    entry->show();  // Explicitly show the widget
    m_currentTurnEntry = entry;

    // THEORY 2: Force repaint on all previous entries
    // When a new entry is added, existing entries might need explicit update() to repaint text
    int entryCount = m_movesLayout->count();
    for (int i = 0; i < entryCount - 1; i++) {
        QLayoutItem *item = m_movesLayout->itemAt(i);
        if (item && item->widget()) {
            TurnEntryWidget *prevEntry = qobject_cast<TurnEntryWidget*>(item->widget());
            if (prevEntry) {
                prevEntry->update();  // Force repaint
            }
        }
    }

    return entry;
}

TurnEntryWidget* PlayerHistoryColumn::addEmptyPlaceholder() {
    // Create an empty placeholder entry that won't be tracked as current
    int variant = getTurnEntryCount();
    TurnEntryWidget *entry = new TurnEntryWidget(this, m_renderMode, variant);

    // Make it invisible - just a spacer to keep column heights synchronized
    entry->setVisible(false);
    entry->setFixedHeight(80);  // Keep the height for layout purposes

    // Add to the end of the layout
    m_movesLayout->addWidget(entry);

    // Don't set as current turn entry - this is just a placeholder
    return entry;
}

TurnEntryWidget* PlayerHistoryColumn::getCurrentTurnEntry() {
    return m_currentTurnEntry;
}

void PlayerHistoryColumn::clearCurrentTurnEntry() {
    // Just clear the pointer - DON'T clear the widget's data!
    // The widget now contains committed move data that should be displayed.
    m_currentTurnEntry = nullptr;
}

// GameHistoryPanel implementation
GameHistoryPanel::GameHistoryPanel(QWidget *parent)
    : QWidget(parent)
    , m_timerRunning(false)
    , m_currentTimerPlayer(0)
    , m_player1TimeSeconds(25 * 60)
    , m_player2TimeSeconds(25 * 60)
    , m_useTwoColumns(false)
    , m_player1HeaderWidget(nullptr)
    , m_player2HeaderWidget(nullptr)
    , m_playerOnTurn(0)
    , m_player1ScrollArea(nullptr)
    , m_player2ScrollArea(nullptr)
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
    m_player1HeaderWidget = new QWidget(headerContainer);
    m_player1HeaderWidget->setObjectName("player1Header");
    m_player1HeaderWidget->setFixedHeight(80);

    QHBoxLayout *p1MainLayout = new QHBoxLayout(m_player1HeaderWidget);
    p1MainLayout->setContentsMargins(12, 12, 12, 12);
    p1MainLayout->setSpacing(12);

    // Avatar
    QLabel *p1Avatar = new QLabel(m_player1HeaderWidget);
    QDir avatarsDir(QCoreApplication::applicationDirPath() + "/../Resources/avatars");
    if (!avatarsDir.exists()) {
        avatarsDir.setPath(QCoreApplication::applicationDirPath() + "/avatars");
    }
    QPixmap p1AvatarPixmap(avatarsDir.filePath("olaugh.png"));
    if (!p1AvatarPixmap.isNull()) {
        // Create circular masked avatar
        QPixmap circularAvatar(56, 56);
        circularAvatar.fill(Qt::transparent);

        QPainter painter(&circularAvatar);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        // Create circular clipping path
        QPainterPath path;
        path.addEllipse(0, 0, 56, 56);
        painter.setClipPath(path);

        // Draw scaled avatar
        QPixmap scaled = p1AvatarPixmap.scaled(56, 56, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        // Center the image if it's larger than the circle
        int x = (56 - scaled.width()) / 2;
        int y = (56 - scaled.height()) / 2;
        painter.drawPixmap(x, y, scaled);

        painter.end();
        p1Avatar->setPixmap(circularAvatar);
    }
    p1Avatar->setFixedSize(56, 56);
    p1Avatar->setStyleSheet("background-color: #FFFFFF; border-radius: 28px;");

    // Info section (name, score, timer)
    QVBoxLayout *p1Layout = new QVBoxLayout();
    p1Layout->setSpacing(6);

    // Row 1: Player name
    QLabel *p1Name = new QLabel("olaugh", m_player1HeaderWidget);
    QFont nameFont = p1Name->font();
    nameFont.setBold(true);
    nameFont.setPointSize(16);
    p1Name->setFont(nameFont);
    p1Name->setStyleSheet("color: #333333;");
    p1Name->setAlignment(Qt::AlignLeft);

    // Row 2: Score (left) and Timer + Button (right)
    QWidget *p1Row2 = new QWidget(m_player1HeaderWidget);
    p1Row2->setMinimumHeight(36);  // Ensure button isn't cropped
    QHBoxLayout *p1Row2Layout = new QHBoxLayout(p1Row2);
    p1Row2Layout->setContentsMargins(0, 0, 0, 0);
    p1Row2Layout->setSpacing(0);

    // LEFT COLUMN: Use QPushButton/QLabel approach with forced palettes
    m_player1Column = new PlayerHistoryColumn("olaugh", TurnEntryWidget::USE_QPUSHBUTTON, this);

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

    // Add avatar and info section to main layout
    p1MainLayout->addWidget(p1Avatar);
    p1MainLayout->addLayout(p1Layout);

    // Right: Player 2 (magpie)
    m_player2HeaderWidget = new QWidget(headerContainer);
    m_player2HeaderWidget->setObjectName("player2Header");
    m_player2HeaderWidget->setFixedHeight(80);

    QHBoxLayout *p2MainLayout = new QHBoxLayout(m_player2HeaderWidget);
    p2MainLayout->setContentsMargins(12, 12, 12, 12);
    p2MainLayout->setSpacing(12);

    // Avatar
    QLabel *p2Avatar = new QLabel(m_player2HeaderWidget);
    QPixmap p2AvatarPixmap(avatarsDir.filePath("magpie.png"));
    if (!p2AvatarPixmap.isNull()) {
        // Create circular masked avatar
        QPixmap circularAvatar(56, 56);
        circularAvatar.fill(Qt::transparent);

        QPainter painter(&circularAvatar);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        // Create circular clipping path
        QPainterPath path;
        path.addEllipse(0, 0, 56, 56);
        painter.setClipPath(path);

        // Draw scaled avatar
        QPixmap scaled = p2AvatarPixmap.scaled(56, 56, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        // Center the image if it's larger than the circle
        int x = (56 - scaled.width()) / 2;
        int y = (56 - scaled.height()) / 2;
        painter.drawPixmap(x, y, scaled);

        painter.end();
        p2Avatar->setPixmap(circularAvatar);
    }
    p2Avatar->setFixedSize(56, 56);
    p2Avatar->setStyleSheet("background-color: #FFFFFF; border-radius: 28px;");

    // Info section (name, score, timer)
    QVBoxLayout *p2Layout = new QVBoxLayout();
    p2Layout->setSpacing(6);

    // Row 1: Player name
    QLabel *p2Name = new QLabel("magpie", m_player2HeaderWidget);
    p2Name->setFont(nameFont);
    p2Name->setStyleSheet("color: #333333;");
    p2Name->setAlignment(Qt::AlignLeft);

    // Row 2: Score (left) and Timer (right)
    QWidget *p2Row2 = new QWidget(m_player2HeaderWidget);
    p2Row2->setMinimumHeight(36);  // Consistent height with player 1
    QHBoxLayout *p2Row2Layout = new QHBoxLayout(p2Row2);
    p2Row2Layout->setContentsMargins(0, 0, 0, 0);
    p2Row2Layout->setSpacing(0);

    // RIGHT COLUMN: Use manual painting with QPainter (no child widgets)
    m_player2Column = new PlayerHistoryColumn("magpie", TurnEntryWidget::USE_MANUAL_PAINT, this);

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

    // Add avatar and info section to main layout
    p2MainLayout->addWidget(p2Avatar);
    p2MainLayout->addLayout(p2Layout);

    headerLayout->addWidget(m_player1HeaderWidget, 1);
    headerLayout->addWidget(m_player2HeaderWidget, 1);

    // Initialize border styling
    updatePlayerHeaderBorders();

    mainLayout->addWidget(headerContainer, 0);

    // Move history area - two columns side by side inside a single scroll area
    QWidget *historyWidget = new QWidget();
    historyWidget->setStyleSheet("background-color: transparent;");
    QHBoxLayout *historyLayout = new QHBoxLayout(historyWidget);
    historyLayout->setContentsMargins(0, 0, 0, 0);
    historyLayout->setSpacing(5);

    // Add both columns directly to the layout
    m_player1Column->show();
    m_player2Column->show();
    historyLayout->addWidget(m_player1Column, 1);
    historyLayout->addWidget(m_player2Column, 1);

    // Create a single scroll area containing both columns
    m_player1ScrollArea = new QScrollArea(this);
    m_player1ScrollArea->setWidget(historyWidget);
    m_player1ScrollArea->setWidgetResizable(true);
    m_player1ScrollArea->setFrameShape(QFrame::NoFrame);
    m_player1ScrollArea->setStyleSheet("QScrollArea { background-color: transparent; border: none; }");
    m_player1ScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_player1ScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Set player2 scroll area to nullptr since we're only using one scroll area now
    m_player2ScrollArea = nullptr;

    mainLayout->addWidget(m_player1ScrollArea, 1);

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

        // Update current turn entry's time
        TurnEntryWidget *turnEntry = m_player1Column->getCurrentTurnEntry();
        if (turnEntry) {
            turnEntry->updateTime(formatTime(m_player1TimeSeconds));
        }
    } else {
        m_player2TimeSeconds--;
        if (m_player2TimeSeconds < 0) m_player2TimeSeconds = 0;
        m_player2Column->setTimeRemaining(m_player2TimeSeconds);

        // Update current turn entry's time
        TurnEntryWidget *turnEntry = m_player2Column->getCurrentTurnEntry();
        if (turnEntry) {
            turnEntry->updateTime(formatTime(m_player2TimeSeconds));
        }
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

void GameHistoryPanel::updateCurrentTurn(int playerIndex, const QString &notation, bool isValidated,
                                         int prevScore, int playScore, const QString &rack, Game *game)
{
    PlayerHistoryColumn *column = (playerIndex == 0) ? m_player1Column : m_player2Column;
    if (!column) return;

    // Get or create current turn entry
    TurnEntryWidget *turnEntry = column->getCurrentTurnEntry();
    if (!turnEntry) {
        turnEntry = column->addTurnEntry();
    }

    // Get current time for this player
    int timeSeconds = (playerIndex == 0) ? m_player1TimeSeconds : m_player2TimeSeconds;
    QString timeStr = formatTime(timeSeconds);

    if (isValidated) {
        // Validated move - show scores
        turnEntry->setValidatedMove(prevScore, playScore, notation, timeStr, rack);
    } else {
        // Unvalidated move - show prevScore only
        turnEntry->setUnvalidatedMove(prevScore, notation, timeStr, rack);
    }

    // Scroll to bottom to show the updated entry
    scrollToBottom(playerIndex);
}

void GameHistoryPanel::clearCurrentTurn(int playerIndex) {
    PlayerHistoryColumn *column = (playerIndex == 0) ? m_player1Column : m_player2Column;
    if (!column) return;

    column->clearCurrentTurnEntry();
}

void GameHistoryPanel::initializePlaceholderTurn(int playerIndex, int currentScore, const QString &rack, Game *game, bool showRack, bool forceNew) {
    Q_UNUSED(game);

    PlayerHistoryColumn *column = (playerIndex == 0) ? m_player1Column : m_player2Column;
    QString playerName = (playerIndex == 0) ? "olaugh" : "magpie";
    if (!column) return;

    emit debugMessage(QString("[%1] initializePlaceholderTurn: score=%2 rack='%3' showRack=%4 forceNew=%5")
                      .arg(playerName).arg(currentScore).arg(rack).arg(showRack ? "true" : "false").arg(forceNew ? "true" : "false"));

    // Get or create current turn entry
    TurnEntryWidget *turnEntry = nullptr;

    if (forceNew) {
        // Starting a new turn - always create a fresh entry
        emit debugMessage(QString("[%1] Force creating new entry at position %2")
                          .arg(playerName).arg(column->getTurnEntryCount()));
        turnEntry = column->addTurnEntry();
        emit debugMessage(QString("[%1] Created new entry, total entries now: %2")
                          .arg(playerName).arg(column->getTurnEntryCount()));
    } else {
        // During current turn - reuse existing entry if it exists
        turnEntry = column->getCurrentTurnEntry();
        if (!turnEntry) {
            emit debugMessage(QString("[%1] Creating new placeholder at position %2")
                              .arg(playerName).arg(column->getTurnEntryCount()));
            turnEntry = column->addTurnEntry();
            emit debugMessage(QString("[%1] Created placeholder, total entries now: %2")
                              .arg(playerName).arg(column->getTurnEntryCount()));
        } else {
            emit debugMessage(QString("[%1] Updating existing placeholder entry").arg(playerName));
        }
    }

    // Get current time for this player
    int timeSeconds = (playerIndex == 0) ? m_player1TimeSeconds : m_player2TimeSeconds;
    QString timeStr = formatTime(timeSeconds);

    // Show placeholder with current state: score, time, and optionally rack
    QString rackToShow = showRack ? rack : QString();
    emit debugMessage(QString("[%1] Setting placeholder: prevScore=%2 time=%3 rack='%4'")
                      .arg(playerName).arg(currentScore).arg(timeStr).arg(rackToShow));
    turnEntry->setUnvalidatedMove(currentScore, "", timeStr, rackToShow);

    // Scroll to bottom to show the new entry
    scrollToBottom(playerIndex);
}

void GameHistoryPanel::setPlayerOnTurn(int playerIndex) {
    m_playerOnTurn = playerIndex;
    updatePlayerHeaderBorders();
}

void GameHistoryPanel::commitTurnAndCreateNext(int playerIndex, int prevScore, int playScore, int newScore,
                                                const QString &notation, const QString &rack, Game *game) {
    Q_UNUSED(game);

    PlayerHistoryColumn *column = (playerIndex == 0) ? m_player1Column : m_player2Column;
    QString playerName = (playerIndex == 0) ? "olaugh" : "magpie";
    if (!column) return;

    emit debugMessage(QString("[%1] commitTurnAndCreateNext: %2 (%3+%4=%5)")
                      .arg(playerName).arg(notation).arg(prevScore).arg(playScore).arg(newScore));

    // Get current turn entry (or create one if it doesn't exist - for computer moves)
    TurnEntryWidget *turnEntry = column->getCurrentTurnEntry();
    if (!turnEntry) {
        // No existing turn entry (happens for computer player)
        // Create one directly for this committed move
        emit debugMessage(QString("[%1] No placeholder exists, creating new entry at position %2")
                          .arg(playerName).arg(column->getTurnEntryCount()));
        turnEntry = column->addTurnEntry();
        emit debugMessage(QString("[%1] Created entry, total entries now: %2")
                          .arg(playerName).arg(column->getTurnEntryCount()));
    } else {
        emit debugMessage(QString("[%1] Using existing placeholder entry").arg(playerName));
    }

    // Get current time for this player (this is when the move was committed)
    int timeSeconds = (playerIndex == 0) ? m_player1TimeSeconds : m_player2TimeSeconds;
    QString timeStr = formatTime(timeSeconds);

    // For computer (player 1), don't show rack. For human (player 0), rack is already in the placeholder
    QString rackToShow = (playerIndex == 1) ? QString() : rack;

    // Commit the move with white background
    emit debugMessage(QString("[%1] Setting entry: notation='%2' prevScore=%3 playScore=%4 newScore=%5 time=%6 rack='%7'")
                      .arg(playerName).arg(notation).arg(prevScore).arg(playScore).arg(newScore).arg(timeStr).arg(rackToShow));
    turnEntry->setCommittedMove(prevScore, playScore, newScore, notation, timeStr, rackToShow);

    // Clear the current turn entry pointer
    column->clearCurrentTurnEntry();
    emit debugMessage(QString("[%1] Entry committed and pointer cleared").arg(playerName));

    // Scroll to bottom to show the committed entry
    scrollToBottom(playerIndex);
}

void GameHistoryPanel::updatePlayerHeaderBorders() {
    if (!m_player1HeaderWidget || !m_player2HeaderWidget) return;

    // Player on turn gets green border, other gets default white background
    // Use direct selector (>) to only style the immediate widget, not children
    if (m_playerOnTurn == 0) {
        m_player1HeaderWidget->setStyleSheet(
            "#player1Header { background-color: #FFFFFF; border: 3px solid #4CAF50; border-radius: 8px; }"
        );
        m_player2HeaderWidget->setStyleSheet(
            "#player2Header { background-color: #FFFFFF; border-radius: 8px; }"
        );
    } else {
        m_player1HeaderWidget->setStyleSheet(
            "#player1Header { background-color: #FFFFFF; border-radius: 8px; }"
        );
        m_player2HeaderWidget->setStyleSheet(
            "#player2Header { background-color: #FFFFFF; border: 3px solid #4CAF50; border-radius: 8px; }"
        );
    }
}

void GameHistoryPanel::synchronizeColumnHeights() {
    if (!m_player1Column || !m_player2Column) return;

    int count1 = m_player1Column->getTurnEntryCount();
    int count2 = m_player2Column->getTurnEntryCount();

    // Add empty placeholders to the shorter column
    if (count1 < count2) {
        for (int i = count1; i < count2; i++) {
            m_player1Column->addEmptyPlaceholder();
        }
    } else if (count2 < count1) {
        for (int i = count2; i < count1; i++) {
            m_player2Column->addEmptyPlaceholder();
        }
    }
}

void GameHistoryPanel::scrollToBottom(int playerIndex) {
    Q_UNUSED(playerIndex);  // Both columns share the same scroll area

    if (!m_player1ScrollArea) return;

    // Use QTimer::singleShot to defer scrolling until after layout updates
    QTimer::singleShot(0, [this]() {
        QScrollBar *vbar = m_player1ScrollArea->verticalScrollBar();
        if (vbar) {
            vbar->setValue(vbar->maximum());
        }
    });
}
