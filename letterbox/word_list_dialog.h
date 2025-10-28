#ifndef WORD_LIST_DIALOG_H
#define WORD_LIST_DIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <vector>
#include <string>
#include <unordered_map>

// Include the structs from letterbox_window.h without including the whole QMainWindow
#include "letterbox_window.h"
#include "magpie_wrapper.h"

class WordListDialog : public QDialog {
    Q_OBJECT
public:
    explicit WordListDialog(const std::vector<AlphagramSet>& allAlphagrams,
                           const std::unordered_map<std::string, int>& playabilityScores,
                           KWG* kwg,
                           LetterDistribution* ld,
                           QWidget *parent = nullptr);

    std::vector<AlphagramSet> getFilteredList() const;
    QString getPattern() const;
    int getMinAnagramCount() const;
    int getMaxAnagramCount() const;

private slots:
    void updatePreview();
    void applyFilter();

private:
    QString formatAlphagramPreview(const AlphagramSet& set);
    std::string sortVowelsFirst(const std::string& word);

    const std::vector<AlphagramSet>& allAlphagrams;
    const std::unordered_map<std::string, int>& playabilityScores;
    KWG* kwg;
    LetterDistribution* ld;
    std::vector<AlphagramSet> filteredList;
    int maxAnagramCount;

    QLineEdit *patternInput;
    QSpinBox *minAnagramSpin;
    QSpinBox *maxAnagramSpin;
    QTextEdit *previewText;
    QLabel *statusLabel;
    QPushButton *applyButton;
    QPushButton *cancelButton;
};

#endif // WORD_LIST_DIALOG_H
