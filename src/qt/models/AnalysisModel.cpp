#include "AnalysisModel.h"
#include <QDebug>

void AnalysisWorker::run() {
    if (game && moves && results && tc) {
        bridge_simulate(game, moves, results, tc, plies);
    }
    emit finished();
}

AnalysisModel::AnalysisModel(QObject *parent)
    : QAbstractListModel(parent), m_plies(6) // Initialize m_plies to 6 (default)
{
    m_updateTimer = new QTimer(this);
    m_updateTimer->setInterval(100); // 0.1 second
    connect(m_updateTimer, &QTimer::timeout, this, &AnalysisModel::updateResults);
}

AnalysisModel::~AnalysisModel()
{
    stopAnalysis();
}

int AnalysisModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return m_rowCount;
}

QVariant AnalysisModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rowCount)
        return QVariant();

    const DisplayItem &item = m_displayCache[index.row()];

    switch (role) {
    case RankRole:
        return item.rank;
    case NotationRole:
        return item.notation;
    case WinPctRole:
        return item.winPct;
    case SpreadRole:
        return item.spread;
    case IterationsRole:
        return item.iterations;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> AnalysisModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles[RankRole] = "rank";
    roles[NotationRole] = "notation";
    roles[WinPctRole] = "winPct";
    roles[SpreadRole] = "spread";
    roles[IterationsRole] = "iterations";
    return roles;
}

void AnalysisModel::startAnalysis(BridgeGame* game) {
    qDebug() << "AnalysisModel::startAnalysis called";
    stopAnalysis(); // Stop any existing, cleans up old m_game

    if (!game) {
        qDebug() << "AnalysisModel: Game is null";
        return;
    }
    m_game = game; // Take ownership

    // Check empty bag
    if (bridge_get_bag_count(game) == 0) {
        qDebug() << "Empty bag, skipping analysis (endgame not supported yet)";
        return;
    }

    // Check if history is empty (default empty game state)
    // If we are at the very start of a *loaded* game (index 0), we might want to analyze the opening move.
    // But if we just opened the app with no game loaded (or default placeholder), we skip.
    // bridge_get_num_events checks the history size.
    // How do we distinguish "default empty" from "loaded game at start"?
    // The default game likely has 0 events.
    // A loaded GCG will have events.
    // However, the user might *want* to analyze an empty board if they are setting up a position?
    // But for now, to fix the crash/behavior:
    
    /*
    if (bridge_get_board_tiles_played(game) == 0) {
        qDebug() << "Empty board, skipping analysis (start of game)";
        return;
    }
    */
    
    // Better check: unseen tiles vs win_pct max?
    // Or simply: don't sim if bag is full?
    
    // Let's check if we can determine if we are in a state where win_pct will crash.
    // bridge_get_bag_count + unseen opponent tiles?
    // We don't have easy access to win_pct limits here.
    
    // User said: "you should not do the analysis on the default empty game state".
    // Default empty game state has 0 moves history.
    // So let's pass GameHistory to AnalysisModel (or access via GameHistoryModel).
    // Wait, AnalysisModel only has BridgeGame*.
    // BridgeGame* has Config*. Config* has GameHistory*.
    // We can use bridge_get_num_events(gh) if we had gh.
    // AnalysisModel doesn't store gh.
    
    // But GameHistoryModel does. And calls startAnalysis.
    // Let's assume if there are 0 events in history, we skip.
    // We need to access history count via bridge.
    // bridge_get_num_events takes BridgeGameHistory*.
    // BridgeGame contains Config* which contains GameHistory*.
    // So we can implement `bridge_game_get_num_history_events(BridgeGame*)`.
    
    // For now, let's rely on the bag count logic?
    // "win_pct shouldn't be called with > 93".
    // Standard start: 100 tiles. 7 P1, 7 P2. 86 Bag. Unseen = 86 + 7 = 93.
    // Wait, P1 is on turn. Unseen is Bag + P2 Rack.
    // 100 total. P1 has 7. 93 left.
    // If win_pct max is 93, then 93 is fine.
    // The crash was "cannot get win percentage value for 94 unseen tiles".
    // That implies 94 unseen. Total tiles > 100? Or P1 has < 7?
    // If P1 has 6, unseen = 94.
    // On default empty game: #lexicon CSW24.
    // Config initializes default racks (random?). Or empty?
    // If empty racks, unseen = 100.
    // `load_game` with `#player1 ...` sets up basic game.
    // If no moves, maybe racks aren't drawn yet?
    // In `game_play_n_events` with 0: `set_rack_from_bag_or_push_to_error_stack`...
    // If 0 events, we might not have drawn tiles if we didn't execute `draw_starting_racks`.
    
    // Let's look at `game_play_n_events` in `config.c`.
    // It calls `game_reset`.
    // `draw_starting_racks` is called inside `game_reset`? No.
    // `game_reset` usually clears racks.
    // `config_game_play_events` calls `game_play_n_events`.
    
    // If racks are empty, unseen = 100.
    // This causes the crash.
    // So we should skip analysis if racks are empty (i.e. game not started/drawn).
    
    char* rack = bridge_get_current_rack(game);
    bool emptyRack = (QString::fromUtf8(rack).isEmpty());
    free(rack);
    
    if (emptyRack) {
        qDebug() << "Empty rack, skipping analysis";
        return;
    }

    // Only run analysis if the game history has events (i.e. not the default empty game state)
    // Note: bridge_game_get_history_num_events was added to qt_bridge.h but might need extern C block or re-check
    int numEvents = bridge_game_get_history_num_events(game);
    qDebug() << "AnalysisModel: History events:" << numEvents;
    if (numEvents == 0) {
        qDebug() << "Game history has no events, skipping analysis";
        return;
    }
    
    // Generate moves (sync for now)
    m_moves = bridge_generate_moves(game);
    if (!m_moves) {
        qDebug() << "No moves generated";
        return;
    }
    qDebug() << "Moves generated for analysis";

    m_results = bridge_sim_results_create();
    m_tc = bridge_thread_control_create();

    // Store plies value from here
    m_plies = 6; // Fixed for now, can be made dynamic
    emit pliesChanged();
    
    // Setup Worker
    m_thread = new QThread;
    m_worker = new AnalysisWorker;
    m_worker->moveToThread(m_thread);
    
    m_worker->game = m_game;
    m_worker->moves = m_moves;
    m_worker->results = m_results;
    m_worker->tc = m_tc;
    m_worker->plies = 6;

    connect(m_thread, &QThread::started, m_worker, &AnalysisWorker::run);
    connect(m_worker, &AnalysisWorker::finished, m_thread, &QThread::quit);
    // Manual cleanup in stopAnalysis to avoid race conditions
    connect(m_thread, &QThread::finished, this, &AnalysisModel::onWorkerFinished);

    m_thread->start();
    m_isRunning = true;
    emit isRunningChanged();
    
    m_updateTimer->start();
    qDebug() << "Analysis thread started";
}

void AnalysisModel::stopAnalysis() {
    if (m_isRunning) qDebug() << "AnalysisModel::stopAnalysis called";
    m_updateTimer->stop();
    
    // Clear data immediately to reflect "stop" state visually
    beginResetModel();
    m_displayCache.clear();
    m_rowCount = 0;
    m_iterations = 0;
    m_confidence = 0.0;
    endResetModel();
    emit iterationsChanged();
    emit confidenceChanged();

    if (m_tc) {
        bridge_thread_control_stop(m_tc);
    }
    
    if (m_thread) {
        // Prevent old thread from triggering slots (like onWorkerFinished)
        disconnect(m_thread, nullptr, this, nullptr);
        
        m_thread->quit();
        m_thread->wait(2000); // Wait up to 2s
        if (m_thread->isRunning()) {
             m_thread->terminate(); // Force kill if stuck
             m_thread->wait();
        }
        
        delete m_worker;
        delete m_thread;
        
        m_worker = nullptr;
        m_thread = nullptr;
    }

    // Cleanup handles
    if (m_game) {
        bridge_game_destroy(m_game);
        m_game = nullptr;
    }
    if (m_moves) {
        bridge_move_list_destroy(m_moves);
        m_moves = nullptr;
    }
    if (m_results) {
        bridge_sim_results_destroy(m_results);
        m_results = nullptr;
    }
    if (m_tc) {
        bridge_thread_control_destroy(m_tc);
        m_tc = nullptr;
    }
    
    m_isRunning = false;
    emit isRunningChanged();
}

bool AnalysisModel::isRunning() const {
    return m_isRunning;
}

int AnalysisModel::iterations() const {
    return (int)m_iterations;
}

double AnalysisModel::confidence() const {
    return m_confidence;
}

int AnalysisModel::plies() const {
    return m_plies;
}

bool AnalysisModel::compareDisplayItems(const AnalysisModel::DisplayItem &a, const AnalysisModel::DisplayItem &b) {
    if (a.winPct != b.winPct) {
        return a.winPct > b.winPct; // Sort by WinPct descending
    }
    return a.spread > b.spread; // Then by Spread descending
}

void AnalysisModel::onWorkerFinished() {
    qDebug() << "AnalysisModel: Worker finished";
    m_isRunning = false;
    emit isRunningChanged();
    m_updateTimer->stop();
    updateResults(); // Final update
}

void AnalysisModel::updateResults() {
    if (!m_results || !m_game) return;
    
    uint64_t iters = bridge_sim_results_get_iterations(m_results);
    if (iters != m_iterations) {
        m_iterations = iters;
        emit iterationsChanged();
    }
    
    double conf = bridge_sim_results_get_confidence(m_results);
    if (qAbs(conf - m_confidence) > 0.0001) {
        m_confidence = conf;
        emit confidenceChanged();
    }

    int count = bridge_sim_results_get_num_plays(m_results);
    // qDebug() << "Analysis update: iters=" << iters << " count=" << count;
    
    // If count changed (it shouldn't for fixed move list, but good to be safe)
    if (count != m_rowCount) {
        qDebug() << "AnalysisModel: row count changed to" << count;
        beginResetModel();
        m_rowCount = count;
        m_displayCache.resize(count);
        endResetModel();
    }
    
    // Update cache data
    for (int i = 0; i < count; ++i) {
        char* notation = nullptr;
        double win = 0;
        double spread = 0;
        int iters = 0;
        
        if (bridge_sim_results_get_play_info(m_game, m_results, i, &notation, &win, &spread, &iters) == 0) {
            m_displayCache[i].notation = QString::fromUtf8(notation);
            m_displayCache[i].winPct = win * 100.0;
            m_displayCache[i].spread = spread;
            // Rank is assigned after sorting.
            m_displayCache[i].iterations = iters;
            
            free(notation);
        }
    }

    // Sort the plays dynamically
    if (count > 0) {
        beginResetModel(); // Reset model to notify view of complete reordering
        std::sort(m_displayCache.begin(), m_displayCache.end(), AnalysisModel::compareDisplayItems);
        // Re-assign ranks based on the new sorted order
        for (int i = 0; i < count; ++i) {
            m_displayCache[i].rank = i + 1;
        }
        endResetModel();
    }
}
