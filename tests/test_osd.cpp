#include "core/Track.h"
#include "ui/TrackOsd.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QSignalSpy>
#include <QTest>

// Stands in for the desktop's notification server.
class FakeNotifications : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    QStringList summaries;
    QList<uint> replaces;

public slots:
    uint Notify(const QString &, uint replacesId, const QString &, const QString &summary, const QString &,
                const QStringList &, const QVariantMap &, int)
    {
        summaries << summary;
        replaces << replacesId;
        return 42;
    }
};

class TestOsd : public QObject
{
    Q_OBJECT

private slots:
    void wanted()
    {
        using W = TrackOsd::When;
        // minimized, visible, active
        QVERIFY(!TrackOsd::wanted(W::Never, true, true, false));
        QVERIFY(TrackOsd::wanted(W::Minimized, true, true, false));
        QVERIFY(TrackOsd::wanted(W::Minimized, false, false, false));   // hidden in the tray
        QVERIFY(!TrackOsd::wanted(W::Minimized, false, true, false));   // behind other windows
        QVERIFY(TrackOsd::wanted(W::Inactive, false, true, false));
        QVERIFY(!TrackOsd::wanted(W::Inactive, false, true, true));     // the user is looking at it
        QVERIFY(TrackOsd::wanted(W::Always, false, true, true));
    }

    void placement()
    {
        using W = TrackOsd::Where;
        const QRect screen(70, 27, 1850, 1053);   // GNOME: top bar, dock on the left
        const QSize size(400, 70);
        for (W where : {W::TopLeft, W::TopCenter, W::TopRight, W::BottomLeft, W::BottomCenter, W::BottomRight}) {
            const QRect r = TrackOsd::placement(screen, size, where);
            QCOMPARE(r.size(), size);
            QVERIFY(screen.contains(r));
        }
        // Close to the edges, not floating in the middle.
        QCOMPARE(TrackOsd::placement(screen, size, W::TopLeft).topLeft(), QPoint(90, 47));
        QCOMPARE(TrackOsd::placement(screen, size, W::BottomRight).bottomRight(), QPoint(1899, 1059));
        const QRect bottom = TrackOsd::placement(screen, size, W::BottomCenter);
        QCOMPARE(bottom.center().x(), screen.center().x());
        QCOMPARE(screen.bottom() - bottom.bottom(), 20);
        // Second monitor to the right.
        const QRect right(1920, 0, 1280, 1024);
        QVERIFY(right.contains(TrackOsd::placement(right, size, W::BottomLeft)));
    }

    void lines()
    {
        Track t = Track::fromPath(QStringLiteral("/m/Kino/05 - Звезда по имени Солнце.mp3"));
        QCOMPARE(TrackOsd::lines(t), (QStringList{"05 - Звезда по имени Солнце", ""}));
        // Not scanned yet: the length from the player.
        QCOMPARE(TrackOsd::lines(t, 225000), (QStringList{"05 - Звезда по имени Солнце", "3:45"}));
        t.title = QStringLiteral("Звезда по имени Солнце");
        t.artist = QStringLiteral("Кино");
        t.album = QStringLiteral("Звезда по имени Солнце");
        t.durationMs = 225400;
        QCOMPARE(TrackOsd::lines(t), (QStringList{"Звезда по имени Солнце", "Кино · Звезда по имени Солнце · 3:45"}));
    }

    // Fades in, stays for the set time, fades out; a new track while shown
    // just replaces the text.
    void showsAndHides()
    {
        TrackOsd osd;
        QCOMPARE(osd.backend(), TrackOsd::Backend::Window);
        TrackOsd::Settings s;
        s.seconds = 1;
        osd.setSettings(s);
        Track t = Track::fromPath(QStringLiteral("/m/a.mp3"));
        t.uid = 1;
        t.title = QStringLiteral("First");
        osd.showTrack(t, 61000);
        QVERIFY(osd.isVisible());
        QVERIFY(osd.fade() < 0.5);                  // starts transparent
        QTRY_COMPARE_WITH_TIMEOUT(osd.fade(), 1.0, 1000);
        QVERIFY(osd.testAttribute(Qt::WA_ShowWithoutActivating));
        QVERIFY(osd.windowFlags() & Qt::WindowTransparentForInput);
        QVERIFY(osd.windowFlags() & Qt::WindowDoesNotAcceptFocus);
        const QSize firstSize = osd.size();

        QTest::qWait(500);
        t.uid = 2;
        t.title = QStringLiteral("Second, with a much longer title than the first one");
        osd.showTrack(t, 61000);
        // The old text fades out at its old size first...
        QCOMPARE(osd.size(), firstSize);
        QTRY_VERIFY_WITH_TIMEOUT(osd.fade() < 0.9, 300);
        QCOMPARE(osd.size(), firstSize);
        // ...then the new one comes in at its own size: never new text in the old frame.
        QTRY_VERIFY_WITH_TIMEOUT(osd.width() > firstSize.width(), 500);
        QVERIFY(osd.fade() < 0.5);
        QTRY_COMPARE_WITH_TIMEOUT(osd.fade(), 1.0, 1000);
        if (!qEnvironmentVariableIsEmpty("TEMBR_OSD_SHOT"))
            osd.grab().save(qEnvironmentVariable("TEMBR_OSD_SHOT"));

        t.artist = QStringLiteral("Tags read later");   // refresh keeps the timer
        osd.refresh(t);
        QTest::qWait(300);
        QVERIFY(osd.isVisible());   // the timer was restarted by the second track
        QTRY_COMPARE_WITH_TIMEOUT(osd.fade(), 1.0, 500);   // the new tags swapped in too
        QTRY_VERIFY_WITH_TIMEOUT(osd.fade() < 1.0 && osd.fade() > 0.0, 1500);   // fading out, not cut
        QTRY_VERIFY_WITH_TIMEOUT(!osd.isVisible(), 2000);
    }

    // A next/previous key held down: no fading in and out (it would never be
    // seen), the window keeps its size and text and backdrop change together.
    // A single change afterwards fades as usual and is compact again.
    void fastSwitching()
    {
        TrackOsd osd;
        TrackOsd::Settings s;
        s.seconds = 2;
        osd.setSettings(s);
        auto track = [](int n, const QString &title) {
            Track t = Track::fromPath(QStringLiteral("/m/x.mp3"));
            t.uid = 100 + n;
            t.title = title;
            t.artist = QStringLiteral("Artist");
            t.durationMs = 180000;
            return t;
        };
        osd.showTrack(track(0, QStringLiteral("Start")));
        QTRY_COMPARE_WITH_TIMEOUT(osd.fade(), 1.0, 1000);
        QVERIFY(!osd.isFastSwitching());

        QTest::qWait(50);
        osd.showTrack(track(1, QStringLiteral("Second, quite a bit longer than the first")));
        QVERIFY(osd.isFastSwitching());
        QCOMPARE(osd.fade(), 1.0);   // no fade at all
        const QSize canvas = osd.size();
        const QRect firstBox = osd.backdrop();
        QVERIFY(canvas.width() >= firstBox.width());
        for (int i = 2; i < 12; ++i) {   // ~30 ms apart, like key repeat
            QTest::qWait(30);
            osd.showTrack(track(i, i % 2 ? QStringLiteral("Short") : QStringLiteral("A much, much longer title here")));
            QCOMPARE(osd.size(), canvas);   // the window never resizes...
            QCOMPARE(osd.fade(), 1.0);      // ...nor blinks
            QVERIFY(QRect(QPoint(0, 0), canvas).contains(osd.backdrop()));
        }
        QVERIFY(osd.backdrop().width() != firstBox.width());   // the backdrop follows the text
        // Bottom centre: the backdrop sits at the bottom, in the middle.
        QCOMPARE(osd.backdrop().bottom(), canvas.height() - 1);
        QVERIFY(qAbs(osd.backdrop().center().x() - canvas.width() / 2) <= 1);

        // A single change after a pause: the usual fade, compact again.
        QTest::qWait(800);
        osd.showTrack(track(50, QStringLiteral("Single")));
        QTRY_VERIFY_WITH_TIMEOUT(osd.fade() < 0.9, 300);
        QTRY_VERIFY_WITH_TIMEOUT(!osd.isFastSwitching() && osd.size().width() < canvas.width(), 600);
        QCOMPARE(osd.backdrop(), QRect(QPoint(0, 0), osd.size()));
    }

    // GNOME Wayland: a click would go to the wrong window, so the OSD leaves
    // when the pointer comes near it: quickly, but fading. Told by the
    // window's enter events (the pointer position may be stale there).
    void hidesUnderThePointer()
    {
        TrackOsd osd;
        osd.setHideOnHover(true);
        QVERIFY(!(osd.windowFlags() & Qt::WindowTransparentForInput));   // it has to get the pointer
        auto enter = [&osd] {
            QEnterEvent e(QPointF(2, 2), QPointF(2, 2), osd.mapToGlobal(QPointF(2, 2)));
            QApplication::sendEvent(&osd, &e);
        };
        auto track = [](const QString &title) {
            Track t = Track::fromPath(QStringLiteral("/m/a.mp3"));
            t.title = title;
            return t;
        };

        osd.showTrack(track(QStringLiteral("Pointer test")));
        // A margin around the backdrop, where the pointer already counts.
        QVERIFY(osd.backdrop().left() >= 20 && osd.backdrop().top() >= 20);
        QVERIFY(osd.rect().contains(osd.backdrop()));
        // Invisible at first: a pointer resting there sends it away unseen.
        QVERIFY(osd.isVisible());
        QTest::qWait(40);
        QCOMPARE(osd.fade(), 0.0);
        enter();
        QVERIFY(!osd.isVisible());
        QVERIFY(!osd.isShowing());

        // A key held down meanwhile: no tries at all.
        for (int i = 0; i < 5; ++i) {
            QTest::qWait(60);
            osd.showTrack(track(QStringLiteral("Held %1").arg(i)));
            QVERIFY(!osd.isVisible());
        }
        // A single change later tries again; nobody there, so it shows.
        QTest::qWait(700);
        osd.showTrack(track(QStringLiteral("Later")));
        QVERIFY(osd.isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(osd.fade(), 1.0, 1000);

        // The pointer comes: a short fade, not a cut.
        enter();
        QVERIFY(osd.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(osd.fade() < 0.9 && osd.fade() > 0.0, 150);
        QTRY_VERIFY_WITH_TIMEOUT(!osd.isVisible(), 300);
        QVERIFY(!osd.isShowing());
    }

    // Native Wayland: a notification that replaces the previous one.
    void notificationFallback()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
            QSKIP("no session bus");
        if (bus.interface()->isServiceRegistered(QStringLiteral("org.freedesktop.Notifications")))
            QSKIP("a real notification server runs on this bus");
        FakeNotifications server;
        QVERIFY(bus.registerService(QStringLiteral("org.freedesktop.Notifications")));
        QVERIFY(bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &server,
                                   QDBusConnection::ExportAllSlots));

        TrackOsd osd;
        osd.setBackend(TrackOsd::Backend::Notification);
        Track t = Track::fromPath(QStringLiteral("/m/One.mp3"));
        osd.showTrack(t, 1000);
        QTRY_COMPARE(server.summaries.size(), 1);
        QTest::qWait(100);   // the reply with the id
        t = Track::fromPath(QStringLiteral("/m/Two.mp3"));
        osd.showTrack(t, 1000);
        QTRY_COMPARE(server.summaries.size(), 2);
        QCOMPARE(server.summaries, (QStringList{"One", "Two"}));
        QCOMPARE(server.replaces, (QList<uint>{0, 42}));   // the second replaces the first
        QVERIFY(!osd.isVisible());                        // no window of its own
        bus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications"));
        bus.unregisterService(QStringLiteral("org.freedesktop.Notifications"));
    }
};

QTEST_MAIN(TestOsd)
#include "test_osd.moc"
