#include "letterbox_window.h"
#include "alphagram_box.h"
#include "word_list_dialog.h"
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
#include <algorithm>
#include <unordered_map>
#include <chrono>
#include <vector>

LetterboxWindow::LetterboxWindow(QWidget *parent)
    : QMainWindow(parent), config(nullptr), kwg(nullptr), ld(nullptr), currentIndex(0),
      globalMaxFrontWidth(0), globalMaxBackWidth(0), globalMaxWordWidth(0),
      renderWindowSize(15), userScrolledUp(false),
      scaleFactor(1.0), scaledWordSize(36), scaledHookSize(24), scaledExtensionSize(14),
      scaledInputSize(20), scaledQueueCurrentSize(24), scaledQueueUpcomingSize(16),
      showDebugInfo(false), showComputeTime(false), showRenderTime(false), lastRenderTimeMicros(0)
{
    setupUI();
    setupMenuBar();
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
    solvedScrollArea = new QScrollArea(this);
    solvedScrollArea->setWidgetResizable(true);
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
    solvedLayout = new QVBoxLayout(solvedWidget);
    solvedLayout->setAlignment(Qt::AlignBottom | Qt::AlignHCenter);
    solvedLayout->setSpacing(10);  // 10px vertical spacing between boxes (2.5x more than 4px)

    solvedScrollArea->setWidget(solvedWidget);
    mainLayout->addWidget(solvedScrollArea, 4);

    // Middle: Input field (full width)
    inputField = new QLineEdit(this);
    inputField->setPlaceholderText("...");
    inputField->setAlignment(Qt::AlignCenter);
    inputField->setStyleSheet("QLineEdit { padding: 15px; font-family: 'Jost', sans-serif; font-size: 20px; font-weight: bold; margin: 2px 0px; background-color: rgb(40, 40, 40); color: white; border: 2px solid #3a7f9f; text-transform: uppercase; }");
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
    QScrollArea* queueScrollArea = new QScrollArea(this);
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
    mainLayout->addWidget(queueScrollArea, 1);

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

    if (currentIndex >= alphagrams.size()) {
        // Clear solved layout and show "Done!" message
        QLayoutItem* item;
        while ((item = solvedLayout->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        QLabel* doneLabel = new QLabel("Done!");
        doneLabel->setAlignment(Qt::AlignCenter);
        doneLabel->setStyleSheet("font-size: 24px; padding: 20px;");
        solvedLayout->addWidget(doneLabel);
        queueLabel->clear();
        inputField->setEnabled(false);
        return;
    }

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
                           frontExtStr, backExtStr, false, cache.computeTimeMicros);
            }
            boxes.push_back(box);
        }
    }

    // Add current alphagram at the bottom (with placeholders for unrevealed words)
    AlphagramBox* currentBox = new AlphagramBox(solvedWidget);
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
                              frontExtStr, backExtStr, false, cache.computeTimeMicros);
        } else {
            // Show placeholder dashes (one dash per letter) - gray and regular weight
            QString placeholder;
            for (size_t i = 0; i < entry.word.length(); i++) {
                placeholder += "-";
            }
            int wordWidth = wordMetrics.horizontalAdvance(placeholder);
            globalMaxWordWidth = std::max(globalMaxWordWidth, wordWidth);
            currentBox->addWord(placeholder, "", "", "", "", true);  // true = isPlaceholder
        }
    }
    boxes.push_back(currentBox);
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
    queueLabel->setText(queueHtml);
    auto t7 = std::chrono::high_resolution_clock::now();
    int queueHtmlMicros = std::chrono::duration_cast<std::chrono::microseconds>(t7 - t6).count();

    // Clear and focus input field
    auto t8 = std::chrono::high_resolution_clock::now();
    inputField->clear();
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
                if (currentIndex < alphagrams.size() - 1) {
                    currentIndex++;
                    // Reset revealed states for new alphagram
                    for (auto& e : alphagrams[currentIndex].words) {
                        e.revealed = false;
                    }
                }
            }

            updateDisplay();
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

void LetterboxWindow::setupMenuBar()
{
    QMenuBar* menuBar = new QMenuBar(this);
    setMenuBar(menuBar);

    // Words menu
    QMenu* wordsMenu = menuBar->addMenu("Words");
    QAction* createListAction = new QAction("Create Word List...", this);
    connect(createListAction, &QAction::triggered, this, &LetterboxWindow::createCustomWordList);
    wordsMenu->addAction(createListAction);

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

    double oldScaleFactor = scaleFactor;

    updateScaledFontSizes();
    updateDebugInfo();

    // Only redraw if scale factor actually changed
    if (oldScaleFactor != scaleFactor && currentIndex < static_cast<int>(alphagrams.size())) {
        updateDisplay();
    }
}

void LetterboxWindow::updateScaledFontSizes()
{
    int windowWidth = width();

    // Calculate scale factor: 1.0 at 1200px and above, proportional below
    if (windowWidth >= 1200) {
        scaleFactor = 1.0;
    } else {
        scaleFactor = static_cast<double>(windowWidth) / 1200.0;
    }

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
    WordListDialog dialog(allAlphagrams, playabilityScores, this);

    if (dialog.exec() == QDialog::Accepted) {
        // Get filtered list and update current alphagrams
        alphagrams = dialog.getFilteredList();
        currentIndex = 0;

        // Reset progress and update display
        for (auto& set : alphagrams) {
            set.studied = false;
            for (auto& word : set.words) {
                word.revealed = false;
            }
        }

        // Clear solved section
        QLayoutItem* item;
        while ((item = solvedLayout->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }

        updateProgress();
        updateDisplay();
    }
}
