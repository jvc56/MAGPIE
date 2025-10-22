#include "blank_designation_dialog.h"
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QFontDatabase>
#include <QCoreApplication>
#include <QDir>

BlankDesignationDialog::BlankDesignationDialog(QWidget *parent)
    : QDialog(parent)
    , m_selectedLetter()
{
    setWindowTitle("Designate Blank");
    setModal(true);

    // Main layout - just the grid, no label
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(12, 12, 12, 12);

    // Load the ClearSans-Bold font (same as tiles)
    QString letterFontFamily;
    QDir fontsDir(QCoreApplication::applicationDirPath() + "/../Resources/fonts");
    if (!fontsDir.exists()) {
        fontsDir.setPath(QCoreApplication::applicationDirPath() + "/fonts");
    }
    int fontId = QFontDatabase::addApplicationFont(fontsDir.filePath("ClearSans-Bold.ttf"));
    if (fontId != -1) {
        QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        if (!families.isEmpty()) {
            letterFontFamily = families.first();
        }
    }
    if (letterFontFamily.isEmpty()) {
        letterFontFamily = "Arial";
    }

    // Letter grid: 7x4 layout
    // Row 0: A B C D E F G
    // Row 1: H I J K L M N
    // Row 2: O P Q R S T U
    // Row 3: V W X Y Z (left justified)
    QGridLayout *gridLayout = new QGridLayout();
    gridLayout->setSpacing(6);

    const QString letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int letterIndex = 0;

    // Tile-like styling colors (beige like board tiles)
    QString buttonStyle =
        "QPushButton {"
        "  background-color: rgb(245, 230, 190);"  // BOARD_TILE_COLOR
        "  border: 2px solid rgb(180, 170, 140);"  // BOARD_BORDER_COLOR
        "  border-radius: 8px;"
        "  color: rgb(0, 0, 0);"  // LETTER_COLOR
        "}"
        "QPushButton:hover {"
        "  background-color: rgb(255, 240, 200);"  // Slightly lighter on hover
        "}"
        "QPushButton:pressed {"
        "  background-color: rgb(235, 220, 180);"  // Slightly darker when pressed
        "}";

    for (int row = 0; row < 4; ++row) {
        int cols = (row < 3) ? 7 : 5;  // First 3 rows have 7 columns, last row has 5
        for (int col = 0; col < cols; ++col) {
            if (letterIndex >= letters.length()) break;

            QChar letter = letters[letterIndex];
            QPushButton *btn = new QPushButton(QString(letter), this);

            // Style the button like a tile
            btn->setMinimumSize(50, 50);
            btn->setMaximumSize(50, 50);
            btn->setStyleSheet(buttonStyle);

            // Use tile font
            QFont btnFont(letterFontFamily);
            btnFont.setPointSize(20);
            btnFont.setBold(true);
            btnFont.setHintingPreference(QFont::PreferNoHinting);
            btn->setFont(btnFont);

            // Connect button click
            connect(btn, &QPushButton::clicked, this, [this, letter]() {
                onLetterClicked(letter);
            });

            gridLayout->addWidget(btn, row, col);
            letterIndex++;
        }
    }

    mainLayout->addLayout(gridLayout);

    // Set fixed size
    setFixedSize(sizeHint());
}

void BlankDesignationDialog::onLetterClicked(QChar letter) {
    m_selectedLetter = letter;
    accept();  // Close dialog with Accepted result
}
