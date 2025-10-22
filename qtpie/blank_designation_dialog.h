#ifndef BLANK_DESIGNATION_DIALOG_H
#define BLANK_DESIGNATION_DIALOG_H

#include <QDialog>
#include <QPushButton>
#include <QChar>

class BlankDesignationDialog : public QDialog {
    Q_OBJECT
public:
    explicit BlankDesignationDialog(QWidget *parent = nullptr);

    // Get the selected letter (uppercase A-Z), or null if cancelled
    QChar getSelectedLetter() const { return m_selectedLetter; }

private:
    void createLetterGrid();
    void onLetterClicked(QChar letter);

    QChar m_selectedLetter;
};

#endif // BLANK_DESIGNATION_DIALOG_H
