#include "word_list_dialog.h"
#include "letterbox_window.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QRegularExpression>
#include <QFont>
#include <QScrollBar>
#include <algorithm>

WordListDialog::WordListDialog(const std::vector<AlphagramSet>& allAlphagrams,
                               const std::unordered_map<std::string, int>& playabilityScores,
                               QWidget *parent)
    : QDialog(parent), allAlphagrams(allAlphagrams), playabilityScores(playabilityScores), maxAnagramCount(0)
{
    setWindowTitle("Create Word List");
    setMinimumSize(800, 600);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Filter controls group
    QGroupBox *filterGroup = new QGroupBox("Filter Options");
    QFormLayout *filterLayout = new QFormLayout(filterGroup);

    // Pattern input
    patternInput = new QLineEdit();
    patternInput->setPlaceholderText("e.g., .......  or  [JQXZ]......  or  A??E???");
    connect(patternInput, &QLineEdit::textChanged, this, &WordListDialog::updatePreview);
    filterLayout->addRow("Anagram Pattern:", patternInput);

    // Min/Max anagram count
    QHBoxLayout *anagramCountLayout = new QHBoxLayout();
    minAnagramSpin = new QSpinBox();
    minAnagramSpin->setMinimum(1);
    minAnagramSpin->setMaximum(999);
    minAnagramSpin->setValue(1);
    connect(minAnagramSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &WordListDialog::updatePreview);

    maxAnagramSpin = new QSpinBox();
    maxAnagramSpin->setMinimum(1);
    maxAnagramSpin->setMaximum(999);
    maxAnagramSpin->setValue(999);
    connect(maxAnagramSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &WordListDialog::updatePreview);

    anagramCountLayout->addWidget(new QLabel("Min:"));
    anagramCountLayout->addWidget(minAnagramSpin);
    anagramCountLayout->addWidget(new QLabel("Max:"));
    anagramCountLayout->addWidget(maxAnagramSpin);
    anagramCountLayout->addStretch();

    filterLayout->addRow("Anagram Count:", anagramCountLayout);

    mainLayout->addWidget(filterGroup);

    // Status label
    statusLabel = new QLabel("Enter a pattern to filter words");
    statusLabel->setStyleSheet("color: #666; font-size: 11px;");
    mainLayout->addWidget(statusLabel);

    // Preview area
    QLabel *previewLabel = new QLabel("Preview:");
    previewLabel->setStyleSheet("font-weight: bold; margin-top: 10px;");
    mainLayout->addWidget(previewLabel);

    previewText = new QTextEdit();
    previewText->setReadOnly(true);
    previewText->setFont(QFont("Jost", 14));
    mainLayout->addWidget(previewText);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    cancelButton = new QPushButton("Cancel");
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelButton);

    applyButton = new QPushButton("Apply");
    applyButton->setEnabled(false);
    connect(applyButton, &QPushButton::clicked, this, &WordListDialog::applyFilter);
    buttonLayout->addWidget(applyButton);

    mainLayout->addLayout(buttonLayout);

    // Initial update
    updatePreview();
}

bool WordListDialog::matchesPattern(const std::string& alphagram, const QString& pattern)
{
    if (pattern.isEmpty()) {
        return true;  // Empty pattern matches everything
    }

    // Convert to uppercase for case-insensitive matching
    QString patternUpper = pattern.toUpper();
    QString alphagramStr = QString::fromStdString(alphagram).toUpper();

    // Build multiset (letter counts) for the alphagram
    std::unordered_map<QChar, int> alphagramCounts;
    for (QChar c : alphagramStr) {
        alphagramCounts[c]++;
    }

    // Parse pattern and count required letters
    int patternLength = 0;
    std::unordered_map<QChar, int> requiredLetters;
    std::vector<QString> characterClasses;

    // Extract character classes like [JQXZ]
    QRegularExpression classRegex("\\[([A-Za-z]+)\\]");
    QRegularExpressionMatchIterator it = classRegex.globalMatch(patternUpper);

    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        characterClasses.push_back(match.captured(1).toUpper());
        patternLength++;
    }

    // Remove character classes from pattern
    QString patternWithoutClasses = patternUpper;
    patternWithoutClasses.remove(classRegex);

    // Process remaining characters
    for (QChar c : patternWithoutClasses) {
        if (c == '.' || c == '?') {
            // Wildcard - just counts toward length
            patternLength++;
        } else if (c.isLetter()) {
            requiredLetters[c.toUpper()]++;
            patternLength++;
        }
        // Skip non-letter, non-wildcard characters
    }

    // Check total length
    if (alphagramStr.length() != patternLength) {
        return false;
    }

    // Check that alphagram contains required letters
    for (const auto& [letter, count] : requiredLetters) {
        if (alphagramCounts[letter] < count) {
            return false;
        }
    }

    // Check character classes - alphagram must contain at least one letter from each class
    for (const QString& charClass : characterClasses) {
        bool found = false;
        for (QChar c : charClass) {
            if (alphagramCounts[c.toUpper()] > 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    return true;
}

QString WordListDialog::formatAlphagramPreview(const AlphagramSet& set)
{
    QString result;

    // Show words in a simple table format (no hooks)
    for (const auto& entry : set.words) {
        if (!result.isEmpty()) {
            result += "  ";  // Space between words
        }
        result += QString::fromStdString(entry.word);
    }

    return result;
}

void WordListDialog::updatePreview()
{
    QString pattern = patternInput->text();
    int minCount = minAnagramSpin->value();
    int maxCount = maxAnagramSpin->value();

    // Filter alphagrams
    filteredList.clear();
    int currentMaxAnagramCount = 0;

    for (const auto& set : allAlphagrams) {
        // Check pattern match
        if (!matchesPattern(set.alphagram, pattern)) {
            continue;
        }

        int anagramCount = set.words.size();

        // Track max anagram count in filtered results
        if (anagramCount > currentMaxAnagramCount) {
            currentMaxAnagramCount = anagramCount;
        }

        // Check anagram count range
        if (anagramCount >= minCount && anagramCount <= maxCount) {
            filteredList.push_back(set);
        }
    }

    // Update max anagram count and adjust spinner
    maxAnagramCount = currentMaxAnagramCount;
    if (maxAnagramCount > 0) {
        maxAnagramSpin->setMaximum(maxAnagramCount);
    }

    // Update status
    statusLabel->setText(QString("Found %1 alphagram(s) matching pattern (max %2 anagrams)")
                        .arg(filteredList.size())
                        .arg(maxAnagramCount));

    // Update preview
    QString preview;
    int previewLimit = 100;  // Show first 100 alphagram sets
    for (int i = 0; i < std::min(previewLimit, (int)filteredList.size()); i++) {
        const auto& set = filteredList[i];
        preview += QString::fromStdString(set.alphagram) + ": ";
        preview += formatAlphagramPreview(set);
        preview += "\n";
    }

    if (filteredList.size() > previewLimit) {
        preview += QString("\n... and %1 more alphagram(s)").arg(filteredList.size() - previewLimit);
    }

    previewText->setPlainText(preview);
    previewText->verticalScrollBar()->setValue(0);  // Scroll to top

    // Enable apply button if we have results
    applyButton->setEnabled(filteredList.size() > 0);
}

void WordListDialog::applyFilter()
{
    accept();  // Close dialog with success
}

std::vector<AlphagramSet> WordListDialog::getFilteredList() const
{
    return filteredList;
}
