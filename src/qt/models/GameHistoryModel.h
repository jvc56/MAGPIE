#ifndef GAMEHISTORYMODEL_H
#define GAMEHISTORYMODEL_H

#include <QAbstractListModel>
#include <QObject>
#include <QString>
#include <QUrl>
#include <cstdint>
#include <vector>

#include "../bridge/qt_bridge.h"
#include "BoardSquare.h"
#include "HistoryItem.h"

// Forward declarations for C structs
// (None needed as we use opaque handles from bridge)

class GameHistoryModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString player1Name READ player1Name NOTIFY gameChanged)
    Q_PROPERTY(QString player2Name READ player2Name NOTIFY gameChanged)
    Q_PROPERTY(int player1Score READ player1Score NOTIFY gameChanged)
    Q_PROPERTY(int player2Score READ player2Score NOTIFY gameChanged)
    Q_PROPERTY(int playerOnTurnIndex READ playerOnTurnIndex NOTIFY gameChanged)
    Q_PROPERTY(int currentEventIndex READ currentEventIndex NOTIFY gameChanged)
    Q_PROPERTY(int totalEvents READ totalEvents NOTIFY gameChanged)
    Q_PROPERTY(QList<QObject*> board READ board NOTIFY boardChanged)
    Q_PROPERTY(QString currentRack READ currentRack NOTIFY gameChanged)
    Q_PROPERTY(QList<QObject*> history READ history NOTIFY historyChanged)
    Q_PROPERTY(int currentHistoryIndex READ currentHistoryIndex NOTIFY gameChanged)
    Q_PROPERTY(int totalHistoryItems READ totalHistoryItems NOTIFY historyChanged)
    Q_PROPERTY(QString unseenTiles READ unseenTiles NOTIFY gameChanged)
    Q_PROPERTY(int bagCount READ bagCount NOTIFY gameChanged)
    Q_PROPERTY(int vowelCount READ vowelCount NOTIFY gameChanged)
    Q_PROPERTY(int consonantCount READ consonantCount NOTIFY gameChanged)

public:
    explicit GameHistoryModel(QObject *parent = nullptr);
    ~GameHistoryModel();

    Q_INVOKABLE void loadGame(const QString &gcgContent);
    Q_INVOKABLE void loadGameFromFile(const QUrl &fileUrl);
    Q_INVOKABLE void openGameDialog();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void jumpTo(int index);
    Q_INVOKABLE void jumpToHistoryIndex(int index);

    QString player1Name() const;
    QString player2Name() const;
    int player1Score() const;
    int player2Score() const;
    int playerOnTurnIndex() const;
    int currentEventIndex() const;
    int currentHistoryIndex() const;
    int totalEvents() const;
    int totalHistoryItems() const;
    QList<QObject*> board() const;
    QString currentRack() const;
    QList<QObject*> history() const;
    QString unseenTiles() const;
    int bagCount() const;
    int vowelCount() const;
    int consonantCount() const;

signals:
    void gameChanged();
    void boardChanged();
    void historyChanged();
    void gameLoadedFromFile(const QUrl &fileUrl);

private:
    void updateGameState();
    void updateHistory();
    void cleanup();

    BridgeGameHistory *m_gameHistory = nullptr;
    BridgeGame *m_game = nullptr;
    
    int m_currentIndex = 0;
    QList<QObject*> m_boardCache;
    QList<QObject*> m_historyCache;
    QList<int> m_historyItemEndIndices;
};

#endif // GAMEHISTORYMODEL_H