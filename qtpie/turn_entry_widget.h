#ifndef TURN_ENTRY_WIDGET_H
#define TURN_ENTRY_WIDGET_H

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include "magpie_wrapper.h"

// Widget to display a single turn's information
// Format:
//   Top row: [MOVE NOTATION] (left)    [PREV SCORE +PLAY SCORE] (right)
//   Bottom row: [TIME RACK] (left)     [empty] (right)
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
    QString convertToStandardNotation(const QString &ucgiNotation);

    QLabel *m_prevScoreLabel;    // Previous score
    QLabel *m_playScoreLabel;    // +Play score
    QLabel *m_cumulativeLabel;   // Cumulative score (hidden)
    QLabel *m_notationLabel;     // Move notation (e.g., "8D FEVER")
    QLabel *m_timeLabel;         // Time at end of turn (e.g., "24:55")
    QLabel *m_rackLabel;         // Full rack (e.g., "AEEHOVZ")

    bool m_isCommitted;
    bool m_isValidated;
};

#endif // TURN_ENTRY_WIDGET_H
