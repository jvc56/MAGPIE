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
#include <QFont>

#include "magpie_wrapper.h"
#include "board_panel_view.h"
#include "responsive_layout.h"
#include "colors.h"

class MainWidget : public QMainWindow {
    Q_OBJECT
public:
    MainWidget(QWidget* parent = nullptr) : QMainWindow(parent) {
      printf("QtPie starting...\n");

      contentWidget = new QWidget;

      boardPanelView = new BoardPanelView(this);

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

private slots:
    void toggleLayoutOverlay(bool show) {
        layout->setDebugOverlayVisible(show);
    }

private:
    Config *config;
    QWidget *contentWidget;
    QScrollArea *scrollArea;
    BoardPanelView *boardPanelView;
    QTextEdit *historyTextView;
    QTextEdit *debugTextView;
    ResponsiveLayout *layout;

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
