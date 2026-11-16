#include "ui/DropChooser.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#include <cmath>

namespace {

// Sizes from the font, so it follows the desktop's scaling.
struct Geometry
{
    QPointF center;
    qreal outer;   // radius of the circle
    qreal inner;   // radius of the centre ("here")
};

Geometry geometryOf(const QWidget *w)
{
    const qreal side = qMin(w->width(), w->height());
    return {QPointF(w->width() / 2.0, w->height() / 2.0), side / 2.0 - 1.5, side * 0.19};
}

} // namespace

DropChooser::DropChooser(QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);   // round, not a square with corners
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    const int side = fontMetrics().height() * 5;
    setFixedSize(side, side);
}

void DropChooser::popup(const QPoint &globalPos)
{
    QRect r(QPoint(0, 0), size());
    r.moveCenter(globalPos);
    QScreen *screen = QGuiApplication::screenAt(globalPos);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        r.moveLeft(qBound(avail.left(), r.left(), avail.right() - r.width() + 1));
        r.moveTop(qBound(avail.top(), r.top(), avail.bottom() - r.height() + 1));
    }
    move(r.topLeft());
    show();
    setFocus(Qt::PopupFocusReason);
}

DropChooser::Place DropChooser::placeAt(const QPointF &pos) const
{
    const Geometry g = geometryOf(this);
    const QPointF d = pos - g.center;
    const qreal dist = std::hypot(d.x(), d.y());
    if (dist > g.outer)
        return Place::None;
    if (dist <= g.inner)
        return Place::Here;
    return d.y() < 0 ? Place::Start : Place::End;
}

QPoint DropChooser::centerOf(Place place) const
{
    const Geometry g = geometryOf(this);
    const qreal ring = (g.outer + g.inner) / 2.0;   // middle of the half ring
    switch (place) {
    case Place::Start: return (g.center - QPointF(0, ring)).toPoint();
    case Place::End: return (g.center + QPointF(0, ring)).toPoint();
    case Place::Here: return g.center.toPoint();
    case Place::None: break;
    }
    return {};
}

void DropChooser::paintEvent(QPaintEvent *)
{
    const Geometry g = geometryOf(this);
    const QPalette pal = palette();
    QColor line = pal.color(QPalette::Text);
    line.setAlphaF(0.25);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QPainterPath outer;
    outer.addEllipse(g.center, g.outer, g.outer);
    QPainterPath inner;
    inner.addEllipse(g.center, g.inner, g.inner);
    QPainterPath upper;
    upper.addRect(QRectF(0, 0, width(), g.center.y()));
    QPainterPath lower;
    lower.addRect(QRectF(0, g.center.y(), width(), height() - g.center.y()));
    const QPainterPath startZone = outer.intersected(upper).subtracted(inner);
    const QPainterPath endZone = outer.intersected(lower).subtracted(inner);

    p.setPen(QPen(line, 1));
    p.setBrush(pal.color(QPalette::Base));
    p.drawPath(outer);

    // The zone under the mouse in the accent colour.
    const QColor accent = pal.color(QPalette::Highlight);
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    if (m_hover == Place::Start)
        p.drawPath(startZone);
    else if (m_hover == Place::End)
        p.drawPath(endZone);

    // The divide between the halves, and the centre.
    p.setPen(QPen(line, 1));
    p.drawLine(QPointF(g.center.x() - g.outer, g.center.y()), QPointF(g.center.x() - g.inner, g.center.y()));
    p.drawLine(QPointF(g.center.x() + g.inner, g.center.y()), QPointF(g.center.x() + g.outer, g.center.y()));
    p.setBrush(m_hover == Place::Here ? accent : pal.color(QPalette::Base));
    p.drawPath(inner);

    // Glyphs: chevrons up and down, a check mark in the centre.
    const qreal a = g.inner * 0.55;   // half width of a glyph
    auto glyphPen = [&](Place place, const QColor &normal) {
        QPen pen(m_hover == place ? pal.color(QPalette::HighlightedText) : normal, qMax(2.0, g.inner / 6.0));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        return pen;
    };
    p.setBrush(Qt::NoBrush);
    const QPointF up = centerOf(Place::Start);
    p.setPen(glyphPen(Place::Start, pal.color(QPalette::Text)));
    p.drawPolyline(QPolygonF({up + QPointF(-a, a / 2), up + QPointF(0, -a / 2), up + QPointF(a, a / 2)}));
    const QPointF down = centerOf(Place::End);
    p.setPen(glyphPen(Place::End, pal.color(QPalette::Text)));
    p.drawPolyline(QPolygonF({down + QPointF(-a, -a / 2), down + QPointF(0, a / 2), down + QPointF(a, -a / 2)}));
    const QPointF c = g.center;
    p.setPen(glyphPen(Place::Here, accent));
    p.drawPolyline(QPolygonF({c + QPointF(-a * 0.8, 0), c + QPointF(-a * 0.2, a * 0.6), c + QPointF(a * 0.9, -a * 0.6)}));
}

void DropChooser::mousePressEvent(QMouseEvent *e)
{
    m_pressed = placeAt(e->position());
    if (m_pressed == Place::None)
        close();   // outside the circle (outside the window Qt closes it itself)
}

void DropChooser::mouseMoveEvent(QMouseEvent *e)
{
    const Place place = placeAt(e->position());
    if (place == m_hover)
        return;
    m_hover = place;
    switch (place) {
    case Place::Start: setToolTip(QStringLiteral("В начало плейлиста  (↑)")); break;
    case Place::Here: setToolTip(QStringLiteral("Сюда, куда указали  (Enter)")); break;
    case Place::End: setToolTip(QStringLiteral("В конец плейлиста  (↓)")); break;
    case Place::None: setToolTip({}); break;
    }
    update();
}

void DropChooser::mouseReleaseEvent(QMouseEvent *e)
{
    const Place place = placeAt(e->position());
    if (place != Place::None && place == m_pressed)
        choose(place);
    m_pressed = Place::None;
}

void DropChooser::leaveEvent(QEvent *)
{
    m_hover = Place::None;
    update();
}

void DropChooser::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Up: choose(Place::Start); break;
    case Qt::Key_Down: choose(Place::End); break;
    case Qt::Key_Return:
    case Qt::Key_Enter: choose(Place::Here); break;
    case Qt::Key_Escape: close(); break;
    default: QWidget::keyPressEvent(e);
    }
}

void DropChooser::choose(Place place)
{
    // Closed first: what is chosen may take a moment (thousands of tracks),
    // and the popup should not stay up meanwhile.
    close();
    emit chosen(place);
}
