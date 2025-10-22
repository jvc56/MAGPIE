#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QTextEdit>
#include <QTextCursor>
#include <QFont>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QEventLoop>
#include <QTime>

#include "magpie_wrapper.h"
#include "board_panel_view.h"
#include "responsive_layout.h"
#include "colors.h"
#include "tile_renderer.h"

class MainWidget : public QMainWindow {
    Q_OBJECT
public:
    MainWidget(QWidget* parent = nullptr) : QMainWindow(parent) {
      printf("QtPie starting...\n");

      // Accept drops at top level to allow dragging anywhere
      setAcceptDrops(true);

      contentWidget = new QWidget;

      boardPanelView = new BoardPanelView(this);

      // Create drag tile preview overlay at top level (initially hidden)
      dragTilePreview = new QLabel(this);
      dragTilePreview->setVisible(false);
      dragTilePreview->setAttribute(Qt::WA_TransparentForMouseEvents);  // Don't interfere with drag events
      dragTilePreview->raise();  // Always on top

      // Create stdout/History text view
      historyTextView = new QTextEdit;
      historyTextView->setReadOnly(true);
      historyTextView->setFont(QFont("Courier", 8));  // 15% smaller: 10 * 0.85 ≈ 8
      historyTextView->setStyleSheet(
          "QTextEdit {"
          "  background-color: #F5F5F5;"
          "  color: #333333;"
          "  border: 1px solid #C0C0D0;"
          "  border-radius: 4px;"
          "  padding: 4px;"
          "}"
      );

      // Create debug text view for Analysis position
      debugTextView = new QTextEdit;
      debugTextView->setReadOnly(true);
      debugTextView->setFont(QFont("Courier", 8));  // 15% smaller: 10 * 0.85 ≈ 8
      debugTextView->setStyleSheet(
          "QTextEdit {"
          "  background-color: #F5F5F5;"
          "  color: #333333;"
          "  border: 1px solid #C0C0D0;"
          "  border-radius: 4px;"
          "  padding: 4px;"
          "}"
      );

      // Connect debug messages from board panel to text view
      connect(boardPanelView, &BoardPanelView::debugMessage,
              debugTextView, &QTextEdit::append);

      // Connect board changes to print updated board
      connect(boardPanelView, &BoardPanelView::boardChanged,
              this, &MainWidget::printBoard);

      // Connect drag preview signals from board panel
      connect(boardPanelView, &BoardPanelView::updateDragPreview,
              this, &MainWidget::onUpdateDragPreview);
      connect(boardPanelView, &BoardPanelView::hideDragPreview,
              this, &MainWidget::onHideDragPreview);

      scrollArea = new QScrollArea(this);
      scrollArea->setWidget(contentWidget);
      scrollArea->setWidgetResizable(true);
      scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

      layout = new ResponsiveLayout(contentWidget, scrollArea, boardPanelView,
                                    historyTextView, debugTextView, 10, 50);
      boardPanelView->setParent(contentWidget);
      historyTextView->setParent(contentWidget);
      debugTextView->setParent(contentWidget);

      // Set up central widget
      setCentralWidget(scrollArea);

      // Create Debug menu
      QMenu *debugMenu = menuBar()->addMenu("Debug");
      QAction *toggleOverlayAction = new QAction("Show Layout Overlay", this);
      toggleOverlayAction->setCheckable(true);
      toggleOverlayAction->setChecked(false);  // Start with overlay disabled
      connect(toggleOverlayAction, &QAction::toggled, this, &MainWidget::toggleLayoutOverlay);
      debugMenu->addAction(toggleOverlayAction);

      // Actually hide the overlay on startup
      layout->setDebugOverlayVisible(false);

      // Now create MAGPIE config after UI is set up
      historyTextView->append("=== Creating MAGPIE config ===");
      QString appPath = QCoreApplication::applicationDirPath();
      QString dataPath = appPath + "/../Resources/data";
      historyTextView->append("Data path: " + dataPath);

      config = magpie_create_config(dataPath.toUtf8().constData());
      if (!config) {
          historyTextView->append("ERROR: Failed to create MAGPIE config");
          return;
      }
      historyTextView->append("Config created successfully");

      // Get the game from the config and set it on the board panel
      Game *game = magpie_get_game_from_config(config);
      if (game) {
          boardPanelView->setGame(game);
          historyTextView->append("Game loaded from config");

          // Print the initial board
          printBoard();
      } else {
          historyTextView->append("WARNING: No game from config");
      }

      // Scroll history view to top after initial setup
      QTextCursor cursor = historyTextView->textCursor();
      cursor.movePosition(QTextCursor::Start);
      historyTextView->setTextCursor(cursor);
      historyTextView->ensureCursorVisible();
    }

    void printBoard() {
        if (!config || !boardPanelView) return;

        Game *game = boardPanelView->getGame();
        if (!game) return;

        char *gameString = magpie_game_to_string(config, game);
        if (gameString) {
            historyTextView->append("=== Board Layout ===");
            historyTextView->append(QString::fromUtf8(gameString));
            free(gameString);
        }
    }

    ~MainWidget() {
      if (config) {
          magpie_destroy_config(config);
      }
      delete m_dragTileRenderer;
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        layout->updateLayout();
    }
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        layout->updateLayout();
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

                // Update preview at global cursor position
                QPoint localPos = event->position().toPoint();
                QPoint globalPos = mapToGlobal(localPos);
                onUpdateDragPreview(tilePixmap, globalPos);
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
    void toggleLayoutOverlay(bool show) {
        layout->setDebugOverlayVisible(show);
    }

    void onUpdateDragPreview(const QPixmap &tilePixmap, const QPoint &globalPos) {
        if (!dragTilePreview) return;

        // Convert global position to this widget's coordinates
        QPoint localPos = mapFromGlobal(globalPos);

        // Update preview
        dragTilePreview->setPixmap(tilePixmap);
        dragTilePreview->resize(tilePixmap.size());
        dragTilePreview->move(localPos.x() - tilePixmap.width() / 2,
                             localPos.y() - tilePixmap.height() / 2);
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
        if (tileChar.isLower() && tileChar >= 'a' && tileChar <= 'z') {
            return m_dragTileRenderer->getBlankTile(tileChar.toUpper().toLatin1());
        } else if (tileChar.isUpper() && tileChar >= 'A' && tileChar <= 'Z') {
            return m_dragTileRenderer->getLetterTile(tileChar.toLatin1());
        } else if (tileChar == '?') {
            return m_dragTileRenderer->getBlankTile('A');
        }
        return QPixmap();
    }

private:
    Config *config;
    QWidget *contentWidget;
    QScrollArea *scrollArea;
    BoardPanelView *boardPanelView;
    QTextEdit *historyTextView;
    QTextEdit *debugTextView;
    ResponsiveLayout *layout;
    QLabel *dragTilePreview;  // Top-level drag preview overlay
    TileRenderer *m_dragTileRenderer = nullptr;  // Cached renderer for drag preview
    int m_lastDragTileSize = 0;  // Track size to recreate when needed

    QWidget* createWidget(const QString &title) {
        QWidget *w = new QWidget;
        // Light theme - white background with subtle border
        w->setStyleSheet("background-color: #FFFFFF; border: 1px solid #C0C0D0; border-radius: 8px;");
        QVBoxLayout *l = new QVBoxLayout(w);
        l->setContentsMargins(10, 10, 10, 10);
        QLabel *label = new QLabel(title, w);
        // Dark text on light background
        label->setStyleSheet("color: #333333; font-weight: bold; font-size: 14px; border: none;");
        l->addWidget(label, 0, Qt::AlignTop | Qt::AlignLeft);
        return w;
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    applyLightPalette(app);  // Use light theme

    MainWidget *mw = new MainWidget;
    mw->setWindowTitle("qtpie prototype");
    mw->resize(1280, 800);
    mw->show();

    return app.exec();
}

#include "main.moc"
