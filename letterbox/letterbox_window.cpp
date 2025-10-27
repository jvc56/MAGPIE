#include "letterbox_window.h"
#include "alphagram_box.h"
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

LetterboxWindow::LetterboxWindow(QWidget *parent)
    : QMainWindow(parent), config(nullptr), kwg(nullptr), ld(nullptr), currentIndex(0),
      globalMaxFrontWidth(0), globalMaxBackWidth(0), globalMaxWordWidth(0)
{
    setupUI();

    // Initialize MAGPIE
    // Get the application directory (where the .app bundle is)
    QString appDir = QCoreApplication::applicationDirPath();
    // On macOS, go up from Letterbox.app/Contents/MacOS to letterbox/
    QDir dir(appDir);
    dir.cdUp();  // Contents
    dir.cdUp();  // Letterbox.app
    dir.cdUp();  // letterbox (or wherever the .app is)
    QString dataPath = dir.absolutePath() + "/data";

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
    mainLayout->addWidget(solvedScrollArea, 3);

    // Middle: Input field (full width)
    inputField = new QLineEdit(this);
    inputField->setPlaceholderText("...");
    inputField->setAlignment(Qt::AlignCenter);
    inputField->setStyleSheet("QLineEdit { padding: 15px; font-size: 20px; font-weight: bold; margin: 2px 0px; background-color: rgb(40, 40, 40); color: white; border: 2px solid #3a7f9f; }");
    connect(inputField, &QLineEdit::textChanged, this, &LetterboxWindow::onTextChanged);
    mainLayout->addWidget(inputField);

    // Bottom: Queue of upcoming alphagrams (scrollable, ~25% of space)
    QScrollArea* queueScrollArea = new QScrollArea(this);
    queueScrollArea->setWidgetResizable(true);
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

    queueLabel = new QLabel("", queueWidget);
    queueLabel->setAlignment(Qt::AlignCenter | Qt::AlignTop);
    queueLabel->setWordWrap(true);
    queueLabel->setTextFormat(Qt::RichText);
    queueLabel->setStyleSheet("QLabel { padding: 0px; margin: 0px; color: #888; }");
    queueLayout->addWidget(queueLabel);

    queueScrollArea->setWidget(queueWidget);
    mainLayout->addWidget(queueScrollArea, 1);

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

        int pb = parts[0].toInt();
        std::string word = parts[1].toStdString();
        std::string alphagram = sortVowelsFirst(word);

        alphagramPlayability[alphagram] += pb;
        alphagramWords[alphagram].push_back(word);
    }
    file.close();

    qDebug() << "Found" << alphagramWords.size() << "unique alphagrams";

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
    for (int i = 0; i < currentIndex; i++) {
        if (alphagrams[i].studied) {
            AlphagramBox* box = new AlphagramBox(solvedWidget);
            for (const auto& entry : alphagrams[i].words) {
                char* frontHooks = letterbox_find_front_hooks(kwg, ld, entry.word.c_str());
                char* backHooks = letterbox_find_back_hooks(kwg, ld, entry.word.c_str());
                char* frontExts = letterbox_find_front_extensions(kwg, ld, entry.word.c_str(), 3);
                char* backExts = letterbox_find_back_extensions(kwg, ld, entry.word.c_str(), 3);

                QString frontStr = QString::fromUtf8(frontHooks ? frontHooks : "");
                QString backStr = QString::fromUtf8(backHooks ? backHooks : "");
                QString frontExtStr = QString::fromUtf8(frontExts ? frontExts : "");
                QString backExtStr = QString::fromUtf8(backExts ? backExts : "");

                // Calculate widths for global max
                int frontWidth = hookMetrics.horizontalAdvance(frontStr);
                int backWidth = hookMetrics.horizontalAdvance(backStr);
                int wordWidth = wordMetrics.horizontalAdvance(QString::fromStdString(entry.word));
                globalMaxFrontWidth = std::max(globalMaxFrontWidth, frontWidth);
                globalMaxBackWidth = std::max(globalMaxBackWidth, backWidth);
                globalMaxWordWidth = std::max(globalMaxWordWidth, wordWidth);

                free(frontHooks);
                free(backHooks);
                free(frontExts);
                free(backExts);

                box->addWord(QString::fromStdString(entry.word), frontStr, backStr,
                           frontExtStr, backExtStr);
            }
            boxes.push_back(box);
        }
    }

    // Add current alphagram at the bottom (with placeholders for unrevealed words)
    AlphagramBox* currentBox = new AlphagramBox(solvedWidget);
    for (const auto& entry : alphagrams[currentIndex].words) {
        if (entry.revealed) {
            // Show the actual word with hooks
            char* frontHooks = letterbox_find_front_hooks(kwg, ld, entry.word.c_str());
            char* backHooks = letterbox_find_back_hooks(kwg, ld, entry.word.c_str());

            QString frontStr = QString::fromUtf8(frontHooks ? frontHooks : "");
            QString backStr = QString::fromUtf8(backHooks ? backHooks : "");

            // Calculate widths for global max
            int frontWidth = hookMetrics.horizontalAdvance(frontStr);
            int backWidth = hookMetrics.horizontalAdvance(backStr);
            int wordWidth = wordMetrics.horizontalAdvance(QString::fromStdString(entry.word));
            globalMaxFrontWidth = std::max(globalMaxFrontWidth, frontWidth);
            globalMaxBackWidth = std::max(globalMaxBackWidth, backWidth);
            globalMaxWordWidth = std::max(globalMaxWordWidth, wordWidth);

            free(frontHooks);
            free(backHooks);

            currentBox->addWord(QString::fromStdString(entry.word), frontStr, backStr);
        } else {
            // Show placeholder dashes (one dash per letter)
            QString placeholder;
            for (size_t i = 0; i < entry.word.length(); i++) {
                placeholder += "-";
            }
            int wordWidth = wordMetrics.horizontalAdvance(placeholder);
            globalMaxWordWidth = std::max(globalMaxWordWidth, wordWidth);
            currentBox->addWord(placeholder, "", "");
        }
    }
    boxes.push_back(currentBox);

    // Finalize and add all boxes to layout
    for (auto* box : boxes) {
        box->finalize();
        solvedLayout->addWidget(box, 0, Qt::AlignCenter);
    }

    // Bottom: Show queue of UPCOMING alphagrams (not including current) - starts with current alphagram
    QString queueHtml = "<div style='text-align: center;'>";

    // Show current alphagram at top of queue (Jost Bold, white)
    queueHtml += QString("<div style='font-family: \"Jost\", sans-serif; font-size: 24px; margin: 5px 0; letter-spacing: 3px; font-weight: bold; color: #fff;'>%1</div>")
            .arg(QString::fromStdString(alphagrams[currentIndex].alphagram));

    // Show upcoming alphagrams (Jost Regular - normal weight, gray)
    int queueCount = std::min(10, (int)alphagrams.size() - currentIndex - 1);
    for (int i = 1; i <= queueCount; i++) {
        const AlphagramSet& set = alphagrams[currentIndex + i];
        queueHtml += QString("<div style='font-family: \"Jost\", sans-serif; font-size: 16px; margin: 5px 0; letter-spacing: 2px; font-weight: normal; color: #888;'>%1</div>")
                .arg(QString::fromStdString(set.alphagram));
    }
    queueHtml += "</div>";
    queueLabel->setText(queueHtml);

    // Clear and focus input field
    inputField->clear();
    inputField->setFocus();

    // Scroll to bottom to keep the current alphagram visible
    // Need a small delay to ensure layout is fully calculated
    QTimer::singleShot(50, this, [this]() {
        solvedWidget->updateGeometry();
        solvedScrollArea->verticalScrollBar()->setValue(
            solvedScrollArea->verticalScrollBar()->maximum()
        );
    });

    updateProgress();
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
