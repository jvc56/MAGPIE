#include "word_list_dialog.h"
#include "letterbox_window.h"
#include "magpie_wrapper.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QRegularExpression>
#include <QFont>
#include <QScrollBar>
#include <algorithm>
#include <unordered_set>

WordListDialog::WordListDialog(const std::vector<AlphagramSet>& allAlphagrams,
                               const std::unordered_map<std::string, int>& playabilityScores,
                               KWG* kwg,
                               LetterDistribution* ld,
                               QWidget *parent)
    : QDialog(parent), allAlphagrams(allAlphagrams), playabilityScores(playabilityScores),
      kwg(kwg), ld(ld), maxAnagramCount(0)
{
    setWindowTitle("Create New Word List");
    setMinimumSize(800, 600);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Filter controls group
    QGroupBox *filterGroup = new QGroupBox();
    QFormLayout *filterLayout = new QFormLayout(filterGroup);

    // Pattern input
    patternInput = new QLineEdit();
    patternInput->setPlaceholderText("...");
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
    previewText = new QTextEdit();
    previewText->setReadOnly(true);
    previewText->setFont(QFont("Consolas", 13));
    mainLayout->addWidget(previewText);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    cancelButton = new QPushButton("Cancel");
    cancelButton->setAutoDefault(false);  // Don't make this the default button
    cancelButton->setShortcut(Qt::Key_Escape);  // Esc key closes dialog
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelButton);

    applyButton = new QPushButton("Study");
    applyButton->setEnabled(false);
    applyButton->setDefault(true);  // Make this the default button (Enter key activates)
    connect(applyButton, &QPushButton::clicked, this, &WordListDialog::applyFilter);
    buttonLayout->addWidget(applyButton);

    mainLayout->addLayout(buttonLayout);

    // Initial update
    updatePreview();
}

QString WordListDialog::formatAlphagramPreview(const AlphagramSet& set)
{
    QString result;

    // Show words in a simple table format (no hooks)
    for (const auto& entry : set.words) {
        if (!result.isEmpty()) {
            result += " ";  // Space between words
        }
        result += QString::fromStdString(entry.word);
    }

    return result;
}

std::string WordListDialog::sortVowelsFirst(const std::string& word)
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

void WordListDialog::updatePreview()
{
    QString pattern = patternInput->text();
    int minCount = minAnagramSpin->value();
    int maxCount = maxAnagramSpin->value();

    // Filter alphagrams using KWG-based pattern search
    filteredList.clear();
    int currentMaxAnagramCount = 0;

    if (pattern.isEmpty()) {
        // No pattern - use all alphagrams
        for (const auto& set : allAlphagrams) {
            int anagramCount = set.words.size();

            if (anagramCount > currentMaxAnagramCount) {
                currentMaxAnagramCount = anagramCount;
            }

            if (anagramCount >= minCount && anagramCount <= maxCount) {
                filteredList.push_back(set);
            }
        }
    } else {
        // Use KWG-based pattern search
        WordList* patternResults = letterbox_find_anagrams_by_pattern(kwg, ld, pattern.toUtf8().constData());

        if (patternResults && patternResults->count > 0) {
            // Create a set of matching alphagrams for fast lookup
            // Convert from alphabetical sorting to vowels-first sorting
            std::unordered_set<std::string> matchingAlphagrams;
            for (int i = 0; i < patternResults->count; i++) {
                // Pattern search returns alphabetically sorted, convert to vowels-first
                std::string vowelsFirst = sortVowelsFirst(patternResults->words[i]);
                matchingAlphagrams.insert(vowelsFirst);
            }

            // Find matching alphagram sets in our full list
            for (const auto& set : allAlphagrams) {
                if (matchingAlphagrams.count(set.alphagram) > 0) {
                    int anagramCount = set.words.size();

                    if (anagramCount > currentMaxAnagramCount) {
                        currentMaxAnagramCount = anagramCount;
                    }

                    if (anagramCount >= minCount && anagramCount <= maxCount) {
                        filteredList.push_back(set);
                    }
                }
            }
        }

        word_list_destroy(patternResults);
    }

    // Update max anagram count and adjust spinner
    maxAnagramCount = currentMaxAnagramCount;
    if (maxAnagramCount > 0) {
        maxAnagramSpin->setMaximum(maxAnagramCount);
    }

    // Update status
    int resultCount = filteredList.size();
    QString resultText = (resultCount == 1) ? "result" : "results";
    statusLabel->setText(QString("Found %1 %2").arg(resultCount).arg(resultText));

    // Update preview
    QString preview;
    int previewLimit = 100;  // Show first 100 alphagram sets
    for (size_t i = 0; i < std::min((size_t)previewLimit, filteredList.size()); i++) {
        const auto& set = filteredList[i];
        preview += QString::fromStdString(set.alphagram) + ": ";
        preview += formatAlphagramPreview(set);
        preview += "\n";
    }

    if (filteredList.size() > (size_t)previewLimit) {
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

QString WordListDialog::getPattern() const
{
    return patternInput->text();
}

int WordListDialog::getMinAnagramCount() const
{
    return minAnagramSpin->value();
}

int WordListDialog::getMaxAnagramCount() const
{
    return maxAnagramSpin->value();
}
