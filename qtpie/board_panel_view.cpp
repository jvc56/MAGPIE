#include "board_panel_view.h"
#include "board_view.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QPalette>

// Helper to create placeholder widgets.
static QWidget* createPlaceholder(const QString &text, const QColor &bgColor = Qt::lightGray) {
    QWidget *widget = new QWidget;
    widget->setAutoFillBackground(true);
    QPalette pal = widget->palette();
    pal.setColor(QPalette::Window, bgColor);
    widget->setPalette(pal);

    QLabel *label = new QLabel(text, widget);
    label->setAlignment(Qt::AlignCenter);

    QVBoxLayout *layout = new QVBoxLayout(widget);
    layout->addWidget(label);
    return widget;
}

BoardPanelView::BoardPanelView(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(5);

    // BoardView is the square canvas displaying board contents.
    BoardView *boardView = new BoardView(this);
    boardView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // Placeholders for the rack view and controls beneath the board.
    QWidget *rackPlaceholder = createPlaceholder("Rack");
    QWidget *controlsPlaceholder = createPlaceholder("Controls");

    rackPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    controlsPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    mainLayout->addWidget(boardView, 0);
    mainLayout->addWidget(rackPlaceholder, 1);
    mainLayout->addWidget(controlsPlaceholder, 1);
}
