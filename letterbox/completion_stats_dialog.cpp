#include "completion_stats_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>

CompletionStatsDialog::CompletionStatsDialog(int totalAlphagrams, int totalWords,
                                             int revealedWords, int missedWords,
                                             QWidget *parent)
    : QDialog(parent), createMissedList(false)
{
    setWindowTitle("Quiz Complete!");
    setMinimumWidth(400);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(20);
    mainLayout->setContentsMargins(30, 30, 30, 30);

    // Title
    QLabel *titleLabel = new QLabel("Quiz Complete!", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(24);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(titleLabel);

    // Stats
    QFont statsFont;
    statsFont.setPointSize(14);

    QLabel *alphagramsLabel = new QLabel(QString("Total alphagrams: %1").arg(totalAlphagrams), this);
    alphagramsLabel->setFont(statsFont);
    mainLayout->addWidget(alphagramsLabel);

    QLabel *totalWordsLabel = new QLabel(QString("Total words: %1").arg(totalWords), this);
    totalWordsLabel->setFont(statsFont);
    mainLayout->addWidget(totalWordsLabel);

    QLabel *revealedLabel = new QLabel(QString("Correctly revealed: %1").arg(revealedWords), this);
    revealedLabel->setFont(statsFont);
    revealedLabel->setStyleSheet("color: green;");
    mainLayout->addWidget(revealedLabel);

    QLabel *missedLabel = new QLabel(QString("Missed: %1").arg(missedWords), this);
    missedLabel->setFont(statsFont);
    missedLabel->setStyleSheet("color: red;");
    mainLayout->addWidget(missedLabel);

    // Calculate accuracy
    double accuracy = totalWords > 0 ? (100.0 * revealedWords / totalWords) : 0.0;
    QLabel *accuracyLabel = new QLabel(QString("Accuracy: %1%").arg(accuracy, 0, 'f', 1), this);
    QFont accuracyFont = statsFont;
    accuracyFont.setBold(true);
    accuracyLabel->setFont(accuracyFont);
    mainLayout->addWidget(accuracyLabel);

    mainLayout->addSpacing(20);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    // Create Missed Words List button (only if there are missed words)
    if (missedWords > 0) {
        QPushButton *missedListButton = new QPushButton("Create Missed Words List", this);
        connect(missedListButton, &QPushButton::clicked, this, [this]() {
            createMissedList = true;
            accept();
        });
        buttonLayout->addWidget(missedListButton);
    }

    QPushButton *doneButton = new QPushButton("Done", this);
    doneButton->setDefault(true);
    connect(doneButton, &QPushButton::clicked, this, &QDialog::accept);
    buttonLayout->addWidget(doneButton);

    mainLayout->addLayout(buttonLayout);
}
