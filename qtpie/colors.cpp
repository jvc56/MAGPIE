#include "colors.h"

#include <QApplication>
#include <QPalette>
#include <QColor>

void applyDarkPalette(QApplication &app) {
    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, QColor(28, 30, 32));
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, QColor(25, 25, 25));
    darkPalette.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
    darkPalette.setColor(QPalette::ToolTipBase, Qt::white);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, QColor(53, 53, 53));
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::BrightText, Qt::red);
    darkPalette.setColor(QPalette::Link, QColor(42, 130, 218));
    darkPalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    darkPalette.setColor(QPalette::HighlightedText, Qt::black);
    app.setPalette(darkPalette);
}

void applyLightPalette(QApplication &app) {
    QPalette lightPalette;
    // Main window background - light gray-purple matching board background
    lightPalette.setColor(QPalette::Window, QColor(230, 230, 240));
    lightPalette.setColor(QPalette::WindowText, QColor(33, 33, 33));  // Dark text

    // Input fields and text areas - white background
    lightPalette.setColor(QPalette::Base, QColor(255, 255, 255));
    lightPalette.setColor(QPalette::AlternateBase, QColor(245, 245, 250));

    // Text colors
    lightPalette.setColor(QPalette::Text, QColor(33, 33, 33));  // Dark text
    lightPalette.setColor(QPalette::BrightText, Qt::black);

    // Buttons
    lightPalette.setColor(QPalette::Button, QColor(240, 240, 245));
    lightPalette.setColor(QPalette::ButtonText, QColor(33, 33, 33));

    // Tooltips
    lightPalette.setColor(QPalette::ToolTipBase, Qt::white);
    lightPalette.setColor(QPalette::ToolTipText, QColor(33, 33, 33));

    // Selection/highlighting
    lightPalette.setColor(QPalette::Highlight, QColor(100, 150, 200));
    lightPalette.setColor(QPalette::HighlightedText, Qt::white);

    // Links
    lightPalette.setColor(QPalette::Link, QColor(42, 130, 218));

    app.setPalette(lightPalette);
}
