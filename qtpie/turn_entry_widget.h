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
    enum RenderMode {
        USE_QPUSHBUTTON,  // LEFT: Use QPushButton widgets (always render text)
        USE_MANUAL_PAINT  // RIGHT: Manual painting with QPainter (no child widgets)
    };

    explicit TurnEntryWidget(QWidget *parent = nullptr, RenderMode mode = USE_QPUSHBUTTON, int variant = 0);

    // Update with committed move data
    void setCommittedMove(int prevScore, int playScore, int cumulativeScore,
                         const QString &notation, const QString &timeStr, const QString &rack);

    // Update with validated but uncommitted move data
    void setValidatedMove(int prevScore, int playScore,
                         const QString &notation, const QString &timeStr, const QString &rack);

    // Update with unvalidated/invalid move (shows prevScore but no play score)
    void setUnvalidatedMove(int prevScore, const QString &notation, const QString &timeStr, const QString &rack);

    // Update just the timer (for live ticking during turn)
    void updateTime(const QString &timeStr);

    // Clear the entry
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString convertToStandardNotation(const QString &ucgiNotation);

    RenderMode m_renderMode;
    int m_variant;  // 0-5 for testing different approaches

    QLabel *m_prevScoreLabel;    // Previous score
    QLabel *m_playScoreLabel;    // +Play score
    QLabel *m_cumulativeLabel;   // Cumulative score (hidden)
    QLabel *m_notationLabel;     // Move notation (e.g., "8D FEVER")
    QLabel *m_timeLabel;         // Time at end of turn (e.g., "24:55")
    QLabel *m_rackLabel;         // Full rack (e.g., "AEEHOVZ")

    bool m_isCommitted;
    bool m_isValidated;

    // For manual painting mode (USE_MANUAL_PAINT)
    QString m_paintNotation;
    QString m_paintPrevScore;
    QString m_paintPlayScore;
    QString m_paintCumulative;
    QString m_paintTime;
    QString m_paintRack;
    bool m_showCumulative;
};

#endif // TURN_ENTRY_WIDGET_H
