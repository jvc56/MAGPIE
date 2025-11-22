#ifndef ANALYSISMODEL_H
#define ANALYSISMODEL_H

#include <QAbstractListModel>
#include <QThread>
#include <QTimer>
#include <QMutex>
#include "../bridge/qt_bridge.h"

class AnalysisWorker : public QObject {
    Q_OBJECT
public:
    BridgeGame* game = nullptr;
    BridgeMoveList* moves = nullptr;
    BridgeSimResults* results = nullptr;
    BridgeThreadControl* tc = nullptr;
    int plies = 6;

public slots:
    void run();
    
signals:
    void finished();
};

class AnalysisModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool isRunning READ isRunning NOTIFY isRunningChanged)
    Q_PROPERTY(int iterations READ iterations NOTIFY iterationsChanged)
    Q_PROPERTY(double confidence READ confidence NOTIFY confidenceChanged)
    Q_PROPERTY(int plies READ plies NOTIFY pliesChanged)

public:
    enum Roles {
        RankRole = Qt::UserRole + 1,
        NotationRole,
        WinPctRole,
        SpreadRole,
        IterationsRole
    };

    explicit AnalysisModel(QObject *parent = nullptr);
    ~AnalysisModel();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void startAnalysis(BridgeGame* game);
    void stopAnalysis();
    
    bool isRunning() const;
    int iterations() const;
    double confidence() const;
    int plies() const;

signals:
    void isRunningChanged();
    void iterationsChanged();
    void confidenceChanged();
    void pliesChanged();

private slots:        void onWorkerFinished();
        void updateResults();

    private:
        BridgeGame* m_game = nullptr; // We don't own this
        
        // Analysis state
        BridgeMoveList* m_moves = nullptr;
        BridgeSimResults* m_results = nullptr;
        BridgeThreadControl* m_tc = nullptr;
        
        QThread* m_thread = nullptr;
        AnalysisWorker* m_worker = nullptr;
        QTimer* m_updateTimer = nullptr;
        
        bool m_isRunning = false;
            uint64_t m_iterations = 0;
            double m_confidence = 0.0;
            int m_plies = 0; // Stored here
            int m_rowCount = 0;
                    
        struct DisplayItem {
            QString notation;
            double winPct;
            double spread;
            int rank;
            int iterations; // Added
        };
        QVector<DisplayItem> m_displayCache;

        static bool compareDisplayItems(const AnalysisModel::DisplayItem &a, const AnalysisModel::DisplayItem &b);
    };
#endif // ANALYSISMODEL_H
