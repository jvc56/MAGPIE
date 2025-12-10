#include "hover_aware_text_browser.h"

HoverAwareTextBrowser::HoverAwareTextBrowser(QWidget *parent) : QTextBrowser(parent)
{
    // No specific implementation needed in constructor
}

void HoverAwareTextBrowser::enterEvent(QEnterEvent *event)
{
    emit mouseEntered();
    QTextBrowser::enterEvent(event);
}

void HoverAwareTextBrowser::leaveEvent(QEvent *event)
{
    emit mouseLeft();
    QTextBrowser::leaveEvent(event);
}
