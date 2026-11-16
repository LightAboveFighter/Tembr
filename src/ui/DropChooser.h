#pragma once

#include <QWidget>

// Asked where tracks dropped into another playlist should go: a small round
// popup at the drop point. Up arrow (upper half) = at the start, the centre
// with a check mark = where the mouse pointed, down arrow (lower half) = at
// the end. Up / Enter / Down choose from the keyboard; Esc or a click outside
// the circle cancel. Deletes itself when closed.
class DropChooser : public QWidget
{
    Q_OBJECT
public:
    enum class Place { None, Start, Here, End };

    explicit DropChooser(QWidget *parent = nullptr);

    // Shown centred on `globalPos`, kept on the screen.
    void popup(const QPoint &globalPos);

    // The zone under `pos` (widget coordinates); None outside the circle.
    Place placeAt(const QPointF &pos) const;
    // The geometric centre of a zone (widget coordinates).
    QPoint centerOf(Place place) const;

signals:
    void chosen(DropChooser::Place place);   // not emitted when cancelled

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    void choose(Place place);

    Place m_hover = Place::None;
    Place m_pressed = Place::None;
};
