// The main window with its desktop integration off (no MPRIS name on the
// bus, no global hotkeys written to gsettings, no tray icon), in a profile of
// its own (QStandardPaths test mode), playing into miniaudio's null device.

#include "TestAudio.h"
#include "audio/AudioEngine.h"
#include "core/LocalFiles.h"
#include "core/SessionFiles.h"
#include "ui/MainWindow.h"
#include "ui/PlaylistModel.h"
#include "ui/PlaylistPage.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace {

QString playlistDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/playlists");
}

// Every file below the profile with a hash of its contents.
QMap<QString, QByteArray> filesBelow(const QString &dir)
{
    QMap<QString, QByteArray> out;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        QFile f(file);
        if (f.open(QIODevice::ReadOnly))
            out.insert(file, QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha1));
    }
    return out;
}

QMap<QString, QVariant> sessionKeys()
{
    QSettings s;
    QMap<QString, QVariant> out;
    for (const QString &key : s.allKeys()) {
        if (key.startsWith(QLatin1String("session/")))
            out.insert(key, s.value(key));
    }
    return out;
}

PlaylistData playlist(const QString &name, const QStringList &paths)
{
    PlaylistData data{name, {}};
    for (const QString &p : paths) {
        Track t = Track::fromPath(p);
        t.title = QFileInfo(p).completeBaseName();
        t.scanned = true;
        data.tracks << t;
    }
    return data;
}

QTabWidget *tabsOf(MainWindow &w)
{
    return w.findChild<QTabWidget *>();
}

PlaylistPage *pageAt(MainWindow &w, int index)
{
    return qobject_cast<PlaylistPage *>(tabsOf(w)->widget(index));
}

} // namespace

class TestMainWindow : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;

    QString audio(const QString &name) { return TestAudio::writeWav(m_dir->filePath(name), 300); }

    MainWindowOptions options(std::function<SessionSnapshot()> load = {})
    {
        MainWindowOptions o;
        o.desktopIntegration = false;   // never the user's bus name or shortcuts
        o.loadSession = std::move(load);
        return o;
    }

    // A loader that waits until the test lets it go.
    static std::function<SessionSnapshot()> gated(std::shared_ptr<QSemaphore> gate, SessionSnapshot snapshot)
    {
        return [gate, snapshot] {
            gate->tryAcquire(1, 20000);
            return snapshot;
        };
    }

private slots:
    void initTestCase()
    {
        qputenv("TEMBR_AUDIO_BACKEND", "null");
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("tembr-test"));
        QCoreApplication::setApplicationName(QStringLiteral("tembr-test-window"));
    }

    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QSettings().clear();
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
        // Local files from an empty folder of the test's, not ~/Music or AIMP's.
        QDir().mkpath(m_dir->filePath(QStringLiteral("music")));
        LocalFilesSettings lf;
        lf.folders = {m_dir->filePath(QStringLiteral("music"))};
        lf.watch = false;
        lf.save();
    }

    void cleanupTestCase()
    {
        QSettings().clear();
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    }

    // Tabs in their saved order, the current one, the track played last;
    // read from the files on disk by the default loader.
    void restoresTabsAndPlayingTrack()
    {
        const QStringList a = {audio(QStringLiteral("a1.wav")), audio(QStringLiteral("a2.wav"))};
        const QStringList b = {audio(QStringLiteral("b1.wav")), audio(QStringLiteral("b2.wav")), audio(QStringLiteral("b3.wav"))};
        QVERIFY(SessionFiles::save(playlistDir(), {playlist(QStringLiteral("A"), a), playlist(QStringLiteral("B"), b)}).isEmpty());
        QSettings s;
        s.setValue(QStringLiteral("session/version"), 2);
        s.setValue(QStringLiteral("session/localTab"), 1);
        s.setValue(QStringLiteral("session/current"), 2);
        s.setValue(QStringLiteral("session/playingTab"), 2);
        s.setValue(QStringLiteral("session/playingRow"), 1);
        s.sync();

        MainWindow w(options());
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        w.show();
        QVERIFY(loaded.wait(10000));
        QCOMPARE(tabsOf(w)->count(), 3);
        QCOMPARE(pageAt(w, 0)->model()->name(), QStringLiteral("A"));
        QVERIFY(pageAt(w, 1)->isReadOnly());   // Local files
        QCOMPARE(pageAt(w, 2)->model()->name(), QStringLiteral("B"));
        QCOMPARE(tabsOf(w)->currentIndex(), 2);
        QCOMPARE(pageAt(w, 2)->model()->currentRow(), 1);
        QCOMPARE(pageAt(w, 0)->model()->currentRow(), -1);
        QVERIFY(tabsOf(w)->isEnabled());
    }

    // Saved before Local files existed: no version, it goes first and every
    // index moves by one.
    void restoresVersionOneSession()
    {
        SessionSnapshot snap;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("a.wav"))}),
                          playlist(QStringLiteral("B"), {audio(QStringLiteral("b1.wav")), audio(QStringLiteral("b2.wav"))})};
        QSettings s;
        s.setValue(QStringLiteral("session/current"), 1);
        s.setValue(QStringLiteral("session/playingTab"), 1);
        s.setValue(QStringLiteral("session/playingRow"), 1);
        s.sync();

        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        QVERIFY(loaded.wait(10000));
        QVERIFY(pageAt(w, 0)->isReadOnly());
        QCOMPARE(pageAt(w, 2)->model()->name(), QStringLiteral("B"));
        QCOMPARE(tabsOf(w)->currentIndex(), 2);
        QCOMPARE(pageAt(w, 2)->model()->currentRow(), 1);
    }

    // The track played last was in Local files: found by its path (rows
    // change with the folders).
    void playingTrackInLocalFilesByPath()
    {
        const QString x = audio(QStringLiteral("music/x.wav"));
        const QString y = audio(QStringLiteral("music/y.wav"));
        SessionSnapshot snap;
        snap.localFiles = playlist({}, {x, y}).tracks;
        QSettings s;
        s.setValue(QStringLiteral("session/version"), 2);
        s.setValue(QStringLiteral("session/localTab"), 0);
        s.setValue(QStringLiteral("session/playingTab"), 0);
        s.setValue(QStringLiteral("session/playingRow"), 0);   // stale: the row moved
        s.setValue(QStringLiteral("session/playingPath"), y);
        s.sync();

        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        QVERIFY(loaded.wait(10000));
        QCOMPARE(pageAt(w, 0)->model()->currentTrack()->path, y);
    }

    // Until the playlists are in they cannot be used; files opened meanwhile
    // (`tembr file.mp3` at startup) go into the playlist restored, and Play
    // pressed meanwhile plays once they are in.
    void actionsBeforeLoadWaitForIt()
    {
        auto gate = std::make_shared<QSemaphore>();
        SessionSnapshot snap;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("a1.wav")), audio(QStringLiteral("a2.wav"))})};
        QSettings s;
        s.setValue(QStringLiteral("session/version"), 2);
        s.setValue(QStringLiteral("session/localTab"), 0);
        s.setValue(QStringLiteral("session/current"), 1);
        s.setValue(QStringLiteral("session/playingTab"), 1);
        s.setValue(QStringLiteral("session/playingRow"), 1);
        s.sync();

        MainWindow w(options(gated(gate, snap)));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        w.show();
        QTest::qWait(50);
        QVERIFY(!w.isSessionLoaded());
        QVERIFY(!tabsOf(w)->isEnabled());
        QCOMPARE(tabsOf(w)->count(), 1);   // only Local files so far
        auto *engine = w.findChild<AudioEngine *>();
        w.handleCommand(QStringLiteral("play"));
        QCOMPARE(engine->state(), AudioEngine::State::Stopped);   // kept for later

        gate->release();
        QVERIFY(loaded.wait(10000));
        QCOMPARE(tabsOf(w)->count(), 2);
        PlaylistModel *a = pageAt(w, 1)->model();
        QCOMPARE(a->name(), QStringLiteral("A"));
        QCOMPARE(engine->state(), AudioEngine::State::Playing);
        QCOMPARE(a->currentRow(), 1);   // from where it stopped last time
    }

    void openedBeforeLoadGoesIntoRestoredPlaylist()
    {
        auto gate = std::make_shared<QSemaphore>();
        SessionSnapshot snap;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("a1.wav"))})};
        QSettings s;
        s.setValue(QStringLiteral("session/version"), 2);
        s.setValue(QStringLiteral("session/localTab"), 0);
        s.setValue(QStringLiteral("session/current"), 1);
        s.sync();

        MainWindow w(options(gated(gate, snap)));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        const QString opened = audio(QStringLiteral("opened.wav"));
        w.openPaths({opened});
        gate->release();
        QVERIFY(loaded.wait(10000));
        QCOMPARE(tabsOf(w)->count(), 2);   // no new empty playlist for it
        PlaylistModel *a = pageAt(w, 1)->model();
        QTRY_COMPARE(a->rowCount(), 2);
        QCOMPARE(a->track(1).path, opened);
    }

    // The scan of the music folders starts from the cache, once it is in
    // (also when asked for before):
    // tracks keep their uids (and tags) instead of coming back as new ones.
    void folderScanStartsFromTheCache()
    {
        const QString x = audio(QStringLiteral("music/x.wav"));
        SessionSnapshot snap;
        snap.localFiles = playlist({}, {x}).tracks;
        snap.localFiles[0].uid = Track::newUid();
        snap.localFiles[0].title = QStringLiteral("From the cache");
        const quint64 uid = snap.localFiles[0].uid;

        auto gate = std::make_shared<QSemaphore>();
        MainWindow w(options(gated(gate, snap)));
        auto *local = w.findChild<LocalFiles *>();
        QSignalSpy scanned(local, &LocalFiles::scanned);
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        emit local->changedOnDisk();   // a folder changed (or F5) before the cache is in
        QVERIFY(!local->isScanning());   // not from an empty list: started once the cache is in
        QTest::qWait(50);
        gate->release();
        QVERIFY(loaded.wait(10000));
        QVERIFY(scanned.wait(10000));
        QTRY_VERIFY(!local->isScanning());
        PlaylistModel *model = pageAt(w, 0)->model();
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->track(0).uid, uid);
    }

    // Closed before the playlists were read: nothing is written. A save then
    // would write an empty set and remove every playlist file.
    void closedBeforeLoadWritesNothing()
    {
        QVERIFY(SessionFiles::save(playlistDir(), {playlist(QStringLiteral("A"), {audio(QStringLiteral("a.wav"))}),
                                                   playlist(QStringLiteral("B"), {audio(QStringLiteral("b.wav"))})})
                    .isEmpty());
        LocalFiles(nullptr).saveCache(playlist({}, {audio(QStringLiteral("music/x.wav"))}).tracks);
        QSettings s;
        s.setValue(QStringLiteral("session/version"), 2);
        s.setValue(QStringLiteral("session/count"), 2);
        s.setValue(QStringLiteral("session/current"), 2);
        s.sync();
        const QString profile = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const auto filesBefore = filesBelow(profile);
        const auto keysBefore = sessionKeys();
        QCOMPARE(filesBefore.size(), 3);

        auto gate = std::make_shared<QSemaphore>();
        {
            MainWindow w(options(gated(gate, SessionSnapshot{})));
            w.show();
            QTest::qWait(50);
            QVERIFY(!w.isSessionLoaded());
            w.close();
        }
        gate->release();   // the job finishes after the window is gone
        QTest::qWait(100);
        QCOMPARE(filesBelow(profile), filesBefore);
        QCOMPARE(sessionKeys(), keysBefore);
    }
};

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
