#include "letterbox_window.h"
#include "alphagram_box.h"
#include "word_list_dialog.h"
#include "completion_stats_dialog.h"
#include <QFile>
#include <QTextStream>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QMessageBox>
#include <QDir>
#include <QCoreApplication>
#include <QDebug>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QShortcut>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QStandardPaths>
#include <QRegularExpression>
#include <algorithm>
#include <unordered_map>
#include <chrono>
#include <vector>

// Chrome-like zoom levels (in percentages converted to scale factors)
const std::vector<double> LetterboxWindow::zoomLevels = {
    0.25, 0.33, 0.50, 0.67, 0.75, 0.80, 0.90, 1.00,  // 25% to 100%
    1.10, 1.25, 1.50, 1.75, 2.00, 2.50, 3.00, 4.00, 5.00  // 110% to 500%
};

LetterboxWindow::LetterboxWindow(QWidget *parent)
    : QMainWindow(parent), config(nullptr), kwg(nullptr), ld(nullptr), currentIndex(0),
      lastMissedIndex(-1), undoAction(nullptr),
      currentMinAnagrams(1), currentMaxAnagrams(999), recentListsMenu(nullptr),
      globalMaxFrontWidth(0), globalMaxBackWidth(0), globalMaxWordWidth(0),
      renderWindowSize(15), userScrolledUp(false),
      scaleFactor(1.0), zoomLevelIndex(7), scaledWordSize(36), scaledHookSize(24), scaledExtensionSize(14),
      scaledInputSize(20), scaledQueueCurrentSize(24), scaledQueueUpcomingSize(16),
      showDebugInfo(false), showComputeTime(false), showRenderTime(false), showHoverDebugInfo(false),
      lastRenderTimeMicros(0)
{
    setupUI();
    setupMenuBar();

    // Initially disable skip action until a word list is loaded
    if (skipAction) {
        skipAction->setEnabled(false);
    }

    // Setup resize debounce timer
    resizeTimer = new QTimer(this);
    resizeTimer->setSingleShot(true);
    resizeTimer->setInterval(100);  // 100ms delay after resize stops
    connect(resizeTimer, &QTimer::timeout, this, &LetterboxWindow::handleResizeComplete);

    updateScaledFontSizes();

    // Initialize MAGPIE
    // Get the application directory (where the .app bundle is)
    QString appDir = QCoreApplication::applicationDirPath();
    // On macOS, look for data in the app bundle Resources directory
    // Path: Letterbox.app/Contents/MacOS -> ../Resources/data
    QDir dir(appDir);
    dir.cdUp();  // Contents
    QString dataPath = dir.absolutePath() + "/Resources/data";

    qDebug() << "App dir:" << appDir;
    qDebug() << "Data path:" << dataPath;
    qDebug() << "Data exists:" << QDir(dataPath).exists();

    config = letterbox_create_config(dataPath.toUtf8().constData(), "CSW24");

    if (!config) {
        QMessageBox::critical(this, "Error", "Failed to load MAGPIE config. Make sure data/ symlink exists.");
        return;
    }

    kwg = letterbox_get_kwg(config);
    ld = letterbox_get_ld(config);
    if (!kwg || !ld) {
        QMessageBox::critical(this, "Error", "Failed to load KWG lexicon or letter distribution.");
        return;
    }

    loadWordList();

    // Automatically load the most recently used list on startup
    QSettings settings("Letterbox", "Letterbox");
    QStringList recentLists = settings.value("recentLists").toStringList();
    if (!recentLists.isEmpty()) {
        QString mostRecentList = recentLists.first();
        if (QFile::exists(mostRecentList)) {
            loadSavedListFromFile(mostRecentList);
        }
    }

    if (!alphagrams.empty()) {
        updateDisplay();
    }
}

LetterboxWindow::~LetterboxWindow()
{
    if (config) {
        letterbox_destroy_config(config);
    }
}

void LetterboxWindow::setupUI()
{
    setWindowTitle("Letterbox");
    resize(800, 800);

    centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    // Dark theme background
    centralWidget->setStyleSheet("background-color: #2b2b2b;");

    mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Top 2/3: Solved alphagrams (scrollable, most recent at bottom)
    // Wrap in a container to allow absolute positioning of word hover overlay
    solvedContainer = new QWidget(this);
    solvedContainer->setMouseTracking(true);  // Enable mouse tracking for the container
    QVBoxLayout* solvedContainerLayout = new QVBoxLayout(solvedContainer);
    solvedContainerLayout->setContentsMargins(0, 0, 0, 0);
    solvedContainerLayout->setSpacing(0);

    solvedScrollArea = new QScrollArea(solvedContainer);
    solvedScrollArea->setWidgetResizable(true);
    solvedScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    solvedScrollArea->setStyleSheet(
        "QScrollArea { border: none; background: transparent; }"
        "QScrollBar:vertical {"
        "    border: none;"
        "    background: transparent;"
        "    width: 8px;"
        "    margin: 0px;"
        "}"
        "QScrollBar::handle:vertical {"
        "    background: rgba(100, 100, 100, 0);"
        "    border-radius: 4px;"
        "    min-height: 20px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "    background: rgba(150, 150, 150, 200);"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "    height: 0px;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "    background: none;"
        "}"
    );

    solvedWidget = new QWidget();
    solvedWidget->setMouseTracking(true);  // Enable mouse tracking for event propagation
    solvedLayout = new QVBoxLayout(solvedWidget);
    solvedLayout->setAlignment(Qt::AlignBottom | Qt::AlignHCenter);
    solvedLayout->setSpacing(10);  // 10px vertical spacing between boxes (2.5x more than 4px)

    solvedScrollArea->setWidget(solvedWidget);
    solvedScrollArea->setMouseTracking(true);  // Enable mouse tracking on scroll area
    solvedContainerLayout->addWidget(solvedScrollArea);

    // Word hover overlay (top-left or top-right corner)
    wordHoverOverlay = new QLabel(solvedContainer);
    wordHoverOverlay->setStyleSheet(
        "background-color: rgb(27, 27, 27); "
        "color: white; "
        "padding: 8px; "
        "font-family: 'Jost', sans-serif; "
        "border: 1px solid rgb(90, 90, 90);"
    );
    wordHoverOverlay->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    wordHoverOverlay->setTextFormat(Qt::RichText);
    wordHoverOverlay->setFixedWidth(200);
    wordHoverOverlay->hide();

    // Debug label for hover detection (bottom-left corner)
    hoverDebugLabel = new QLabel(solvedContainer);
    hoverDebugLabel->setStyleSheet(
        "background-color: rgba(0, 0, 0, 180); "
        "color: yellow; "
        "padding: 5px; "
        "font-family: 'Courier', monospace; "
        "font-size: 10px;"
    );
    hoverDebugLabel->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
    hoverDebugLabel->setText("Hover debug");
    hoverDebugLabel->hide();  // Start hidden, toggled by Debug menu
    hoverDebugLabel->raise();

    mainLayout->addWidget(solvedContainer, 4);

    // Middle: Input field (full width)
    inputField = new QLineEdit(this);
    inputField->setPlaceholderText("·······");  // Default placeholder with middle dots
    inputField->setAlignment(Qt::AlignCenter);
    inputField->setStyleSheet("QLineEdit { padding: 15px; font-family: 'Jost', sans-serif; font-size: 20px; font-weight: bold; margin: 2px 0px; background-color: rgb(40, 40, 40); color: white; border: 2px solid #3a7f9f; text-transform: uppercase; }");

    // Install event filter to intercept Cmd-Z before the text field gets it
    inputField->installEventFilter(this);

    connect(inputField, &QLineEdit::textChanged, this, [this](const QString& text) {
        // Force uppercase without triggering another textChanged signal
        if (text != text.toUpper()) {
            int cursorPos = inputField->cursorPosition();
            inputField->blockSignals(true);
            inputField->setText(text.toUpper());
            inputField->setCursorPosition(cursorPos);
            inputField->blockSignals(false);
        }
        onTextChanged(text.toUpper());
    });
    mainLayout->addWidget(inputField);

    // Bottom: Queue of upcoming alphagrams (scrollable, ~20% of space, 4:1 ratio)
    // Wrap in a container to allow absolute positioning of progress counter
    queueContainer = new QWidget(this);
    QVBoxLayout* queueContainerLayout = new QVBoxLayout(queueContainer);
    queueContainerLayout->setContentsMargins(0, 0, 0, 0);
    queueContainerLayout->setSpacing(0);

    QScrollArea* queueScrollArea = new QScrollArea(queueContainer);
    queueScrollArea->setWidgetResizable(true);
    queueScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    queueScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    queueScrollArea->setStyleSheet(
        "QScrollArea { border: none; background: transparent; }"
        "QScrollBar:vertical {"
        "    border: none;"
        "    background: transparent;"
        "    width: 8px;"
        "    margin: 0px;"
        "}"
        "QScrollBar::handle:vertical {"
        "    background: rgba(100, 100, 100, 0);"
        "    border-radius: 4px;"
        "    min-height: 20px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "    background: rgba(150, 150, 150, 200);"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "    height: 0px;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "    background: none;"
        "}"
    );

    QWidget* queueWidget = new QWidget();
    QVBoxLayout* queueLayout = new QVBoxLayout(queueWidget);
    queueLayout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    queueLayout->setContentsMargins(0, 0, 0, 0);

    queueLabel = new QLabel("", queueWidget);
    queueLabel->setAlignment(Qt::AlignCenter | Qt::AlignTop);
    queueLabel->setWordWrap(true);
    queueLabel->setTextFormat(Qt::RichText);
    queueLabel->setStyleSheet("QLabel { padding: 0px; margin: 0px; color: #888; }");
    queueLayout->addWidget(queueLabel);

    queueScrollArea->setWidget(queueWidget);
    queueContainerLayout->addWidget(queueScrollArea);

    // Progress counter overlay (bottom right corner)
    progressCounterLabel = new QLabel(queueContainer);
    progressCounterLabel->setStyleSheet(
        "QLabel { "
        "  color: #fff; "
        "  background-color: rgb(28, 28, 28); "
        "  font-family: 'Jost', sans-serif; "
        "  font-size: 14px; "
        "  font-weight: bold; "
        "  padding: 4px 8px; "
        "  border-radius: 4px; "
        "}"
    );
    progressCounterLabel->setAlignment(Qt::AlignCenter);
    progressCounterLabel->setAttribute(Qt::WA_TransparentForMouseEvents);  // Don't block mouse events
    progressCounterLabel->raise();  // Keep on top
    progressCounterLabel->hide();  // Start hidden, will be shown in updateDisplay()

    // Zoom indicator overlay (above progress counter, bottom right corner)
    zoomIndicatorLabel = new QLabel(queueContainer);
    zoomIndicatorLabel->setStyleSheet(
        "QLabel { "
        "  color: #fff; "
        "  background-color: rgb(28, 28, 28); "
        "  font-family: 'Jost', sans-serif; "
        "  font-size: 18px; "
        "  font-weight: bold; "
        "  padding: 8px 12px; "
        "  border-radius: 4px; "
        "}"
    );
    zoomIndicatorLabel->setAlignment(Qt::AlignCenter);
    zoomIndicatorLabel->setAttribute(Qt::WA_TransparentForMouseEvents);  // Don't block mouse events
    zoomIndicatorLabel->raise();  // Keep on top
    zoomIndicatorLabel->hide();  // Start hidden

    // Setup zoom indicator fade-out timer
    zoomIndicatorTimer = new QTimer(this);
    zoomIndicatorTimer->setSingleShot(true);
    connect(zoomIndicatorTimer, &QTimer::timeout, [this]() {
        zoomIndicatorLabel->hide();
    });

    mainLayout->addWidget(queueContainer, 1);

    // Debug info label (initially hidden)
    debugLabel = new QLabel(this);
    debugLabel->setStyleSheet("QLabel { color: #00ff00; background-color: rgba(0, 0, 0, 180); padding: 10px; font-family: monospace; font-size: 12px; }");
    debugLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    debugLabel->setVisible(false);
    debugLabel->raise(); // Keep it on top

    // Render time label (created on demand in updateDisplay)
    renderTimeLabel = nullptr;

    // Progress info (keep but don't add buttons)
    progressLabel = new QLabel("", this);
    progressLabel->setAlignment(Qt::AlignCenter);
    progressLabel->setVisible(false);  // Hide progress for now

    progressBar = new QProgressBar(this);
    progressBar->setVisible(false);  // Hide progress bar

    // No buttons needed
    studiedButton = nullptr;
    nextButton = nullptr;

    // Set background color
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(220, 220, 220));
    setPalette(pal);
    setAutoFillBackground(true);

    // Connect scroll bar to detect when user scrolls up
    connect(solvedScrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &LetterboxWindow::onScrollChanged);
}

std::string LetterboxWindow::sortVowelsFirst(const std::string& word)
{
    std::string vowels;
    std::string consonants;

    for (char c : word) {
        char upper = toupper(c);
        if (upper == 'A' || upper == 'E' || upper == 'I' || upper == 'O' || upper == 'U') {
            vowels += upper;
        } else {
            consonants += upper;
        }
    }

    std::sort(vowels.begin(), vowels.end());
    std::sort(consonants.begin(), consonants.end());

    return vowels + consonants;
}

void LetterboxWindow::loadWordList()
{
    // Get the path to csw24-pb.txt (same directory as the .app)
    QString appDir = QCoreApplication::applicationDirPath();
    QDir dir(appDir);
    dir.cdUp();  // Contents
    dir.cdUp();  // Letterbox.app
    dir.cdUp();  // letterbox directory
    QString filePath = dir.absolutePath() + "/csw24-pb.txt";

    qDebug() << "Looking for word list at:" << filePath;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Error", QString("Cannot open csw24-pb.txt at: %1").arg(filePath));
        return;
    }

    // Read all words and group by alphagram with their actual words from the file
    std::unordered_map<std::string, std::vector<std::string>> alphagramWords;
    std::unordered_map<std::string, int> alphagramPlayability;

    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() != 2) continue;

        int pb = (int)parts[0].toDouble();  // Use toDouble() because scores can be floats like "86374.7"
        std::string word = parts[1].toStdString();
        std::string alphagram = sortVowelsFirst(word);

        // Store playability score for this word
        playabilityScores[word] = pb;

        alphagramPlayability[alphagram] += pb;
        alphagramWords[alphagram].push_back(word);
    }
    file.close();

    qDebug() << "Found" << alphagramWords.size() << "unique alphagrams";
    qDebug() << "Loaded" << playabilityScores.size() << "playability scores";

    // Debug: Check if specific words are loaded
    if (playabilityScores.count("JEER")) {
        qDebug() << "JEER score:" << playabilityScores["JEER"];
    }
    if (playabilityScores.count("EWER")) {
        qDebug() << "EWER score:" << playabilityScores["EWER"];
    }

    // Create alphagram sets from the words we already have (no KWG lookup needed!)
    for (auto& [alphagram, words] : alphagramWords) {
        AlphagramSet set;
        set.alphagram = alphagram;
        set.totalPlayability = alphagramPlayability[alphagram];
        set.studied = false;

        for (const auto& word : words) {
            WordEntry entry;
            entry.word = word;
            entry.revealed = false;
            entry.missed = false;
            set.words.push_back(entry);
        }

        // Sort words alphabetically within the set
        std::sort(set.words.begin(), set.words.end(),
                 [](const WordEntry& a, const WordEntry& b) {
                     return a.word < b.word;
                 });

        alphagrams.push_back(set);
    }

    // Sort alphagrams by total playability (descending)
    std::sort(alphagrams.begin(), alphagrams.end(),
              [](const AlphagramSet& a, const AlphagramSet& b) {
                  return a.totalPlayability > b.totalPlayability;
              });

    qDebug() << "Loaded" << alphagrams.size() << "alphagram sets";

    // Store a copy of all alphagrams for filtering
    allAlphagrams = alphagrams;

    // Clear undo state when loading new word list
    lastMissedIndex = -1;
    if (undoAction) {
        undoAction->setEnabled(false);
    }

    updateProgress();
}

QString LetterboxWindow::formatAlphagramSet(const AlphagramSet& set, bool showAll)
{
    // Check if there are any revealed/visible words first
    bool hasVisibleWords = false;
    for (const auto& entry : set.words) {
        if (showAll || entry.revealed) {
            hasVisibleWords = true;
            break;
        }
    }

    if (!hasVisibleWords) {
        return "";  // Don't show anything if no words are revealed
    }

    QString html = "<div style='text-align: center; margin: 10px 0;'>";

    // ONE rounded rectangle border around the entire anagram set
    // Black border for visibility, rounded corners, padding, light background
    html += "<div style='display: inline-block; "
            "border: 3px solid black; "
            "border-radius: 10px; "
            "padding: 15px 25px; "
            "background-color: #f8f8f8;'>";

    // Display words vertically (already sorted alphabetically in loadWordList)
    for (const auto& entry : set.words) {
        if (showAll || entry.revealed) {
            // Get hooks using MAGPIE
            char* frontHooks = letterbox_find_front_hooks(kwg, ld, entry.word.c_str());
            char* backHooks = letterbox_find_back_hooks(kwg, ld, entry.word.c_str());

            QString frontStr = QString::fromUtf8(frontHooks ? frontHooks : "");
            QString backStr = QString::fromUtf8(backHooks ? backHooks : "");

            free(frontHooks);
            free(backHooks);

            // Each word on its own line with hooks on left and right
            html += "<div style='margin: 3px 0; white-space: nowrap;'>";

            // Front hooks (left, smaller, grey)
            html += QString("<span style='font-size: 12px; color: #666; margin-right: 8px; display: inline-block; min-width: 30px; text-align: right;'>%1</span>")
                    .arg(frontStr);

            // Word (center, bold, black, Jost)
            html += QString("<span style='font-family: \"Jost\", sans-serif; font-size: 18px; font-weight: bold; letter-spacing: 1px; color: #000;'>%1</span>")
                    .arg(QString::fromStdString(entry.word));

            // Back hooks (right, smaller, grey)
            html += QString("<span style='font-size: 12px; color: #666; margin-left: 8px; display: inline-block; min-width: 30px; text-align: left;'>%1</span>")
                    .arg(backStr);

            html += "</div>";
        }
    }

    html += "</div>";  // Close rounded rectangle border
    html += "</div>";
    return html;
}

void LetterboxWindow::updateDisplay()
{
    auto renderStartTime = std::chrono::high_resolution_clock::now();
    auto t0 = renderStartTime;

    // Clear the solved layout
    QLayoutItem* item;
    while ((item = solvedLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    int clearLayoutMicros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    // First pass: collect all boxes and calculate global maximum hook widths
    std::vector<AlphagramBox*> boxes;
    QFont hookFont;
    hookFont.setPixelSize(12);
    QFontMetrics hookMetrics(hookFont);

    QFont wordFont("Jost", 18, QFont::Bold);
    QFontMetrics wordMetrics(wordFont);

    int globalMaxFrontWidth = 0;
    int globalMaxBackWidth = 0;
    int globalMaxWordWidth = 0;

    // Add all studied alphagrams (fully revealed) - oldest first
    // Only render the last N solved alphagrams to keep performance constant
    // renderWindowSize increases if user scrolls up to see more history
    auto t2 = std::chrono::high_resolution_clock::now();
    int startIndex = std::max(0, currentIndex - renderWindowSize);

    for (int i = startIndex; i < currentIndex; i++) {
        if (alphagrams[i].studied) {
            AlphagramBox* box = new AlphagramBox(solvedWidget);
            box->setShowHoverDebug(showHoverDebugInfo);

            // Connect hover signals
            connect(box, &AlphagramBox::wordHovered, this, &LetterboxWindow::showWordHoverOverlay);
            connect(box, &AlphagramBox::hoverLeft, this, &LetterboxWindow::hideWordHoverOverlay);
            connect(box, &AlphagramBox::hoverDebug, this, &LetterboxWindow::updateHoverDebug);

            for (const auto& entry : alphagrams[i].words) {
                // Get cached or compute hook/extension data
                const HookExtensionCache& cache = getOrComputeHookExtensions(entry.word);

                QString frontStr = QString::fromStdString(cache.frontHooks);
                QString backStr = QString::fromStdString(cache.backHooks);
                QString frontExtStr = QString::fromStdString(cache.frontExtensions);
                QString backExtStr = QString::fromStdString(cache.backExtensions);

                // Calculate widths for global max
                int frontWidth = hookMetrics.horizontalAdvance(frontStr);
                int backWidth = hookMetrics.horizontalAdvance(backStr);
                int wordWidth = wordMetrics.horizontalAdvance(QString::fromStdString(entry.word));
                globalMaxFrontWidth = std::max(globalMaxFrontWidth, frontWidth);
                globalMaxBackWidth = std::max(globalMaxBackWidth, backWidth);
                globalMaxWordWidth = std::max(globalMaxWordWidth, wordWidth);

                box->addWord(QString::fromStdString(entry.word), frontStr, backStr,
                           frontExtStr, backExtStr, false, entry.missed, cache.computeTimeMicros);
            }
            boxes.push_back(box);
        }
    }

    // Add current alphagram at the bottom (with placeholders for unrevealed words)
    // Only if we haven't completed all alphagrams yet
    if (currentIndex < static_cast<int>(alphagrams.size())) {
        AlphagramBox* currentBox = new AlphagramBox(solvedWidget);
        currentBox->setShowHoverDebug(showHoverDebugInfo);

        // Connect hover signals
        connect(currentBox, &AlphagramBox::wordHovered, this, &LetterboxWindow::showWordHoverOverlay);
        connect(currentBox, &AlphagramBox::hoverLeft, this, &LetterboxWindow::hideWordHoverOverlay);
        connect(currentBox, &AlphagramBox::hoverDebug, this, &LetterboxWindow::updateHoverDebug);

        for (const auto& entry : alphagrams[currentIndex].words) {
            if (entry.revealed) {
                // Show the actual word with hooks and extensions (cached)
                const HookExtensionCache& cache = getOrComputeHookExtensions(entry.word);

                QString frontStr = QString::fromStdString(cache.frontHooks);
                QString backStr = QString::fromStdString(cache.backHooks);
                QString frontExtStr = QString::fromStdString(cache.frontExtensions);
                QString backExtStr = QString::fromStdString(cache.backExtensions);

                // Calculate widths for global max
                int frontWidth = hookMetrics.horizontalAdvance(frontStr);
                int backWidth = hookMetrics.horizontalAdvance(backStr);
                int wordWidth = wordMetrics.horizontalAdvance(QString::fromStdString(entry.word));
                globalMaxFrontWidth = std::max(globalMaxFrontWidth, frontWidth);
                globalMaxBackWidth = std::max(globalMaxBackWidth, backWidth);
                globalMaxWordWidth = std::max(globalMaxWordWidth, wordWidth);

                currentBox->addWord(QString::fromStdString(entry.word), frontStr, backStr,
                                  frontExtStr, backExtStr, false, entry.missed, cache.computeTimeMicros);
            } else {
                // Show placeholder dots (one dot per letter) - gray and regular weight
                QString placeholder;
                for (size_t i = 0; i < entry.word.length(); i++) {
                    placeholder += "·";  // Middle dot (U+00B7)
                }
                int wordWidth = wordMetrics.horizontalAdvance(placeholder);
                globalMaxWordWidth = std::max(globalMaxWordWidth, wordWidth);
                currentBox->addWord(placeholder, "", "", "", "", true, false);  // isPlaceholder=true, isMissed=false
            }
        }
        boxes.push_back(currentBox);
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    int createWidgetsMicros = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

    // Finalize and add all boxes to layout
    auto t4 = std::chrono::high_resolution_clock::now();
    for (auto* box : boxes) {
        box->finalize(scaledWordSize, scaledHookSize, scaledExtensionSize, showComputeTime);
        solvedLayout->addWidget(box, 0, Qt::AlignCenter);
    }
    auto t5 = std::chrono::high_resolution_clock::now();
    int finalizeAndAddMicros = std::chrono::duration_cast<std::chrono::microseconds>(t5 - t4).count();

    // Add placeholder timing label now (will be updated at the end)
    if (showRenderTime) {
        renderTimeLabel = new QLabel(solvedWidget);
        renderTimeLabel->setStyleSheet("QLabel { color: #666; font-family: monospace; font-size: 9px; padding: 5px; line-height: 1.4; }");
        renderTimeLabel->setAlignment(Qt::AlignCenter);
        renderTimeLabel->setText("Calculating...");
        solvedLayout->addWidget(renderTimeLabel, 0, Qt::AlignCenter);
    }

    // Bottom: Show queue of UPCOMING alphagrams (not including current) - starts with current alphagram
    auto t6 = std::chrono::high_resolution_clock::now();
    QString queueHtml = "";

    if (currentIndex >= static_cast<int>(alphagrams.size())) {
        // Quiz complete! Show completion message
        queueHtml = QString("<div style='font-family: \"Jost\", sans-serif; font-size: %1px; margin: 20px 0; font-weight: bold; color: #4CAF50; text-align: center;'>Complete!</div>")
                .arg(scaledQueueCurrentSize + 4);  // Slightly larger font

        // Disable input field and clear placeholder
        inputField->setEnabled(false);
        inputField->clear();
        inputField->setPlaceholderText("");

        // Disable skip action when quiz is complete
        if (skipAction) {
            skipAction->setEnabled(false);
        }
    } else {
        // Enable skip action when quiz is active
        if (skipAction) {
            skipAction->setEnabled(true);
        }

        // Show current alphagram at top of queue (Jost Bold, white)
        queueHtml += QString("<div style='font-family: \"Jost\", sans-serif; font-size: %1px; margin: 5px 0; font-weight: bold; color: #fff; text-align: center;'>%2</div>")
                .arg(scaledQueueCurrentSize)
                .arg(QString::fromStdString(alphagrams[currentIndex].alphagram));

        // Show upcoming alphagrams (Jost Regular - normal weight, gray)
        int queueCount = std::min(10, (int)alphagrams.size() - currentIndex - 1);
        for (int i = 1; i <= queueCount; i++) {
            const AlphagramSet& set = alphagrams[currentIndex + i];
            queueHtml += QString("<div style='font-family: \"Jost\", sans-serif; font-size: %1px; margin: 5px 0; font-weight: normal; color: #888; text-align: center;'>%2</div>")
                    .arg(scaledQueueUpcomingSize)
                    .arg(QString::fromStdString(set.alphagram));
        }
    }
    queueLabel->setText(queueHtml);

    // Update progress counter (e.g., "1 / 50" or "50 / 50" when complete)
    int totalAlphagrams = alphagrams.size();
    int displayIndex = std::min(currentIndex + 1, totalAlphagrams);  // Cap at total
    QString counterText = QString("%1 / %2").arg(displayIndex).arg(totalAlphagrams);
    progressCounterLabel->setText(counterText);
    progressCounterLabel->adjustSize();
    progressCounterLabel->show();

    // Position counter after layout is complete (use timer to ensure geometry is final)
    // Align flush with container edges (0 margin)
    QTimer::singleShot(10, this, [this]() {
        if (progressCounterLabel && queueContainer) {
            progressCounterLabel->adjustSize();
            int x = queueContainer->width() - progressCounterLabel->width();
            int y = queueContainer->height() - progressCounterLabel->height();
            progressCounterLabel->move(x, y);
        }
    });

    auto t7 = std::chrono::high_resolution_clock::now();
    int queueHtmlMicros = std::chrono::duration_cast<std::chrono::microseconds>(t7 - t6).count();

    // Clear and focus input field
    auto t8 = std::chrono::high_resolution_clock::now();
    inputField->clear();

    // Update placeholder to show dots for the first unrevealed word
    if (currentIndex < static_cast<int>(alphagrams.size())) {
        QString placeholderText;
        for (const auto& entry : alphagrams[currentIndex].words) {
            if (!entry.revealed) {
                for (size_t i = 0; i < entry.word.length(); i++) {
                    placeholderText += "·";  // Middle dot (U+00B7)
                }
                break;  // Only use the first unrevealed word
            }
        }
        inputField->setPlaceholderText(placeholderText);
    }

    inputField->setFocus();
    auto t9 = std::chrono::high_resolution_clock::now();
    int inputClearMicros = std::chrono::duration_cast<std::chrono::microseconds>(t9 - t8).count();

    // Scroll to bottom to keep the current alphagram visible
    auto t10 = std::chrono::high_resolution_clock::now();
    auto t11 = std::chrono::high_resolution_clock::now();
    int scrollMicros = std::chrono::duration_cast<std::chrono::microseconds>(t11 - t10).count();

    auto t12 = std::chrono::high_resolution_clock::now();
    updateProgress();
    auto t13 = std::chrono::high_resolution_clock::now();
    int updateProgressMicros = std::chrono::duration_cast<std::chrono::microseconds>(t13 - t12).count();

    // Capture render time at the end
    auto renderEndTime = std::chrono::high_resolution_clock::now();
    lastRenderTimeMicros = std::chrono::duration_cast<std::chrono::microseconds>(renderEndTime - renderStartTime).count();

    // Update timing label with final values
    if (showRenderTime && renderTimeLabel) {
        QString timingText = QString(
            "Total: %1μs | Clear: %2μs | Create: %3μs | Finalize: %4μs | Queue: %5μs | Input: %6μs | Scroll: %7μs | Progress: %8μs"
        ).arg(lastRenderTimeMicros)
         .arg(clearLayoutMicros)
         .arg(createWidgetsMicros)
         .arg(finalizeAndAddMicros)
         .arg(queueHtmlMicros)
         .arg(inputClearMicros)
         .arg(scrollMicros)
         .arg(updateProgressMicros);

        renderTimeLabel->setText(timingText);
    }

    // Force layout update and scroll to bottom (only if user hasn't manually scrolled)
    // Use a very small delay (10ms) to ensure layout is complete but still feel responsive
    solvedWidget->updateGeometry();
    solvedLayout->update();

    // Center the scroll area horizontally
    QTimer::singleShot(10, this, [this]() {
        QScrollBar* hbar = solvedScrollArea->horizontalScrollBar();
        if (hbar) {
            hbar->setValue((hbar->minimum() + hbar->maximum()) / 2);
        }
    });

    if (!userScrolledUp) {
        // Only auto-scroll if user hasn't manually scrolled up
        QTimer::singleShot(10, this, [this]() {
            solvedScrollArea->verticalScrollBar()->setValue(
                solvedScrollArea->verticalScrollBar()->maximum()
            );
        });
    } else {
        // User has scrolled up, expand window and don't auto-scroll
        renderWindowSize = std::min(50, currentIndex);
    }
}

void LetterboxWindow::onScrollChanged(int value)
{
    QScrollBar* vbar = solvedScrollArea->verticalScrollBar();

    // Detect if user scrolled up (not at maximum)
    if (value < vbar->maximum() - 10) {  // 10px threshold to avoid jitter
        if (!userScrolledUp) {
            userScrolledUp = true;
            // Expand window size to show more history
            renderWindowSize = std::min(50, currentIndex);
        }
    } else {
        // User scrolled back to bottom, reset to normal mode
        if (userScrolledUp) {
            userScrolledUp = false;
            renderWindowSize = 15;
        }
    }
}

void LetterboxWindow::onTextChanged(const QString& text)
{
    if (currentIndex >= alphagrams.size()) return;

    AlphagramSet& set = alphagrams[currentIndex];
    QString upper = text.toUpper().trimmed();

    // Check if typed word matches any unrevealed word
    for (auto& entry : set.words) {
        if (!entry.revealed && QString::fromStdString(entry.word) == upper) {
            entry.revealed = true;

            // Clear undo state when revealing a word
            lastMissedIndex = -1;
            if (undoAction) {
                undoAction->setEnabled(false);
            }

            // Check if all words are revealed
            bool allRevealed = true;
            for (const auto& e : set.words) {
                if (!e.revealed) {
                    allRevealed = false;
                    break;
                }
            }

            // If all revealed, mark as studied and advance
            if (allRevealed) {
                set.studied = true;
                currentIndex++;

                // Reset revealed states for new alphagram (if not at end)
                if (currentIndex < alphagrams.size()) {
                    for (auto& e : alphagrams[currentIndex].words) {
                        e.revealed = false;
                    }
                }
            }

            updateDisplay();

            // Auto-save after revealing a word
            saveCurrentList();

            // Check for completion after advancing
            checkForCompletion();

            break;
        }
    }
}

void LetterboxWindow::updateProgress()
{
    if (alphagrams.empty()) {
        progressLabel->setText("No words loaded");
        progressBar->setValue(0);
        return;
    }

    int studied = 0;
    for (const auto& set : alphagrams) {
        if (set.studied) studied++;
    }

    int total = alphagrams.size();
    int percentage = (studied * 100) / total;

    progressLabel->setText(QString("Progress: %1 / %2 alphagrams studied (%3%)")
                          .arg(studied).arg(total).arg(percentage));
    progressBar->setMaximum(total);
    progressBar->setValue(studied);
}

void LetterboxWindow::nextWord()
{
    if (currentIndex < alphagrams.size() - 1) {
        currentIndex++;

        // Reset all revealed states for new alphagram
        for (auto& entry : alphagrams[currentIndex].words) {
            entry.revealed = false;
        }

        updateDisplay();
    }
}

void LetterboxWindow::markStudied()
{
    if (currentIndex < alphagrams.size()) {
        alphagrams[currentIndex].studied = true;
        updateProgress();
        nextWord();
    }
}

void LetterboxWindow::skipCurrentAlphagram()
{
    if (currentIndex >= alphagrams.size()) return;

    AlphagramSet& set = alphagrams[currentIndex];

    // Mark all unrevealed words as missed
    for (auto& entry : set.words) {
        if (!entry.revealed) {
            entry.missed = true;
            entry.revealed = true;  // Mark as revealed so they show up
        }
    }

    // Save this index for undo
    lastMissedIndex = currentIndex;
    if (undoAction) {
        undoAction->setEnabled(true);
    }

    // Mark as studied and advance
    set.studied = true;
    currentIndex++;

    // Reset revealed/missed states for new alphagram (if not at end)
    if (currentIndex < alphagrams.size()) {
        for (auto& e : alphagrams[currentIndex].words) {
            e.revealed = false;
            e.missed = false;
        }
    }

    updateDisplay();

    // Auto-save after marking as missed
    saveCurrentList();

    // Check for completion after advancing
    checkForCompletion();
}

void LetterboxWindow::undoMarkAsMissed()
{
    // Check if undo is currently available
    if (!undoAction || !undoAction->isEnabled()) {
        return;
    }

    // Can only undo if we're one step ahead of the last missed index
    if (lastMissedIndex < 0 || currentIndex != lastMissedIndex + 1) {
        return;
    }

    // Go back to the missed alphagram
    currentIndex = lastMissedIndex;
    AlphagramSet& set = alphagrams[currentIndex];

    // Reset all words to unrevealed and not missed
    for (auto& entry : set.words) {
        entry.revealed = false;
        entry.missed = false;
    }

    // Mark as not studied
    set.studied = false;

    // Clear undo state
    lastMissedIndex = -1;
    if (undoAction) {
        undoAction->setEnabled(false);
    }

    updateDisplay();
}

void LetterboxWindow::setupMenuBar()
{
    QMenuBar* menuBar = new QMenuBar(this);
    setMenuBar(menuBar);

    // Words menu
    QMenu* wordsMenu = menuBar->addMenu("Words");
    QAction* createListAction = new QAction("Create New Word List...", this);
    createListAction->setShortcut(QKeySequence::New);  // Command-N on macOS
    connect(createListAction, &QAction::triggered, this, &LetterboxWindow::createCustomWordList);
    wordsMenu->addAction(createListAction);

    wordsMenu->addSeparator();

    // Recent Lists submenu
    recentListsMenu = wordsMenu->addMenu("Recent Lists");
    updateRecentListsMenu();

    wordsMenu->addSeparator();

    skipAction = new QAction("Mark as missed", this);
    skipAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));  // Cmd-M on macOS
    connect(skipAction, &QAction::triggered, this, &LetterboxWindow::skipCurrentAlphagram);
    wordsMenu->addAction(skipAction);

    undoAction = new QAction("Undo Mark as missed", this);
    undoAction->setEnabled(false);  // Disabled until there's something to undo
    connect(undoAction, &QAction::triggered, this, &LetterboxWindow::undoMarkAsMissed);
    wordsMenu->addAction(undoAction);

    // Create a global shortcut for undo that works even when input has focus
    QShortcut* undoShortcut = new QShortcut(QKeySequence::Undo, this);
    undoShortcut->setContext(Qt::ApplicationShortcut);
    connect(undoShortcut, &QShortcut::activated, this, &LetterboxWindow::undoMarkAsMissed);

    // View menu
    QMenu* viewMenu = menuBar->addMenu("View");

    QAction* fullscreenAction = new QAction("Toggle Fullscreen", this);
    fullscreenAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F));  // Cmd-F on macOS
    connect(fullscreenAction, &QAction::triggered, this, &LetterboxWindow::toggleFullscreen);
    viewMenu->addAction(fullscreenAction);

    viewMenu->addSeparator();

    QAction* zoomInAction = new QAction("Zoom In", this);
    zoomInAction->setShortcut(QKeySequence::ZoomIn);  // Cmd-+ on macOS
    connect(zoomInAction, &QAction::triggered, this, &LetterboxWindow::zoomIn);
    viewMenu->addAction(zoomInAction);

    QAction* zoomOutAction = new QAction("Zoom Out", this);
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);  // Cmd-- on macOS
    connect(zoomOutAction, &QAction::triggered, this, &LetterboxWindow::zoomOut);
    viewMenu->addAction(zoomOutAction);

    // Debug menu
    QMenu* debugMenu = menuBar->addMenu("Debug");

    debugAction = new QAction("Show Debug Info", this);
    debugAction->setCheckable(true);
    debugAction->setChecked(false);
    connect(debugAction, &QAction::triggered, this, &LetterboxWindow::toggleDebugInfo);
    debugMenu->addAction(debugAction);

    computeTimeAction = new QAction("Show Compute Time", this);
    computeTimeAction->setCheckable(true);
    computeTimeAction->setChecked(false);
    connect(computeTimeAction, &QAction::triggered, this, &LetterboxWindow::toggleComputeTime);
    debugMenu->addAction(computeTimeAction);

    renderTimeAction = new QAction("Show Render Time", this);
    renderTimeAction->setCheckable(true);
    renderTimeAction->setChecked(false);
    connect(renderTimeAction, &QAction::triggered, this, &LetterboxWindow::toggleRenderTime);
    debugMenu->addAction(renderTimeAction);

    hoverDebugAction = new QAction("Show Hover Debug Info", this);
    hoverDebugAction->setCheckable(true);
    hoverDebugAction->setChecked(false);  // Disabled by default
    connect(hoverDebugAction, &QAction::triggered, this, &LetterboxWindow::toggleHoverDebugInfo);
    debugMenu->addAction(hoverDebugAction);
}

void LetterboxWindow::toggleFullscreen()
{
    if (isFullScreen()) {
        showNormal();
    } else {
        showFullScreen();
    }
}

void LetterboxWindow::zoomIn()
{
    // Move to next zoom level (Chrome-style discrete levels)
    if (zoomLevelIndex < static_cast<int>(zoomLevels.size()) - 1) {
        zoomLevelIndex++;
        scaleFactor = zoomLevels[zoomLevelIndex];
        updateScaledFontSizes();
        if (currentIndex < static_cast<int>(alphagrams.size())) {
            updateDisplay();
        }
        showZoomIndicator();
    }
}

void LetterboxWindow::zoomOut()
{
    // Move to previous zoom level (Chrome-style discrete levels)
    if (zoomLevelIndex > 0) {
        zoomLevelIndex--;
        scaleFactor = zoomLevels[zoomLevelIndex];
        updateScaledFontSizes();
        if (currentIndex < static_cast<int>(alphagrams.size())) {
            updateDisplay();
        }
        showZoomIndicator();
    }
}

void LetterboxWindow::showZoomIndicator()
{
    // Format zoom level as percentage
    int zoomPercent = static_cast<int>(scaleFactor * 100);
    zoomIndicatorLabel->setText(QString("%1%").arg(zoomPercent));
    zoomIndicatorLabel->adjustSize();

    // Position above progress counter (with 10px gap)
    if (queueContainer) {
        int x = queueContainer->width() - zoomIndicatorLabel->width();
        int y = queueContainer->height() - zoomIndicatorLabel->height() -
                (progressCounterLabel->isVisible() ? progressCounterLabel->height() + 10 : 10);
        zoomIndicatorLabel->move(x, y);
    }

    // Show and start fade-out timer (1 second)
    zoomIndicatorLabel->show();
    zoomIndicatorTimer->start(1000);
}

void LetterboxWindow::toggleDebugInfo()
{
    showDebugInfo = debugAction->isChecked();
    debugLabel->setVisible(showDebugInfo);
    if (showDebugInfo) {
        updateDebugInfo();
    }
}

void LetterboxWindow::toggleComputeTime()
{
    showComputeTime = computeTimeAction->isChecked();
    updateDisplay();
}

void LetterboxWindow::toggleRenderTime()
{
    showRenderTime = renderTimeAction->isChecked();
    updateDisplay();
}

void LetterboxWindow::toggleHoverDebugInfo()
{
    showHoverDebugInfo = hoverDebugAction->isChecked();
    // Show or hide the hover debug label
    if (hoverDebugLabel) {
        hoverDebugLabel->setVisible(showHoverDebugInfo);
    }

    // Update all existing AlphagramBox widgets
    for (int i = 0; i < solvedLayout->count(); i++) {
        QLayoutItem* item = solvedLayout->itemAt(i);
        if (item && item->widget()) {
            AlphagramBox* box = qobject_cast<AlphagramBox*>(item->widget());
            if (box) {
                box->setShowHoverDebug(showHoverDebugInfo);
            }
        }
    }
}

void LetterboxWindow::updateDebugInfo()
{
    if (!showDebugInfo) return;

    QString debugText;
    debugText += QString("Window Size: %1 x %2\n").arg(width()).arg(height());
    debugText += QString("Central Widget: %1 x %2\n").arg(centralWidget->width()).arg(centralWidget->height());
    debugText += QString("\nScale Factor: %1\n").arg(scaleFactor, 0, 'f', 2);
    debugText += QString("\nFont Sizes:\n");
    debugText += QString("  Input Field: %1px\n").arg(scaledInputSize);
    debugText += QString("  Word (Main): %1px (semibold)\n").arg(scaledWordSize);
    debugText += QString("  Hooks: %1px (medium)\n").arg(scaledHookSize);
    debugText += QString("  Extensions: %1px (regular)\n").arg(scaledExtensionSize);
    debugText += QString("  Queue Current: %1px (bold)\n").arg(scaledQueueCurrentSize);
    debugText += QString("  Queue Upcoming: %1px (regular)\n").arg(scaledQueueUpcomingSize);

    debugLabel->setText(debugText);
    debugLabel->setGeometry(10, 30, 350, 250);
}

void LetterboxWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);

    // Restart the debounce timer - only process resize after user stops resizing
    resizeTimer->start();
}

bool LetterboxWindow::eventFilter(QObject *obj, QEvent *event)
{
    // Intercept Cmd-Z on the input field and handle it ourselves
    if (obj == inputField && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        // Check for Cmd-Z (Ctrl on macOS is actually Command key)
        if (keyEvent->matches(QKeySequence::Undo)) {
            // Trigger our undo action if it's enabled
            if (undoAction && undoAction->isEnabled()) {
                undoMarkAsMissed();
                return true;  // Event handled, don't pass to input field
            }
        }
    }
    // Pass event to base class
    return QMainWindow::eventFilter(obj, event);
}

void LetterboxWindow::handleResizeComplete()
{
    double oldScaleFactor = scaleFactor;

    updateScaledFontSizes();
    updateDebugInfo();

    // Position progress counter in bottom right corner of queue container
    // Align flush with container edges (0 margin)
    if (progressCounterLabel && queueContainer) {
        progressCounterLabel->adjustSize();
        int x = queueContainer->width() - progressCounterLabel->width();
        int y = queueContainer->height() - progressCounterLabel->height();
        progressCounterLabel->move(x, y);
    }

    // Only redraw if scale factor actually changed
    if (oldScaleFactor != scaleFactor && currentIndex < static_cast<int>(alphagrams.size())) {
        updateDisplay();
    }
}

void LetterboxWindow::updateScaledFontSizes()
{
    // scaleFactor is now controlled by zoom level (zoomLevels[zoomLevelIndex])
    // Don't recalculate it based on window width

    // Base sizes and minimums
    const int baseWordSize = 36;
    const int baseHookSize = 24;
    const int baseExtensionSize = 14;
    const int baseInputSize = 20;
    const int baseQueueCurrentSize = 24;
    const int baseQueueUpcomingSize = 16;

    const int minWordSize = 16;
    const int minHookSize = 9;
    const int minExtensionSize = 7;
    const int minInputSize = 10;
    const int minQueueCurrentSize = 10;
    const int minQueueUpcomingSize = 8;

    // Calculate scaled sizes with minimums
    scaledWordSize = std::max(minWordSize, static_cast<int>(baseWordSize * scaleFactor));
    scaledHookSize = std::max(minHookSize, static_cast<int>(baseHookSize * scaleFactor));
    scaledExtensionSize = std::max(minExtensionSize, static_cast<int>(baseExtensionSize * scaleFactor));
    scaledInputSize = std::max(minInputSize, static_cast<int>(baseInputSize * scaleFactor));
    scaledQueueCurrentSize = std::max(minQueueCurrentSize, static_cast<int>(baseQueueCurrentSize * scaleFactor));
    scaledQueueUpcomingSize = std::max(minQueueUpcomingSize, static_cast<int>(baseQueueUpcomingSize * scaleFactor));

    // Update input field font size
    QFont inputFont = inputField->font();
    inputFont.setPixelSize(scaledInputSize);
    inputField->setFont(inputFont);
}

const HookExtensionCache& LetterboxWindow::getOrComputeHookExtensions(const std::string& word)
{
    // Check if already cached
    auto it = hookExtensionCache.find(word);
    if (it != hookExtensionCache.end()) {
        return it->second;
    }

    // Not cached, compute now
    HookExtensionCache cache;

    auto startTime = std::chrono::high_resolution_clock::now();

    char* frontHooks = letterbox_find_front_hooks(kwg, ld, word.c_str());
    char* backHooks = letterbox_find_back_hooks(kwg, ld, word.c_str());
    char* frontExts = letterbox_find_front_extensions(kwg, ld, word.c_str(), 7);
    char* backExts = letterbox_find_back_extensions(kwg, ld, word.c_str(), 7);

    auto endTime = std::chrono::high_resolution_clock::now();
    cache.computeTimeMicros = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();

    cache.frontHooks = frontHooks ? frontHooks : "";
    cache.backHooks = backHooks ? backHooks : "";
    cache.frontExtensions = frontExts ? frontExts : "";
    cache.backExtensions = backExts ? backExts : "";
    cache.cached = true;

    free(frontHooks);
    free(backHooks);
    free(frontExts);
    free(backExts);

    // Sort extensions by playability before caching
    QString qWord = QString::fromStdString(word);
    cache.frontExtensions = sortExtensionsByPlayability(QString::fromStdString(cache.frontExtensions), qWord, true).toStdString();
    cache.backExtensions = sortExtensionsByPlayability(QString::fromStdString(cache.backExtensions), qWord, false).toStdString();

    // Store in cache and return reference
    hookExtensionCache[word] = cache;
    return hookExtensionCache[word];
}

QString LetterboxWindow::sortExtensionsByPlayability(const QString& extensions, const QString& baseWord, bool isFront)
{
    if (extensions.isEmpty()) {
        return extensions;
    }

    // Split by newlines (one line per extension length)
    QStringList lines = extensions.split('\n', Qt::SkipEmptyParts);
    QString result;

    for (const QString& line : lines) {
        // Split words by spaces (these are extension parts, not full words)
        QStringList extensionParts = line.split(' ', Qt::SkipEmptyParts);

        // Remove ellipsis if present
        bool hadEllipsis = false;
        if (!extensionParts.isEmpty() && extensionParts.last().contains("…")) {
            hadEllipsis = true;
            extensionParts.last().remove("…");
            if (extensionParts.last().isEmpty()) {
                extensionParts.removeLast();
            }
        }

        // Sort by playability (descending)
        std::vector<std::pair<QString, int>> extensionsWithScores;
        for (const QString& ext : extensionParts) {
            // Construct the full word by combining base word with extension
            QString fullWord;
            if (isFront) {
                fullWord = ext + baseWord;  // Front extension: EXT + WORD
            } else {
                fullWord = baseWord + ext;  // Back extension: WORD + EXT
            }

            std::string stdFullWord = fullWord.toStdString();
            int score = playabilityScores.count(stdFullWord) ? playabilityScores[stdFullWord] : 0;
            extensionsWithScores.push_back({ext, score});  // Store the extension part with the full word's score

            // Debug: print specific cases for ER
            if (baseWord == "ER" && isFront && (ext == "JE" || ext == "EW" || ext == "OY" || extensionsWithScores.size() <= 5)) {
                qDebug() << "Extension:" << ext << "-> Full word:" << fullWord << "-> Score:" << score;
            }
        }

        std::sort(extensionsWithScores.begin(), extensionsWithScores.end(),
                  [](const auto& a, const auto& b) {
                      return a.second > b.second; // Descending order
                  });

        // Rebuild the line with sorted extension parts, truncating at 40 characters
        QStringList sortedExtensions;
        int letterCount = 0;
        const int MAX_CHARS_PER_LINE = 40;
        bool truncated = false;

        for (const auto& pair : extensionsWithScores) {
            int extLen = pair.first.length();
            if (letterCount + extLen > MAX_CHARS_PER_LINE) {
                truncated = true;
                break;
            }
            sortedExtensions.append(pair.first);
            letterCount += extLen;
        }

        QString sortedLine = sortedExtensions.join(' ');
        if (truncated) {
            sortedLine += "…";
        }

        if (!result.isEmpty()) {
            result += '\n';
        }
        result += sortedLine;
    }

    return result;
}

void LetterboxWindow::createCustomWordList()
{
    // Open dialog with all alphagrams
    WordListDialog dialog(allAlphagrams, playabilityScores, kwg, ld, this);

    if (dialog.exec() == QDialog::Accepted) {
        // Get filtered list and filter parameters
        alphagrams = dialog.getFilteredList();
        currentPattern = dialog.getPattern();
        currentMinAnagrams = dialog.getMinAnagramCount();
        currentMaxAnagrams = dialog.getMaxAnagramCount();
        currentIndex = 0;

        // Generate list name
        QDateTime now = QDateTime::currentDateTime();
        currentListName = generateListName(currentPattern, currentMinAnagrams,
                                          currentMaxAnagrams, alphagrams.size(), QDateTime());

        // Create filepath for saving
        QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/saved_lists";
        QDir dir;
        if (!dir.exists(dataDir)) {
            dir.mkpath(dataDir);
        }

        QString sanitizedName = currentListName;
        sanitizedName.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
        QString filename = QString("%1_%2.json").arg(sanitizedName).arg(now.toString("yyyyMMdd_HHmmss"));
        currentListFilepath = dataDir + "/" + filename;

        // Clear undo state when creating new word list
        lastMissedIndex = -1;
        if (undoAction) {
            undoAction->setEnabled(false);
        }

        // Reset progress and update display
        for (auto& set : alphagrams) {
            set.studied = false;
            for (auto& word : set.words) {
                word.revealed = false;
                word.missed = false;
            }
        }

        // Clear solved section
        QLayoutItem* item;
        while ((item = solvedLayout->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }

        // Update UI
        updateWindowTitle();
        updateProgress();
        updateDisplay();

        // Save the new list
        saveCurrentList();

        // Add to recent lists
        addToRecentLists(currentListFilepath);
    }
}

QString LetterboxWindow::generateListName(const QString& pattern, int minCount, int maxCount, int totalSets, const QDateTime& timestamp)
{
    QStringList nameParts;

    // Pattern part
    if (!pattern.isEmpty()) {
        // Check if pattern is all wildcards
        bool allWildcards = true;
        for (const QChar& c : pattern) {
            if (c != '.' && c != '?') {
                allWildcards = false;
                break;
            }
        }

        if (allWildcards) {
            nameParts << QString("%1-letter words").arg(pattern.length());
        } else {
            nameParts << QString("%1 pattern").arg(pattern);
        }
    }

    // Anagram count part
    if (minCount == maxCount) {
        if (minCount == 1) {
            nameParts << "single anagram";
        } else {
            nameParts << QString("%1 anagrams").arg(minCount);
        }
    } else {
        nameParts << QString("%1-%2 anagrams").arg(minCount).arg(maxCount);
    }

    // Total sets part
    nameParts << QString("%1 sets").arg(totalSets);

    QString name = nameParts.join(", ");

    // Add timestamp if provided (for disambiguation)
    if (timestamp.isValid()) {
        name += QString(" (%1)").arg(timestamp.toString("MMM d, h:mm AP"));
    }

    return name;
}

void LetterboxWindow::saveCurrentList()
{
    // Don't save if no active custom list
    if (currentListFilepath.isEmpty()) {
        return;
    }

    // Create JSON object
    QJsonObject root;
    root["name"] = currentListName;
    root["created"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["lastAccessed"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["pattern"] = currentPattern;
    root["minAnagrams"] = currentMinAnagrams;
    root["maxAnagrams"] = currentMaxAnagrams;
    root["currentIndex"] = currentIndex;

    // Save alphagrams array
    QJsonArray alphagramsArray;
    for (const auto& set : alphagrams) {
        QJsonObject alphagramObj;
        alphagramObj["alphagram"] = QString::fromStdString(set.alphagram);
        alphagramObj["studied"] = set.studied;

        QJsonArray wordsArray;
        for (const auto& entry : set.words) {
            QJsonObject wordObj;
            wordObj["word"] = QString::fromStdString(entry.word);
            wordObj["revealed"] = entry.revealed;
            wordObj["missed"] = entry.missed;
            wordsArray.append(wordObj);
        }
        alphagramObj["words"] = wordsArray;

        alphagramsArray.append(alphagramObj);
    }
    root["alphagrams"] = alphagramsArray;

    // Write to file
    QJsonDocument doc(root);
    QFile file(currentListFilepath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson());
        file.close();
        qDebug() << "Saved list to:" << currentListFilepath;
    } else {
        qWarning() << "Failed to save list to:" << currentListFilepath;
    }
}

void LetterboxWindow::loadSavedListFromFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, "Error", QString("Cannot open saved list: %1").arg(filepath));
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        QMessageBox::critical(this, "Error", "Invalid saved list format");
        return;
    }

    QJsonObject root = doc.object();

    // Load metadata
    currentListName = root["name"].toString();
    currentPattern = root["pattern"].toString();
    currentMinAnagrams = root["minAnagrams"].toInt(1);
    currentMaxAnagrams = root["maxAnagrams"].toInt(999);
    currentIndex = root["currentIndex"].toInt(0);
    currentListFilepath = filepath;

    // Load alphagrams
    alphagrams.clear();
    QJsonArray alphagramsArray = root["alphagrams"].toArray();
    for (const QJsonValue& val : alphagramsArray) {
        QJsonObject alphagramObj = val.toObject();

        AlphagramSet set;
        set.alphagram = alphagramObj["alphagram"].toString().toStdString();
        set.studied = alphagramObj["studied"].toBool();
        set.totalPlayability = 0;  // Will recalculate if needed

        QJsonArray wordsArray = alphagramObj["words"].toArray();
        for (const QJsonValue& wordVal : wordsArray) {
            QJsonObject wordObj = wordVal.toObject();

            WordEntry entry;
            entry.word = wordObj["word"].toString().toStdString();
            entry.revealed = wordObj["revealed"].toBool();
            entry.missed = wordObj["missed"].toBool();

            // Recalculate total playability
            if (playabilityScores.count(entry.word)) {
                set.totalPlayability += playabilityScores[entry.word];
            }

            set.words.push_back(entry);
        }

        alphagrams.push_back(set);
    }

    // Clear undo state
    lastMissedIndex = -1;
    if (undoAction) {
        undoAction->setEnabled(false);
    }

    // Clear solved section
    QLayoutItem* item;
    while ((item = solvedLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    // Update UI
    updateWindowTitle();
    updateProgress();
    updateDisplay();
    addToRecentLists(filepath);

    qDebug() << "Loaded list:" << currentListName << "from:" << filepath;
}

void LetterboxWindow::addToRecentLists(const QString& filepath)
{
    QSettings settings("Letterbox", "Letterbox");
    QStringList recentLists = settings.value("recentLists").toStringList();

    // Remove if already in list
    recentLists.removeAll(filepath);

    // Add to front
    recentLists.prepend(filepath);

    // Keep only last 10
    while (recentLists.size() > 10) {
        recentLists.removeLast();
    }

    settings.setValue("recentLists", recentLists);
    updateRecentListsMenu();
}

void LetterboxWindow::updateRecentListsMenu()
{
    if (!recentListsMenu) return;

    // Clear existing items (except separator and "Clear Recent Lists")
    recentListsMenu->clear();

    QSettings settings("Letterbox", "Letterbox");
    QStringList recentLists = settings.value("recentLists").toStringList();

    // Add menu items for each recent list
    for (const QString& filepath : recentLists) {
        // Extract just the list name from JSON file
        QFile file(filepath);
        if (file.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            file.close();

            if (!doc.isNull() && doc.isObject()) {
                QString name = doc.object()["name"].toString();
                QAction* action = new QAction(name, this);
                action->setData(filepath);  // Store filepath in action
                connect(action, &QAction::triggered, this, [this, filepath]() {
                    loadSavedListFromFile(filepath);
                });
                recentListsMenu->addAction(action);
            }
        }
    }

    // Add separator and "Clear Recent Lists" if there are items
    if (!recentLists.isEmpty()) {
        recentListsMenu->addSeparator();
        QAction* clearAction = new QAction("Clear Recent Lists", this);
        connect(clearAction, &QAction::triggered, this, &LetterboxWindow::clearRecentLists);
        recentListsMenu->addAction(clearAction);
    }
}

void LetterboxWindow::clearRecentLists()
{
    QSettings settings("Letterbox", "Letterbox");
    settings.remove("recentLists");
    updateRecentListsMenu();
}

void LetterboxWindow::loadSavedList()
{
    // This slot is called by menu actions with filepath in their data
    QAction* action = qobject_cast<QAction*>(sender());
    if (action) {
        QString filepath = action->data().toString();
        loadSavedListFromFile(filepath);
    }
}

void LetterboxWindow::updateWindowTitle()
{
    if (currentListName.isEmpty()) {
        setWindowTitle("Letterbox");
    } else {
        setWindowTitle(currentListName + " - Letterbox");
    }
}

void LetterboxWindow::showWordHoverOverlay(const QString& word, bool alignLeft, bool isHookOrExtension)
{
    if (!wordHoverOverlay || !solvedContainer) {
        return;
    }

    QString html;
    QString wordUpper = word.toUpper();

    // Only show the word header for main words (not hooks/extensions)
    if (!isHookOrExtension) {
        html += QString("<div style='font-size: 24px; font-weight: 600; text-align: center; margin-bottom: 8px; letter-spacing: 1px;'>%1</div>")
                .arg(wordUpper);
    } else {
        // For hooks/extensions, add the anagram/hooks table
        QString tableHtml = generateSidebarTable(word);
        html += tableHtml;
    }

    wordHoverOverlay->setText(html);
    wordHoverOverlay->adjustSize();

    // Make it wider to accommodate the table
    int overlayWidth = std::min(400, solvedContainer->width() / 3);
    wordHoverOverlay->setFixedWidth(overlayWidth);

    // Position in corner (top-left or top-right)
    int x = alignLeft ? 0 : (solvedContainer->width() - wordHoverOverlay->width());
    int y = 0;  // Top of container

    wordHoverOverlay->move(x, y);
    wordHoverOverlay->show();
    wordHoverOverlay->raise();  // Ensure it's on top
}

QString LetterboxWindow::generateSidebarTable(const QString& word)
{
    QString html;
    QString wordUpper = word.toUpper();
    std::string wordStr = wordUpper.toStdString();
    int wordLength = wordUpper.length();

    // Find anagrams
    WordList* anagrams = letterbox_find_anagrams(kwg, ld, wordStr.c_str());

    // Debug: log anagram count
    int totalAnagrams = anagrams ? anagrams->count : 0;
    int exactAnagrams = 0;

    // Build rows: for each anagram (including the word itself), show its hooks
    std::vector<std::tuple<QString, QString, QString, bool, bool>> rows;  // (frontHooks, word, backHooks, hasFrontHooks, hasBackHooks)
    bool anyHasFrontHooks = false;
    bool anyHasBackHooks = false;

    if (anagrams && anagrams->count > 0) {
        for (int i = 0; i < anagrams->count; i++) {
            QString anagram = QString(anagrams->words[i]).toUpper();

            // Only include exact anagrams (same length)
            if (anagram.length() != wordLength) {
                continue;
            }
            exactAnagrams++;

            // Find hooks for this anagram
            std::string anagramStr = anagram.toStdString();
            char* frontHooksStr = letterbox_find_front_hooks(kwg, ld, anagramStr.c_str());
            char* backHooksStr = letterbox_find_back_hooks(kwg, ld, anagramStr.c_str());

            QString frontHooks = frontHooksStr ? QString(frontHooksStr) : QString();
            QString backHooks = backHooksStr ? QString(backHooksStr) : QString();

            // Add spaces between hook letters (like in AlphagramBox)
            QString spacedFrontHooks;
            if (!frontHooks.isEmpty()) {
                for (int j = 0; j < frontHooks.length(); j++) {
                    if (j > 0) spacedFrontHooks += " ";
                    spacedFrontHooks += frontHooks[j];
                }
            }

            QString spacedBackHooks;
            if (!backHooks.isEmpty()) {
                for (int j = 0; j < backHooks.length(); j++) {
                    if (j > 0) spacedBackHooks += " ";
                    spacedBackHooks += backHooks[j];
                }
            }

            bool hasFront = !spacedFrontHooks.isEmpty();
            bool hasBack = !spacedBackHooks.isEmpty();

            if (hasFront) anyHasFrontHooks = true;
            if (hasBack) anyHasBackHooks = true;

            rows.push_back(std::make_tuple(spacedFrontHooks, anagram, spacedBackHooks, hasFront, hasBack));

            if (frontHooksStr) free(frontHooksStr);
            if (backHooksStr) free(backHooksStr);
        }
    }

    // Sort rows: put the original word first, then alphabetically
    std::sort(rows.begin(), rows.end(), [&wordUpper](const auto& a, const auto& b) {
        if (std::get<1>(a) == wordUpper) return true;
        if (std::get<1>(b) == wordUpper) return false;
        return std::get<1>(a) < std::get<1>(b);
    });

    // Wrap table in a centered container div
    html += "<div style='display: flex; justify-content: center; width: 100%;'>";

    // Build HTML table in AlphagramBox style with matching borders and tight cells
    html += "<table style='border-collapse: collapse; background-color: rgb(50, 50, 50); border: 1px solid rgb(90, 90, 90);'>";

    for (const auto& row : rows) {
        QString frontHooks = std::get<0>(row);
        QString anagram = std::get<1>(row);
        QString backHooks = std::get<2>(row);

        html += "<tr>";

        // Front hooks column (right-aligned) - only show if any word has front hooks
        if (anyHasFrontHooks) {
            html += "<td style='font-family: \"Jost\", sans-serif; color: #fff; padding: 8px 4px; text-align: right; border-right: 1px solid #666; vertical-align: top; font-size: 16px; font-weight: 500; white-space: nowrap;'>";
            html += frontHooks;
            html += "</td>";
        }

        // Word column (centered, bold, larger)
        QString wordBorder = anyHasBackHooks ? "border-right: 1px solid #666;" : "";
        html += QString("<td style='font-family: \"Jost\", sans-serif; font-size: 20px; font-weight: 600; letter-spacing: 1px; color: #fff; text-align: center; padding: 8px 4px; white-space: nowrap; %1'>%2</td>")
                .arg(wordBorder)
                .arg(anagram);

        // Back hooks column (left-aligned) - only show if any word has back hooks
        if (anyHasBackHooks) {
            html += "<td style='font-family: \"Jost\", sans-serif; color: #fff; padding: 8px 4px; text-align: left; vertical-align: top; font-size: 16px; font-weight: 500; white-space: nowrap;'>";
            html += backHooks;
            html += "</td>";
        }

        html += "</tr>";
    }

    html += "</table>";
    html += "</div>";  // Close centering div

    // Debug output
    if (showDebugInfo) {
        qDebug() << "Sidebar for" << wordUpper << ":" << totalAnagrams << "total anagrams," << exactAnagrams << "exact anagrams";
    }

    // Clean up
    if (anagrams) {
        word_list_destroy(anagrams);
    }

    return html;
}

void LetterboxWindow::hideWordHoverOverlay()
{
    if (wordHoverOverlay) {
        wordHoverOverlay->hide();
    }
}

void LetterboxWindow::updateHoverDebug(const QString& debugInfo)
{
    if (!showHoverDebugInfo || !hoverDebugLabel || !solvedContainer) {
        return;
    }

    hoverDebugLabel->setText(debugInfo);
    hoverDebugLabel->adjustSize();
    // Position at bottom-left corner
    int y = solvedContainer->height() - hoverDebugLabel->height();
    hoverDebugLabel->move(0, y);
    hoverDebugLabel->raise();
}

void LetterboxWindow::checkForCompletion()
{
    // Check if we've completed all alphagrams
    if (currentIndex >= static_cast<int>(alphagrams.size())) {
        showCompletionStats();
    }
}

void LetterboxWindow::showCompletionStats()
{
    // Calculate stats
    int totalAlphagrams = alphagrams.size();
    int totalWords = 0;
    int revealedWords = 0;
    int missedWords = 0;

    for (const auto& set : alphagrams) {
        for (const auto& entry : set.words) {
            totalWords++;
            if (entry.revealed && !entry.missed) {
                revealedWords++;
            }
            if (entry.missed) {
                missedWords++;
            }
        }
    }

    // Show dialog
    CompletionStatsDialog dialog(totalAlphagrams, totalWords, revealedWords, missedWords, this);
    dialog.exec();

    // Check if user wants to create missed words list
    if (dialog.shouldCreateMissedList()) {
        createMissedWordsList();
    }
}

void LetterboxWindow::createMissedWordsList()
{
    // Filter alphagrams to only those with at least one missed word
    std::vector<AlphagramSet> missedAlphagrams;
    for (const auto& set : alphagrams) {
        bool hasMissed = false;
        for (const auto& entry : set.words) {
            if (entry.missed) {
                hasMissed = true;
                break;
            }
        }
        if (hasMissed) {
            // Copy the alphagram set
            AlphagramSet missedSet = set;
            // Reset studied flag so it can be studied again
            missedSet.studied = false;
            // Keep revealed/missed states intact
            missedAlphagrams.push_back(missedSet);
        }
    }

    if (missedAlphagrams.empty()) {
        QMessageBox::information(this, "No Missed Words", "No missed words to create a list from.");
        return;
    }

    // Generate name for missed list
    QString missedListName = QString("Missed from %1").arg(currentListName);

    // Create filepath
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/saved_lists";
    QDir dir;
    if (!dir.exists(dataDir)) {
        dir.mkpath(dataDir);
    }

    QDateTime now = QDateTime::currentDateTime();
    QString sanitizedName = missedListName;
    sanitizedName.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
    QString filename = QString("%1_%2.json").arg(sanitizedName).arg(now.toString("yyyyMMdd_HHmmss"));
    QString filepath = dataDir + "/" + filename;

    // Save current state first (to preserve progress on original list)
    if (!currentListFilepath.isEmpty()) {
        saveCurrentList();
    }

    // Update state for new missed list
    alphagrams = missedAlphagrams;
    currentIndex = 0;
    currentListName = missedListName;
    currentListFilepath = filepath;

    // Save the new missed list
    saveCurrentList();

    // Add to recent lists
    addToRecentLists(filepath);

    // Clear undo state
    lastMissedIndex = -1;
    if (undoAction) {
        undoAction->setEnabled(false);
    }

    // Clear solved section
    QLayoutItem* item;
    while ((item = solvedLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    // Update UI
    updateWindowTitle();
    updateProgress();
    updateDisplay();

    QMessageBox::information(this, "Missed Words List Created",
                            QString("Created new list with %1 alphagram(s) containing missed words.").arg(alphagrams.size()));
}
