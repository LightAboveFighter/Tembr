#pragma once

#include <QElapsedTimer>
#include <QRect>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class QPropertyAnimation;
struct Track;

// On-screen display: the title of the new track for a few seconds when the
// track changes, e.g. after "next" from a global hotkey while Tembr is
// minimized (as AIMP's OSD). A translucent backdrop with text that fades in
// and out, click-through, never takes focus, above other windows.
//
// It needs a window the app may place itself: X11, and XWayland (where Qt
// runs on GNOME). On a native Wayland session (KDE, Sway, ... with Qt's
// Wayland plugin) clients cannot place windows, so a desktop notification
// shows the same text instead.
class TrackOsd : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal fade READ fade WRITE setFade)
public:
    enum class When { Never, Minimized, Inactive, Always };
    // Stored as numbers: keep the order, add new ones at the end.
    enum class Where { BottomCenter, TopCenter, TopRight, BottomRight, TopLeft, BottomLeft };
    enum class Backend { Window, Notification };

    struct Settings
    {
        When when = When::Minimized;
        Where where = Where::BottomCenter;
        int seconds = 3;

        bool operator==(const Settings &o) const { return when == o.when && where == o.where && seconds == o.seconds; }
    };

    TrackOsd();

    void setSettings(const Settings &s) { m_settings = s; }
    Settings settings() const { return m_settings; }
    static Backend defaultBackend();   // from the Qt platform in use
    void setBackend(Backend b) { m_backend = b; }
    Backend backend() const { return m_backend; }

    // Whether a track change should be shown, given the state of the main window.
    static bool wanted(When when, bool windowMinimized, bool windowVisible, bool windowActive);
    // Where the OSD of `size` goes on a screen with `available` geometry.
    static QRect placement(const QRect &available, const QSize &size, Where where);
    // The two lines: the title (file name without tags), then
    // "artist · album · length". `durationMs` is used when the tags have none yet.
    static QStringList lines(const Track &t, qint64 durationMs = 0);

    // Shows (or updates, if already shown) the track and restarts the timer.
    void showTrack(const Track &t, qint64 durationMs = 0);
    // New tags of the track on screen (read after it started): new text, same timer.
    void refresh(const Track &t, qint64 durationMs = 0);
    // Fast switching (a next/previous key held down): the window keeps one
    // size and only what is drawn in it changes, text and backdrop together.
    bool isFastSwitching() const { return m_fast; }
    QRect backdrop() const { return m_box; }   // where the backdrop is drawn (tests)
    bool isShowing() const
    {
        return m_hold.isActive() || m_swapping || m_probe.isActive() || (isVisible() && m_fadeTo > 0);
    }

    qreal fade() const { return m_fade; }
    void setFade(qreal f);

    // Whether clicks really pass through the window. Not on GNOME Wayland:
    // the compositor ignores the input shape of XWayland windows and the X
    // server hands the click to whatever X11 window lies below in its own
    // stacking, not the one seen under the OSD. There the OSD gets out of
    // the way as soon as the pointer comes near it (see setHideOnHover()).
    static bool clickThroughWorks();
    // The window takes the pointer and has a transparent margin around the
    // backdrop: the pointer coming into it fades the OSD out quickly. It is
    // told by the window's own enter events, not by the pointer position:
    // over native Wayland windows the X server keeps the last position it
    // saw, which may well be on the OSD. A new OSD first comes up invisible
    // for a moment, so one under a resting pointer never shows at all.
    void setHideOnHover(bool on);
    bool hidesOnHover() const { return m_hideOnHover; }

signals:
    void notified(const QString &summary, const QString &body);   // Notification backend (tests)

protected:
    void paintEvent(QPaintEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;

private:
    void setText(const QStringList &lines);
    void showWindow(bool trackChanged = true);   // false: new tags of the same track
    void applyLayout();       // size and place for the current text (while hidden)
    void applyFastLayout();   // fixed-size canvas, backdrop drawn to fit the text
    QSize takeText(QRect *area);   // next text -> shown text (elided); its backdrop size
    QRect place(const QRect &area, const QSize &canvas);   // window geometry; the canvas in it
    void outOfTheWay();   // the pointer came near (hide on hover)
    void gone();          // hidden at once
    void forgetPointer(QRect area = QRect());   // see the .cpp
    void notify();
    void fadeTo(qreal target, int ms);

    Settings m_settings;
    Backend m_backend;
    quint64 m_uid = 0;
    QString m_title;         // on screen
    QString m_details;
    QString m_nextTitle;     // to show (the same unless a swap is under way)
    QString m_nextDetails;
    QFont m_titleFont;
    QFont m_detailsFont;
    QTimer m_hold;
    QPropertyAnimation *m_anim;
    qreal m_fade = 0;
    qreal m_fadeTo = 0;
    bool m_swapping = false;   // fading out the old text, the new one follows
    bool m_fast = false;       // see isFastSwitching()
    QElapsedTimer m_sinceChange;
    QRect m_box;               // backdrop inside the window
    QRect m_canvas;            // the window without the hover margin
    bool m_hideOnHover = false;
    bool m_hoverHidden = false;   // went away from the pointer; stays away during a burst of changes
    QTimer m_probe;               // shown invisible: is the pointer resting there?
    uint m_notificationId = 0;
};
