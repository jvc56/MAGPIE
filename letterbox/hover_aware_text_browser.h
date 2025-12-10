#ifndef HOVER_AWARE_TEXT_BROWSER_H
#define HOVER_AWARE_TEXT_BROWSER_H

#include <QTextBrowser>
#include <QEnterEvent>

class HoverAwareTextBrowser : public QTextBrowser {
    Q_OBJECT

public:
    explicit HoverAwareTextBrowser(QWidget *parent = nullptr);

signals:
    void mouseEntered();
    void mouseLeft();

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
};

#endif // HOVER_AWARE_TEXT_BROWSER_H
