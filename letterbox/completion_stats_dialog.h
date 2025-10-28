#ifndef COMPLETION_STATS_DIALOG_H
#define COMPLETION_STATS_DIALOG_H

#include <QDialog>
#include <QPushButton>
#include <QLabel>

class CompletionStatsDialog : public QDialog {
    Q_OBJECT

public:
    explicit CompletionStatsDialog(int totalAlphagrams, int totalWords,
                                  int revealedWords, int missedWords,
                                  QWidget *parent = nullptr);

    bool shouldCreateMissedList() const { return createMissedList; }

private:
    bool createMissedList;
};

#endif // COMPLETION_STATS_DIALOG_H
