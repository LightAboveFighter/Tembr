#include "ui/TrackOsd.h"

#include "core/Track.h"

#include <QCursor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QEnterEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScreen>
#include <QWindow>

namespace {

constexpr int kEdge = 20;          // distance from the screen edges
constexpr int kPadX = 18;
constexpr int kPadY = 12;
constexpr int kLineGap = 3;
constexpr int kRadius = 12;
constexpr int kFadeInMs = 260;
constexpr int kFadeOutMs = 600;
constexpr int kSwapOutMs = 110;    // old text out before the new one comes in
// Changes closer together than this are a held key (the desktop repeats it
// every ~30 ms after a ~500 ms delay): no fades then, see isFastSwitching().
constexpr int kFastMs = 600;
// Hide on hover: the margin around the backdrop where the pointer makes the
// OSD leave, how fast it leaves, and how long a new OSD stays invisible
// waiting for the compositor to tell whether the pointer rests there.
constexpr int kHoverMargin = 28;
constexpr int kHoverOutMs = 170;
constexpr int kProbeMs = 120;

} // namespace

TrackOsd::TrackOsd()
    : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                           | Qt::BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus)
    , m_backend(defaultBackend())
    , m_anim(new QPropertyAnimation(this, "fade", this))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setHideOnHover(!clickThroughWorks());
    setWindowTitle(QStringLiteral("Tembr OSD"));

    m_titleFont = font();
    m_titleFont.setPointSizeF(m_titleFont.pointSizeF() * 1.45);
    m_titleFont.setBold(true);
    m_detailsFont = font();
    m_detailsFont.setPointSizeF(m_detailsFont.pointSizeF() * 1.1);

    m_hold.setSingleShot(true);
    connect(&m_hold, &QTimer::timeout, this, [this] {
        if (m_backend == Backend::Window)
            fadeTo(0.0, kFadeOutMs);
    });
    connect(m_anim, &QPropertyAnimation::finished, this, [this] {
        if (m_fadeTo != 0.0)
            return;
        hide();
        if (m_hoverHidden)
            forgetPointer();
        if (m_swapping) {   // resized while unmapped: never a frame with text and size apart
            m_swapping = false;
            applyLayout();
            show();
            raise();
            fadeTo(1.0, kFadeInMs);
        }
    });
    m_probe.setSingleShot(true);
    m_probe.setInterval(kProbeMs);
    connect(&m_probe, &QTimer::timeout, this, [this] { fadeTo(1.0, kFadeInMs); });
}

void TrackOsd::setHideOnHover(bool on)
{
    m_hideOnHover = on;
    setAttribute(Qt::WA_TransparentForMouseEvents, !on);   // before the flag: it sets the flag too
    setWindowFlag(Qt::WindowTransparentForInput, !on);
    setMouseTracking(on);
}

bool TrackOsd::clickThroughWorks()
{
    if (QGuiApplication::platformName() != QLatin1String("xcb"))
        return true;   // offscreen (tests); on native Wayland no window is shown
    return qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
           && qEnvironmentVariable("XDG_SESSION_TYPE") != QLatin1String("wayland");
}

void TrackOsd::enterEvent(QEnterEvent *)
{
    outOfTheWay();
}

void TrackOsd::mouseMoveEvent(QMouseEvent *)
{
    outOfTheWay();
}

void TrackOsd::mousePressEvent(QMouseEvent *)
{
    if (m_hideOnHover) {
        m_hoverHidden = true;
        gone();
    }
}

void TrackOsd::outOfTheWay()
{
    if (!m_hideOnHover || !isVisible())
        return;
    const bool leaving = m_hoverHidden;
    m_hoverHidden = true;
    m_hold.stop();
    m_swapping = false;
    if (m_probe.isActive() || m_fade <= 0.0)
        gone();   // never seen: nothing to fade
    else if (!leaving)
        fadeTo(0.0, kHoverOutMs);   // quicker than the usual fade out
}

void TrackOsd::gone()
{
    m_anim->stop();
    m_probe.stop();
    m_hold.stop();
    m_swapping = false;
    m_fade = m_fadeTo = 0.0;
    const QRect was = geometry();
    hide();
    if (m_hoverHidden)
        forgetPointer(was);
}

// Over native Wayland windows the X server keeps the last pointer position it
// saw. Left on the OSD, the next OSD mapped there would get an enter event at
// once though the pointer is long gone, and hide again, every time, until the
// pointer happened to cross some X11 window. So once the OSD has gone out of
// the pointer's way, the X server's pointer is put in a far corner. That does
// not move the real pointer (a Wayland client cannot); if it really still
// rests there, the compositor tells the next OSD so with a real enter event.
void TrackOsd::forgetPointer(QRect area)
{
    if (QGuiApplication::platformName() != QLatin1String("xcb"))
        return;
    if (!area.isValid())
        area = geometry();
    QScreen *screen = QGuiApplication::screenAt(area.center());
    if (!screen || !area.contains(QCursor::pos()))
        return;
    const QRect g = screen->geometry();
    const QPoint far(area.center().x() < g.center().x() ? g.right() : g.left(),
                     area.center().y() < g.center().y() ? g.bottom() : g.top());
    QCursor::setPos(screen, far);
}

TrackOsd::Backend TrackOsd::defaultBackend()
{
    // Only X11 lets a client put a window anywhere (offscreen: tests).
    const QString p = QGuiApplication::platformName();
    return p == QLatin1String("xcb") || p == QLatin1String("offscreen") ? Backend::Window : Backend::Notification;
}

bool TrackOsd::wanted(When when, bool windowMinimized, bool windowVisible, bool windowActive)
{
    const bool unseen = windowMinimized || !windowVisible;
    switch (when) {
    case When::Never: return false;
    case When::Minimized: return unseen;
    case When::Inactive: return unseen || !windowActive;
    case When::Always: return true;
    }
    return false;
}

QRect TrackOsd::placement(const QRect &a, const QSize &size, Where where)
{
    const int left = a.x() + kEdge;
    const int center = a.x() + (a.width() - size.width()) / 2;
    const int right = a.x() + a.width() - kEdge - size.width();
    const int top = a.y() + kEdge;
    const int bottom = a.y() + a.height() - kEdge - size.height();
    switch (where) {
    case Where::TopLeft: return {{left, top}, size};
    case Where::TopCenter: return {{center, top}, size};
    case Where::TopRight: return {{right, top}, size};
    case Where::BottomLeft: return {{left, bottom}, size};
    case Where::BottomCenter: return {{center, bottom}, size};
    case Where::BottomRight: return {{right, bottom}, size};
    }
    return {{center, bottom}, size};
}

QStringList TrackOsd::lines(const Track &t, qint64 durationMs)
{
    QString title = t.title;
    if (title.isEmpty())
        title = t.isUrl() ? t.path : QFileInfo(t.path).completeBaseName();
    QStringList details;
    if (!t.artist.isEmpty())
        details << t.artist;
    if (!t.album.isEmpty())
        details << t.album;
    const qint64 length = t.durationMs > 0 ? t.durationMs : durationMs;
    if (length > 0)
        details << formatDuration(length);
    return {title, details.join(QStringLiteral(" · "))};
}

void TrackOsd::showTrack(const Track &t, qint64 durationMs)
{
    m_uid = t.uid;
    setText(lines(t, durationMs));
    m_hold.start(qMax(1, m_settings.seconds) * 1000);
    if (m_backend == Backend::Notification)
        notify();
    else
        showWindow();
}

void TrackOsd::refresh(const Track &t, qint64 durationMs)
{
    if (t.uid != m_uid || !isShowing())
        return;
    const QStringList text = lines(t, durationMs);
    if (text == QStringList{m_nextTitle, m_nextDetails})
        return;
    setText(text);
    if (m_backend == Backend::Notification)
        notify();   // replaces the shown one
    else
        showWindow(false);
}

void TrackOsd::setText(const QStringList &lines)
{
    m_nextTitle = lines.value(0);
    m_nextDetails = lines.value(1);
}

void TrackOsd::showWindow(bool trackChanged)
{
    if (m_hoverHidden) {
        // Out of the pointer's way. A key held down keeps it away (the
        // pointer has hardly moved); the next single change tries again.
        const bool burst = trackChanged && m_sinceChange.isValid() && m_sinceChange.elapsed() < kFastMs;
        if (trackChanged)
            m_sinceChange.restart();
        if (isVisible() || burst || !trackChanged) {
            m_hold.stop();
            return;
        }
        m_hoverHidden = false;
    }
    const bool rapid = isVisible()
                       && (trackChanged ? m_sinceChange.isValid() && m_sinceChange.elapsed() < kFastMs : m_fast);
    if (trackChanged)
        m_sinceChange.restart();
    if (!isVisible()) {
        applyLayout();
        m_fade = m_fadeTo = 0.0;
        show();
        raise();
        if (m_hideOnHover)
            m_probe.start();   // fades in unless the pointer turns out to be there
        else
            fadeTo(1.0, kFadeInMs);
    } else if (rapid) {
        m_anim->stop();
        m_probe.stop();
        m_swapping = false;
        if (!m_fast) {
            // Once, on entering: resized while unmapped, then shown at once.
            hide();
            applyFastLayout();
            m_fade = m_fadeTo = 1.0;
            show();
            raise();
        } else {
            applyFastLayout();   // same window size: only the drawing changes
            m_fade = m_fadeTo = 1.0;
            update();
        }
    } else if (!m_swapping) {
        // Shown (or fading out): let the old text go first; the new text and
        // size come in once it is invisible (see the animation's finished()).
        m_swapping = true;
        m_probe.stop();
        fadeTo(0.0, kSwapOutMs);
    }
}

QSize TrackOsd::takeText(QRect *area)
{
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());   // where the user is looking
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    *area = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
    if (screen && windowHandle() && !isVisible())
        windowHandle()->setScreen(screen);
    const int maxText = qBound(240, area->width() / 2, 900);

    const QFontMetrics tm(m_titleFont);
    const QFontMetrics dm(m_detailsFont);
    m_title = tm.elidedText(m_nextTitle, Qt::ElideRight, maxText);
    m_details = dm.elidedText(m_nextDetails, Qt::ElideRight, maxText);
    int w = tm.horizontalAdvance(m_title);
    int h = tm.height();
    if (!m_details.isEmpty()) {
        w = qMax(w, dm.horizontalAdvance(m_details));
        h += kLineGap + dm.height();
    }
    return {w + 2 * kPadX, h + 2 * kPadY};
}

void TrackOsd::applyLayout()
{
    QRect area;
    const QSize size = takeText(&area);
    m_fast = false;
    m_box = place(area, size);
}

QRect TrackOsd::place(const QRect &area, const QSize &canvas)
{
    const QRect at = placement(area, canvas, m_settings.where);
    const int m = m_hideOnHover ? kHoverMargin : 0;
    // The margin stays on the screen: off it, it would cover panels.
    const QRect window = at.adjusted(-m, -m, m, m).intersected(area.united(at));
    setGeometry(window);
    m_canvas = at.translated(-window.topLeft());
    return m_canvas;
}

void TrackOsd::applyFastLayout()
{
    QRect area;
    const QSize box = takeText(&area);
    const QFontMetrics tm(m_titleFont);
    const QFontMetrics dm(m_detailsFont);
    const QSize canvas(qBound(240, area.width() / 2, 900) + 2 * kPadX,
                       tm.height() + kLineGap + dm.height() + 2 * kPadY);
    if (!m_fast) {
        m_fast = true;
        place(area, canvas);
    }
    // The backdrop sits in the canvas where the window itself would be.
    const QSize c = m_canvas.size();
    int x = (c.width() - box.width()) / 2;
    if (m_settings.where == Where::TopLeft || m_settings.where == Where::BottomLeft)
        x = 0;
    else if (m_settings.where == Where::TopRight || m_settings.where == Where::BottomRight)
        x = c.width() - box.width();
    const bool top = m_settings.where == Where::TopLeft || m_settings.where == Where::TopCenter
                     || m_settings.where == Where::TopRight;
    m_box = QRect(m_canvas.topLeft() + QPoint(x, top ? 0 : c.height() - box.height()), box);
}

void TrackOsd::fadeTo(qreal target, int ms)
{
    m_anim->stop();
    m_fadeTo = target;
    // The same speed from wherever the fade is now.
    m_anim->setDuration(qMax(1, int(ms * qAbs(target - m_fade))));
    m_anim->setStartValue(m_fade);
    m_anim->setEndValue(target);
    m_anim->setEasingCurve(target > m_fade ? QEasingCurve::OutCubic : QEasingCurve::InOutQuad);
    m_anim->start();
}

void TrackOsd::setFade(qreal f)
{
    m_fade = f;
    update();
}

// Faded by painting with opacity into the translucent window rather than with
// the window opacity, which not every window manager applies.
void TrackOsd::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setOpacity(m_fade);

    const QRect box = m_box.isValid() ? m_box : rect();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(12, 12, 12, 175));
    p.drawRoundedRect(QRectF(box), kRadius, kRadius);

    // Both lines centred: the longer fills the width, the shorter sits in the middle.
    const QFontMetrics tm(m_titleFont);
    auto centred = [&](const QFontMetrics &fm, const QString &text) {
        return box.x() + (box.width() - fm.horizontalAdvance(text)) / 2;
    };
    int y = box.y() + kPadY + tm.ascent();
    p.setFont(m_titleFont);
    p.setPen(QColor(0xf5, 0xf5, 0xf5));
    p.drawText(centred(tm, m_title), y, m_title);
    if (!m_details.isEmpty()) {
        const QFontMetrics dm(m_detailsFont);
        y += tm.descent() + kLineGap + dm.ascent();
        p.setFont(m_detailsFont);
        p.setPen(QColor(0xc8, 0xc8, 0xc8));
        p.drawText(centred(dm, m_details), y, m_details);
    }
}

// org.freedesktop.Notifications: transient (not kept in the history), and
// replacing the previous one, so skipping through tracks shows one bubble.
void TrackOsd::notify()
{
    emit notified(m_nextTitle, m_nextDetails);
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                                      QStringLiteral("/org/freedesktop/Notifications"),
                                                      QStringLiteral("org.freedesktop.Notifications"),
                                                      QStringLiteral("Notify"));
    const QVariantMap hints{{QStringLiteral("transient"), true},
                            {QStringLiteral("urgency"), QVariant::fromValue<uchar>(0)},
                            {QStringLiteral("category"), QStringLiteral("x-tembr.track")},
                            {QStringLiteral("desktop-entry"), QStringLiteral("tembr")}};
    msg << QStringLiteral("Tembr") << m_notificationId << QStringLiteral("tembr") << m_nextTitle << m_nextDetails
        << QStringList() << hints << int(qMax(1, m_settings.seconds) * 1000);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        const QDBusPendingReply<uint> reply = *w;
        if (reply.isValid())
            m_notificationId = reply.value();
        w->deleteLater();
    });
}
