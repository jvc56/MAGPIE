#ifndef LETTERBOX_WINDOW_H
#define LETTERBOX_WINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QVBoxLayout>
#include <QProgressBar>
#include <QPushButton>
#include <QLineEdit>
#include <QScrollArea>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QResizeEvent>
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

struct HookExtensionCache {
    std::string frontHooks;
    std::string backHooks;
    std::string frontExtensions;
    std::string backExtensions;
    int computeTimeMicros;
    bool cached;
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
    void toggleDebugInfo();
    void toggleComputeTime();
    void toggleRenderTime();
    void onScrollChanged(int value);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void setupUI();
    void setupMenuBar();
    void updateDisplay();
    void updateProgress();
    void updateDebugInfo();
    void calculateGlobalMaxWidths();
    void updateScaledFontSizes();
    void loadPlayabilityScores(const QString& filepath);
    std::string sortVowelsFirst(const std::string& word);
    QString formatAlphagramSet(const AlphagramSet& set, bool showAll);
    const HookExtensionCache& getOrComputeHookExtensions(const std::string& word);
    QString sortExtensionsByPlayability(const QString& extensions, const QString& baseWord, bool isFront);

    Config* config;
    KWG* kwg;
    LetterDistribution* ld;
    std::vector<AlphagramSet> alphagrams;
    int currentIndex;

    // Playability scores for sorting extensions
    std::unordered_map<std::string, int> playabilityScores;

    // Cache for hook/extension computations
    std::unordered_map<std::string, HookExtensionCache> hookExtensionCache;

    // Global maximum widths for alignment (calculated once at startup)
    int globalMaxFrontWidth;
    int globalMaxBackWidth;
    int globalMaxWordWidth;

    // Rendering window control
    int renderWindowSize;
    bool userScrolledUp;

    // Responsive font scaling
    double scaleFactor;
    int scaledWordSize;
    int scaledHookSize;
    int scaledExtensionSize;
    int scaledInputSize;
    int scaledQueueCurrentSize;
    int scaledQueueUpcomingSize;

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

    // Debug UI
    QLabel* debugLabel;
    QLabel* renderTimeLabel;
    QAction* debugAction;
    QAction* computeTimeAction;
    QAction* renderTimeAction;
    bool showDebugInfo;
    bool showComputeTime;
    bool showRenderTime;
    int lastRenderTimeMicros;
};

#endif // LETTERBOX_WINDOW_H
