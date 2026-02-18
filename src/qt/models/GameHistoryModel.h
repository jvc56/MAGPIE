#ifndef GAMEHISTORYMODEL_H
#define GAMEHISTORYMODEL_H

#include <QAbstractListModel>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <cstdint>
#include <vector>

#include "../bridge/qt_bridge.h"
#include "AnalysisModel.h"
#include "BoardSquare.h"
#include "HistoryItem.h"

// Forward declarations for C structs
// (None needed as we use opaque handles from bridge)

class GameHistoryModel : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString player1Name READ player1Name NOTIFY gameChanged)
  Q_PROPERTY(QString player2Name READ player2Name NOTIFY gameChanged)
  Q_PROPERTY(int player1Score READ player1Score NOTIFY gameChanged)
  Q_PROPERTY(int player2Score READ player2Score NOTIFY gameChanged)
  Q_PROPERTY(int playerOnTurnIndex READ playerOnTurnIndex NOTIFY gameChanged)
  Q_PROPERTY(int currentEventIndex READ currentEventIndex NOTIFY gameChanged)
  Q_PROPERTY(int totalEvents READ totalEvents NOTIFY gameChanged)
  Q_PROPERTY(QList<QObject *> board READ board NOTIFY boardChanged)
  Q_PROPERTY(QString currentRack READ currentRack NOTIFY gameChanged)
  Q_PROPERTY(QList<QObject *> history READ history NOTIFY historyChanged)
  Q_PROPERTY(
      int currentHistoryIndex READ currentHistoryIndex NOTIFY gameChanged)
  Q_PROPERTY(int totalHistoryItems READ totalHistoryItems NOTIFY historyChanged)
  Q_PROPERTY(QString unseenTiles READ unseenTiles NOTIFY gameChanged)
  Q_PROPERTY(int bagCount READ bagCount NOTIFY gameChanged)
  Q_PROPERTY(int vowelCount READ vowelCount NOTIFY gameChanged)
  Q_PROPERTY(int consonantCount READ consonantCount NOTIFY gameChanged)
  Q_PROPERTY(int blankCount READ blankCount NOTIFY gameChanged)
  Q_PROPERTY(AnalysisModel *analysisModel READ analysisModel CONSTANT)
  Q_PROPERTY(QString lexiconName READ lexiconName NOTIFY gameChanged)
  Q_PROPERTY(int gameMode READ gameMode NOTIFY gameModeChanged)
  Q_PROPERTY(int player1Clock READ player1Clock NOTIFY clocksChanged)
  Q_PROPERTY(int player2Clock READ player2Clock NOTIFY clocksChanged)
  Q_PROPERTY(bool aiThinking READ aiThinking NOTIFY aiThinkingChanged)
  Q_PROPERTY(bool timerRunning READ timerRunning NOTIFY clocksChanged)
  Q_PROPERTY(
      QString previewNotation READ previewNotation NOTIFY previewChanged)
  Q_PROPERTY(int previewScore READ previewScore NOTIFY previewChanged)
  Q_PROPERTY(int previewStatus READ previewStatus NOTIFY previewChanged)
  Q_PROPERTY(QString previewLeave READ previewLeave NOTIFY previewChanged)

public:
  enum GameMode { SetupMode = 0, PlayMode = 1, ReviewMode = 2 };
  Q_ENUM(GameMode)

  explicit GameHistoryModel(QObject *parent = nullptr);
  ~GameHistoryModel();

  Q_INVOKABLE void loadGame(const QString &gcgContent);
  Q_INVOKABLE void loadGameFromFile(const QUrl &fileUrl);
  Q_INVOKABLE void openGameDialog();
  Q_INVOKABLE void next();
  Q_INVOKABLE void previous();
  Q_INVOKABLE void jumpTo(int index);
  Q_INVOKABLE void jumpToHistoryIndex(int index);

  // Gameplay
  Q_INVOKABLE void startNewGame(const QString &lexicon, int timeControlMinutes);
  Q_INVOKABLE void submitMove(const QString &notation);
  Q_INVOKABLE void pass();
  Q_INVOKABLE void exchange(const QString &tiles);
  Q_INVOKABLE QString getComputerMove();
  Q_INVOKABLE void toggleTimer();
  Q_INVOKABLE void previewMove(const QString &notation);
  Q_INVOKABLE void clearPreview();
  Q_INVOKABLE void challengeLastMove();

  QString player1Name() const;
  QString player2Name() const;
  int player1Score() const;
  int player2Score() const;
  int playerOnTurnIndex() const;
  int currentEventIndex() const;
  int currentHistoryIndex() const;
  int totalEvents() const;
  int totalHistoryItems() const;
  QList<QObject *> board() const;
  QString currentRack() const;
  QList<QObject *> history() const;
  QString unseenTiles() const;
  int bagCount() const;
  int vowelCount() const;
  int consonantCount() const;
  int blankCount() const;
  AnalysisModel *analysisModel() const;
  QString lexiconName() const;

  int gameMode() const { return m_gameMode; }
  int player1Clock() const { return m_clocks[0]; }
  int player2Clock() const { return m_clocks[1]; }
  bool aiThinking() const { return m_aiThinking; }
  bool timerRunning() const;
  QString previewNotation() const { return m_previewNotation; }
  int previewScore() const { return m_previewScore; }
  int previewStatus() const { return m_previewStatus; }
  QString previewLeave() const { return m_previewLeave; }

signals:
  void gameChanged();
  void boardChanged();
  void historyChanged();
  void gameLoadedFromFile(const QUrl &fileUrl);
  void gameModeChanged();
  void clocksChanged();
  void aiThinkingChanged();
  void previewChanged();

private slots:
  void onTimerTick();

private:
  void updateGameState();
  void updateHistory();
  void cleanup();

  BridgeGameHistory *m_gameHistory = nullptr;
  BridgeGame *m_game = nullptr;
  AnalysisModel *m_analysisModel = nullptr;

  int m_currentIndex = 0;
  QList<QObject *> m_boardCache;
  QList<QObject *> m_historyCache;
  QList<int> m_historyItemEndIndices;

  int m_gameMode = SetupMode;
  int m_clocks[2] = {0, 0};
  bool m_aiThinking = false;
  QTimer *m_timer = nullptr;

  QString m_previewNotation;
  int m_previewScore = 0;
  int m_previewStatus = 0; // 0=none, 1=valid, 2=phony
  QString m_previewLeave;
};

#endif // GAMEHISTORYMODEL_H