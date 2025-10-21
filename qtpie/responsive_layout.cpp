#include "responsive_layout.h"
#include "board_panel_view.h"
#include <QString>
#include <algorithm>

ResponsiveLayout::ResponsiveLayout(QWidget *content, QScrollArea *scroll,
                                   QWidget *board, QWidget *history,
                                   QWidget *analysis, int margin,
                                   int minAnalysisHeight)
    : m_content(content), m_scroll(scroll), m_board(board), m_history(history),
      m_analysis(analysis), m_margin(margin),
      m_minAnalysisHeight(minAnalysisHeight) {
  // Debug overlay - enabled
  m_debugText = new QPlainTextEdit(m_content);
  m_debugText->setStyleSheet(
      "background-color: yellow; color: black; border: 1px solid red;");
  m_debugText->setReadOnly(true);
  m_debugText->setFixedSize(350, 150);
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

  // Get board rendering info
  BoardPanelView *boardPanel = qobject_cast<BoardPanelView*>(m_board);
  if (boardPanel) {
    BoardView *boardView = boardPanel->getBoardView();
    if (boardView) {
      int squareSize = boardView->getSquareSize();
      int labelFontSize = boardView->getLabelFontSize();
      debugInfo.append(QString("Square: %1px, Label font: %2pt\n").arg(squareSize).arg(labelFontSize));
    }
  }

  // Get BoardPanelView's minimum size constraints
  QSize boardMinSize = m_board->minimumSizeHint();
  int minBoardWidth = boardMinSize.width();
  int minBoardHeight = boardMinSize.height();

  if (aspect >= (14.0 / 9.0)) {
    // Landscape: three widgets in one row.
    int availWidth = w - 4 * M;
    int availHeight = h - 2 * M;
    int boardWidth = availHeight * 3 / 4;

    // Enforce minimum board size
    boardWidth = std::max(boardWidth, minBoardWidth);
    int boardHeight = std::max(availHeight, minBoardHeight);

    m_board->setGeometry(M, M, boardWidth, boardHeight);

    int remainingWidth = availWidth - boardWidth;
    int eachWidth = remainingWidth / 2;
    m_history->setGeometry(M + boardWidth + M, M, eachWidth, availHeight);
    m_analysis->setGeometry(M + boardWidth + M + eachWidth + M, M,
                            remainingWidth - eachWidth, availHeight);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    debugInfo.append(QString("Layout: Landscape\n"));
    debugInfo.append(QString("Board: %1 x %2\n").arg(boardWidth).arg(boardHeight));
    debugInfo.append(QString("History: %1 x %2\n").arg(eachWidth).arg(availHeight));
    debugInfo.append(QString("Analysis: %1 x %2\n").arg(remainingWidth - eachWidth).arg(availHeight));
  } else if (aspect >= (2.0 / 3.0)) {
    // Intermediate layout.
    int topAvailWidth = w - 3 * M;
    int desiredBoardWidth = static_cast<int>(topAvailWidth * 0.4);
    int desiredBoardHeight = desiredBoardWidth * 4 / 3;
    int boardHeight =
        std::min(desiredBoardHeight, h - 3 * M - m_minAnalysisHeight);
    int boardWidth = boardHeight * 3 / 4;

    // Enforce minimum board size
    boardWidth = std::max(boardWidth, minBoardWidth);
    boardHeight = std::max(boardHeight, minBoardHeight);

    m_board->setGeometry(M, M, boardWidth, boardHeight);
    int historyWidth = topAvailWidth - boardWidth;
    int analysisWidth = w - 2 * M;
    int analysisHeight = h - boardHeight - 3 * M;
    m_history->setGeometry(M + boardWidth + M, M, historyWidth, boardHeight);
    m_analysis->setGeometry(M, M + boardHeight + M, analysisWidth, analysisHeight);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    debugInfo.append(QString("Layout: Intermediate\n"));
    debugInfo.append(QString("Board: %1 x %2\n").arg(boardWidth).arg(boardHeight));
    debugInfo.append(QString("History: %1 x %2\n").arg(historyWidth).arg(boardHeight));
    debugInfo.append(QString("Analysis: %1 x %2\n").arg(analysisWidth).arg(analysisHeight));
  } else {
    // Portrait layout: content might exceed available vertical space.
    int boardWidth = w - 2 * M;
    int desiredBoardHeight = boardWidth * 4 / 3;
    int boardHeight =
        std::min(desiredBoardHeight, h - 3 * M - m_minAnalysisHeight);

    // Enforce minimum board size
    boardWidth = std::max(boardWidth, minBoardWidth);
    boardHeight = std::max(boardHeight, minBoardHeight);

    m_board->setGeometry(M, M, boardWidth, boardHeight);
    int bottomAvailHeight = h - boardHeight - 3 * M;
    int bottomAvailWidth = w - 3 * M;
    int eachWidth = bottomAvailWidth / 2;
    int historyWidth = eachWidth;
    int analysisWidth = bottomAvailWidth - eachWidth;
    m_history->setGeometry(M, M + boardHeight + M, historyWidth, bottomAvailHeight);
    m_analysis->setGeometry(M + eachWidth + M, M + boardHeight + M,
                            analysisWidth, bottomAvailHeight);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    debugInfo.append(QString("Layout: Portrait\n"));
    debugInfo.append(QString("Board: %1 x %2\n").arg(boardWidth).arg(boardHeight));
    debugInfo.append(QString("History: %1 x %2\n").arg(historyWidth).arg(bottomAvailHeight));
    debugInfo.append(QString("Analysis: %1 x %2\n").arg(analysisWidth).arg(bottomAvailHeight));
  }

  // Update and center the debug text view (if enabled).
  if (m_debugText) {
    m_debugText->setPlainText(debugInfo);
    m_debugText->move((w - m_debugText->width()) / 2,
                      (h - m_debugText->height()) / 2);
    m_debugText->raise();
  }
}

void ResponsiveLayout::setDebugOverlayVisible(bool visible) {
  if (m_debugText) {
    m_debugText->setVisible(visible);
  }
}
