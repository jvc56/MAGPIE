#include "GameHistoryModel.h"
#include "../../def/game_history_defs.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QSet>
#include <QTextStream>

GameHistoryModel::GameHistoryModel(QObject *parent) : QObject(parent) {
  m_analysisModel = new AnalysisModel(this);
}

GameHistoryModel::~GameHistoryModel() { cleanup(); }

void GameHistoryModel::cleanup() {
  if (m_analysisModel) {
    m_analysisModel->stopAnalysis();
  }
  qDeleteAll(m_boardCache);
  m_boardCache.clear();
  qDeleteAll(m_historyCache);
  m_historyCache.clear();
  m_historyItemEndIndices.clear();
  if (m_game) {
    bridge_game_destroy(m_game);
    m_game = nullptr;
  }
  if (m_gameHistory) {
    bridge_game_history_destroy(m_gameHistory);
    m_gameHistory = nullptr;
  }
}

void GameHistoryModel::loadGame(const QString &gcgContent) {
  qDebug() << "Loading GCG content:" << gcgContent;
  cleanup();

  m_gameHistory = bridge_game_history_create();

  QString dataPath =
      QCoreApplication::applicationDirPath() + "/../Resources/data";
  char errorMsg[256] = {0};

  if (bridge_load_gcg(m_gameHistory, gcgContent.toUtf8().constData(),
                      dataPath.toUtf8().constData(), errorMsg,
                      sizeof(errorMsg)) != 0) {
    qWarning() << "Error loading GCG:" << errorMsg;
    return;
  }

  // Create Game from History
  m_game = bridge_game_create_from_history(m_gameHistory);
  if (!m_game) {
    qWarning() << "Error creating game from history";
    return;
  }

  // Play to end to populate board for string formatting
  int total = bridge_get_num_events(m_gameHistory);
  bridge_game_play_to_index(m_gameHistory, m_game, total);

  updateHistory();

  // Set initial state (Jump to first move if available)
  if (!m_historyItemEndIndices.isEmpty()) {
    jumpToHistoryIndex(0);
  } else {
    jumpTo(0);
  }
}

void GameHistoryModel::loadGameFromFile(const QUrl &fileUrl) {
  QString localPath = fileUrl.toLocalFile();
  QFile file(localPath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "Failed to open file:" << localPath;
    return;
  }

  QTextStream in(&file);
  QString content = in.readAll();
  file.close();

  loadGame(content);

  if (m_game) {
    emit gameLoadedFromFile(fileUrl);
  }
}

void GameHistoryModel::openGameDialog() {
  qDebug() << "openGameDialog called";

  QString fileName =
      QFileDialog::getOpenFileName(nullptr, tr("Open Game"), QDir::homePath(),
                                   tr("GCG Files (*.gcg);;All Files (*)"));

  qDebug() << "Dialog returned, fileName:" << fileName;
  if (!fileName.isEmpty()) {
    qDebug() << "Loading file:" << fileName;
    loadGameFromFile(QUrl::fromLocalFile(fileName));
  } else {
    qDebug() << "No file selected or dialog cancelled";
  }
}

void GameHistoryModel::next() {
  if (!m_gameHistory || !m_game)
    return;
  int currentHIdx = currentHistoryIndex();
  if (currentHIdx < m_historyItemEndIndices.size() - 1) {
    jumpToHistoryIndex(currentHIdx + 1);
  }
}

void GameHistoryModel::previous() {
  if (!m_game)
    return;
  int currentHIdx = currentHistoryIndex();
  if (currentHIdx > 0) {
    jumpToHistoryIndex(currentHIdx - 1);
  }
}

void GameHistoryModel::jumpTo(int index) {
  if (!m_gameHistory || !m_game)
    return;
  int total = bridge_get_num_events(m_gameHistory);
  if (index < 0)
    index = 0;
  if (index > total)
    index = total;

  m_currentIndex = index;
  bridge_game_play_to_index(m_gameHistory, m_game, m_currentIndex);

  updateGameState();
  emit gameChanged();
}

void GameHistoryModel::jumpToHistoryIndex(int index) {
  if (index < 0 || index >= m_historyItemEndIndices.size())
    return;
  jumpTo(m_historyItemEndIndices[index]);
}

int GameHistoryModel::currentHistoryIndex() const {
  if (m_currentIndex == 0)
    return -1;
  // Find the index i such that m_historyItemEndIndices[i] == m_currentIndex
  // Or if m_currentIndex is intermediate, map it to the containing item
  for (int i = 0; i < m_historyItemEndIndices.size(); ++i) {
    // Since we always jump to end indices, exact match is likely.
    // If we are at an intermediate index (e.g. 1 when item covers 0,1 -> end
    // index 2), we should probably return i. m_historyItemEndIndices contains
    // EXCLUSIVE upper bounds (e.g. 2 means events 0,1 are played).
    if (m_currentIndex <= m_historyItemEndIndices[i]) {
      return i;
    }
  }
  return -1;
}

void GameHistoryModel::updateHistory() {
  qDeleteAll(m_historyCache);
  m_historyCache.clear();
  m_historyItemEndIndices.clear();

  if (!m_gameHistory || !m_game)
    return;

  // Use a scratch game for replay so we don't destroy m_game's state.
  // In play mode, m_game holds the live game with correctly drawn racks;
  // replaying from history would lose racks that aren't recorded as events.
  BridgeGame *scratch = bridge_game_create_from_history(m_gameHistory);
  bridge_game_play_to_index(m_gameHistory, scratch, 0);
  int total = bridge_get_num_events(m_gameHistory);

  int lastScores[2] = {0, 0};

  struct ItemBuilder {
    int playerIndex;
    int type;
    QList<QObject *> scoreLines; // List of ScoreLineItem*
    QString rackStr;
    int totalTurnScore; // Aggregated score for this merged entry
    int cumulativeScore;
    int eventIndex; // Index of the last event in this merged item
    // Pre-move tile tracking state (captured when starting a new item)
    QString unseenTiles;
    int bagCount;
    int vowelCount;
    int consonantCount;
    int blankCount;
    // Rack breakdown
    QString playedTiles;
    QString leaveStr;
    QString fullRack;
    bool valid = false;
  } current;

  auto flush = [&](bool /*isEnd*/ = false) {
    if (!current.valid)
      return;
    m_historyCache.append(new HistoryItem(
        current.playerIndex, current.type,
        current.scoreLines, // Pass the list
        current.rackStr, current.totalTurnScore, current.cumulativeScore,
        current.eventIndex, current.unseenTiles, current.bagCount,
        current.vowelCount, current.consonantCount, current.blankCount,
        current.playedTiles, current.leaveStr, current.fullRack));
    // Store the end index (event index + 1) for this history item
    m_historyItemEndIndices.append(current.eventIndex + 1);

    // Clear the list for next usage, but DO NOT delete the items as they are
    // now owned by HistoryItem (conceptually)
    current.scoreLines.clear();
    current.valid = false;
  };

  for (int i = 0; i < total; i++) {
    int playerIndex, type, score, cumulativeScore;
    char *moveStrC = nullptr;
    char *rackStrC = nullptr;

    bridge_get_event_details(m_gameHistory, scratch, i, &playerIndex, &type,
                             &moveStrC, &rackStrC, &score, &cumulativeScore);

    QString moveText = QString::fromUtf8(moveStrC);
    QString rackText = QString::fromUtf8(rackStrC);
    if (moveStrC)
      free(moveStrC);
    if (rackStrC)
      free(rackStrC);

    int turnScore = cumulativeScore - lastScores[playerIndex];
    lastScores[playerIndex] = cumulativeScore;

    QString formattedScore =
        QString("%1%2").arg(turnScore >= 0 ? "+" : "").arg(turnScore);

    bool isSecondary = (type == GAME_EVENT_PHONY_TILES_RETURNED ||
                        type == GAME_EVENT_CHALLENGE_BONUS ||
                        type == GAME_EVENT_TIME_PENALTY ||
                        type == GAME_EVENT_END_RACK_POINTS ||
                        type == GAME_EVENT_END_RACK_PENALTY);

    if (current.valid && current.playerIndex == playerIndex && isSecondary) {
      // Merge: append new ScoreLineItem
      current.scoreLines.append(
          new ScoreLineItem(moveText, formattedScore, type));

      // Update rack to latest state, UNLESS it's end rack points (which shows
      // opponent's tiles)
      if (type != GAME_EVENT_END_RACK_POINTS) {
        current.rackStr = rackText;
      }

      current.totalTurnScore += turnScore; // Sum scores
      current.cumulativeScore = cumulativeScore;
      current.eventIndex = i; // Last event index in this merged item
    } else {
      flush(); // Flush previous item

      // Capture pre-move tile tracking state (game is at position before event
      // i)
      char *tilesC = nullptr;
      int vowels = 0, consonants = 0, blanks = 0;
      bridge_get_unseen_tiles(scratch, &tilesC, &vowels, &consonants, &blanks);
      current.unseenTiles = tilesC ? QString::fromUtf8(tilesC) : "";
      if (tilesC)
        free(tilesC);
      current.bagCount = bridge_get_bag_count(scratch);
      current.vowelCount = vowels;
      current.consonantCount = consonants;
      current.blankCount = blanks;

      current.playerIndex = playerIndex;
      current.type = type; // Type of the primary event
      current.scoreLines.append(new ScoreLineItem(
          moveText, formattedScore, type)); // Add first ScoreLineItem
      current.rackStr = rackText;
      current.totalTurnScore = turnScore;
      current.cumulativeScore = cumulativeScore;
      current.eventIndex = i;
      current.valid = true;
    }

    // Advance game state after processing this event
    // (this also sets vms on event i via game_play_n_events)
    bridge_game_play_to_index(m_gameHistory, scratch, i + 1);

    // Get rack breakdown (played tiles, leave, full rack) now that vms is set
    if (!isSecondary) {
      char *playedC = nullptr, *leaveC = nullptr, *fullRackC = nullptr;
      bridge_get_event_rack_info(m_gameHistory, scratch, i, &playedC, &leaveC,
                                 &fullRackC);
      current.playedTiles = playedC ? QString::fromUtf8(playedC) : "";
      current.leaveStr = leaveC ? QString::fromUtf8(leaveC) : "";
      current.fullRack = fullRackC ? QString::fromUtf8(fullRackC) : "";
      free(playedC);
      free(leaveC);
      free(fullRackC);
    }
  }
  flush(true);

  bridge_game_destroy(scratch);

  emit historyChanged();
}

void GameHistoryModel::updateGameState() {
  if (!m_game)
    return;

  BridgeGame *analysisGame = nullptr;

  if (m_gameMode == PlayMode) {
    // In play mode, only analyze when it's the human's turn (player 0).
    // When it's the computer's turn, it will play immediately, so
    // analyzing its position is wasted work and causes rapid start/stop
    // cycles that can crash.
    int onTurn = bridge_get_player_on_turn_index(m_game);
    if (onTurn == 0) {
      analysisGame = bridge_game_clone(m_game);
    } else {
      // Stop any running analysis without starting a new one
      if (m_analysisModel) {
        m_analysisModel->stopAnalysis();
      }
    }
  } else {
    // In review mode, replay from history to the start of the current turn
    int hIndex = currentHistoryIndex();
    int analysisIndex = 0;
    if (hIndex > 0) {
      analysisIndex = m_historyItemEndIndices[hIndex - 1];
    }
    analysisGame = bridge_game_create_from_history(m_gameHistory);
    bridge_game_play_to_index(m_gameHistory, analysisGame, analysisIndex);
  }

  // Trigger analysis
  if (analysisGame && m_analysisModel) {
    m_analysisModel->startAnalysis(analysisGame);
  } else if (analysisGame) {
    bridge_game_destroy(analysisGame);
  }

  qDeleteAll(m_boardCache);
  m_boardCache.clear();

  // Identify last move tiles
  QSet<int> lastMoveIndices;
  if (m_currentIndex > 0) {
    // Get tiles for the event that just happened (index - 1)
    int rows[15]; // Max 7 tiles usually, but up to 15
    int cols[15];
    int count = bridge_get_last_move_tiles(m_gameHistory, m_currentIndex - 1,
                                           rows, cols, 15);
    for (int i = 0; i < count; i++) {
      lastMoveIndices.insert(rows[i] * 15 + cols[i]);
    }
  }

  for (int r = 0; r < 15; r++) {
    for (int c = 0; c < 15; c++) {
      uint8_t ml = bridge_get_machine_letter(m_game, r, c);

      QString letterStr = "";
      int score = 0;
      bool isBlank = false;

      if (ml != 0) { // 0 is ALPHABET_EMPTY_SQUARE_MARKER
        char *str = bridge_get_board_square_string(m_game, r, c);
        letterStr = QString::fromUtf8(str);
        free(str);

        isBlank = bridge_is_blank(ml);
        if (isBlank) {
          letterStr = letterStr.toUpper();
        }
        score = bridge_get_letter_score(m_game, ml);
      }

      uint8_t bonusRaw = bridge_get_board_bonus(m_game, r, c);
      int letterMultiplier = bonusRaw & 0x0F;
      int wordMultiplier = (bonusRaw >> 4) & 0x0F;

      bool isLastMove = lastMoveIndices.contains(r * 15 + c);

      m_boardCache.append(new BoardSquare(letterStr, score, isBlank,
                                          letterMultiplier, wordMultiplier,
                                          isLastMove));
    }
  }

  emit boardChanged();
}

QString GameHistoryModel::player1Name() const {
  if (!m_gameHistory)
    return "Player 1";
  return QString::fromUtf8(bridge_get_player_name(m_gameHistory, 0));
}

QString GameHistoryModel::player2Name() const {
  if (!m_gameHistory)
    return "Player 2";
  return QString::fromUtf8(bridge_get_player_name(m_gameHistory, 1));
}

int GameHistoryModel::player1Score() const {
  if (!m_game)
    return 0;
  return bridge_get_player_score(m_game, 0);
}

int GameHistoryModel::player2Score() const {
  if (!m_game)
    return 0;
  return bridge_get_player_score(m_game, 1);
}

int GameHistoryModel::playerOnTurnIndex() const {
  if (!m_game)
    return 0;
  return bridge_get_player_on_turn_index(m_game);
}

int GameHistoryModel::currentEventIndex() const { return m_currentIndex; }

int GameHistoryModel::totalEvents() const {
  if (!m_gameHistory)
    return 0;
  return bridge_get_num_events(m_gameHistory);
}

int GameHistoryModel::totalHistoryItems() const {
  return m_historyItemEndIndices.size();
}

QList<QObject *> GameHistoryModel::board() const { return m_boardCache; }

QString GameHistoryModel::currentRack() const {
  if (!m_game)
    return "";

  if (m_gameMode == PlayMode) {
    char *rack = bridge_get_current_rack(m_game);
    QString qs = QString::fromUtf8(rack);
    free(rack);
    if (qs.isEmpty()) {
      return "DEBUGME";
    }
    return qs;
  }

  int hIndex = currentHistoryIndex();
  if (hIndex >= 0 && hIndex < m_historyCache.size()) {
    HistoryItem *item = qobject_cast<HistoryItem *>(m_historyCache.at(hIndex));
    if (item) {
      return item->rackString();
    }
  }

  // Fallback for m_currentIndex == 0 (no history item selected, start of game)
  char *rackStr = bridge_get_current_rack(m_game);
  QString result = QString::fromUtf8(rackStr);
  free(rackStr);
  return result;
}

QList<QObject *> GameHistoryModel::history() const { return m_historyCache; }

QString GameHistoryModel::unseenTiles() const {
  int hIndex = currentHistoryIndex();
  if (hIndex >= 0 && hIndex < m_historyCache.size()) {
    HistoryItem *item = qobject_cast<HistoryItem *>(m_historyCache.at(hIndex));
    if (item) {
      return item->unseenTiles();
    }
  }

  // Fallback for initial state
  if (!m_game)
    return "";
  char *tiles = nullptr;
  bridge_get_unseen_tiles(m_game, &tiles, nullptr, nullptr, nullptr);
  QString s = QString::fromUtf8(tiles);
  if (tiles)
    free(tiles);
  return s;
}

int GameHistoryModel::bagCount() const {
  int hIndex = currentHistoryIndex();
  if (hIndex >= 0 && hIndex < m_historyCache.size()) {
    HistoryItem *item = qobject_cast<HistoryItem *>(m_historyCache.at(hIndex));
    if (item) {
      return item->bagCount();
    }
  }

  // Fallback for initial state
  if (!m_game)
    return 0;
  return bridge_get_bag_count(m_game);
}

int GameHistoryModel::vowelCount() const {
  int hIndex = currentHistoryIndex();
  if (hIndex >= 0 && hIndex < m_historyCache.size()) {
    HistoryItem *item = qobject_cast<HistoryItem *>(m_historyCache.at(hIndex));
    if (item) {
      return item->vowelCount();
    }
  }

  // Fallback for initial state
  if (!m_game)
    return 0;
  int v = 0;
  bridge_get_unseen_tiles(m_game, nullptr, &v, nullptr, nullptr);
  return v;
}

int GameHistoryModel::consonantCount() const {
  int hIndex = currentHistoryIndex();
  if (hIndex >= 0 && hIndex < m_historyCache.size()) {
    HistoryItem *item = qobject_cast<HistoryItem *>(m_historyCache.at(hIndex));
    if (item) {
      return item->consonantCount();
    }
  }

  // Fallback for initial state
  if (!m_game)
    return 0;
  int c = 0;
  bridge_get_unseen_tiles(m_game, nullptr, nullptr, &c, nullptr);
  return c;
}

int GameHistoryModel::blankCount() const {
  int hIndex = currentHistoryIndex();
  if (hIndex >= 0 && hIndex < m_historyCache.size()) {
    HistoryItem *item = qobject_cast<HistoryItem *>(m_historyCache.at(hIndex));
    if (item) {
      return item->blankCount();
    }
  }

  // Fallback for initial state
  if (!m_game)
    return 0;
  int b = 0;
  bridge_get_unseen_tiles(m_game, nullptr, nullptr, nullptr, &b);
  return b;
}

AnalysisModel *GameHistoryModel::analysisModel() const {
  return m_analysisModel;
}

QString GameHistoryModel::lexiconName() const {
  return QString::fromUtf8(bridge_get_lexicon(m_gameHistory));
}

void GameHistoryModel::startNewGame(const QString &lexicon,
                                    int timeControlMinutes) {
  cleanup();
  m_gameMode = PlayMode;
  m_clocks[0] = timeControlMinutes * 60;
  m_clocks[1] = timeControlMinutes * 60;

  QString dataPath =
      QCoreApplication::applicationDirPath() + "/../Resources/data";
  char errorMsg[256] = {0};

  m_gameHistory = bridge_game_create_fresh(dataPath.toUtf8().constData(),
                                           lexicon.toUtf8().constData(),
                                           errorMsg, sizeof(errorMsg));
  if (!m_gameHistory) {
    qWarning() << "Error creating fresh game:" << errorMsg;
    return;
  }

  m_game = bridge_game_create_from_history(m_gameHistory);

  if (!m_timer) {
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &GameHistoryModel::onTimerTick);
  }
  m_timer->start(1000);

  m_currentIndex = 0;
  updateHistory();

  // Draw racks BEFORE updateGameState() so the analysis clone has tiles
  bridge_game_draw_racks(m_game);

  updateGameState();

  emit gameModeChanged();
  emit historyChanged();
  emit boardChanged();
  emit clocksChanged();
  emit gameChanged();
}

void GameHistoryModel::previewMove(const QString &notation) {
  if (!m_game) {
    clearPreview();
    return;
  }

  char *notationOut = nullptr;
  int score = 0;
  bool isPhony = false;
  char *leaveOut = nullptr;
  char *errorOut = nullptr;

  int rc = bridge_preview_move(m_game, notation.toUtf8().constData(),
                               &notationOut, &score, &isPhony, &leaveOut,
                               &errorOut);

  if (rc != 0) {
    if (errorOut)
      free(errorOut);
    clearPreview();
    return;
  }

  m_previewNotation = QString::fromUtf8(notationOut);
  m_previewScore = score;
  m_previewStatus = isPhony ? 2 : 1;
  m_previewLeave = QString::fromUtf8(leaveOut);
  free(notationOut);
  free(leaveOut);
  emit previewChanged();
}

void GameHistoryModel::clearPreview() {
  if (m_previewStatus == 0)
    return;
  m_previewNotation.clear();
  m_previewScore = 0;
  m_previewStatus = 0;
  m_previewLeave.clear();
  emit previewChanged();
}

void GameHistoryModel::submitMove(const QString &notation) {
  if (m_gameMode != PlayMode || !m_game || !m_gameHistory)
    return;
  clearPreview();

  char *err =
      bridge_play_move(m_gameHistory, m_game, notation.toUtf8().constData());
  if (err) {
    qWarning() << "Move failed:" << err;
    free(err);
    return;
  }

  m_currentIndex = bridge_get_num_events(m_gameHistory);
  updateHistory();
  updateGameState();
  emit gameChanged();
}

void GameHistoryModel::challengeLastMove() {
  if (m_gameMode != PlayMode || !m_game || !m_gameHistory)
    return;

  char *err = bridge_challenge_last_move(m_gameHistory, m_game);
  if (err) {
    qWarning() << "Challenge failed:" << err;
    free(err);
    return;
  }

  m_currentIndex = bridge_get_num_events(m_gameHistory);
  updateHistory();
  updateGameState();
  emit gameChanged();
}

void GameHistoryModel::pass() { submitMove("PASS"); }

void GameHistoryModel::exchange(const QString &tiles) {
  submitMove(QString("EXCHANGE.%1").arg(tiles));
}

void GameHistoryModel::onTimerTick() {
  if (m_gameMode != PlayMode || !m_game)
    return;

  int turn = bridge_get_player_on_turn_index(m_game);
  if (m_clocks[turn] > 0) {
    m_clocks[turn]--;
    emit clocksChanged();
  }
}
bool GameHistoryModel::timerRunning() const {
  return m_timer && m_timer->isActive();
}

void GameHistoryModel::toggleTimer() {
  if (!m_timer)
    return;
  if (m_timer->isActive())
    m_timer->stop();
  else
    m_timer->start(1000);
  emit clocksChanged();
}

QString GameHistoryModel::getComputerMove() {
  if (!m_game)
    return QString();

  m_aiThinking = true;
  emit aiThinkingChanged();

  char *move = bridge_get_computer_move(m_game);
  QString result = QString::fromUtf8(move);
  free(move);

  m_aiThinking = false;
  emit aiThinkingChanged();

  return result;
}
