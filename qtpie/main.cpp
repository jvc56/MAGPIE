#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QScrollArea>

extern "C" {
#include "../src/ent/move.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"

#include "../src/str/game_string.h"

#include "../test/tsrc/test_util.h"
}

#include "board_panel_view.h"
#include "responsive_layout.h"
#include "colors.h"

class MainWidget : public QWidget {
public:
    MainWidget(QWidget* parent = nullptr) : QWidget(parent) {
        Config *config = config_create_or_die(
            "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
        Game *game = config_game_create(config);
        draw_starting_racks(game);
      
        MoveList *move_list = move_list_create(10000);
        Move *move = get_top_equity_move(game, 0, move_list);
        StringBuilder *sb = string_builder_create();
        string_builder_add_game(sb, game, move_list);
        printf("%s\n", string_builder_peek(sb));
        string_builder_destroy(sb);

        contentWidget = new QWidget;

        boardPanelView = new BoardPanelView(this);
        boardPanelView->setGame(game);
        history = createWidget("History");
        analysis = createWidget("Analysis");

        scrollArea = new QScrollArea(this);
        scrollArea->setWidget(contentWidget);
        scrollArea->setWidgetResizable(true);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        layout = new ResponsiveLayout(contentWidget, scrollArea, boardPanelView,
                                      history, analysis, 10, 50);
        boardPanelView->setParent(contentWidget);
        history->setParent(contentWidget);
        analysis->setParent(contentWidget);

        QVBoxLayout* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->addWidget(scrollArea);
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

private:
    QWidget *contentWidget;
    QScrollArea *scrollArea;
    BoardPanelView *boardPanelView;
    QWidget *history;
    QWidget *analysis;
    ResponsiveLayout *layout;

    QWidget* createWidget(const QString &title) {
        QWidget *w = new QWidget;
        w->setStyleSheet("background-color: #242628; border-radius: 10px;");
        QVBoxLayout *l = new QVBoxLayout(w);
        l->setContentsMargins(5, 5, 5, 5);
        QLabel *label = new QLabel(title, w);
        label->setStyleSheet("color: white;");
        l->addWidget(label, 0, Qt::AlignTop | Qt::AlignLeft);
        return w;
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    applyDarkPalette(app);

    MainWidget *mw = new MainWidget;
    mw->setWindowTitle("qtpie prototype");
    mw->resize(1280, 800);
    mw->show();

    return app.exec();
}
