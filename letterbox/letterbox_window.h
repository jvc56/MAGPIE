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
#include <QDateTime>
#include <vector>
#include <string>
#include <unordered_map>

extern "C" {
    #include "magpie_wrapper.h"
}

struct WordEntry {
    std::string word;
    bool revealed;
    bool missed;  // True if word was skipped without being revealed
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
    void skipCurrentAlphagram();
    void undoMarkAsMissed();
    void loadWordList();
    void createCustomWordList();
    void loadSavedList();  // Load from Recent Lists menu
    void clearRecentLists();
    void onTextChanged(const QString& text);
    void toggleFullscreen();
    void zoomIn();
    void zoomOut();
    void toggleDebugInfo();
    void toggleComputeTime();
    void toggleRenderTime();
    void onScrollChanged(int value);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

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

    // Word list persistence
    QString generateListName(const QString& pattern, int minCount, int maxCount, int totalSets, const QDateTime& timestamp);
    void saveCurrentList();
    void loadSavedListFromFile(const QString& filepath);
    void updateRecentListsMenu();
    void addToRecentLists(const QString& filepath);
    void checkForCompletion();
    void showCompletionStats();
    void createMissedWordsList();
    void updateWindowTitle();

    Config* config;
    KWG* kwg;
    LetterDistribution* ld;
    std::vector<AlphagramSet> allAlphagrams;  // All loaded alphagrams (before filtering)
    std::vector<AlphagramSet> alphagrams;      // Current working set (possibly filtered)
    int currentIndex;

    // Undo state for "Mark as missed" action
    int lastMissedIndex;  // Index of last alphagram marked as missed (-1 if none)
    QAction* undoAction;  // Reference to undo menu action for enabling/disabling

    // Word list persistence state
    QString currentListName;       // Human-readable name of current list
    QString currentListFilepath;   // Full path to saved list file
    QString currentPattern;        // Filter pattern used
    int currentMinAnagrams;        // Min anagram count filter
    int currentMaxAnagrams;        // Max anagram count filter
    QMenu* recentListsMenu;        // Recent Lists submenu

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
    int zoomLevelIndex;  // Index into zoomLevels array
    static const std::vector<double> zoomLevels;  // Chrome-like zoom levels
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
    QWidget* queueContainer;       // Container for queue area with counter overlay
    QLabel* queueLabel;            // Bottom: upcoming alphagrams
    QLabel* progressCounterLabel;  // Progress counter overlay (e.g., "1 / 50")
    QLabel* zoomIndicatorLabel;    // Zoom level indicator (e.g., "125%")
    QTimer* zoomIndicatorTimer;    // Timer for fading zoom indicator
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
    QAction* skipAction;
    bool showDebugInfo;
    bool showComputeTime;
    bool showRenderTime;
    int lastRenderTimeMicros;

    // Resize debounce timer
    QTimer* resizeTimer;
    void handleResizeComplete();

    // Zoom indicator
    void showZoomIndicator();
};

#endif // LETTERBOX_WINDOW_H
