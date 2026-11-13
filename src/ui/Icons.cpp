#include "ui/Icons.h"

#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QtMath>

namespace Icons {

namespace {

constexpr int kBase = 64;   // drawing grid; pixmaps are rendered at several sizes

void drawGlyph(QPainter &p, Kind kind, const QColor &c)
{
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    QPainterPath path;
    switch (kind) {
    case Kind::Play:
        path.moveTo(22, 14);
        path.lineTo(50, 32);
        path.lineTo(22, 50);
        path.closeSubpath();
        p.drawPath(path);
        break;
    case Kind::Pause:
        p.drawRoundedRect(QRectF(18, 14, 10, 36), 2, 2);
        p.drawRoundedRect(QRectF(36, 14, 10, 36), 2, 2);
        break;
    case Kind::Stop:
        p.drawRoundedRect(QRectF(17, 17, 30, 30), 3, 3);
        break;
    case Kind::Next:
        path.moveTo(14, 16);
        path.lineTo(40, 32);
        path.lineTo(14, 48);
        path.closeSubpath();
        p.drawPath(path);
        p.drawRoundedRect(QRectF(42, 16, 8, 32), 2, 2);
        break;
    case Kind::Previous:
        path.moveTo(50, 16);
        path.lineTo(24, 32);
        path.lineTo(50, 48);
        path.closeSubpath();
        p.drawPath(path);
        p.drawRoundedRect(QRectF(14, 16, 8, 32), 2, 2);
        break;
    case Kind::Volume: {
        path.moveTo(10, 25);
        path.lineTo(20, 25);
        path.lineTo(32, 14);
        path.lineTo(32, 50);
        path.lineTo(20, 39);
        path.lineTo(10, 39);
        path.closeSubpath();
        p.drawPath(path);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c, 4, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(28, 22, 16, 20), -50 * 16, 100 * 16);
        p.drawArc(QRectF(26, 14, 28, 36), -50 * 16, 100 * 16);
        break;
    }
    case Kind::Note: {
        p.drawEllipse(QRectF(12, 40, 16, 12));
        p.drawEllipse(QRectF(36, 34, 16, 12));
        p.drawRect(QRectF(24, 14, 4, 32));
        p.drawRect(QRectF(48, 8, 4, 32));
        QPainterPath beam;
        beam.moveTo(24, 14);
        beam.lineTo(52, 8);
        beam.lineTo(52, 16);
        beam.lineTo(24, 22);
        beam.closeSubpath();
        p.drawPath(beam);
        break;
    }
    case Kind::Settings: {
        p.save();
        p.translate(32, 32);
        for (int i = 0; i < 8; ++i) {
            p.drawRoundedRect(QRectF(-4, -26, 8, 12), 2, 2);
            p.rotate(45);
        }
        p.restore();
        QPainterPath ring;
        ring.addEllipse(QPointF(32, 32), 17, 17);
        ring.addEllipse(QPointF(32, 32), 7, 7);
        p.drawPath(ring);
        break;
    }
    case Kind::Search:
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c, 6, Qt::SolidLine, Qt::RoundCap));
        p.drawEllipse(QRectF(12, 12, 28, 28));
        p.drawLine(QPointF(38, 38), QPointF(52, 52));
        break;
    }
}

QPixmap render(int size, const std::function<void(QPainter &)> &draw)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / qreal(kBase), size / qreal(kBase));
    draw(p);
    return pm;
}

QIcon build(const std::function<void(QPainter &, QIcon::Mode)> &draw)
{
    QIcon icon;
    for (int size : {16, 22, 24, 32, 48, 64, 128}) {
        for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active}) {
            icon.addPixmap(render(size, [&](QPainter &p) { draw(p, mode); }), mode);
        }
    }
    return icon;
}

} // namespace

QIcon make(Kind kind, const QColor &color)
{
    return build([=](QPainter &p, QIcon::Mode mode) {
        QColor c = color;
        if (mode == QIcon::Disabled)
            c.setAlphaF(0.35);
        else if (mode == QIcon::Active)
            c = Theme::accent();
        drawGlyph(p, kind, c);
    });
}

QIcon accentButton(Kind kind)
{
    return build([=](QPainter &p, QIcon::Mode mode) {
        QColor disc = mode == QIcon::Active ? Theme::accent().lighter(115) : Theme::accent();
        if (mode == QIcon::Disabled)
            disc.setAlphaF(0.4);
        p.setPen(Qt::NoPen);
        p.setBrush(disc);
        p.drawEllipse(QRectF(1, 1, 62, 62));
        // Optical centring: the play triangle sits a little to the right.
        p.translate(kind == Kind::Play ? 2 : 0, 0);
        p.translate(32, 32);
        p.scale(0.62, 0.62);
        p.translate(-32, -32);
        drawGlyph(p, kind, QColor(0x14, 0x14, 0x14));
    });
}

QPixmap appPixmap(int size)
{
    return render(size, [](QPainter &p) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x16, 0x16, 0x16));
        p.drawRoundedRect(QRectF(2, 2, 60, 60), 14, 14);
        p.setBrush(Theme::accent());
        p.drawEllipse(QRectF(10, 10, 44, 44));
        p.translate(33, 32);
        p.scale(0.55, 0.55);
        p.translate(-32, -32);
        drawGlyph(p, Kind::Play, QColor(0x14, 0x14, 0x14));
    });
}

QIcon app()
{
    QIcon icon;
    for (int size : {16, 22, 24, 32, 48, 64, 128, 256})
        icon.addPixmap(appPixmap(size));
    return icon;
}

} // namespace Icons
