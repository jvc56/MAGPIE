#ifndef RESPONSIVE_LAYOUT_H
#define RESPONSIVE_LAYOUT_H

#include <QWidget>
#include <QScrollArea>
#include <QPlainTextEdit>

class ResponsiveLayout {
public:
    ResponsiveLayout(QWidget *content, QScrollArea *scroll,
                     QWidget *board, QWidget *history, QWidget *analysis,
                     int margin, int minAnalysisHeight);
    void updateLayout();
    void setDebugOverlayVisible(bool visible);

private:
    QWidget *m_content;
    QScrollArea *m_scroll;
    QWidget *m_board;
    QWidget *m_history;
    QWidget *m_analysis;
    int m_margin;
    int m_minAnalysisHeight;
    QPlainTextEdit *m_debugText;
};

#endif // RESPONSIVE_LAYOUT_H
