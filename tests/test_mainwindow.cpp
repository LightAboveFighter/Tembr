// The main window with its desktop integration off (no MPRIS name on the
// bus, no global hotkeys written to gsettings, no tray icon), in a profile of
// its own (QStandardPaths test mode), playing into miniaudio's null device.

#include "TestAudio.h"
#include "audio/AudioEngine.h"
#include "core/LocalFiles.h"
#include "core/SessionFiles.h"
#include "ui/MainWindow.h"
#include "ui/PlaylistFilterProxy.h"
#include "ui/PlaylistModel.h"
#include "ui/PlaylistPage.h"
#include "ui/PlaylistPanes.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QWheelEvent>
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

PlaylistPage *localOf(MainWindow &w)
{
    return w.panes()->localPage();
}

// The user's playlist at `index` in their order (nullptr: none).
PlaylistPage *playlistAt(MainWindow &w, int index)
{
    return w.panes()->playlists().value(index, nullptr);
}

int playlistCount(MainWindow &w)
{
    return int(w.panes()->playlists().size());
}

// The totals in the status bar (the other label there is the sound device).
QString statsText(MainWindow &w)
{
    for (QLabel *label : w.statusBar()->findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("треков")))
            return label->text();
    }
    return {};
}

QAction *menuAction(QMenu *menu, const QString &text)
{
    for (QAction *a : menu->actions()) {
        if (a->text() == text)
            return a;
    }
    return nullptr;
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

    // Version 2 (tab positions, Local files at session/localTab): playlists
    // in their saved order, the one that was current shown, the track played
    // last; read from the files on disk by the default loader.
    void restoresVersionTwoSession()
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
        QCOMPARE(playlistCount(w), 2);
        QCOMPARE(playlistAt(w, 0)->model()->name(), QStringLiteral("A"));
        QCOMPARE(playlistAt(w, 1)->model()->name(), QStringLiteral("B"));
        QVERIFY(localOf(w)->isReadOnly());
        QCOMPARE(w.panes()->shownPlaylist(), playlistAt(w, 1));   // tab 2, after Local files at 1
        QCOMPARE(w.panes()->activePage(), playlistAt(w, 1));
        QCOMPARE(playlistAt(w, 1)->model()->currentRow(), 1);
        QCOMPARE(playlistAt(w, 0)->model()->currentRow(), -1);
        QVERIFY(w.panes()->isEnabled());
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
        QCOMPARE(playlistCount(w), 2);
        QCOMPARE(w.panes()->shownPlaylist()->model()->name(), QStringLiteral("B"));
        QCOMPARE(w.panes()->activePage(), w.panes()->shownPlaylist());
        QCOMPARE(playlistAt(w, 1)->model()->currentRow(), 1);
        QCOMPARE(playlistAt(w, 0)->model()->currentRow(), -1);
    }

    // Version 2 with Local files in front: they are the active list, and the
    // playlist that played is shown beside them.
    void restoresVersionTwoWithLocalFilesCurrent()
    {
        SessionSnapshot snap;
        snap.localFiles = playlist({}, {audio(QStringLiteral("music/x.wav"))}).tracks;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("a.wav"))}),
                          playlist(QStringLiteral("B"), {audio(QStringLiteral("b1.wav"))}),
                          playlist(QStringLiteral("C"), {audio(QStringLiteral("c1.wav")), audio(QStringLiteral("c2.wav"))})};
        QSettings s;
        s.setValue(QStringLiteral("session/version"), 2);
        s.setValue(QStringLiteral("session/localTab"), 1);
        s.setValue(QStringLiteral("session/current"), 1);      // Local files
        s.setValue(QStringLiteral("session/playingTab"), 3);   // C: A, Local files, B, C
        s.setValue(QStringLiteral("session/playingRow"), 1);
        s.sync();

        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        QVERIFY(loaded.wait(10000));
        QCOMPARE(w.panes()->activePage(), localOf(w));
        QCOMPARE(w.panes()->shownPlaylist(), playlistAt(w, 2));
        QCOMPARE(playlistAt(w, 2)->model()->currentRow(), 1);
    }

    // Version 3: the playlist shown, the active list and the playlist that
    // played are named by their place among the playlists.
    void restoresVersionThreeSession()
    {
        SessionSnapshot snap;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("a.wav"))}),
                          playlist(QStringLiteral("B"), {audio(QStringLiteral("b1.wav")), audio(QStringLiteral("b2.wav"))}),
                          playlist(QStringLiteral("C"), {audio(QStringLiteral("c.wav"))})};
        QSettings s;
        s.setValue(QStringLiteral("session/version"), 3);
        s.setValue(QStringLiteral("session/shown"), 2);
        s.setValue(QStringLiteral("session/active"), QStringLiteral("local"));
        s.setValue(QStringLiteral("session/playingList"), QStringLiteral("playlist"));
        s.setValue(QStringLiteral("session/playingPlaylist"), 1);
        s.setValue(QStringLiteral("session/playingRow"), 1);
        s.sync();

        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        QVERIFY(loaded.wait(10000));
        QCOMPARE(w.panes()->shownPlaylist(), playlistAt(w, 2));
        QCOMPARE(w.panes()->activePage(), localOf(w));
        QCOMPARE(playlistAt(w, 1)->model()->currentRow(), 1);
        QCOMPARE(playlistAt(w, 2)->model()->currentRow(), -1);
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

        {
            MainWindow w(options([snap] { return snap; }));
            QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
            QVERIFY(loaded.wait(10000));
            QCOMPARE(localOf(w)->model()->currentTrack()->path, y);
            QCOMPARE(w.panes()->activePage(), localOf(w));   // no playlists: Local files
            w.close();   // saved as version 3
        }
        QSettings saved;
        QCOMPARE(saved.value(QStringLiteral("session/version")).toInt(), 3);
        QCOMPARE(saved.value(QStringLiteral("session/playingList")).toString(), QStringLiteral("local"));
        QVERIFY(!saved.contains(QStringLiteral("session/playingTab")));

        // The same in version 3.
        snap.localFiles = {snap.localFiles[1], snap.localFiles[0]};
        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        QVERIFY(loaded.wait(10000));
        QCOMPARE(localOf(w)->model()->currentTrack()->path, y);
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
        QVERIFY(!w.panes()->isEnabled());
        QCOMPARE(playlistCount(w), 0);   // only Local files so far
        auto *engine = w.findChild<AudioEngine *>();
        w.handleCommand(QStringLiteral("play"));
        QCOMPARE(engine->state(), AudioEngine::State::Stopped);   // kept for later

        gate->release();
        QVERIFY(loaded.wait(10000));
        QCOMPARE(playlistCount(w), 1);
        PlaylistModel *a = playlistAt(w, 0)->model();
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
        QCOMPARE(playlistCount(w), 1);   // no new empty playlist for it
        PlaylistModel *a = playlistAt(w, 0)->model();
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
        PlaylistModel *model = localOf(w)->model();
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->track(0).uid, uid);
    }

    // Playing from the window: Space plays the row in focus, a double click
    // or Enter on a row plays it, Enter in the search box the first match;
    // the window title shows the track.
    void playFromTheWindow()
    {
        SessionSnapshot snap;
        PlaylistData a = playlist(QStringLiteral("A"), {audio(QStringLiteral("w0.wav")), audio(QStringLiteral("w1.wav")),
                                                        audio(QStringLiteral("w2.wav")), audio(QStringLiteral("w3.wav"))});
        a.tracks[2].title = QStringLiteral("Needle");
        snap.playlists = {a};
        QSettings s;
        s.setValue(QStringLiteral("session/version"), 2);
        s.setValue(QStringLiteral("session/localTab"), 0);
        s.setValue(QStringLiteral("session/current"), 1);
        s.sync();

        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        w.show();
        QVERIFY(loaded.wait(10000));
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        auto *engine = w.findChild<AudioEngine *>();
        PlaylistPage *page = playlistAt(w, 0);
        PlaylistModel *model = page->model();
        PlaylistView *view = page->view();

        view->setFocus();
        view->setCurrentIndex(page->proxy()->index(1, PlaylistModel::ColTitle));
        QTest::keyClick(view, Qt::Key_Space);
        QCOMPARE(model->currentRow(), 1);
        QCOMPARE(engine->state(), AudioEngine::State::Playing);
        QCOMPARE(w.windowTitle(), model->track(1).displayTitle() + QStringLiteral(" — Tembr"));
        QTest::keyClick(view, Qt::Key_Space);   // pause, not another row
        QCOMPARE(engine->state(), AudioEngine::State::Paused);

        const QRect row3 = view->visualRect(page->proxy()->index(3, PlaylistModel::ColTitle));
        // As a real double click comes: press, release, then the double click.
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, row3.center());
        QTest::mouseDClick(view->viewport(), Qt::LeftButton, {}, row3.center());
        QCOMPARE(model->currentRow(), 3);
        QCOMPARE(engine->state(), AudioEngine::State::Playing);

        view->setCurrentIndex(page->proxy()->index(0, PlaylistModel::ColTitle));
        QTest::keyClick(view, Qt::Key_Return);
        QCOMPARE(model->currentRow(), 0);

        auto *search = w.findChild<QLineEdit *>();
        search->setFocus();
        QTest::keyClicks(search, QStringLiteral("needle"));
        QTRY_COMPARE(page->proxy()->rowCount(), 1);
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(model->currentRow(), 2);
        QCOMPARE(w.windowTitle(), QStringLiteral("Needle — Tembr"));
        w.handleCommand(QStringLiteral("stop"));
        QCOMPARE(engine->state(), AudioEngine::State::Stopped);
    }

    // Commands from MPRIS, the tray and `tembr --next`; the session saved on
    // close keeps the playlist and row that played.
    void commandsAndSavedPlayingTrack()
    {
        SessionSnapshot snap;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("c0.wav")), audio(QStringLiteral("c1.wav")),
                                                         audio(QStringLiteral("c2.wav"))})};
        QSettings s;
        s.setValue(QStringLiteral("session/version"), 2);
        s.setValue(QStringLiteral("session/localTab"), 0);
        s.setValue(QStringLiteral("session/current"), 1);
        s.setValue(QStringLiteral("volume"), 97);
        s.sync();

        {
            MainWindow w(options([snap] { return snap; }));
            QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
            QVERIFY(loaded.wait(10000));
            auto *engine = w.findChild<AudioEngine *>();
            PlaylistModel *model = playlistAt(w, 0)->model();

            w.handleCommand(QStringLiteral("play"));   // not the active window: the first row
            QCOMPARE(model->currentRow(), 0);
            QCOMPARE(engine->state(), AudioEngine::State::Playing);
            w.handleCommand(QStringLiteral("play"));   // already playing: stays so
            QCOMPARE(engine->state(), AudioEngine::State::Playing);
            w.handleCommand(QStringLiteral("next"));
            w.handleCommand(QStringLiteral("next"));
            QCOMPARE(model->currentRow(), 2);
            w.handleCommand(QStringLiteral("previous"));
            QCOMPARE(model->currentRow(), 1);
            w.handleCommand(QStringLiteral("pause"));
            QCOMPARE(engine->state(), AudioEngine::State::Paused);
            w.handleCommand(QStringLiteral("play-pause"));
            QCOMPARE(engine->state(), AudioEngine::State::Playing);
            w.handleCommand(QStringLiteral("volume-up"));
            QVERIFY(qAbs(engine->volume() - 1.0f) < 0.001f);   // 97 + 5, at most 100
            w.handleCommand(QStringLiteral("volume-down"));
            QVERIFY(qAbs(engine->volume() - 0.95f) < 0.001f);
            w.handleCommand(QStringLiteral("stop"));
            QCOMPARE(engine->state(), AudioEngine::State::Stopped);
            w.close();   // saves
        }
        QSettings saved;
        QCOMPARE(saved.value(QStringLiteral("session/version")).toInt(), 3);
        QCOMPARE(saved.value(QStringLiteral("session/playingList")).toString(), QStringLiteral("playlist"));
        QCOMPARE(saved.value(QStringLiteral("session/playingPlaylist")).toInt(), 0);
        QCOMPARE(saved.value(QStringLiteral("session/playingRow")).toInt(), 1);
        QCOMPARE(saved.value(QStringLiteral("session/shown")).toInt(), 0);
        QCOMPARE(saved.value(QStringLiteral("volume")).toInt(), 95);
        QCOMPARE(SessionFiles::load(playlistDir()).playlists.size(), 1);
    }

    // Right click on tracks of Local files -> "Send to playlist" -> a
    // playlist: copied to its end; neither Local files nor the playlist the
    // tracks are in are offered.
    void sendToPlaylistFromTheMenu()
    {
        SessionSnapshot snap;
        snap.localFiles = playlist({}, {audio(QStringLiteral("music/l0.wav")), audio(QStringLiteral("music/l1.wav")),
                                        audio(QStringLiteral("music/l2.wav"))}).tracks;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("a0.wav"))}),
                          playlist(QStringLiteral("B"), {})};
        QSettings s;
        s.setValue(QStringLiteral("session/version"), 2);
        s.setValue(QStringLiteral("session/localTab"), 0);
        s.setValue(QStringLiteral("session/current"), 0);
        s.sync();

        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        QSignalSpy scanned(w.findChild<LocalFiles *>(), &LocalFiles::scanned);
        w.show();
        QVERIFY(loaded.wait(10000));
        QVERIFY(scanned.wait(10000));   // the folder scan replaces the list (and its selection)
        PlaylistPage *local = localOf(w);
        PlaylistModel *a = playlistAt(w, 0)->model();
        QCOMPARE(w.panes()->activePage(), local);   // tab 0 was current
        auto *sel = local->view()->selectionModel();
        sel->select(local->proxy()->index(0, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        sel->select(local->proxy()->index(2, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        local->view()->setCurrentIndex(local->proxy()->index(2, 0));
        sel->select(local->proxy()->index(0, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);

        // The menu runs its own loop: pick from it once it is up.
        QStringList offered;
        QTimer::singleShot(200, this, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            QVERIFY(menu);
            QMenu *send = nullptr;
            for (QAction *act : menu->actions()) {
                if (act->text() == QStringLiteral("Отправить в плейлист"))
                    send = act->menu();
            }
            QVERIFY(send);
            for (QAction *act : send->actions()) {
                if (!act->isSeparator())
                    offered << act->text();
            }
            for (QAction *act : send->actions()) {
                if (act->text() == QStringLiteral("A"))
                    act->trigger();
            }
            menu->close();
        });
        emit local->contextMenuRequested(local->view()->viewport()->mapToGlobal(QPoint(10, 10)));
        QCOMPARE(offered, (QStringList{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("Новый плейлист…")}));
        QCOMPARE(a->rowCount(), 3);
        QCOMPARE(a->track(1).path, local->model()->track(0).path);
        QCOMPARE(a->track(2).path, local->model()->track(2).path);
        QCOMPARE(local->model()->rowCount(), 3);
        QVERIFY(w.statusBar()->currentMessage().contains(QStringLiteral("«A»: 2")));
    }

    // No playlists: the right pane holds no list (no "Плейлист 1" is made
    // at startup); files added then go into a new playlist, which is shown.
    void noPlaylistsLeavesTheRightPaneFree()
    {
        MainWindow w(options([] { return SessionSnapshot{}; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        w.show();
        QVERIFY(loaded.wait(10000));
        QCOMPARE(playlistCount(w), 0);
        QVERIFY(!w.panes()->shownPlaylist());
        QCOMPARE(w.findChildren<PlaylistPage *>().size(), 1);   // Local files only
        QCOMPARE(w.panes()->activePage(), localOf(w));

        const QString file = audio(QStringLiteral("added.wav"));
        w.openPaths({file});
        QCOMPARE(playlistCount(w), 1);
        PlaylistPage *made = playlistAt(w, 0);
        QCOMPARE(made->model()->name(), QStringLiteral("Плейлист 1"));
        QCOMPARE(w.panes()->shownPlaylist(), made);
        QCOMPARE(w.panes()->activePage(), made);
        QTRY_COMPARE(made->model()->rowCount(), 1);
        QCOMPARE(made->model()->track(0).path, file);
        w.handleCommand(QStringLiteral("stop"));
    }

    // Files dropped from a file manager on the free right pane make a
    // playlist too.
    void filesDroppedOnTheFreePaneMakeAPlaylist()
    {
        MainWindow w(options([] { return SessionSnapshot{}; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        w.show();
        QVERIFY(loaded.wait(10000));
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QWidget *free = nullptr;
        for (QPushButton *b : w.panes()->findChildren<QPushButton *>()) {
            if (b->text() == QStringLiteral("Новый плейлист"))
                free = b->parentWidget();
        }
        QVERIFY(free && free->isVisible() && free->acceptDrops());

        const QString file = audio(QStringLiteral("dropped.wav"));
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(file)});
        const QPoint pos = free->rect().center();
        QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(free, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(free, &drop);
        QCOMPARE(playlistCount(w), 1);
        QCOMPARE(w.panes()->shownPlaylist(), playlistAt(w, 0));
        QTRY_COMPARE(playlistAt(w, 0)->model()->rowCount(), 1);
        QCOMPARE(playlistAt(w, 0)->model()->track(0).path, file);
    }

    // The header of the right pane lists the playlists; picking one shows
    // it (the pages are kept, not made again) and makes its pane active.
    void pickingFromTheHeaderSwitchesThePlaylist()
    {
        SessionSnapshot snap;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("a.wav"))}),
                          playlist(QStringLiteral("B"), {audio(QStringLiteral("b.wav"))}),
                          playlist(QStringLiteral("C"), {audio(QStringLiteral("c.wav"))})};
        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        w.show();
        QVERIFY(loaded.wait(10000));
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        PlaylistPanes *panes = w.panes();
        PlaylistPage *a = playlistAt(w, 0);
        PlaylistPage *c = playlistAt(w, 2);
        QCOMPARE(panes->shownPlaylist(), a);
        panes->activate(PlaylistPanes::Pane::Local);
        QVERIFY(panes->picker()->text().startsWith(QStringLiteral("A")));

        QTest::mouseClick(panes->picker(), Qt::LeftButton);
        QTRY_VERIFY(panes->pickerMenu()->isVisible());
        QStringList names;
        for (QAction *act : panes->pickerMenu()->actions()) {
            if (!act->isSeparator())
                names << act->text();
        }
        QCOMPARE(names, (QStringList{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"),
                                     QStringLiteral("Новый плейлист"), QStringLiteral("Переименовать плейлист…"),
                                     QStringLiteral("Закрыть плейлист")}));
        QVERIFY(menuAction(panes->pickerMenu(), QStringLiteral("A"))->isChecked());
        menuAction(panes->pickerMenu(), QStringLiteral("C"))->trigger();
        panes->pickerMenu()->close();
        QCOMPARE(panes->shownPlaylist(), c);
        QVERIFY(c->isVisible());
        QVERIFY(!a->isVisible());
        QCOMPARE(panes->activePage(), c);
        QVERIFY(panes->picker()->text().startsWith(QStringLiteral("C")));

        panes->showPlaylist(a);   // back: the same page as before
        QCOMPARE(panes->shownPlaylist(), a);
        QCOMPARE(playlistAt(w, 0), a);
        QCOMPARE(w.findChildren<PlaylistPage *>().size(), 4);
    }

    // The mouse wheel over the header does not change the playlist (QTabBar
    // and QComboBox would), and the scroll goes no further either.
    void wheelOverTheHeaderDoesNothing()
    {
        SessionSnapshot snap;
        snap.playlists = {playlist(QStringLiteral("A"), {}), playlist(QStringLiteral("B"), {}),
                          playlist(QStringLiteral("C"), {})};
        QSettings s;
        s.setValue(QStringLiteral("session/version"), 3);
        s.setValue(QStringLiteral("session/shown"), 1);
        s.sync();
        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        w.show();
        QVERIFY(loaded.wait(10000));
        PlaylistPanes *panes = w.panes();
        PlaylistPage *b = playlistAt(w, 1);
        QCOMPARE(panes->shownPlaylist(), b);
        QSignalSpy shown(panes, &PlaylistPanes::shownChanged);

        PlaylistPicker *picker = panes->picker();
        const QPointF pos(picker->width() / 2.0, picker->height() / 2.0);
        for (int delta : {-120, -120, 120, 120, 120, -360}) {
            QWheelEvent wheel(pos, picker->mapToGlobal(pos), QPoint(), QPoint(0, delta), Qt::NoButton,
                              Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(picker, &wheel);
            QVERIFY(wheel.isAccepted());   // taken by the header, not passed on
        }
        QCOMPARE(panes->shownPlaylist(), b);
        QCOMPARE(shown.count(), 0);
        QVERIFY(panes->picker()->text().startsWith(QStringLiteral("B")));
    }

    // By default the search filters both lists; Enter plays the first match
    // of the active list, else of the other one.
    void searchFiltersBothLists()
    {
        SessionSnapshot snap;
        snap.localFiles = playlist({}, {audio(QStringLiteral("music/needle-local.wav")),
                                        audio(QStringLiteral("music/other-local.wav"))}).tracks;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("x.wav")), audio(QStringLiteral("needle-a.wav")),
                                                         audio(QStringLiteral("y.wav"))})};
        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        w.show();
        QVERIFY(loaded.wait(10000));
        PlaylistPage *local = localOf(w);
        PlaylistPage *a = playlistAt(w, 0);
        QCOMPARE(w.panes()->activePage(), a);

        auto *search = w.findChild<QLineEdit *>();
        search->setText(QStringLiteral("needle"));
        QTRY_COMPARE(a->proxy()->rowCount(), 1);
        QTRY_COMPARE(local->proxy()->rowCount(), 1);
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(a->model()->currentRow(), 1);   // the active list first

        search->setText(QStringLiteral("local"));
        QTRY_COMPARE(a->proxy()->rowCount(), 0);
        QTRY_COMPARE(local->proxy()->rowCount(), 2);
        QTest::keyClick(search, Qt::Key_Return);   // nothing in A: from Local files
        QVERIFY(local->model()->currentTrack());
        QVERIFY(local->model()->currentTrack()->path.endsWith(QStringLiteral("needle-local.wav")));
        w.handleCommand(QStringLiteral("stop"));
    }

    // Set to "only the active list": the other list stays whole, and the
    // filter moves with the active pane.
    void searchFiltersOnlyTheActiveList()
    {
        AppSettings settings = AppSettings::load();
        settings.searchScope = AppSettings::SearchScope::ActiveList;
        settings.save();
        SessionSnapshot snap;
        snap.localFiles = playlist({}, {audio(QStringLiteral("music/needle-local.wav")),
                                        audio(QStringLiteral("music/other-local.wav"))}).tracks;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("x.wav")), audio(QStringLiteral("needle-a.wav")),
                                                         audio(QStringLiteral("y.wav"))})};
        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        w.show();
        QVERIFY(loaded.wait(10000));
        PlaylistPage *local = localOf(w);
        PlaylistPage *a = playlistAt(w, 0);
        QCOMPARE(w.panes()->activePage(), a);

        auto *search = w.findChild<QLineEdit *>();
        search->setText(QStringLiteral("needle"));
        QTRY_COMPARE(a->proxy()->rowCount(), 1);
        QTest::qWait(300);   // past the search delay: Local files stay whole
        QCOMPARE(local->proxy()->rowCount(), local->model()->rowCount());
        QCOMPARE(local->proxy()->rowCount(), 2);

        w.panes()->activate(PlaylistPanes::Pane::Local);
        QCOMPARE(local->proxy()->rowCount(), 1);
        QCOMPARE(a->proxy()->rowCount(), 3);

        search->setText(QStringLiteral("x"));   // matches only in A: Enter plays nothing elsewhere
        QTRY_COMPARE(local->proxy()->rowCount(), 0);
        QTest::keyClick(search, Qt::Key_Return);
        QVERIFY(!a->model()->currentTrack());
    }

    // The active list is the one the focus was in last: the status bar
    // counts it and Del removes from it (from Local files nothing).
    void activePaneFollowsTheFocus()
    {
        SessionSnapshot snap;
        snap.localFiles = playlist({}, {audio(QStringLiteral("music/l0.wav")), audio(QStringLiteral("music/l1.wav"))}).tracks;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("a0.wav")), audio(QStringLiteral("a1.wav")),
                                                         audio(QStringLiteral("a2.wav"))})};
        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        QSignalSpy scanned(w.findChild<LocalFiles *>(), &LocalFiles::scanned);
        w.show();
        QVERIFY(loaded.wait(10000));
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        PlaylistPanes *panes = w.panes();
        PlaylistPage *local = localOf(w);
        PlaylistPage *a = playlistAt(w, 0);
        QCOMPARE(panes->activePage(), a);
        QTRY_VERIFY(statsText(w).startsWith(QStringLiteral("3 треков")));

        local->view()->setFocus();
        QTRY_COMPARE(panes->activePage(), local);
        QCOMPARE(panes->otherPage(), a);
        QTRY_VERIFY(statsText(w).startsWith(QStringLiteral("2 треков")));
        local->view()->selectRow(0);
        QTest::keyClick(local->view(), Qt::Key_Delete);
        QCOMPARE(local->model()->rowCount(), 2);   // read-only
        QCOMPARE(a->model()->rowCount(), 3);
        QVERIFY(w.statusBar()->currentMessage().contains(QStringLiteral("Локальные файлы")));

        a->view()->setFocus();
        QTRY_COMPARE(panes->activePage(), a);
        QTRY_VERIFY(statsText(w).startsWith(QStringLiteral("3 треков")));
        a->view()->selectRow(1);
        QTest::keyClick(a->view(), Qt::Key_Delete);
        QCOMPARE(a->model()->rowCount(), 2);
        QTRY_VERIFY(statsText(w).startsWith(QStringLiteral("2 треков")));
        QCOMPARE(local->model()->rowCount(), 2);

        // The search field takes the focus without changing the active list.
        w.findChild<QLineEdit *>()->setFocus();
        QTest::qWait(20);
        QCOMPARE(panes->activePage(), a);
    }

    // The split between the two lists and the playlist shown come back
    // after a restart, as does the active list.
    void panesSurviveARestart()
    {
        SessionSnapshot snap;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("a.wav"))}),
                          playlist(QStringLiteral("B"), {audio(QStringLiteral("b.wav"))}),
                          playlist(QStringLiteral("C"), {audio(QStringLiteral("c.wav"))})};
        QList<int> sizes;
        {
            MainWindow w(options([snap] { return snap; }));
            QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
            w.show();
            QVERIFY(loaded.wait(10000));
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            QSplitter *split = w.panes()->splitter();
            const int total = split->sizes().value(0) + split->sizes().value(1);
            split->setSizes({total / 4, total - total / 4});
            sizes = split->sizes();
            w.panes()->showPlaylist(playlistAt(w, 2));
            w.panes()->activate(PlaylistPanes::Pane::Local);
            w.close();
        }
        QCOMPARE(QSettings().value(QStringLiteral("session/shown")).toInt(), 2);

        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        w.show();
        QVERIFY(loaded.wait(10000));
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QCOMPARE(w.panes()->shownPlaylist(), playlistAt(w, 2));
        QCOMPARE(w.panes()->activePage(), localOf(w));
        QCOMPARE(w.panes()->splitter()->sizes(), sizes);
    }

    // Columns shown or hidden from the header menu: in both lists at once.
    void columnsShownInBothLists()
    {
        SessionSnapshot snap;
        snap.localFiles = playlist({}, {audio(QStringLiteral("music/l0.wav"))}).tracks;
        snap.playlists = {playlist(QStringLiteral("A"), {audio(QStringLiteral("a0.wav"))}),
                          playlist(QStringLiteral("B"), {audio(QStringLiteral("b0.wav"))})};
        MainWindow w(options([snap] { return snap; }));
        QSignalSpy loaded(&w, &MainWindow::sessionLoaded);
        w.resize(2400, 800);   // room for every column in both lists, no scrolling
        w.show();
        QVERIFY(loaded.wait(10000));
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.panes()->splitter()->setSizes({1200, 800});   // the two lists differ in width
        PlaylistPage *local = localOf(w);
        PlaylistPage *a = playlistAt(w, 0);
        PlaylistPage *b = playlistAt(w, 1);
        for (PlaylistPage *p : {local, a, b})
            QVERIFY(p->view()->horizontalHeader()->isSectionHidden(PlaylistModel::ColPath));

        const QString pathTitle = local->model()->headerData(PlaylistModel::ColPath, Qt::Horizontal, Qt::DisplayRole).toString();
        QTimer::singleShot(200, this, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            QVERIFY(menu);
            if (QAction *act = menuAction(menu, pathTitle))
                act->trigger();
            menu->close();
        });
        emit a->view()->horizontalHeader()->customContextMenuRequested(QPoint(5, 5));
        for (PlaylistPage *p : {local, a, b})
            QVERIFY(!p->view()->horizontalHeader()->isSectionHidden(PlaylistModel::ColPath));
        // Each list on screen fits its columns to its own width.
        QVERIFY(local->view()->viewport()->width() > a->view()->viewport()->width() + 100);
        for (PlaylistPage *p : {local, a}) {
            QHeaderView *header = p->view()->horizontalHeader();
            QTRY_VERIFY(header->sectionSize(PlaylistModel::ColPath) > 0);
            QTRY_VERIFY(qAbs(header->length() - p->view()->viewport()->width()) <= 2);
        }
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
