#ifndef GAME_HISTORY_PANEL_H
#define GAME_HISTORY_PANEL_H

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QScrollArea>
#include "turn_entry_widget.h"
#include "magpie_wrapper.h"

class PlayerHistoryColumn : public QWidget {
    Q_OBJECT
public:
    explicit PlayerHistoryColumn(const QString &playerName, QWidget *parent = nullptr);

    void setScore(int score);
    void setTimeRemaining(int seconds);
    void addMoveEntry(const QString &moveText);
    void clearHistory();

    // Add turn entry widget for current/uncommitted turn
    TurnEntryWidget* addTurnEntry();
    TurnEntryWidget* getCurrentTurnEntry();
    void clearCurrentTurnEntry();

    QString m_playerName;
    QLabel *m_nameLabel;
    QLabel *m_scoreLabel;
    QLabel *m_timerLabel;
    QVBoxLayout *m_movesLayout;
    int m_timeSeconds;
    TurnEntryWidget *m_currentTurnEntry;
};

class GameHistoryPanel : public QWidget {
    Q_OBJECT
public:
    explicit GameHistoryPanel(QWidget *parent = nullptr);

    void setPlayerNames(const QString &player1, const QString &player2);
    void setPlayerScore(int playerIndex, int score);
    void addMove(int playerIndex, const QString &moveText);
    void clearHistory();

    // Timer controls
    void startTimer(int playerIndex);
    void pauseTimer();
    bool isTimerRunning() const { return m_timerRunning; }
    int getCurrentTimerPlayer() const { return m_currentTimerPlayer; }

    // Update current turn entry (uncommitted move)
    void updateCurrentTurn(int playerIndex, const QString &notation, bool isValidated,
                          int prevScore, int playScore, const QString &rack, Game *game);

    // Clear the current turn entry for a player
    void clearCurrentTurn(int playerIndex);

    // Initialize placeholder turn entry for a player
    void initializePlaceholderTurn(int playerIndex, int currentScore, const QString &rack, Game *game);

    // Set which player is on turn (updates visual indicator)
    void setPlayerOnTurn(int playerIndex);

signals:
    void timerToggled(bool running);
    void debugMessage(const QString &message);

private slots:
    void onTimerTick();
    void onToggleTimer();

private:
    QString formatTime(int seconds) const;
    void updatePlayerHeaderBorders();

    PlayerHistoryColumn *m_player1Column;
    PlayerHistoryColumn *m_player2Column;
    QPushButton *m_timerButton;

    QTimer *m_gameTimer;
    bool m_timerRunning;
    int m_currentTimerPlayer;  // 0 or 1
    int m_player1TimeSeconds;
    int m_player2TimeSeconds;

    bool m_useTwoColumns;

    QVBoxLayout *m_historyLayout;  // Layout for turn entries

    // Player header widgets for styling
    QWidget *m_player1HeaderWidget;
    QWidget *m_player2HeaderWidget;
    int m_playerOnTurn;  // 0 or 1
};

#endif // GAME_HISTORY_PANEL_H
