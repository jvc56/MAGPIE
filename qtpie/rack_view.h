#ifndef RACK_VIEW_H
#define RACK_VIEW_H

#include <QWidget>
#include <QString>
#include <QPushButton>
#include <QHBoxLayout>
#include <QPoint>

class RackView : public QWidget {
    Q_OBJECT
public:
    explicit RackView(QWidget *parent = nullptr);

    void setRack(const QString& rack);
    QSize sizeHint() const override;

signals:
    void debugMessage(const QString &msg);
    void rackChanged(const QString& newRack);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void alphabetizeRack();
    void shuffleRack();

private:
    int getTileIndexAtPosition(const QPoint &pos) const;
    int getStartX() const;

    QString m_rack;
    int m_tileSize = 0;
    QPushButton *m_alphabetizeButton;
    QPushButton *m_shuffleButton;

    // Drag state
    int m_draggedTileIndex = -1;
    QPoint m_dragStartPos;
    int m_dropIndicatorPosition = -1;  // -1 means no indicator, otherwise index where tile would be inserted
};

#endif // RACK_VIEW_H
