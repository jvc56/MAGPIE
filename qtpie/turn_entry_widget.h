#ifndef TURN_ENTRY_WIDGET_H
#define TURN_ENTRY_WIDGET_H

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include "magpie_wrapper.h"

// Widget to display a single turn's information
// Format:
//   Row 1: [PREVIOUS SCORE]  [+PLAY SCORE]
//   Row 2: [CUMULATIVE SCORE]
//   Row 3: [MOVE NOTATION]
//   Row 4: [TIME]  [RACK]
class TurnEntryWidget : public QWidget {
    Q_OBJECT
public:
    explicit TurnEntryWidget(QWidget *parent = nullptr);

    // Update with committed move data
    void setCommittedMove(int prevScore, int playScore, int cumulativeScore,
                         const QString &notation, const QString &timeStr, const QString &rack);

    // Update with validated but uncommitted move data
    void setValidatedMove(int prevScore, int playScore,
                         const QString &notation, const QString &timeStr, const QString &rack);

    // Update with unvalidated/invalid move (notation only, no scores)
    void setUnvalidatedMove(const QString &notation, const QString &timeStr, const QString &rack);

    // Update just the timer (for live ticking during turn)
    void updateTime(const QString &timeStr);

    // Clear the entry
    void clear();

private:
    QLabel *m_prevScoreLabel;    // Previous score (left)
    QLabel *m_playScoreLabel;    // +Play score (right)
    QLabel *m_cumulativeLabel;   // Cumulative score
    QLabel *m_notationLabel;     // Move notation (e.g., "8D ZOEAE")
    QLabel *m_timeLabel;         // Time at end of turn (e.g., "24:55")
    QLabel *m_rackLabel;         // Full rack (e.g., "AEEHOVZ")

    bool m_isCommitted;
    bool m_isValidated;
};

#endif // TURN_ENTRY_WIDGET_H
