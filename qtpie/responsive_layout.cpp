#include "responsive_layout.h"
#include <QString>
#include <algorithm>

ResponsiveLayout::ResponsiveLayout(QWidget *content, QScrollArea *scroll,
                                   QWidget *board, QWidget *history,
                                   QWidget *analysis, int margin,
                                   int minAnalysisHeight)
    : m_content(content), m_scroll(scroll), m_board(board), m_history(history),
      m_analysis(analysis), m_margin(margin),
      m_minAnalysisHeight(minAnalysisHeight) {
  m_debugText = new QPlainTextEdit(m_content);
  m_debugText->setStyleSheet(
      "background-color: yellow; color: black; border: 1px solid red;");
  m_debugText->setReadOnly(true);
  m_debugText->setFixedSize(300, 100);
  m_debugText->show();
}

QString approxAspectRatio(int w, int h) {
  int bestNumerator = 0;
  int bestDenominator = 0;
  double bestError = 1.0;
  for (int denominator = 1; denominator <= 10; ++denominator) {
    int numerator =
        static_cast<int>(denominator * static_cast<double>(w) / h + 0.5);
    double error = std::abs(static_cast<double>(numerator) / denominator -
                            static_cast<double>(w) / h);
    if (error < bestError) {
      bestError = error;
      bestNumerator = numerator;
      bestDenominator = denominator;
    }
  }
  if (bestError < 0.01) {
    return QString("%1:%2").arg(bestNumerator).arg(bestDenominator);
  } else {
    return QString("%1:%2 (approx)").arg(bestNumerator).arg(bestDenominator);
  }
}

void ResponsiveLayout::updateLayout() {
  int M = m_margin;
  int w = m_content->width();
  int h = m_content->height();

  QString debugInfo = QString("Resolution: %1 x %2, %3\n")
                          .arg(w)
                          .arg(h)
                          .arg(approxAspectRatio(w, h));
  double aspect = static_cast<double>(w) / h;

  if (aspect >= (14.0 / 9.0)) {
    // Landscape: three widgets in one row.
    int availWidth = w - 4 * M;
    int availHeight = h - 2 * M;
    int boardWidth = availHeight * 3 / 4;
    m_board->setGeometry(M, M, boardWidth, availHeight);

    int remainingWidth = availWidth - boardWidth;
    int eachWidth = remainingWidth / 2;
    m_history->setGeometry(M + boardWidth + M, M, eachWidth, availHeight);
    m_analysis->setGeometry(M + boardWidth + M + eachWidth + M, M,
                            remainingWidth - eachWidth, availHeight);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    debugInfo.append("Layout: Landscape\n");
  } else if (aspect >= (2.0 / 3.0)) {
    // Intermediate layout.
    int topAvailWidth = w - 3 * M;
    int desiredBoardWidth = static_cast<int>(topAvailWidth * 0.4);
    int desiredBoardHeight = desiredBoardWidth * 4 / 3;
    int boardHeight =
        std::min(desiredBoardHeight, h - 3 * M - m_minAnalysisHeight);
    int boardWidth = boardHeight * 3 / 4;
    m_board->setGeometry(M, M, boardWidth, boardHeight);
    m_history->setGeometry(M + boardWidth + M, M, topAvailWidth - boardWidth,
                           boardHeight);
    m_analysis->setGeometry(M, M + boardHeight + M, w - 2 * M,
                            h - boardHeight - 3 * M);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    debugInfo.append("Layout: Intermediate\n");
  } else {
    // Portrait layout: content might exceed available vertical space.
    int boardWidth = w - 2 * M;
    int desiredBoardHeight = boardWidth * 4 / 3;
    int boardHeight =
        std::min(desiredBoardHeight, h - 3 * M - m_minAnalysisHeight);
    m_board->setGeometry(M, M, boardWidth, boardHeight);
    int bottomAvailHeight = h - boardHeight - 3 * M;
    int bottomAvailWidth = w - 3 * M;
    int eachWidth = bottomAvailWidth / 2;
    m_history->setGeometry(M, M + boardHeight + M, eachWidth,
                           bottomAvailHeight);
    m_analysis->setGeometry(M + eachWidth + M, M + boardHeight + M,
                            bottomAvailWidth - eachWidth, bottomAvailHeight);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    debugInfo.append("Layout: Portrait\n");
  }

  // Update and center the debug text view.
  m_debugText->setPlainText(debugInfo);
  m_debugText->move((w - m_debugText->width()) / 2,
                    (h - m_debugText->height()) / 2);
  m_debugText->raise();
}
