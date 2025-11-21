#include "GameHistoryModel.h"
#include "../../def/game_history_defs.h"

#include <QDebug>
#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QFileDialog>
#include <QSet>

GameHistoryModel::GameHistoryModel(QObject *parent) : QObject(parent)
{
}


GameHistoryModel::~GameHistoryModel()
{
    cleanup();
}

void GameHistoryModel::cleanup()
{
    qDeleteAll(m_boardCache);
    m_boardCache.clear();
    qDeleteAll(m_historyCache);
    m_historyCache.clear();
    if (m_game) {
        bridge_game_destroy(m_game);
        m_game = nullptr;
    }
    if (m_gameHistory) {
        bridge_game_history_destroy(m_gameHistory);
        m_gameHistory = nullptr;
    }
}

void GameHistoryModel::loadGame(const QString &gcgContent)
{
    cleanup();

    m_gameHistory = bridge_game_history_create();

    QString dataPath = QCoreApplication::applicationDirPath() + "/../Resources/data";
    char errorMsg[256] = {0};
    
    if (bridge_load_gcg(m_gameHistory, gcgContent.toUtf8().constData(), dataPath.toUtf8().constData(), errorMsg, sizeof(errorMsg)) != 0) {
        qWarning() << "Error loading GCG:" << errorMsg;
        return;
    }

    // Create Game from History
    m_game = bridge_game_create_from_history(m_gameHistory, dataPath.toUtf8().constData());
    if (!m_game) {
        qWarning() << "Error creating game from history";
        return;
    }

    // Play to end to populate board for string formatting
    int total = bridge_get_num_events(m_gameHistory);
    bridge_game_play_to_index(m_gameHistory, m_game, total);
    
    updateHistory();

    // Reset to start
    m_currentIndex = 0;
    bridge_game_play_to_index(m_gameHistory, m_game, 0);

    updateGameState();
    emit gameChanged();
}

void GameHistoryModel::loadGameFromFile(const QUrl &fileUrl)
{
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

void GameHistoryModel::openGameDialog()
{
    qDebug() << "openGameDialog called";
    
    QString fileName = QFileDialog::getOpenFileName(
        nullptr, 
        tr("Open Game"), 
        QDir::homePath(), 
        tr("GCG Files (*.gcg);;All Files (*)")
    );
    
    qDebug() << "Dialog returned, fileName:" << fileName;
    if (!fileName.isEmpty()) {
        qDebug() << "Loading file:" << fileName;
        loadGameFromFile(QUrl::fromLocalFile(fileName));
    } else {
        qDebug() << "No file selected or dialog cancelled";
    }
}

void GameHistoryModel::next()
{
    if (!m_gameHistory || !m_game) return;
    
    int total = bridge_get_num_events(m_gameHistory);
    if (m_currentIndex >= total) return;

    m_currentIndex++;
    bridge_game_play_to_index(m_gameHistory, m_game, m_currentIndex);

    updateGameState();
    emit gameChanged();
}

void GameHistoryModel::previous()
{
    if (!m_game) return;
    if (m_currentIndex > 0) {
        jumpTo(m_currentIndex - 1);
    }
}

void GameHistoryModel::jumpTo(int index)
{
    if (!m_gameHistory || !m_game) return;
    int total = bridge_get_num_events(m_gameHistory);
    if (index < 0) index = 0;
    if (index > total) index = total;

    m_currentIndex = index;
    bridge_game_play_to_index(m_gameHistory, m_game, m_currentIndex);

    updateGameState();
    emit gameChanged();
}

void GameHistoryModel::updateHistory()
{
    qDeleteAll(m_historyCache);
    m_historyCache.clear();

    if (!m_gameHistory || !m_game) return;

    int lastScores[2] = {0, 0};
    int total = bridge_get_num_events(m_gameHistory);
    
    struct ItemBuilder {
        int playerIndex;
        int type;
        QList<QObject*> scoreLines; // List of ScoreLineItem*
        QString rackStr;
        int totalTurnScore; // Aggregated score for this merged entry
        int cumulativeScore;
        int eventIndex; // Index of the last event in this merged item
        bool valid = false;
    } current;

    auto flush = [&](bool /*isEnd*/ = false) {
        if (!current.valid) return;
        m_historyCache.append(new HistoryItem(
            current.playerIndex, 
            current.type, 
            current.scoreLines, // Pass the list
            current.rackStr, 
            current.totalTurnScore, 
            current.cumulativeScore, 
            current.eventIndex
        ));
        // Clear the list for next usage, but DO NOT delete the items as they are now owned by HistoryItem (conceptually)
        current.scoreLines.clear();
        current.valid = false;
    };

    for (int i = 0; i < total; i++) {
        int playerIndex, type, score, cumulativeScore;
        char *moveStrC = nullptr;
        char *rackStrC = nullptr;

        bridge_get_event_details(m_gameHistory, m_game, i, &playerIndex, &type, &moveStrC, &rackStrC, &score, &cumulativeScore);
        
        QString moveText = QString::fromUtf8(moveStrC);
        QString rackText = QString::fromUtf8(rackStrC);
        if (moveStrC) free(moveStrC);
        if (rackStrC) free(rackStrC);

        int turnScore = cumulativeScore - lastScores[playerIndex];
        lastScores[playerIndex] = cumulativeScore;
        
        QString formattedScore = QString("(%1%2)")
            .arg(turnScore >= 0 ? "+" : "")
            .arg(turnScore);

        bool isSecondary = (type == GAME_EVENT_PHONY_TILES_RETURNED ||
                            type == GAME_EVENT_CHALLENGE_BONUS ||
                            type == GAME_EVENT_TIME_PENALTY ||
                            type == GAME_EVENT_END_RACK_POINTS ||
                            type == GAME_EVENT_END_RACK_PENALTY);

        if (current.valid && current.playerIndex == playerIndex && isSecondary) {
            // Merge: append new ScoreLineItem
            current.scoreLines.append(new ScoreLineItem(moveText, formattedScore, type));
            current.rackStr = rackText; // Update rack to latest state
            current.totalTurnScore += turnScore; // Sum scores
            current.cumulativeScore = cumulativeScore;
            current.eventIndex = i; // Last event index in this merged item
        } else {
            flush(); // Flush previous item
            current.playerIndex = playerIndex;
            current.type = type; // Type of the primary event
            current.scoreLines.append(new ScoreLineItem(moveText, formattedScore, type)); // Add first ScoreLineItem
            current.rackStr = rackText;
            current.totalTurnScore = turnScore;
            current.cumulativeScore = cumulativeScore;
            current.eventIndex = i;
            current.valid = true;
        }
    }
    flush(true);
    
    emit historyChanged();
}

void GameHistoryModel::updateGameState()
{
    if (!m_game) return;

    qDeleteAll(m_boardCache);
    m_boardCache.clear();

    // Identify last move tiles
    QSet<int> lastMoveIndices;
    if (m_currentIndex > 0) {
        // Get tiles for the event that just happened (index - 1)
        int rows[15]; // Max 7 tiles usually, but up to 15
        int cols[15];
        int count = bridge_get_last_move_tiles(m_gameHistory, m_currentIndex - 1, rows, cols, 15);
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
                // uint8_t unblanked_ml = ml & 0x7F; // Remove blank bit if any? Bridge logic handles it.
                // bridge_get_letter_score takes ml directly.
                score = bridge_get_letter_score(m_game, ml);
            }

            uint8_t bonusRaw = bridge_get_board_bonus(m_game, r, c);
            int letterMultiplier = bonusRaw & 0x0F;
            int wordMultiplier = (bonusRaw >> 4) & 0x0F;

            bool isLastMove = lastMoveIndices.contains(r * 15 + c);

            m_boardCache.append(new BoardSquare(letterStr, score, isBlank, letterMultiplier, wordMultiplier, isLastMove));
        }
    }
    
    emit boardChanged();
}

QString GameHistoryModel::player1Name() const
{
    if (!m_gameHistory) return "Player 1";
    return QString::fromUtf8(bridge_get_player_name(m_gameHistory, 0));
}

QString GameHistoryModel::player2Name() const
{
    if (!m_gameHistory) return "Player 2";
    return QString::fromUtf8(bridge_get_player_name(m_gameHistory, 1));
}

int GameHistoryModel::player1Score() const
{
    if (!m_game) return 0;
    return bridge_get_player_score(m_game, 0);
}

int GameHistoryModel::player2Score() const
{
    if (!m_game) return 0;
    return bridge_get_player_score(m_game, 1);
}

int GameHistoryModel::playerOnTurnIndex() const
{
    if (!m_game) return 0;
    return bridge_get_player_on_turn_index(m_game);
}

int GameHistoryModel::currentEventIndex() const
{
    return m_currentIndex;
}

int GameHistoryModel::totalEvents() const
{
    if (!m_gameHistory) return 0;
    return bridge_get_num_events(m_gameHistory);
}

QList<QObject*> GameHistoryModel::board() const
{
    return m_boardCache;
}

QString GameHistoryModel::currentRack() const
{
    if (!m_game) return "";
    char* rackStr = bridge_get_current_rack(m_game);
    QString result = QString::fromUtf8(rackStr);
    free(rackStr);
    return result;
}

QList<QObject*> GameHistoryModel::history() const
{
    return m_historyCache;
}
