#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QTextEdit>
#include <QTextCursor>
#include <QLineEdit>
#include <QPushButton>
#include <QFont>
#include <QFontDatabase>
#include <QDir>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QEventLoop>
#include <QTime>
#include <QSplitter>
#include <QClipboard>
#include <QGuiApplication>

#include "magpie_wrapper.h"
#include "board_panel_view.h"
#include "colors.h"
#include "tile_renderer.h"
#include "game_history_panel.h"

// Separate debug window for development
class DebugWindow : public QMainWindow {
    Q_OBJECT
public:
    DebugWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("QtPie Debug");
        resize(800, 600);

        QWidget *central = new QWidget(this);
        QVBoxLayout *layout = new QVBoxLayout(central);

        // Board History section
        QLabel *historyLabel = new QLabel("Board History");
        historyLabel->setStyleSheet("color: #333333; font-weight: bold; font-size: 12px;");

        // History filter bar
        historyFilter = new QLineEdit;
        historyFilter->setPlaceholderText("Filter (shows only matching lines)...");
        historyFilter->setClearButtonEnabled(true);
        historyFilter->setStyleSheet(
            "QLineEdit {"
            "  background-color: white;"
            "  color: #333333;"
            "  border: 1px solid #C0C0D0;"
            "  border-radius: 4px;"
            "  padding: 4px;"
            "}"
        );

        // History search bar with navigation
        QWidget *historySearchWidget = new QWidget;
        QHBoxLayout *historySearchLayout = new QHBoxLayout(historySearchWidget);
        historySearchLayout->setContentsMargins(0, 0, 0, 0);
        historySearchLayout->setSpacing(4);

        historySearch = new QLineEdit;
        historySearch->setPlaceholderText("Search (highlights matches)...");
        historySearch->setClearButtonEnabled(true);
        historySearch->setStyleSheet(
            "QLineEdit {"
            "  background-color: white;"
            "  color: #333333;"
            "  border: 1px solid #C0C0D0;"
            "  border-radius: 4px;"
            "  padding: 4px;"
            "}"
        );

        historyPrevButton = new QPushButton("◀");
        historyPrevButton->setFixedWidth(30);
        historyPrevButton->setToolTip("Previous match");
        historyPrevButton->setStyleSheet(
            "QPushButton {"
            "  background-color: #E8E8F0;"
            "  color: #333333;"
            "  border: 1px solid #C0C0D0;"
            "  border-radius: 4px;"
            "  padding: 4px;"
            "}"
            "QPushButton:hover {"
            "  background-color: #D8D8E8;"
            "}"
        );

        historyNextButton = new QPushButton("▶");
        historyNextButton->setFixedWidth(30);
        historyNextButton->setToolTip("Next match");
        historyNextButton->setStyleSheet(
            "QPushButton {"
            "  background-color: #E8E8F0;"
            "  color: #333333;"
            "  border: 1px solid #C0C0D0;"
            "  border-radius: 4px;"
            "  padding: 4px;"
            "}"
            "QPushButton:hover {"
            "  background-color: #D8D8E8;"
            "}"
        );

        historyMatchLabel = new QLabel("0/0");
        historyMatchLabel->setFixedWidth(50);
        historyMatchLabel->setAlignment(Qt::AlignCenter);
        historyMatchLabel->setStyleSheet("color: #666666; font-size: 11px;");

        historySearchLayout->addWidget(historySearch, 1);
        historySearchLayout->addWidget(historyPrevButton);
        historySearchLayout->addWidget(historyNextButton);
        historySearchLayout->addWidget(historyMatchLabel);

        historyTextView = new QTextEdit;
        historyTextView->setReadOnly(true);
        historyTextView->setFont(QFont("Courier", 8));
        historyTextView->setStyleSheet(
            "QTextEdit {"
            "  background-color: #F5F5F5;"
            "  color: #333333;"
            "  border: 1px solid #C0C0D0;"
            "  border-radius: 4px;"
            "  padding: 4px;"
            "}"
        );

        // Debug Messages section with copy button
        QWidget *debugHeaderWidget = new QWidget;
        QHBoxLayout *debugHeaderLayout = new QHBoxLayout(debugHeaderWidget);
        debugHeaderLayout->setContentsMargins(0, 0, 0, 0);

        QLabel *debugLabel = new QLabel("Debug Messages");
        debugLabel->setStyleSheet("color: #333333; font-weight: bold; font-size: 12px;");

        QPushButton *copyDebugButton = new QPushButton("Copy All");
        copyDebugButton->setFixedWidth(80);
        copyDebugButton->setToolTip("Copy all debug output to clipboard");
        copyDebugButton->setStyleSheet(
            "QPushButton {"
            "  background-color: #E8E8F0;"
            "  color: #333333;"
            "  border: 1px solid #C0C0D0;"
            "  border-radius: 4px;"
            "  padding: 4px;"
            "  font-size: 11px;"
            "}"
            "QPushButton:hover {"
            "  background-color: #D8D8E8;"
            "}"
        );

        debugHeaderLayout->addWidget(debugLabel);
        debugHeaderLayout->addStretch();
        debugHeaderLayout->addWidget(copyDebugButton);

        // Debug filter bar
        debugFilter = new QLineEdit;
        debugFilter->setPlaceholderText("Filter (shows only matching lines)...");
        debugFilter->setClearButtonEnabled(true);
        debugFilter->setStyleSheet(
            "QLineEdit {"
            "  background-color: white;"
            "  color: #333333;"
            "  border: 1px solid #C0C0D0;"
            "  border-radius: 4px;"
            "  padding: 4px;"
            "}"
        );

        // Debug search bar with navigation
        QWidget *debugSearchWidget = new QWidget;
        QHBoxLayout *debugSearchLayout = new QHBoxLayout(debugSearchWidget);
        debugSearchLayout->setContentsMargins(0, 0, 0, 0);
        debugSearchLayout->setSpacing(4);

        debugSearch = new QLineEdit;
        debugSearch->setPlaceholderText("Search (highlights matches)...");
        debugSearch->setClearButtonEnabled(true);
        debugSearch->setStyleSheet(
            "QLineEdit {"
            "  background-color: white;"
            "  color: #333333;"
            "  border: 1px solid #C0C0D0;"
            "  border-radius: 4px;"
            "  padding: 4px;"
            "}"
        );

        debugPrevButton = new QPushButton("◀");
        debugPrevButton->setFixedWidth(30);
        debugPrevButton->setToolTip("Previous match");
        debugPrevButton->setStyleSheet(
            "QPushButton {"
            "  background-color: #E8E8F0;"
            "  color: #333333;"
            "  border: 1px solid #C0C0D0;"
            "  border-radius: 4px;"
            "  padding: 4px;"
            "}"
            "QPushButton:hover {"
            "  background-color: #D8D8E8;"
            "}"
        );

        debugNextButton = new QPushButton("▶");
        debugNextButton->setFixedWidth(30);
        debugNextButton->setToolTip("Next match");
        debugNextButton->setStyleSheet(
            "QPushButton {"
            "  background-color: #E8E8F0;"
            "  color: #333333;"
            "  border: 1px solid #C0C0D0;"
            "  border-radius: 4px;"
            "  padding: 4px;"
            "}"
            "QPushButton:hover {"
            "  background-color: #D8D8E8;"
            "}"
        );

        debugMatchLabel = new QLabel("0/0");
        debugMatchLabel->setFixedWidth(50);
        debugMatchLabel->setAlignment(Qt::AlignCenter);
        debugMatchLabel->setStyleSheet("color: #666666; font-size: 11px;");

        debugSearchLayout->addWidget(debugSearch, 1);
        debugSearchLayout->addWidget(debugPrevButton);
        debugSearchLayout->addWidget(debugNextButton);
        debugSearchLayout->addWidget(debugMatchLabel);

        debugTextView = new QTextEdit;
        debugTextView->setReadOnly(true);
        debugTextView->setFont(QFont("Courier", 8));
        debugTextView->setStyleSheet(
            "QTextEdit {"
            "  background-color: #F5F5F5;"
            "  color: #333333;"
            "  border: 1px solid #C0C0D0;"
            "  border-radius: 4px;"
            "  padding: 4px;"
            "}"
        );

        layout->addWidget(historyLabel);
        layout->addWidget(historyFilter);
        layout->addWidget(historySearchWidget);
        layout->addWidget(historyTextView, 1);
        layout->addWidget(debugHeaderWidget);
        layout->addWidget(debugFilter);
        layout->addWidget(debugSearchWidget);
        layout->addWidget(debugTextView, 1);

        setCentralWidget(central);

        // Initialize search state
        historyCurrentMatch = -1;
        debugCurrentMatch = -1;

        // Connect filter signals
        connect(historyFilter, &QLineEdit::textChanged, this, &DebugWindow::onHistoryFilterChanged);
        connect(debugFilter, &QLineEdit::textChanged, this, &DebugWindow::onDebugFilterChanged);

        // Connect search signals
        connect(historySearch, &QLineEdit::textChanged, this, &DebugWindow::onHistorySearchChanged);
        connect(historyPrevButton, &QPushButton::clicked, this, &DebugWindow::onHistoryPrevMatch);
        connect(historyNextButton, &QPushButton::clicked, this, &DebugWindow::onHistoryNextMatch);

        connect(debugSearch, &QLineEdit::textChanged, this, &DebugWindow::onDebugSearchChanged);
        connect(debugPrevButton, &QPushButton::clicked, this, &DebugWindow::onDebugPrevMatch);
        connect(debugNextButton, &QPushButton::clicked, this, &DebugWindow::onDebugNextMatch);

        // Connect copy button
        connect(copyDebugButton, &QPushButton::clicked, this, [this]() {
            QClipboard *clipboard = QGuiApplication::clipboard();
            clipboard->setText(this->debugTextView->toPlainText());
        });
    }

    QTextEdit *getHistoryTextView() { return historyTextView; }
    QTextEdit *getDebugTextView() { return debugTextView; }

    void appendHistory(const QString &text) {
        historyLines.append(text);
        applyHistoryFilter();
    }

    void appendDebug(const QString &text) {
        debugLines.append(text);
        applyDebugFilter();
    }

private slots:
    void onHistoryFilterChanged(const QString &filter) {
        Q_UNUSED(filter);
        applyHistoryFilter();
        performHistorySearch();
    }

    void onDebugFilterChanged(const QString &filter) {
        Q_UNUSED(filter);
        applyDebugFilter();
        performDebugSearch();
    }

    void onHistorySearchChanged(const QString &text) {
        Q_UNUSED(text);
        performHistorySearch();
    }

    void onDebugSearchChanged(const QString &text) {
        Q_UNUSED(text);
        performDebugSearch();
    }

    void onHistoryPrevMatch() {
        if (historyCurrentMatch > 0) {
            historyCurrentMatch--;
            highlightHistoryMatch();
        }
    }

    void onHistoryNextMatch() {
        if (historyCurrentMatch < historyMatchPositions.size() - 1) {
            historyCurrentMatch++;
            highlightHistoryMatch();
        }
    }

    void onDebugPrevMatch() {
        if (debugCurrentMatch > 0) {
            debugCurrentMatch--;
            highlightDebugMatch();
        }
    }

    void onDebugNextMatch() {
        if (debugCurrentMatch < debugMatchPositions.size() - 1) {
            debugCurrentMatch++;
            highlightDebugMatch();
        }
    }

private:
    void applyHistoryFilter() {
        QString filterText = historyFilter->text().trimmed();
        if (filterText.isEmpty()) {
            historyTextView->setPlainText(historyLines.join("\n"));
        } else {
            QStringList filtered;
            for (const QString &line : historyLines) {
                if (line.contains(filterText, Qt::CaseInsensitive)) {
                    filtered.append(line);
                }
            }
            historyTextView->setPlainText(filtered.join("\n"));

            // Highlight filter matches
            if (!filterText.isEmpty()) {
                QTextDocument *doc = historyTextView->document();
                QTextCursor highlightCursor(doc);
                QTextCharFormat highlightFormat;
                highlightFormat.setBackground(QColor(200, 255, 200, 150)); // Light green for filter

                while (!highlightCursor.isNull() && !highlightCursor.atEnd()) {
                    highlightCursor = doc->find(filterText, highlightCursor);  // Case-insensitive by default
                    if (!highlightCursor.isNull()) {
                        highlightCursor.mergeCharFormat(highlightFormat);
                    }
                }
            }
        }

        // Scroll to bottom
        QTextCursor cursor = historyTextView->textCursor();
        cursor.movePosition(QTextCursor::End);
        historyTextView->setTextCursor(cursor);
        historyTextView->ensureCursorVisible();
    }

    void applyDebugFilter() {
        QString filterText = debugFilter->text().trimmed();
        if (filterText.isEmpty()) {
            debugTextView->setPlainText(debugLines.join("\n"));
        } else {
            QStringList filtered;
            for (const QString &line : debugLines) {
                if (line.contains(filterText, Qt::CaseInsensitive)) {
                    filtered.append(line);
                }
            }
            debugTextView->setPlainText(filtered.join("\n"));

            // Highlight filter matches
            if (!filterText.isEmpty()) {
                QTextDocument *doc = debugTextView->document();
                QTextCursor highlightCursor(doc);
                QTextCharFormat highlightFormat;
                highlightFormat.setBackground(QColor(200, 255, 200, 150)); // Light green for filter

                while (!highlightCursor.isNull() && !highlightCursor.atEnd()) {
                    highlightCursor = doc->find(filterText, highlightCursor);  // Case-insensitive by default
                    if (!highlightCursor.isNull()) {
                        highlightCursor.mergeCharFormat(highlightFormat);
                    }
                }
            }
        }

        // Scroll to bottom
        QTextCursor cursor = debugTextView->textCursor();
        cursor.movePosition(QTextCursor::End);
        debugTextView->setTextCursor(cursor);
        debugTextView->ensureCursorVisible();
    }

    void performHistorySearch() {
        QString searchText = historySearch->text().trimmed();
        historyMatchPositions.clear();
        historyCurrentMatch = -1;

        if (searchText.isEmpty()) {
            // Clear highlighting
            applyHistoryFilter();
            historyMatchLabel->setText("0/0");
            return;
        }

        // Find all matches in the current text
        QTextDocument *doc = historyTextView->document();
        QTextCursor highlightCursor(doc);
        QTextCursor cursor(doc);

        QTextCharFormat normalFormat;
        QTextCharFormat highlightFormat;
        highlightFormat.setBackground(QColor(255, 255, 0, 100)); // Yellow highlight

        // Clear existing highlighting
        cursor.select(QTextCursor::Document);
        cursor.setCharFormat(normalFormat);

        // Find and highlight all matches
        while (!highlightCursor.isNull() && !highlightCursor.atEnd()) {
            highlightCursor = doc->find(searchText, highlightCursor, QTextDocument::FindCaseSensitively);
            if (!highlightCursor.isNull()) {
                historyMatchPositions.append(highlightCursor.position() - searchText.length());
                highlightCursor.mergeCharFormat(highlightFormat);
            }
        }

        // Update match counter
        if (historyMatchPositions.isEmpty()) {
            historyMatchLabel->setText("0/0");
        } else {
            historyCurrentMatch = 0;
            historyMatchLabel->setText(QString("%1/%2").arg(1).arg(historyMatchPositions.size()));
            highlightHistoryMatch();
        }
    }

    void performDebugSearch() {
        QString searchText = debugSearch->text().trimmed();
        debugMatchPositions.clear();
        debugCurrentMatch = -1;

        if (searchText.isEmpty()) {
            // Clear highlighting
            applyDebugFilter();
            debugMatchLabel->setText("0/0");
            return;
        }

        // Find all matches in the current text
        QTextDocument *doc = debugTextView->document();
        QTextCursor highlightCursor(doc);
        QTextCursor cursor(doc);

        QTextCharFormat normalFormat;
        QTextCharFormat highlightFormat;
        highlightFormat.setBackground(QColor(255, 255, 0, 100)); // Yellow highlight

        // Clear existing highlighting
        cursor.select(QTextCursor::Document);
        cursor.setCharFormat(normalFormat);

        // Find and highlight all matches
        while (!highlightCursor.isNull() && !highlightCursor.atEnd()) {
            highlightCursor = doc->find(searchText, highlightCursor, QTextDocument::FindCaseSensitively);
            if (!highlightCursor.isNull()) {
                debugMatchPositions.append(highlightCursor.position() - searchText.length());
                highlightCursor.mergeCharFormat(highlightFormat);
            }
        }

        // Update match counter
        if (debugMatchPositions.isEmpty()) {
            debugMatchLabel->setText("0/0");
        } else {
            debugCurrentMatch = 0;
            debugMatchLabel->setText(QString("%1/%2").arg(1).arg(debugMatchPositions.size()));
            highlightDebugMatch();
        }
    }

    void highlightHistoryMatch() {
        if (historyCurrentMatch < 0 || historyCurrentMatch >= historyMatchPositions.size()) {
            return;
        }

        QString searchText = historySearch->text().trimmed();
        int pos = historyMatchPositions[historyCurrentMatch];

        // Highlight all matches first
        QTextDocument *doc = historyTextView->document();
        QTextCursor highlightCursor(doc);
        QTextCharFormat normalFormat;
        QTextCharFormat yellowFormat;
        yellowFormat.setBackground(QColor(255, 255, 0, 100)); // Yellow highlight

        // Clear existing highlighting
        QTextCursor clearCursor(doc);
        clearCursor.select(QTextCursor::Document);
        clearCursor.setCharFormat(normalFormat);

        // Highlight all matches in yellow
        while (!highlightCursor.isNull() && !highlightCursor.atEnd()) {
            highlightCursor = doc->find(searchText, highlightCursor, QTextDocument::FindCaseSensitively);
            if (!highlightCursor.isNull()) {
                highlightCursor.mergeCharFormat(yellowFormat);
            }
        }

        // Highlight current match in orange
        QTextCursor cursor(doc);
        cursor.setPosition(pos);
        cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, searchText.length());
        QTextCharFormat currentFormat;
        currentFormat.setBackground(QColor(255, 165, 0)); // Orange for current match
        cursor.mergeCharFormat(currentFormat);

        // Scroll to the match
        historyTextView->setTextCursor(cursor);
        historyTextView->ensureCursorVisible();

        // Update counter
        historyMatchLabel->setText(QString("%1/%2")
            .arg(historyCurrentMatch + 1)
            .arg(historyMatchPositions.size()));
    }

    void highlightDebugMatch() {
        if (debugCurrentMatch < 0 || debugCurrentMatch >= debugMatchPositions.size()) {
            return;
        }

        QString searchText = debugSearch->text().trimmed();
        int pos = debugMatchPositions[debugCurrentMatch];

        // Highlight all matches first
        QTextDocument *doc = debugTextView->document();
        QTextCursor highlightCursor(doc);
        QTextCharFormat normalFormat;
        QTextCharFormat yellowFormat;
        yellowFormat.setBackground(QColor(255, 255, 0, 100)); // Yellow highlight

        // Clear existing highlighting
        QTextCursor clearCursor(doc);
        clearCursor.select(QTextCursor::Document);
        clearCursor.setCharFormat(normalFormat);

        // Highlight all matches in yellow
        while (!highlightCursor.isNull() && !highlightCursor.atEnd()) {
            highlightCursor = doc->find(searchText, highlightCursor, QTextDocument::FindCaseSensitively);
            if (!highlightCursor.isNull()) {
                highlightCursor.mergeCharFormat(yellowFormat);
            }
        }

        // Highlight current match in orange
        QTextCursor cursor(doc);
        cursor.setPosition(pos);
        cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, searchText.length());
        QTextCharFormat currentFormat;
        currentFormat.setBackground(QColor(255, 165, 0)); // Orange for current match
        cursor.mergeCharFormat(currentFormat);

        // Scroll to the match
        debugTextView->setTextCursor(cursor);
        debugTextView->ensureCursorVisible();

        // Update counter
        debugMatchLabel->setText(QString("%1/%2")
            .arg(debugCurrentMatch + 1)
            .arg(debugMatchPositions.size()));
    }

    QTextEdit *historyTextView;
    QTextEdit *debugTextView;
    QLineEdit *historyFilter;
    QLineEdit *debugFilter;
    QLineEdit *historySearch;
    QLineEdit *debugSearch;
    QPushButton *historyPrevButton;
    QPushButton *historyNextButton;
    QPushButton *debugPrevButton;
    QPushButton *debugNextButton;
    QLabel *historyMatchLabel;
    QLabel *debugMatchLabel;
    QStringList historyLines;
    QStringList debugLines;
    QVector<int> historyMatchPositions;
    QVector<int> debugMatchPositions;
    int historyCurrentMatch;
    int debugCurrentMatch;
};

class MainWidget : public QMainWindow {
    Q_OBJECT
public:
    MainWidget(QWidget* parent = nullptr) : QMainWindow(parent) {
      printf("QtPie starting...\n");

      // Accept drops at top level to allow dragging anywhere
      setAcceptDrops(true);

      // Create debug window (initially hidden)
      debugWindow = new DebugWindow(this);

      boardPanelView = new BoardPanelView(this);

      // Create drag tile preview overlay at top level (initially hidden)
      dragTilePreview = new QLabel(this);
      dragTilePreview->setVisible(false);
      dragTilePreview->setAttribute(Qt::WA_TransparentForMouseEvents);  // Don't interfere with drag events
      dragTilePreview->raise();  // Always on top

      // Connect debug messages from board panel to debug window
      connect(boardPanelView, &BoardPanelView::debugMessage,
              debugWindow, &DebugWindow::appendDebug);

      // Create game history panel with player timers and move history
      historyPanel = new GameHistoryPanel(this);

      // Connect validation messages from board panel to board history
      connect(boardPanelView, &BoardPanelView::validationMessage,
              debugWindow, &DebugWindow::appendHistory);

      // Connect debug messages from history panel to debug window
      connect(historyPanel, &GameHistoryPanel::debugMessage,
              debugWindow, &DebugWindow::appendDebug);

      // Connect turn changes to timer control and player indicator
      connect(boardPanelView, &BoardPanelView::playerTurnChanged,
              this, [this](int playerIndex) {
          Game *game = this->boardPanelView->getGame();
          if (!game) return;

          // Start timer for new player
          this->historyPanel->startTimer(playerIndex);

          // Set visual indicator for player on turn
          this->historyPanel->setPlayerOnTurn(playerIndex);

          // Only create placeholder for human player (player 0)
          // Computer turn entries are created directly when the move is committed
          // Note: For future delayed computer moves, you can create a placeholder with:
          //   historyPanel->initializePlaceholderTurn(1, score, "", game, false, true);
          if (playerIndex == 0) {
              char *rack = magpie_get_player_rack_string(game, playerIndex);
              QString rackStr = rack ? QString::fromUtf8(rack) : QString();
              if (rack) free(rack);

              int score = magpie_get_player_score(game, playerIndex);
              // forceNew=true because this is a NEW turn, not updating an existing turn
              this->historyPanel->initializePlaceholderTurn(playerIndex, score, rackStr, game, true, true);
          }
      });

      // Connect move committed to history panel
      connect(boardPanelView, &BoardPanelView::moveCommitted,
              this, [this](int playerIndex, int prevScore, int playScore, int newScore,
                          QString notation, QString rack) {
          this->historyPanel->commitTurnAndCreateNext(playerIndex, prevScore, playScore, newScore,
                                                      notation, rack, this->boardPanelView->getGame());
          // Update the player's score in the header
          this->historyPanel->setPlayerScore(playerIndex, newScore);
      });

      historyPanel->setPlayerNames("olaugh", "magpie");

      // Check Consolas font loading after connecting signals
      debugWindow->appendDebug("=== Checking Consolas font ===");
      debugWindow->appendDebug(QString("Current working directory: %1").arg(QDir::currentPath()));

      // Try multiple possible paths
      QStringList fontPaths = {
          "fonts/Consolas.ttf",
          "../fonts/Consolas.ttf",
          "../../fonts/Consolas.ttf",
          "../../../fonts/Consolas.ttf",
          QCoreApplication::applicationDirPath() + "/../../../fonts/Consolas.ttf",
          QCoreApplication::applicationDirPath() + "/../../fonts/Consolas.ttf"
      };

      int fontId = -1;
      QString successPath;
      for (const QString &path : fontPaths) {
          debugWindow->appendDebug(QString("  Trying: %1").arg(path));
          fontId = QFontDatabase::addApplicationFont(path);
          if (fontId != -1) {
              successPath = path;
              break;
          }
      }

      if (fontId == -1) {
          debugWindow->appendDebug("ERROR: Failed to load Consolas font from any path");
      } else {
          QStringList fontFamilies = QFontDatabase::applicationFontFamilies(fontId);
          if (fontFamilies.isEmpty()) {
              debugWindow->appendDebug("WARNING: Consolas font loaded but no families found");
          } else {
              debugWindow->appendDebug(QString("SUCCESS: Loaded Consolas font from: %1 (ID: %2, families: %3)")
                              .arg(successPath)
                              .arg(fontId)
                              .arg(fontFamilies.join(", ")));
          }
      }

      // Connect board changes to print updated board to debug window
      connect(boardPanelView, &BoardPanelView::boardChanged,
              this, &MainWidget::printBoard);

      // Connect uncommitted move changes to update game history
      connect(boardPanelView, &BoardPanelView::uncommittedMoveChanged,
              this, &MainWidget::onUncommittedMoveChanged);

      // Connect drag preview signals from board panel
      connect(boardPanelView, &BoardPanelView::updateDragPreview,
              this, &MainWidget::onUpdateDragPreview);
      connect(boardPanelView, &BoardPanelView::hideDragPreview,
              this, &MainWidget::onHideDragPreview);

      // Create two-panel layout: board on left, history on right
      QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
      splitter->addWidget(boardPanelView);
      splitter->addWidget(historyPanel);
      splitter->setStretchFactor(0, 2);  // Board gets 2/3 of space
      splitter->setStretchFactor(1, 1);  // History gets 1/3 of space

      // Set up central widget
      setCentralWidget(splitter);

      // Create dimension overlay (initially hidden)
      dimensionOverlay = new QLabel(this);
      dimensionOverlay->setStyleSheet(
          "QLabel {"
          "  background-color: rgba(0, 0, 0, 180);"
          "  color: #00FF00;"
          "  font-family: 'Courier';"
          "  font-size: 14px;"
          "  font-weight: bold;"
          "  padding: 8px 12px;"
          "  border-radius: 4px;"
          "}"
      );
      dimensionOverlay->setVisible(false);
      dimensionOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
      dimensionOverlay->raise();

      // Create Debug menu
      QMenu *debugMenu = menuBar()->addMenu("Debug");

      QAction *showDebugWindowAction = new QAction("Show Debug Window", this);
      showDebugWindowAction->setCheckable(true);
      showDebugWindowAction->setChecked(false);
      connect(showDebugWindowAction, &QAction::toggled, this, &MainWidget::toggleDebugWindow);
      debugMenu->addAction(showDebugWindowAction);

      QAction *showDimensionsAction = new QAction("Show Window Dimensions", this);
      showDimensionsAction->setCheckable(true);
      showDimensionsAction->setChecked(false);
      connect(showDimensionsAction, &QAction::toggled, this, &MainWidget::toggleDimensionOverlay);
      debugMenu->addAction(showDimensionsAction);

      // Now create MAGPIE config after UI is set up
      debugWindow->appendHistory("=== Creating MAGPIE config ===");
      QString appPath = QCoreApplication::applicationDirPath();
      QString dataPath = appPath + "/../Resources/data";
      debugWindow->appendHistory("Data path: " + dataPath);

      config = magpie_create_config(dataPath.toUtf8().constData());
      if (!config) {
          debugWindow->appendHistory("ERROR: Failed to create MAGPIE config");
          return;
      }
      debugWindow->appendHistory("Config created successfully");

      // Set up game with CSW24, player names, and seed
      debugWindow->appendHistory("=== Setting up new game ===");
      magpie_config_load_command(config, "set -lex CSW24");
      magpie_config_load_command(config, "set -p1 olaugh");
      magpie_config_load_command(config, "set -p2 magpie");
      magpie_config_load_command(config, "set -seed 1337");
      debugWindow->appendHistory("Game settings: CSW24, players: olaugh vs magpie, seed: 1337");

      // Get the game from the config
      Game *game = magpie_get_game_from_config(config);
      if (game) {
          boardPanelView->setGame(game);
          debugWindow->appendHistory("Game created");

          // Draw starting racks for both players
          magpie_draw_starting_racks(game);
          debugWindow->appendHistory("Starting racks drawn");

          // Get and display olaugh's rack (player 0)
          char *rackString = magpie_get_player_rack_string(game, 0);
          if (rackString) {
              QString rackQStr = QString::fromUtf8(rackString);
              debugWindow->appendHistory("olaugh's rack: " + rackQStr);
              free(rackString);

              // Set the rack on the rack view
              boardPanelView->getRackView()->setRack(rackQStr);
          }

          // Update the CGP display to show the current game state
          boardPanelView->updateCgpDisplay();

          // Initialize placeholder turn entry for the player on turn only
          int playerOnTurn = magpie_get_player_on_turn_index(game);

          // Set visual indicator for player on turn
          historyPanel->setPlayerOnTurn(playerOnTurn);

          char *rack = magpie_get_player_rack_string(game, playerOnTurn);
          QString rackStr = rack ? QString::fromUtf8(rack) : QString();
          if (rack) free(rack);

          int score = magpie_get_player_score(game, playerOnTurn);
          historyPanel->initializePlaceholderTurn(playerOnTurn, score, rackStr, game);

          // Print the initial board
          printBoard();
      } else {
          debugWindow->appendHistory("WARNING: No game from config");
      }

      // Scroll history view to top after initial setup
      QTextCursor cursor = debugWindow->getHistoryTextView()->textCursor();
      cursor.movePosition(QTextCursor::Start);
      debugWindow->getHistoryTextView()->setTextCursor(cursor);
      debugWindow->getHistoryTextView()->ensureCursorVisible();

      // Install event filter on application to redirect all keyboard input to board
      qApp->installEventFilter(this);
    }

    void printBoard() {
        if (!config || !boardPanelView || !debugWindow) return;

        Game *game = boardPanelView->getGame();
        if (!game) return;

        char *gameString = magpie_game_to_string(config, game);
        if (gameString) {
            debugWindow->appendHistory("=== Board Layout ===");
            debugWindow->appendHistory(QString::fromUtf8(gameString));
            free(gameString);
        }
    }

    ~MainWidget() {
      if (config) {
          magpie_destroy_config(config);
      }
      delete m_dragTileRenderer;
      delete debugWindow;
    }

    // Event filter to redirect keyboard events to board
    bool eventFilter(QObject *obj, QEvent *event) override {
        // Track debug window focus changes
        if (obj == debugWindow->getDebugTextView()) {
            if (event->type() == QEvent::FocusIn) {
                boardPanelView->setDebugWindowFocused(true);
            } else if (event->type() == QEvent::FocusOut) {
                boardPanelView->setDebugWindowFocused(false);
            }
        }

        if (event->type() == QEvent::KeyPress) {
            // If event is already being sent to boardPanelView, don't redirect
            // to avoid infinite recursion
            if (obj == boardPanelView) {
                return QMainWindow::eventFilter(obj, event);
            }

            // Check if obj is a child widget of boardPanelView
            if (obj->isWidgetType()) {
                QWidget *widget = static_cast<QWidget*>(obj);
                if (widget->isAncestorOf(boardPanelView) || boardPanelView->isAncestorOf(widget)) {
                    // Event is within board panel widget tree, let it through
                    return QMainWindow::eventFilter(obj, event);
                }
            }

            // Event is going somewhere else, redirect to board panel
            QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
            QCoreApplication::sendEvent(boardPanelView, keyEvent);
            return true;  // Event handled
        }
        return QMainWindow::eventFilter(obj, event);
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QMainWindow::resizeEvent(event);
        updateDimensionOverlay();
    }
    void showEvent(QShowEvent* event) override {
        QMainWindow::showEvent(event);
        updateDimensionOverlay();
    }

    // Drag events to keep preview visible across entire window
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (event->mimeData()->hasText()) {
            event->accept();
        } else {
            QMainWindow::dragEnterEvent(event);
        }
    }

    void dragMoveEvent(QDragMoveEvent *event) override {
        if (event->mimeData()->hasText()) {
            event->accept();

            // Update preview position based on global cursor
            // Extract tile character from mime data
            QString mimeText = event->mimeData()->text();
            QStringList parts = mimeText.split(':');

            QChar tileChar;
            if (mimeText.startsWith("board:")) {
                // Board drag format: "board:row:col:char"
                if (parts.size() >= 4) {
                    tileChar = parts[3][0];
                }
            } else {
                // Rack drag format: "index:char"
                if (parts.size() >= 2) {
                    tileChar = parts[1][0];
                }
            }

            if (!tileChar.isNull() && boardPanelView) {
                // Get a reasonable tile size (use rack size as default)
                int tileSize = 60;  // Default size
                if (boardPanelView->getBoardView()) {
                    int boardSize = boardPanelView->getBoardView()->getSquareSize();
                    if (boardSize > 0) {
                        tileSize = boardSize;
                    }
                }

                // Render tile at current size
                QPixmap tilePixmap = renderDragTile(tileChar, tileSize);

                // Update preview - event position is already in MainWidget coordinates
                QPoint mainWidgetPos = event->position().toPoint();
                onUpdateDragPreview(tilePixmap, mainWidgetPos);
            }
        } else {
            QMainWindow::dragMoveEvent(event);
        }
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override {
        // Hide preview when leaving window entirely
        if (dragTilePreview && dragTilePreview->isVisible()) {
            dragTilePreview->setVisible(false);
        }
        QMainWindow::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent *event) override {
        // Accept all drops at top level to avoid macOS rejection animation
        // Child widgets already handled the actual drop logic
        if (event->mimeData()->hasText()) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else {
            event->ignore();
            QMainWindow::dropEvent(event);
        }
    }

private slots:
    void toggleDebugWindow(bool show) {
        if (show) {
            debugWindow->show();
        } else {
            debugWindow->hide();
        }
    }

    void toggleDimensionOverlay(bool show) {
        if (dimensionOverlay) {
            dimensionOverlay->setVisible(show);
            if (show) {
                updateDimensionOverlay();
            }
        }
    }

    void updateDimensionOverlay() {
        if (!dimensionOverlay || !dimensionOverlay->isVisible()) return;

        // Get window dimensions
        int windowWidth = width();
        int windowHeight = height();

        // Get board panel width if available
        int boardPanelWidth = 0;
        if (boardPanelView) {
            boardPanelWidth = boardPanelView->width();
        }

        // Update overlay text
        QString text = QString("Window: %1 × %2\nBoard Panel: %3")
            .arg(windowWidth)
            .arg(windowHeight)
            .arg(boardPanelWidth);

        dimensionOverlay->setText(text);
        dimensionOverlay->adjustSize();

        // Position in top-right corner with margin
        int x = width() - dimensionOverlay->width() - 10;
        int y = 10;
        dimensionOverlay->move(x, y);
        dimensionOverlay->raise();
    }

    void onUpdateDragPreview(const QPixmap &tilePixmap, const QPoint &mainWidgetPos) {
        if (!dragTilePreview) return;

        // mainWidgetPos is already in this widget's coordinates - no conversion needed!
        // Calculate where the tile will be drawn (top-left corner)
        int tileX = mainWidgetPos.x() - tilePixmap.width() / 2;
        int tileY = mainWidgetPos.y() - tilePixmap.height() / 2;

        // Update preview
        dragTilePreview->setPixmap(tilePixmap);
        dragTilePreview->resize(tilePixmap.size());
        dragTilePreview->move(tileX, tileY);
        dragTilePreview->setVisible(true);
        dragTilePreview->raise();
    }

    void onHideDragPreview() {
        if (dragTilePreview && dragTilePreview->isVisible()) {
            dragTilePreview->setVisible(false);
        }
    }

    void onUncommittedMoveChanged() {
        if (!boardPanelView || !historyPanel) return;

        Game *game = boardPanelView->getGame();
        if (!game) return;

        // Get current player on turn
        int playerIndex = magpie_get_player_on_turn_index(game);

        // Only update turn entries for the human player (player 0)
        // Computer moves are handled by the moveCommitted signal
        if (playerIndex != 0) {
            return;
        }

        // Update visual indicator for player on turn
        historyPanel->setPlayerOnTurn(playerIndex);

        // Get move notation
        QString notation = boardPanelView->getBoardView()->generateMoveNotation();

        // Get rack string for current player only (don't show opponent's rack)
        char *rackCStr = magpie_get_player_rack_string(game, playerIndex);
        QString rack = rackCStr ? QString::fromUtf8(rackCStr) : QString();
        if (rackCStr) free(rackCStr);

        if (notation.isEmpty()) {
            // No tiles placed - show placeholder with current state
            int currentScore = magpie_get_player_score(game, playerIndex);
            historyPanel->initializePlaceholderTurn(playerIndex, currentScore, rack, game);
            return;
        }

        // Check if move is validated
        char *errorMsg = magpie_validate_move(game, playerIndex, notation.toUtf8().constData());
        bool isValidated = (errorMsg == NULL);

        int prevScore = 0;
        int playScore = 0;

        if (isValidated) {
            // Get scores
            prevScore = magpie_get_player_score(game, playerIndex);
            playScore = magpie_get_move_score(game, playerIndex, notation.toUtf8().constData());
        }

        if (errorMsg) {
            free(errorMsg);
        }

        // Update game history panel with current turn
        historyPanel->updateCurrentTurn(playerIndex, notation, isValidated,
                                       prevScore, playScore, rack, game);
    }

    QPixmap renderDragTile(QChar tileChar, int size) {
        // Recreate renderer if size changed
        if (!m_dragTileRenderer || m_lastDragTileSize != size) {
            delete m_dragTileRenderer;
            m_dragTileRenderer = new TileRenderer(size, TileRenderer::TileStyle::Rack);
            m_lastDragTileSize = size;
        }

        // Render the tile
        if (tileChar.isLower() && tileChar >= 'a' && tileChar >= 'z') {
            // Designated blank - show the letter it represents
            return m_dragTileRenderer->getBlankTile(tileChar.toUpper().toLatin1());
        } else if (tileChar.isUpper() && tileChar >= 'A' && tileChar <= 'Z') {
            // Regular letter tile
            return m_dragTileRenderer->getLetterTile(tileChar.toLatin1());
        } else if (tileChar == '?') {
            // Undesignated blank - show as '?' with 0 subscript
            return m_dragTileRenderer->getUndesignatedBlank();
        }
        return QPixmap();
    }

private:
    Config *config;
    BoardPanelView *boardPanelView;
    GameHistoryPanel *historyPanel;
    DebugWindow *debugWindow;
    QLabel *dragTilePreview;  // Top-level drag preview overlay
    QLabel *dimensionOverlay;  // Dimension display overlay
    TileRenderer *m_dragTileRenderer = nullptr;  // Cached renderer for drag preview
    int m_lastDragTileSize = 0;  // Track size to recreate when needed
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    applyLightPalette(app);  // Use light theme

    MainWidget *mw = new MainWidget;
    mw->setWindowTitle("qtpie prototype");

    // Set minimum window size to ensure comfortable gameplay
    constexpr int MIN_WINDOW_HEIGHT = 640;
    constexpr int MIN_WINDOW_WIDTH = 522;  // Board panel min + some history space

    mw->setMinimumSize(MIN_WINDOW_WIDTH, MIN_WINDOW_HEIGHT);
    mw->resize(1280, 800);
    mw->show();

    return app.exec();
}

#include "main.moc"
