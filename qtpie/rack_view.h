#ifndef RACK_VIEW_H
#define RACK_VIEW_H

#include <QWidget>
#include <QString>

class RackView : public QWidget {
    Q_OBJECT
public:
    explicit RackView(QWidget *parent = nullptr);

    void setRack(const QString& rack);
    QSize sizeHint() const override;

signals:
    void debugMessage(const QString &msg);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QString m_rack;
    int m_tileSize = 0;
};

#endif // RACK_VIEW_H
