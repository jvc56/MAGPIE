#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QFontDatabase>
#include <QDir>
#include <QCoreApplication>

// Test 1: setAutoFillBackground + class selector
class Test1Widget : public QWidget {
public:
    Test1Widget(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedHeight(80);
        setAutoFillBackground(true);
        setStyleSheet(
            "Test1Widget {"
            "  background-color: #FFF9E6;"
            "  border: 2px solid #FFE082;"
            "  border-radius: 8px;"
            "}"
        );

        QHBoxLayout *layout = new QHBoxLayout(this);
        QLabel *label = new QLabel("Test 1: setAutoFillBackground + class | 0O Il1 (Courier test)", this);
        QFont font("Courier", 14);
        label->setFont(font);
        label->setStyleSheet("color: black; background: transparent;");
        layout->addWidget(label);
    }
};

// Test 2: ID selector + WA_StyledBackground
class Test2Widget : public QWidget {
public:
    Test2Widget(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("test2");
        setFixedHeight(80);
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet(
            "#test2 {"
            "  background-color: #FFF9E6;"
            "  border: 2px solid #FFE082;"
            "  border-radius: 8px;"
            "}"
        );

        QHBoxLayout *layout = new QHBoxLayout(this);
        QLabel *label = new QLabel("Test 2: ID + WA_StyledBG | 0O Il1 (Courier test)", this);
        QFont font("Courier", 14);
        label->setFont(font);
        label->setStyleSheet("color: black; background: transparent;");
        layout->addWidget(label);
    }
};

// Test 3: Custom paintEvent
class Test3Widget : public QWidget {
public:
    Test3Widget(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedHeight(80);

        QHBoxLayout *layout = new QHBoxLayout(this);
        QLabel *label = new QLabel("Test 3: Custom paintEvent | 0O Il1 (Courier test)", this);
        QFont font("Courier", 14);
        label->setFont(font);
        label->setStyleSheet("color: black; background: transparent;");
        layout->addWidget(label);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QPainterPath path;
        path.addRoundedRect(rect(), 8, 8);

        painter.fillPath(path, QColor("#FFF9E6"));
        painter.setPen(QPen(QColor("#FFE082"), 2));
        painter.drawPath(path);
    }
};

// Test 4: setAutoFillBackground + QPalette
class Test4Widget : public QWidget {
public:
    Test4Widget(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedHeight(80);
        setAutoFillBackground(true);

        QPalette pal = palette();
        pal.setColor(QPalette::Window, QColor("#FFF9E6"));
        setPalette(pal);

        setStyleSheet(
            "Test4Widget {"
            "  border: 2px solid #FFE082;"
            "  border-radius: 8px;"
            "}"
        );

        QHBoxLayout *layout = new QHBoxLayout(this);
        QLabel *label = new QLabel("Test 4: setAutoFillBG + QPalette | 0O Il1 (Courier test)", this);
        QFont font("Courier", 14);
        label->setFont(font);
        label->setStyleSheet("color: black; background: transparent;");
        layout->addWidget(label);
    }
};

// Test 5: ID selector + WA_StyledBackground + separate label font rule
class Test5Widget : public QWidget {
public:
    Test5Widget(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("test5");
        setFixedHeight(80);
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet(
            "#test5 {"
            "  background-color: #FFF9E6;"
            "  border: 2px solid #FFE082;"
            "  border-radius: 8px;"
            "}"
            "#test5 QLabel {"
            "  font-family: 'Courier';"
            "  font-size: 14pt;"
            "}"
        );

        QHBoxLayout *layout = new QHBoxLayout(this);
        QLabel *label = new QLabel("Test 5: ID + WA_StyledBG + label font rule | 0O Il1", this);
        label->setStyleSheet("color: black; background: transparent;");
        layout->addWidget(label);
    }
};

// Test 6: QFrame subclass
#include <QFrame>
class Test6Widget : public QFrame {
public:
    Test6Widget(QWidget *parent = nullptr) : QFrame(parent) {
        setObjectName("test6");
        setFixedHeight(80);
        setFrameShape(QFrame::Box);
        setStyleSheet(
            "#test6 {"
            "  background-color: #FFF9E6;"
            "  border: 2px solid #FFE082;"
            "  border-radius: 8px;"
            "}"
        );

        QHBoxLayout *layout = new QHBoxLayout(this);
        QLabel *label = new QLabel("Test 6: QFrame with stylesheet | 0O Il1 (Courier test)", this);
        QFont font("Courier", 14);
        label->setFont(font);
        label->setStyleSheet("color: black; background: transparent;");
        layout->addWidget(label);
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // Load Consolas font
    QDir fontsDir(QCoreApplication::applicationDirPath() + "/../Resources/fonts");
    if (!fontsDir.exists()) {
        fontsDir.setPath(QCoreApplication::applicationDirPath() + "/fonts");
    }
    int fontId = QFontDatabase::addApplicationFont(fontsDir.filePath("Consolas.ttf"));
    if (fontId == -1) {
        qWarning("Failed to load Consolas font from %s", qPrintable(fontsDir.filePath("Consolas.ttf")));
    }

    QWidget window;
    window.setWindowTitle("Style Test - 6 Approaches");
    window.resize(800, 600);
    window.setStyleSheet("QWidget { background-color: #F5F5F5; }");  // Light gray like qtpie

    QVBoxLayout *mainLayout = new QVBoxLayout(&window);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    mainLayout->addWidget(new Test1Widget());
    mainLayout->addWidget(new Test2Widget());
    mainLayout->addWidget(new Test3Widget());
    mainLayout->addWidget(new Test4Widget());
    mainLayout->addWidget(new Test5Widget());
    mainLayout->addWidget(new Test6Widget());

    mainLayout->addStretch();

    window.show();
    return app.exec();
}
