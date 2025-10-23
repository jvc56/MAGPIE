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

        // History text view (board layout logs)
        QLabel *historyLabel = new QLabel("Board History");
        historyLabel->setStyleSheet("color: #333333; font-weight: bold; font-size: 12px;");
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

        // Debug text view (analysis/debug messages)
        QLabel *debugLabel = new QLabel("Debug Messages");
        debugLabel->setStyleSheet("color: #333333; font-weight: bold; font-size: 12px;");
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
        layout->addWidget(historyTextView, 1);
        layout->addWidget(debugLabel);
        layout->addWidget(debugTextView, 1);

        setCentralWidget(central);
    }

    QTextEdit *getHistoryTextView() { return historyTextView; }
    QTextEdit *getDebugTextView() { return debugTextView; }

private:
    QTextEdit *historyTextView;
    QTextEdit *debugTextView;
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
              debugWindow->getDebugTextView(), &QTextEdit::append);

      // Create game history panel with player timers and move history
      historyPanel = new GameHistoryPanel(this);

      // Connect debug messages from history panel to debug window
      connect(historyPanel, &GameHistoryPanel::debugMessage,
              debugWindow->getDebugTextView(), &QTextEdit::append);

      historyPanel->setPlayerNames("olaugh", "magpie");

      // Check Consolas font loading after connecting signals
      debugWindow->getDebugTextView()->append("=== Checking Consolas font ===");
      debugWindow->getDebugTextView()->append(QString("Current working directory: %1").arg(QDir::currentPath()));

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
          debugWindow->getDebugTextView()->append(QString("  Trying: %1").arg(path));
          fontId = QFontDatabase::addApplicationFont(path);
          if (fontId != -1) {
              successPath = path;
              break;
          }
      }

      if (fontId == -1) {
          debugWindow->getDebugTextView()->append("ERROR: Failed to load Consolas font from any path");
      } else {
          QStringList fontFamilies = QFontDatabase::applicationFontFamilies(fontId);
          if (fontFamilies.isEmpty()) {
              debugWindow->getDebugTextView()->append("WARNING: Consolas font loaded but no families found");
          } else {
              debugWindow->getDebugTextView()->append(QString("SUCCESS: Loaded Consolas font from: %1 (ID: %2, families: %3)")
                              .arg(successPath)
                              .arg(fontId)
                              .arg(fontFamilies.join(", ")));
          }
      }

      // Connect board changes to print updated board to debug window
      connect(boardPanelView, &BoardPanelView::boardChanged,
              this, &MainWidget::printBoard);

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
      debugWindow->getHistoryTextView()->append("=== Creating MAGPIE config ===");
      QString appPath = QCoreApplication::applicationDirPath();
      QString dataPath = appPath + "/../Resources/data";
      debugWindow->getHistoryTextView()->append("Data path: " + dataPath);

      config = magpie_create_config(dataPath.toUtf8().constData());
      if (!config) {
          debugWindow->getHistoryTextView()->append("ERROR: Failed to create MAGPIE config");
          return;
      }
      debugWindow->getHistoryTextView()->append("Config created successfully");

      // Set up game with CSW24, player names, and seed
      debugWindow->getHistoryTextView()->append("=== Setting up new game ===");
      magpie_config_load_command(config, "set -lex CSW24");
      magpie_config_load_command(config, "set -p1 olaugh");
      magpie_config_load_command(config, "set -p2 magpie");
      magpie_config_load_command(config, "set -seed 1337");
      debugWindow->getHistoryTextView()->append("Game settings: CSW24, players: olaugh vs magpie, seed: 1337");

      // Get the game from the config
      Game *game = magpie_get_game_from_config(config);
      if (game) {
          boardPanelView->setGame(game);
          debugWindow->getHistoryTextView()->append("Game created");

          // Draw starting racks for both players
          magpie_draw_starting_racks(game);
          debugWindow->getHistoryTextView()->append("Starting racks drawn");

          // Get and display olaugh's rack (player 0)
          char *rackString = magpie_get_player_rack_string(game, 0);
          if (rackString) {
              QString rackQStr = QString::fromUtf8(rackString);
              debugWindow->getHistoryTextView()->append("olaugh's rack: " + rackQStr);
              free(rackString);

              // Set the rack on the rack view
              boardPanelView->getRackView()->setRack(rackQStr);
          }

          // Update the CGP display to show the current game state
          boardPanelView->updateCgpDisplay();

          // Print the initial board
          printBoard();
      } else {
          debugWindow->getHistoryTextView()->append("WARNING: No game from config");
      }

      // Scroll history view to top after initial setup
      QTextCursor cursor = debugWindow->getHistoryTextView()->textCursor();
      cursor.movePosition(QTextCursor::Start);
      debugWindow->getHistoryTextView()->setTextCursor(cursor);
      debugWindow->getHistoryTextView()->ensureCursorVisible();
    }

    void printBoard() {
        if (!config || !boardPanelView || !debugWindow) return;

        Game *game = boardPanelView->getGame();
        if (!game) return;

        char *gameString = magpie_game_to_string(config, game);
        if (gameString) {
            debugWindow->getHistoryTextView()->append("=== Board Layout ===");
            debugWindow->getHistoryTextView()->append(QString::fromUtf8(gameString));
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
