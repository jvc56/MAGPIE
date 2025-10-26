#ifndef LETTERBOX_WINDOW_H
#define LETTERBOX_WINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QVBoxLayout>
#include <QProgressBar>
#include <QPushButton>
#include <QLineEdit>
#include <QScrollArea>
#include <vector>
#include <string>
#include <unordered_map>

extern "C" {
    #include "magpie_wrapper.h"
}

struct WordEntry {
    std::string word;
    bool revealed;
};

struct AlphagramSet {
    std::string alphagram;  // Vowels-first sorted
    std::vector<WordEntry> words;
    int totalPlayability;
    bool studied;
};

class LetterboxWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit LetterboxWindow(QWidget *parent = nullptr);
    ~LetterboxWindow();

private slots:
    void nextWord();
    void markStudied();
    void loadWordList();
    void onTextChanged(const QString& text);

private:
    void setupUI();
    void updateDisplay();
    void updateProgress();
    std::string sortVowelsFirst(const std::string& word);
    QString formatAlphagramSet(const AlphagramSet& set, bool showAll);

    Config* config;
    KWG* kwg;
    LetterDistribution* ld;
    std::vector<AlphagramSet> alphagrams;
    int currentIndex;

    // UI elements
    QWidget* centralWidget;
    QVBoxLayout* mainLayout;
    QWidget* solvedWidget;         // Container for solved alphagrams
    QVBoxLayout* solvedLayout;     // Layout for solved alphagrams
    QScrollArea* solvedScrollArea; // Scroll area for solved section
    QLineEdit* inputField;         // Middle: answer input
    QLabel* queueLabel;            // Bottom: upcoming alphagrams
    QLabel* progressLabel;
    QProgressBar* progressBar;
    QPushButton* nextButton;
    QPushButton* studiedButton;
};

#endif // LETTERBOX_WINDOW_H
