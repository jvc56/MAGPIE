#include "board_panel_view.h"
#include "board_view.h"
#include "magpie_wrapper.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QFont>
#include <QFontMetrics>
#include <QDebug>

// Helper to create placeholder widgets with light theme.
static QWidget* createPlaceholder(const QString &text, const QColor &bgColor = QColor(255, 255, 255)) {
    QWidget *widget = new QWidget;
    // Light theme styling
    widget->setStyleSheet(
        QString("background-color: %1; border: 1px solid #C0C0D0; border-radius: 8px;")
        .arg(bgColor.name())
    );

    QLabel *label = new QLabel(text, widget);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("color: #333333; font-weight: bold; font-size: 14px; border: none;");

    QVBoxLayout *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->addWidget(label);
    return widget;
}

BoardPanelView::BoardPanelView(QWidget *parent)
    : QWidget(parent)
    , game(nullptr)
{
    // Enforce minimum size to prevent container from shrinking smaller than board
    // Board minimum: 20px * 15 + margins = 322px
    // Add space for CGP input, rack, controls
    constexpr int MIN_BOARD_SIZE = 322;
    constexpr int MIN_WIDTH = MIN_BOARD_SIZE;
    constexpr int MIN_HEIGHT = MIN_BOARD_SIZE + 200;  // board + other elements
    setMinimumSize(MIN_WIDTH, MIN_HEIGHT);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(5);

    // BoardView is the square canvas displaying board contents.
    boardView = new BoardView(this);
    boardView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // CGP input section (label + text field)
    QWidget *cgpWidget = new QWidget(this);
    QHBoxLayout *cgpLayout = new QHBoxLayout(cgpWidget);
    cgpLayout->setContentsMargins(5, 5, 5, 5);
    cgpLayout->setSpacing(10);

    QLabel *cgpLabel = new QLabel("CGP:", cgpWidget);
    QFont labelFont = cgpLabel->font();
    labelFont.setBold(true);
    cgpLabel->setFont(labelFont);
    cgpLabel->setStyleSheet("color: #333333;");

    cgpInput = new QTextEdit(cgpWidget);
    cgpInput->setAcceptRichText(false);  // Only accept plain text
    cgpInput->setPlainText("4AUREOLED3/11O3/11Z3/10FY3/10A4/10C4/10I4/7THANX3/10GUV2/15/15/15/15/15/15 AHMPRTU/ 177/44 0");
    cgpInput->setPlaceholderText("Enter CGP position (e.g., 15/15/15/... / 0/0 0)");

    QFont monoFont("Courier", 11);
    cgpInput->setFont(monoFont);

    // Set height for 3 lines of text
    QFontMetrics fm(monoFont);
    int lineHeight = fm.lineSpacing();
    int textEditHeight = lineHeight * 3 + 12;  // 3 lines + padding
    cgpInput->setFixedHeight(textEditHeight);

    // Light theme styling for input field
    cgpInput->setStyleSheet(
        "QTextEdit {"
        "  background-color: white;"
        "  color: #333333;"
        "  border: 1px solid #C0C0D0;"
        "  border-radius: 4px;"
        "  padding: 4px 8px;"
        "}"
        "QTextEdit:focus {"
        "  border: 1px solid #6496C8;"
        "}"
    );

    cgpLayout->addWidget(cgpLabel);
    cgpLayout->addWidget(cgpInput, 1);

    // Connect to update board on text change
    connect(cgpInput, &QTextEdit::textChanged, this, &BoardPanelView::onCgpTextChanged);

    // Rack view for displaying tiles
    rackView = new RackView(this);
    rackView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Connect rack debug messages to forward through this widget
    connect(rackView, &RackView::debugMessage, this, &BoardPanelView::debugMessage);

    // Placeholder for controls beneath the rack
    QWidget *controlsPlaceholder = createPlaceholder("Controls");
    controlsPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    mainLayout->addWidget(boardView, 0);
    mainLayout->addWidget(cgpWidget, 0);
    mainLayout->addWidget(rackView, 1);
    mainLayout->addWidget(controlsPlaceholder, 1);
}

void BoardPanelView::setGame(Game *game) {
    this->game = game;
    Board *board = magpie_get_board_from_game(game);
    boardView->setBoard(board);

    // Trigger initial CGP load
    onCgpTextChanged();
}

void BoardPanelView::onCgpTextChanged() {
    // Update the board view with the new CGP position
    QString text = cgpInput->toPlainText();
    boardView->setCgpPosition(text);

    emit debugMessage("=== CGP text changed ===");
    emit debugMessage("Text: " + text);

    // If we have a MAGPIE game, load the CGP into it
    if (game) {
        QByteArray cgpBytes = text.toUtf8();
        magpie_load_cgp(game, cgpBytes.constData());
        emit boardChanged();  // Signal that the board has been updated
    }

    // Parse and update rack from CGP
    // CGP format: "board rack/ / scores consecutive_zeros"
    // Example: "15/15/.../15 AEINRST/ / 0/0 0"
    // The board section contains 14 slashes (between 15 rows)
    // After board, there's a space, then the rack, then a slash

    // Split by space to separate board from rack/scores
    QStringList parts = text.split(' ', Qt::SkipEmptyParts);

    emit debugMessage(QString("Split into %1 parts").arg(parts.size()));

    if (parts.size() >= 2) {
        // parts[0] is the board (15/15/15...)
        // parts[1] is the rack with trailing slash (AEINRST/)
        QString rackPart = parts[1];
        emit debugMessage("Rack part: '" + rackPart + "'");

        // Remove trailing slash if present
        if (rackPart.endsWith('/')) {
            rackPart.chop(1);
        }

        emit debugMessage("Parsed rack: '" + rackPart + "'");
        rackView->setRack(rackPart);
    } else {
        emit debugMessage("Not enough parts - setting empty rack");
        rackView->setRack("");
    }
    emit debugMessage("");  // blank line
}

QSize BoardPanelView::minimumSizeHint() const {
    // Must match the minimum size set in constructor
    constexpr int MIN_BOARD_SIZE = 322;  // 20px * 15 + margins
    constexpr int MIN_WIDTH = MIN_BOARD_SIZE;
    constexpr int MIN_HEIGHT = MIN_BOARD_SIZE + 200;  // board + other elements
    return QSize(MIN_WIDTH, MIN_HEIGHT);
}