#ifndef WORD_WALL_H
#define WORD_WALL_H

// Word Walls mode: an Aerolith-style timed grid quiz, ported from wordmonger.
// Each question occupies as many grid rows as it has answers.

#include <QDialog>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QGraphicsBlurEffect>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "letterbox_window.h"

class QCheckBox;
class QComboBox;
class QSpinBox;
class QPushButton;
class QTableWidget;

enum class WordWallOrder {
    Alphagram, Probability, Playability, Random,
    Familiarity,  // Most familiar first
    Difficulty,   // Hardest anagram first
    Frequency,    // Most common in English first
    Newness       // Newest to CSW first
};

enum class NwlFilter { Any, HasCswOnly, AllInNwl };

// What a question asks for
enum class WordWallType {
    Anagrams,   // Every anagram of an alphagram
    WordStems,  // Every word made from a word plus a blank (APNOEAL + ? = JALAPENO)
    RackStems   // Every word made from any rack plus a blank
};


// Inclusive range of percentiles (0-100) within the quiz's word length
struct PercentileRange {
    int min = 0;
    int max = 100;
    bool contains(double percentile) const { return percentile >= min && percentile <= max; }
};

struct WordWallSettings {
    WordWallType type = WordWallType::Anagrams;
    int length = 7;        // Length of the answers (a blank question's stem is one shorter)
    int minAnagrams = 1;
    int maxAnagrams = 999;
    WordWallOrder order = WordWallOrder::Probability;
    bool reverse = false;
    bool groupByAnswers = false;  // Fewest answers first, each group in the chosen order
    bool dropSHooks = false;      // Blank questions: omit unistems whose answer is the stem + S (PEPTALK + ?)
    PercentileRange familiarity;
    PercentileRange difficulty;
    PercentileRange frequency;
    PercentileRange playability;
    PercentileRange probability;
    int addedSince = 0;  // CSW edition year a set must have a word from (or later); 0 for any
    NwlFilter nwl = NwlFilter::Any;
    int rows = 10;
    int cols = 5;
    // Great Wall: pick grids from a map, misses stay missed, and the clock counts up
    bool greatWall = false;
    bool autoTime = true;  // Tune each page's limit from past performance
    int seconds = 270;     // Fixed limit per page when autoTime is off
};

// Per-word study features from csw24_features.tsv (see tools/build_word_features.py)
struct WordFeatures {
    int firstCsw = 2007;      // First CSW edition with the word
    bool inNwl = true;        // Also valid in NWL23
    double lemmaFreq = 0.0;   // English frequency of the lemma family, per million
    double surprise = 0.0;    // Bits of letter-order surprise (higher = harder to anagram)
    double familiarity = 50;  // 0-100: how likely the word is already well known
};

// Where a question sits in a grid; it spans one row per answer
struct WallPlacement {
    int row;
    int col;
    int span;
};

// Questions packed into a grid: columns fill top to bottom. The grid only
// gets taller than the rows asked for if a question can't fit in an empty column.
struct WallGridPacking {
    std::vector<WallPlacement> placements;  // One per question, from the start index
    int rows = 0;
    bool full = false;  // No room for another question (or none left over)
};
// spanAt(i) is question i's number of answers; questions run from start to end
WallGridPacking packWallGrid(const std::function<int(size_t)>& spanAt, size_t start, size_t end, int rows, int cols);

// A question as shown in the setup dialog's preview
struct WordWallPreviewRow {
    QString clue;
    QStringList answers;
    int playability = 0;          // Summed over the answers
    double playabilityPct = 0;
    double probabilityPct = 0;
    double familiarityPct = 0;
};

// Scrollable thumbnails of every grid in a list
class WallGridPreview : public QWidget {
public:
    struct Page {
        size_t start;  // Index of the page's first question in the list
        WallGridPacking packing;
    };
    explicit WallGridPreview(QWidget* parent = nullptr);
    // rowAt(i) describes question i; isDone(i) says whether it's already done
    void setPages(std::vector<Page> pages, int cols, std::function<const WordWallPreviewRow&(size_t)> rowAt,
                  std::function<bool(size_t)> isDone);
    void setShowAnswers(bool show);

    // For the Great Wall map: every grid fits in view as plain rectangles,
    // each answer row filled with its status color; hovering shows the grid's
    // label and clicking opens it
    bool fitAll = false;
    std::function<QColor(size_t question, int answer)> answerFill;
    std::function<QString(size_t page)> pageLabel;
    std::function<void(size_t page)> onPageClicked;
    std::function<void(int page)> onHoverChanged;  // -1 when not over a grid
    void setHighlightPage(int page);
    QRect pageRect(size_t page) const;  // Including its label
    size_t pageCount() const { return pages.size(); }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void updateLayout();
    void paintPage(QPainter& painter, const Page& page, const QRect& rect) const;
    int pageAt(const QPoint& pos) const;
    int labelHeight() const;

    std::vector<Page> pages;
    std::vector<QRect> thumbRects;  // Grid area of each page
    int cols = 1;
    bool showAnswers = false;
    int hoverPage = -1;
    int highlightPage = -1;
    std::function<const WordWallPreviewRow&(size_t)> rowAt;
    std::function<bool(size_t)> isDone;
};

class WordWallSetupDialog : public QDialog {
    Q_OBJECT

public:
    struct Callbacks {
        // The questions matching the settings, in quiz order
        std::function<std::vector<std::string>(const WordWallSettings&)> buildList;
        std::function<WordWallPreviewRow(const WordWallSettings&, const std::string& key)> describe;
        // Number of answers (cheap, for laying out grids)
        std::function<int(const WordWallSettings&, const std::string& key)> answerCount;
    };
    // Editing an existing list when doneKeys (the questions done so far) is given
    WordWallSetupDialog(const WordWallSettings& initial, Callbacks callbacks,
                        const std::set<std::string>* doneKeys = nullptr, QWidget* parent = nullptr);
    WordWallSettings settings() const;
    // The previewed list, which is the one to quiz
    const std::vector<std::string>& list() const { return builtList; }
    bool keepProgress() const;

private slots:
    void refresh();

private:
    void fillPreview();
    const WordWallPreviewRow& previewRow(size_t index) const;

    struct RangeSpins {
        QSpinBox* min;
        QSpinBox* max;
    };
    RangeSpins addRangeRow(class QFormLayout* form, const QString& label, const PercentileRange& range);
    static PercentileRange rangeFrom(const RangeSpins& spins);

    Callbacks callbacks;
    const std::set<std::string>* doneKeys;
    std::vector<std::string> builtList;
    mutable std::unordered_map<size_t, WordWallPreviewRow> previewRows;  // By list index
    QTimer* refreshTimer;
    QTableWidget* previewTable;
    WallGridPreview* gridPreview;
    QCheckBox* showAnswersCheck;
    QCheckBox* keepProgressCheck = nullptr;
    QComboBox* modeCombo;
    QComboBox* typeCombo;
    QLabel* lengthLabel;
    QSpinBox* lengthSpin;
    QSpinBox* minAnagramSpin;
    QSpinBox* maxAnagramSpin;
    RangeSpins familiaritySpins;
    RangeSpins difficultySpins;
    RangeSpins frequencySpins;
    RangeSpins playabilitySpins;
    RangeSpins probabilitySpins;
    QComboBox* addedSinceCombo;
    QComboBox* nwlCombo;
    QComboBox* orderCombo;
    QCheckBox* reverseCheck;
    QCheckBox* groupCheck;
    QCheckBox* dropSHooksCheck;
    QSpinBox* rowsSpin;
    QSpinBox* colsSpin;
    QSpinBox* secondsSpin;
    QCheckBox* autoTimeCheck;
    QLabel* countLabel;
    QPushButton* startButton;
};

struct WallQuestion {
    // Vowels-first alphagram; for a blank question, the stem's followed by ?
    std::string alphagram;
    std::string clue;                 // As displayed
    std::vector<std::string> answers; // Alphabetical
    std::set<int> solved;             // Indices into answers
};

// A saved Word Walls list, as shown in the Recent Lists menu
struct WordWallListSummary {
    QString path;
    QString name;
    qint64 position;  // Questions done in the current pass
    qint64 total;     // Questions in the current pass
    qint64 missed;    // Missed so far in the current pass
    bool active;      // Not yet finished (questions left, or misses to retry)
};

class WordWallWidget;

class WordWallCell : public QWidget {
public:
    enum class Flash { None, Phony, Duplicate };

    WordWallCell(WordWallWidget* wall, int questionIndex, QWidget* parent = nullptr);
    // Briefly highlight the border (and, for a duplicate, the repeated answer)
    void flash(Flash kind, int answer = -1);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    WordWallWidget* wall;
    int questionIndex;
    Flash flashKind = Flash::None;
    int flashAnswer = -1;
    QTimer* flashTimer;
};

class WordWallWidget : public QWidget {
    Q_OBJECT

public:
    WordWallWidget(const std::vector<AlphagramSet>& allAlphagrams,
                   const std::unordered_map<std::string, int>& playabilityScores,
                   const QString& featuresPath,
                   KWG* kwg, LetterDistribution* ld, QWidget* parent = nullptr);
    ~WordWallWidget() override;

    // Opens the setup dialog; returns true if a new list was started
    bool newQuiz();
    // Changes the current list's settings; returns true if it was changed
    bool editList();
    // Restores the most recent list; returns true if one was found
    bool loadSavedState();
    // Switches to a saved list; a page in progress is dropped (it isn't saved)
    bool openList(const QString& path);
    QString currentListPath() const { return currentPath; }
    // Saved lists, most recently used first
    static std::vector<WordWallListSummary> recentLists();

    void togglePause();
    void giveUp();
    void focusInput();
    QString listName() const;

    // Used by WordWallCell
    const WallQuestion& question(int index) const { return questions[index]; }
    bool pageFinished() const { return finished; }
    // The answer to type next after a failed review, if it's in this question; otherwise -1
    int copyTargetAnswer(int questionIndex) const;
    void hooksFor(const std::string& word, QString* front, QString* back);
    bool isCswOnly(const std::string& word) const;

signals:
    void listNameChanged(const QString& name);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void onTextChanged(const QString& text);
    void onReturnPressed();
    void onTick();

private:
    // A question's features. A question is as familiar as its least familiar
    // answer, as hard as its most surprising one, and as common as its rarest.
    struct Candidate {
        std::string key;    // Vowels-first alphagram (plus ? for a blank question)
        std::string sorted; // Alphabetical alphagram (the stem, for a blank question)
        std::string stemWord; // Most playable word made from the stem, if any
        int anagrams;       // Answers
        int playability;    // Summed over the answers
        double probability; // Ways to draw the rack, counting blanks
        double familiarity;
        double difficulty;
        double frequency;
        int newest;         // Newest CSW edition among the answers
        int nwlCount;       // Answers also valid in NWL
        // Percentiles within sets of the same length
        double familiarityPct;
        double difficultyPct;
        double frequencyPct;
        double playabilityPct;
        double probabilityPct;
    };
    void loadFeatures(const QString& path, const std::unordered_map<std::string, int>& playabilityScores);
    // What decides a question's answers
    struct QuestionKind {
        WordWallType type;
        bool dropSHooks;
    };
    static QuestionKind kindOf(const WordWallSettings& s) {
        // Only matters when single-answer questions are included
        return {s.type, s.type != WordWallType::Anagrams && s.dropSHooks && s.minAnagrams <= 1};
    }
    const std::vector<Candidate>& candidatesFor(QuestionKind kind, int length) const;
    void addFeatures(Candidate* c, const std::vector<std::string>& words) const;
    // Whether word is a word made from the stem (a vowels-first alphagram) plus a final S
    bool isPlainSHook(const std::string& word, const std::string& stemKey) const;
    static bool isBlankKey(const std::string& key) { return !key.empty() && key.back() == '?'; }
    bool validKey(const std::string& key) const;
    std::string clueFor(const std::string& key) const { return clueFor(key, kindOf(settings)); }
    std::string clueFor(const std::string& key, QuestionKind kind) const;
    // Whether guess uses a question's letters (a phony answer to it)
    bool isPhonyFor(const WallQuestion& q, const std::string& guess) const;
    std::vector<const Candidate*> filteredCandidates(const WordWallSettings& settings) const;
    std::vector<std::string> buildList(const WordWallSettings& settings) const;
    std::vector<std::string> answersFor(const std::string& alphagram) const;
    double drawWays(const std::string& alphagram) const;
    void startList(const std::vector<std::string>& list, bool retryingMissed);
    void startPage();
    // Quizzes the first grid of misses waiting for review
    void startReviewPage();
    // Shows the first grid of misses waiting for review, answers revealed
    void showMisses();
    void finishPage(const QString& reason);
    void clearGrid();
    void layoutCells();
    void updateStatus();
    void flashInput(const QColor& color);
    void saveState() const;
    void rememberList() const;
    QJsonObject settingsJson() const;
    const Candidate* candidateFor(const std::string& alphagram) const;
    const Candidate* candidateFor(const std::string& alphagram, QuestionKind kind) const;
    WordWallSetupDialog::Callbacks setupCallbacks() const;
    // Milliseconds of quiz time used on the current page (excludes pauses)
    int quizClockMillis() const;
    // Logs a Word Walls event tagged with the current list, page and quiz clock
    void logEvent(const QString& event, QJsonObject fields = {}) const;

    const std::vector<AlphagramSet>& allAlphagrams;
    std::unordered_map<std::string, const AlphagramSet*> setsByAlphagram;
    // Keyed by type and length
    mutable std::unordered_map<int, std::vector<Candidate>> candidateCache;
    mutable std::unordered_map<int, std::unordered_map<std::string, const Candidate*>> candidateByKey;
    const std::unordered_map<std::string, int>& playabilityScores;
    std::unordered_map<std::string, WordFeatures> wordFeatures;
    KWG* kwg;
    LetterDistribution* ld;

    WordWallSettings settings;
    QString currentPath;               // Save file for the current list
    std::vector<std::string> list;     // Vowels-first alphagrams in quiz order
    std::vector<std::string> missed;   // Missed so far in this pass through the list
    // Times each question was missed this pass: once on its page, plus once
    // for each failed review try
    std::unordered_map<std::string, int> missCounts;
    std::vector<std::string> missedAtLeast(int times) const;
    // Misses waiting to be reviewed, oldest first. Each grid's worth is quizzed
    // until every answer is found in time, then dropped from the queue.
    std::vector<std::string> missQueue;
    size_t position = 0;               // Index of the first question on the current page
    size_t pageSize = 0;               // Number of questions on the current page
    int pass = 1;                      // 1 for the original list, 2+ for missed retries
    bool retryingMissed = false;

    int gridRows = 0;                  // Rows on the current page (at least settings.rows)

    // What the grid is showing
    enum class View {
        Page,    // A page of the list
        Review,  // A grid of misses being quizzed
        Misses,  // The misses waiting for review, answers revealed
        Map      // Great Wall: every grid in the list, to pick from
    };
    View view = View::Page;
    int reviewAttempt = 0;             // Tries at the current grid of misses
    // After a failed review, each missed word (question, answer) is typed out
    // once, in grid order, before the grid is tried again
    std::vector<std::pair<int, int>> copyQueue;
    size_t copyIndex = 0;
    bool copying() const { return copyIndex < copyQueue.size(); }
    // Checks text typed while copying out missed words
    void checkCopy(const QString& text, bool submitted);

    std::vector<WallQuestion> questions;
    using Placement = WallPlacement;
    std::vector<Placement> placements;

    // Questions packed into a grid: columns fill top to bottom, and a question
    // spans one row per answer
    struct GridLayout {
        std::vector<WallQuestion> questions;
        std::vector<Placement> placements;
        int rows = 0;
        bool full = false;  // No room for another question (or none left over)
    };
    GridLayout packGrid(const std::vector<std::string>& keys, size_t start) const;
    // Replaces the grid's questions and cells
    void showGrid(GridLayout layout);
    // Starts the clock on the questions in the grid
    void beginTimedPage();
    bool allSolved() const;
    // A grid of misses is due when it's full, or at the end of the list
    bool reviewDue() const;
    // word -> (question, answer) for each question it answers (with blanks, a
    // word can answer more than one)
    std::unordered_map<std::string, std::vector<std::pair<int, int>>> answerIndex;
    std::unordered_map<std::string, std::pair<QString, QString>> hookCache;

    // A finished page, for tuning automatic time limits
    struct PageRecord {
        WordWallType type;
        int length;
        double familiarityPct;  // Mean familiarity percentile of the page's sets
        int words;
        bool completed;
        // Time until done: when the last word was found if the page wasn't
        // finished, since waiting past that doesn't help
        double seconds;
    };
    std::vector<PageRecord> pageHistory;
    void loadPageHistory();
    double pageFamiliarityPct() const;
    // Limit for the current page, and a description of how it was chosen
    int automaticLimitMillis(QJsonObject* basis) const;

    int pageLimitMillis = 0;           // Time limit for the current page
    int lastSolveMillis = 0;           // Quiz clock at the latest solve on this page
    QString pageId;                    // Ties a page's logged events together
    bool finished = true;
    bool paused = false;
    int remainingMillis = 0;
    QElapsedTimer elapsed;
    QTimer* tickTimer;

    QWidget* gridContainer;
    QGraphicsBlurEffect* blurEffect;
    std::vector<WordWallCell*> cells;
    QLineEdit* inputField;
    QLabel* progressLabel;
    QLabel* timeLabel;
    QLabel* scoreLabel;
    QTimer* wrongFlashTimer;

    // Great Wall
    struct WallResult {
        std::set<std::string> solved;
        std::set<std::string> missed;  // Not solved yet, as of leaving its grid
    };
    std::unordered_map<std::string, WallResult> wallResults;  // By question key
    std::unordered_map<std::string, qint64> wallMillis;      // Time spent, by the grid's first question key
    std::unordered_map<std::string, qint64> wallVisited;     // Last left (ms since epoch), by the same key
    QString wallHoverCard(size_t page, bool lastPlayed) const;
    QString wallOverallCard() const;
    void updateMapFooter();
    int mapHoverPage = -1;
    QWidget* mapFooter;
    QLabel* mapGridLabel;
    QLabel* mapOverallLabel;
    std::vector<WallGridPreview::Page> wallPages;
    std::vector<int> wallSpans;        // Answers per question, by list index
    const WordWallPreviewRow& wallRow(size_t index) const;
    mutable std::unordered_map<size_t, WordWallPreviewRow> wallRows;  // By list index, for the map
    int currentWallPage = -1;
    bool typedThisVisit = false;       // Whether anything was typed on this visit to a grid
    QElapsedTimer lastTyping;          // For pausing after a while without typing
    class QScrollArea* mapScroll;
    WallGridPreview* mapPreview;
    void buildWallPages();
    void showMap();
    void startWallPage(size_t page);
    // The first grid with unstudied words, else with missed ones, else -1
    int nextWallPage() const;
    void setMapVisible(bool visible);
    void autoPause(const QString& why);
    std::string wallPageKey(size_t page) const;
    struct WallCounts {
        qint64 words = 0;
        qint64 solved = 0;
        qint64 missed = 0;
        qint64 studiedQuestions = 0;
        qint64 missedQuestions = 0;
    };
    WallCounts wallCounts(size_t from = 0, size_t to = SIZE_MAX) const;  // Over list indices [from, to)
};

#endif // WORD_WALL_H
