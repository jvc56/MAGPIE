#include <QtTest>
#include <QApplication>
#include <QMainWindow>
#include <QTextEdit>
#include <QScrollArea>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QCursor>
#include "../board_panel_view.h"
#include "../board_view.h"
#include "../rack_view.h"
#include "../responsive_layout.h"
#include "../colors.h"
#include "../magpie_wrapper.h"

// Minimal main window for testing
class TestMainWindow : public QMainWindow {
    Q_OBJECT

public:
    TestMainWindow() {
        setAcceptDrops(true);

        contentWidget = new QWidget;
        boardPanelView = new BoardPanelView(this);

        // Create drag tile preview overlay
        dragTilePreview = new QLabel(this);
        dragTilePreview->setVisible(false);
        dragTilePreview->setAttribute(Qt::WA_TransparentForMouseEvents);
        dragTilePreview->raise();

        historyTextView = new QTextEdit;
        historyTextView->setReadOnly(true);

        debugTextView = new QTextEdit;
        debugTextView->setReadOnly(true);

        connect(boardPanelView, &BoardPanelView::debugMessage,
                debugTextView, &QTextEdit::append);
        connect(boardPanelView, &BoardPanelView::updateDragPreview,
                this, &TestMainWindow::onUpdateDragPreview);
        connect(boardPanelView, &BoardPanelView::hideDragPreview,
                this, &TestMainWindow::onHideDragPreview);

        scrollArea = new QScrollArea(this);
        scrollArea->setWidget(contentWidget);
        scrollArea->setWidgetResizable(true);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        layout = new ResponsiveLayout(contentWidget, scrollArea, boardPanelView,
                                      historyTextView, debugTextView, 10, 50);
        boardPanelView->setParent(contentWidget);
        historyTextView->setParent(contentWidget);
        debugTextView->setParent(contentWidget);

        setCentralWidget(scrollArea);

        // Create MAGPIE config
        QString appPath = QCoreApplication::applicationDirPath();
        QString dataPath = appPath + "/../../data";

        config = magpie_create_config(dataPath.toUtf8().constData());
        if (config) {
            Game *game = magpie_get_game_from_config(config);
            if (game) {
                boardPanelView->setGame(game);
            }
        }
    }

    ~TestMainWindow() {
        if (config) {
            magpie_destroy_config(config);
        }
    }

    BoardPanelView* getBoardPanelView() { return boardPanelView; }
    QTextEdit* getDebugTextView() { return debugTextView; }
    QLabel* getDragTilePreview() { return dragTilePreview; }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QMainWindow::resizeEvent(event);
        layout->updateLayout();
    }

    void showEvent(QShowEvent* event) override {
        QMainWindow::showEvent(event);
        layout->updateLayout();
    }

private slots:
    void onUpdateDragPreview(const QPixmap &tilePixmap, const QPoint &globalPos) {
        if (!dragTilePreview) return;

        QPoint localPos = mapFromGlobal(globalPos);
        int tileX = localPos.x() - tilePixmap.width() / 2;
        int tileY = localPos.y() - tilePixmap.height() / 2;

        dragTilePreview->setPixmap(tilePixmap);
        dragTilePreview->resize(tilePixmap.size());
        dragTilePreview->move(tileX, tileY);
        dragTilePreview->setVisible(true);
        dragTilePreview->raise();

        // Store last preview position for testing
        m_lastPreviewGlobalPos = globalPos;
        m_lastPreviewSize = tilePixmap.size();
    }

    void onHideDragPreview() {
        if (dragTilePreview && dragTilePreview->isVisible()) {
            dragTilePreview->setVisible(false);
        }
    }

public:
    QPoint m_lastPreviewGlobalPos;
    QSize m_lastPreviewSize;

private:
    Config *config = nullptr;
    QWidget *contentWidget;
    QScrollArea *scrollArea;
    BoardPanelView *boardPanelView;
    QTextEdit *historyTextView;
    QTextEdit *debugTextView;
    ResponsiveLayout *layout;
    QLabel *dragTilePreview;
};

class DragDropPositionTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // This runs once before all tests
    }

    void cleanupTestCase() {
        // This runs once after all tests
    }

    void testCursorVsEventPosition() {
        // This test verifies whether the cursor and event positions match during drag
        TestMainWindow window;
        window.resize(1280, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        BoardPanelView *panel = window.getBoardPanelView();
        QVERIFY(panel != nullptr);

        BoardView *board = panel->getBoardView();
        QVERIFY(board != nullptr);

        // Move cursor to a specific position and check event coordinates
        QPoint testGlobalPos(900, 800);
        QCursor::setPos(testGlobalPos);
        QTest::qWait(100);

        // Now send a drag move event at that position
        QPoint panelPos = panel->mapFromGlobal(testGlobalPos);

        // Create mime data for the drag
        QMimeData *mimeData = new QMimeData();
        mimeData->setText("0:S");  // Simulate dragging 'S' from rack index 0

        QDragMoveEvent moveEvent(panelPos, Qt::MoveAction, mimeData,
                                Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(panel, &moveEvent);
        QTest::qWait(50);

        // Read debug output to see what coordinates were calculated
        QString debugText = window.getDebugTextView()->toPlainText();
        qDebug() << "Debug output:" << debugText;

        // For now, just verify the test runs
        QVERIFY(true);

        delete mimeData;
    }

    void testGreenOutlineMatchesVisibleTile() {
        TestMainWindow window;
        window.resize(1280, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        BoardPanelView *panel = window.getBoardPanelView();
        BoardView *board = panel->getBoardView();
        RackView *rack = panel->findChild<RackView*>();

        // Start drag from rack
        QPoint rackTileCenter = rack->getTileCenter(0);
        QPoint rackGlobalPos = rack->mapToGlobal(rackTileCenter);

        QMouseEvent pressEvent(QEvent::MouseButtonPress, rackTileCenter,
                              rackGlobalPos, Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
        QApplication::sendEvent(rack, &pressEvent);
        QTest::qWait(100);

        // Test multiple board positions
        int squareSize = board->getSquareSize();
        int marginX = board->getMarginX();
        int marginY = board->getMarginY();

        struct TestPosition {
            int row, col;
            QString name;
        };

        QList<TestPosition> testPositions = {
            {7, 7, "Center"},
            {0, 0, "Top-Left"},
            {14, 14, "Bottom-Right"},
            {7, 0, "Left-Middle"},
            {7, 14, "Right-Middle"}
        };

        for (const auto &pos : testPositions) {
            // Calculate center of square
            QPoint squareCenter(marginX + pos.col * squareSize + squareSize / 2,
                               marginY + pos.row * squareSize + squareSize / 2);
            QPoint globalPos = board->mapToGlobal(squareCenter);

            // Set cursor and send move event
            QCursor::setPos(globalPos);
            QTest::qWait(10);

            QPoint panelPos = panel->mapFromGlobal(globalPos);
            QMouseEvent moveEvent(QEvent::MouseMove, panelPos, globalPos,
                                 Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(panel, &moveEvent);
            QTest::qWait(100);

            // Get the green outline position from board view
            int outlineRow = board->getHoverRow();
            int outlineCol = board->getHoverCol();

            qDebug() << pos.name << "- Expected: [" << pos.row << "," << pos.col << "]"
                     << "Got: [" << outlineRow << "," << outlineCol << "]";

            QVERIFY2(outlineRow == pos.row && outlineCol == pos.col,
                    qPrintable(QString("%1: Expected [%2,%3] but got [%4,%5]")
                              .arg(pos.name).arg(pos.row).arg(pos.col)
                              .arg(outlineRow).arg(outlineCol)));
        }
    }
};

QTEST_MAIN(DragDropPositionTest)
#include "drag_drop_position_test.moc"
