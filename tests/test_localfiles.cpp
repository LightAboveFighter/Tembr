#include "core/LocalFiles.h"
#include "core/TrackSort.h"

#include <QDateTime>

#include <QDir>
#include <QFile>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

void writeUtf16(const QString &path, const QString &text)
{
    QByteArray b("\xFF\xFE", 2);
    b.append(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
    writeFile(path, b);
}

QStringList relPaths(const TrackList &tracks, const QString &root)
{
    QStringList out;
    for (const Track &t : tracks)
        out << t.path.mid(root.size() + 1);
    return out;
}

} // namespace

class TestLocalFiles : public QObject
{
    Q_OBJECT

    QTemporaryDir m_prefix;   // fake Wine prefix

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("tembr-test"));
        QCoreApplication::setApplicationName(QStringLiteral("tembr-test-localfiles"));
        QSettings().clear();
        qputenv("WINEPREFIX", m_prefix.path().toUtf8());
    }

    void cleanupTestCase() { QSettings().clear(); }

    // The same settings as AIMP's music library "Local files".
    void settingsFromAimp()
    {
        const QString ini = m_prefix.filePath(QStringLiteral("drive_c/users/me/AppData/Roaming/AIMP/AIMP.ini"));
        writeUtf16(ini, QStringLiteral("[MusicLibrary]\r\nActiveDataStorage=TAIMPMLLocalDataStorage\r\n\r\n"
                                       "[MusicLibrary.TAIMPMLLocalDataStorage.SearchOptions]\r\nSearchPathCount=2\r\n"
                                       "SearchPath0=Z:\\media\\Common_space\\Music\\\r\nSearchPath1=Z:\\home\\me\\Аудио\\\r\n"
                                       "ExcludedPathCount=1\r\nExcludedPath0=Z:\\media\\Common_space\\Music\\Playlists\\\r\n"));
        LocalFilesSettings s;
        QString source;
        QVERIFY(LocalFilesSettings::fromAimp(s, &source));
        QCOMPARE(source, ini);
        QCOMPARE(s.folders, (QStringList{QStringLiteral("/media/Common_space/Music"), QStringLiteral("/home/me/Аудио")}));
        QCOMPARE(s.excluded, QStringList{QStringLiteral("/media/Common_space/Music/Playlists")});
        QVERIFY(s.watch);

        // First run takes them over; afterwards the saved ones are used.
        bool firstRun = false;
        QCOMPARE(LocalFilesSettings::load(&firstRun).folders, s.folders);
        QVERIFY(firstRun);
        LocalFilesSettings mine;
        mine.folders = {QStringLiteral("/srv/music")};
        mine.watch = false;
        mine.save();
        const LocalFilesSettings back = LocalFilesSettings::load(&firstRun);
        QVERIFY(!firstRun);
        QVERIFY(back == mine);
    }

    void scanGroupsByFolderAndExcludes()
    {
        QTemporaryDir music;
        writeFile(music.filePath(QStringLiteral("b 10.mp3")), "x");
        writeFile(music.filePath(QStringLiteral("b 2.mp3")), "x");
        writeFile(music.filePath(QStringLiteral("Artist/Album/01.flac")), "x");
        writeFile(music.filePath(QStringLiteral("Artist/cover.jpg")), "x");
        writeFile(music.filePath(QStringLiteral("Podcasts/ep1.mp3")), "x");
        writeFile(music.filePath(QStringLiteral("Image/album.flac")), "x");
        writeFile(music.filePath(QStringLiteral("Image/album.cue")),
                  "FILE \"album.flac\" WAVE\n TRACK 01 AUDIO\n  INDEX 01 00:00:00\n TRACK 02 AUDIO\n  INDEX 01 01:00:00\n");
        LocalFilesSettings s;
        s.folders = {music.path()};
        s.excluded = {music.filePath(QStringLiteral("Podcasts"))};
        QStringList dirs;
        const TrackList t = LocalLibrary::scan(s, {}, &dirs);
        QCOMPARE(relPaths(t, music.path()),
                 (QStringList{QStringLiteral("b 2.mp3"), QStringLiteral("b 10.mp3"), QStringLiteral("Artist/Album/01.flac"),
                              QStringLiteral("Image/album.flac"), QStringLiteral("Image/album.flac")}));
        QVERIFY(t[3].isCueTrack());
        QVERIFY(!dirs.contains(music.filePath(QStringLiteral("Podcasts"))));
        QVERIFY(dirs.contains(music.filePath(QStringLiteral("Artist/Album"))));
    }

    // Unchanged files keep their tags (no rereading), changed ones are reread.
    void rescanReusesKnownTracks()
    {
        QTemporaryDir music;
        writeFile(music.filePath(QStringLiteral("a.mp3")), "aaaa");
        writeFile(music.filePath(QStringLiteral("b.mp3")), "bbbb");
        LocalFilesSettings s;
        s.folders = {music.path()};
        TrackList known = LocalLibrary::scan(s, {});
        for (Track &t : known) {
            t.scanned = true;
            t.title = QStringLiteral("tagged");
            t.fileSize = 4;
        }
        writeFile(music.filePath(QStringLiteral("b.mp3")), "bigger now");   // changed
        writeFile(music.filePath(QStringLiteral("c.mp3")), "cccc");         // new
        const TrackList t = LocalLibrary::scan(s, known);
        QCOMPARE(t.size(), 3);
        QCOMPARE(t[0].uid, known[0].uid);
        QVERIFY(t[0].scanned);
        QCOMPARE(t[0].title, QStringLiteral("tagged"));
        QCOMPARE(t[1].uid, known[1].uid);   // same entry...
        QVERIFY(!t[1].scanned);              // ...tags read again
        QCOMPARE(t[1].fileSize, 10);         // marked changed
        QVERIFY(!t[2].scanned);
        QVERIFY(t[2].uid != known[0].uid && t[2].uid != known[1].uid);
    }

    // Two CUE sheets for one audio file: their tracks stay apart across rescans.
    void twoCueSheetsForOneFile()
    {
        QTemporaryDir music;
        writeFile(music.filePath(QStringLiteral("a.flac")), "flac");
        const QByteArray cue = "FILE \"a.flac\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n"
                               "  TRACK 02 AUDIO\r\n    INDEX 01 01:00:00\r\n";
        writeFile(music.filePath(QStringLiteral("a.cue")), cue);
        writeFile(music.filePath(QStringLiteral("b.cue")), cue);
        LocalFilesSettings s;
        s.folders = {music.path()};
        const TrackList first = LocalLibrary::scan(s, {});
        const TrackList second = LocalLibrary::scan(s, first);
        QCOMPARE(second.size(), 4);
        QSet<quint64> uids;
        for (int i = 0; i < second.size(); ++i) {
            uids.insert(second[i].uid);
            QCOMPARE(second[i].cueSheet, first[i].cueSheet);
            QCOMPARE(second[i].uid, first[i].uid);
        }
        QCOMPARE(uids.size(), 4);
    }

    // A music folder that is gone or empty (a drive not mounted) keeps its
    // tracks, marked missing, with their dates and tags.
    void unmountedFolderKeepsItsTracks()
    {
        QTemporaryDir music, other;
        writeFile(music.filePath(QStringLiteral("x/a.mp3")), "aaaa");
        writeFile(other.filePath(QStringLiteral("b.mp3")), "bbbb");
        const QString drive = music.filePath(QStringLiteral("x"));
        LocalFilesSettings s;
        s.folders = {drive, other.path()};
        TrackList known = LocalLibrary::scan(s, {});
        QCOMPARE(known.size(), 2);
        for (Track &t : known) {
            t.scanned = true;
            t.title = QStringLiteral("tagged");
        }
        QVERIFY(QFile::remove(music.filePath(QStringLiteral("x/a.mp3"))));   // the mount point stays, empty
        TrackList t = LocalLibrary::scan(s, known);
        QCOMPARE(t.size(), 2);
        const auto a = std::find_if(t.cbegin(), t.cend(), [&](const Track &x) { return x.path.startsWith(drive); });
        QVERIFY(a != t.cend());
        QVERIFY(a->missing);
        QCOMPARE(a->uid, known[0].uid);
        QCOMPARE(a->addedMs, known[0].addedMs);
        QCOMPARE(a->title, QStringLiteral("tagged"));

        QVERIFY(QDir(drive).removeRecursively());   // no mount point at all
        t = LocalLibrary::scan(s, known);
        QCOMPARE(t.size(), 2);

        writeFile(music.filePath(QStringLiteral("x/a.mp3")), "aaaa");   // mounted again
        t = LocalLibrary::scan(s, t);
        QCOMPARE(t.size(), 2);
        for (const Track &x : t)
            QVERIFY(!x.missing);
    }

    // The list is replaced when files come or go, and also when a file
    // changed in place (same entries, new tags to read).
    void sameListOnlyWhenNothingChanged()
    {
        QTemporaryDir music;
        writeFile(music.filePath(QStringLiteral("a.mp3")), "aaaa");
        writeFile(music.filePath(QStringLiteral("b.mp3")), "bbbb");
        LocalFilesSettings s;
        s.folders = {music.path()};
        TrackList shown = LocalLibrary::scan(s, {});
        QVERIFY(LocalLibrary::sameList(shown, LocalLibrary::scan(s, shown)));   // tags not read yet: kept
        for (Track &t : shown) {
            t.scanned = true;
            t.fileSize = 4;
        }
        QVERIFY(LocalLibrary::sameList(shown, LocalLibrary::scan(s, shown)));
        writeFile(music.filePath(QStringLiteral("b.mp3")), "retagged");
        QVERIFY(!LocalLibrary::sameList(shown, LocalLibrary::scan(s, shown)));
    }

    // A rescan asked for while one runs follows it, from the list as it is
    // after the first result (the owner rescans on changedOnDisk).
    void rescanDuringScanFollowsIt()
    {
        QTemporaryDir music;
        writeFile(music.filePath(QStringLiteral("a.mp3")), "x");
        LocalFiles lf;
        LocalFilesSettings s;
        s.folders = {music.path()};
        s.watch = false;
        lf.setSettings(s);
        QSignalSpy scanned(&lf, &LocalFiles::scanned);
        QSignalSpy again(&lf, &LocalFiles::changedOnDisk);
        QSignalSpy scanning(&lf, &LocalFiles::scanningChanged);
        lf.rescan({});
        lf.rescan({});   // still running: follows it
        QVERIFY(scanned.wait(5000));
        QCOMPARE(again.count(), 1);
        QTRY_VERIFY(!scanning.isEmpty() && !scanning.last().at(0).toBool());
    }

    // A scan comes back in the order asked for, sorted on its own thread,
    // and says which sort that was.
    void scanComesBackSorted()
    {
        QTemporaryDir music;
        for (const char *n : {"b/2.mp3", "a/1.mp3", "c/3.mp3"})
            writeFile(music.filePath(QString::fromLatin1(n)), "x");
        LocalFiles lf;
        LocalFilesSettings s;
        s.folders = {music.path()};
        s.watch = false;
        lf.setSettings(s);
        QSignalSpy scanned(&lf, &LocalFiles::scanned);
        lf.setSort(true, TrackSort::Key::Path, Qt::DescendingOrder);
        lf.rescan({});
        QVERIFY(scanned.wait(5000));
        const TrackList t = scanned.last().at(0).value<TrackList>();
        QCOMPARE(relPaths(t, music.path()),
                 (QStringList{QStringLiteral("c/3.mp3"), QStringLiteral("b/2.mp3"), QStringLiteral("a/1.mp3")}));
        QCOMPARE(scanned.last().at(1).toInt(), lf.sortGeneration());
    }

    // Dates: added = first seen (kept), modified = mtime; retagging that keeps
    // the size is still noticed through the mtime.
    void datesAddedAndModified()
    {
        QTemporaryDir music;
        const QString a = music.filePath(QStringLiteral("a.mp3"));
        writeFile(a, "aaaa");
        const QDateTime old = QDateTime::fromString(QStringLiteral("2021-01-31T09:39:16Z"), Qt::ISODate);
        {
            QFile f(a);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(old, QFileDevice::FileModificationTime));   // as copied with its mtime
        }
        LocalFilesSettings s;
        s.folders = {music.path()};
        TrackList known = LocalLibrary::scan(s, {});
        QCOMPARE(known.size(), 1);
        QCOMPARE(known[0].modifiedMs, old.toMSecsSinceEpoch());
        // Added when it landed here, not when it was made.
        QVERIFY(qAbs(known[0].addedMs - QDateTime::currentMSecsSinceEpoch()) < 60000);
        known[0].scanned = true;
        known[0].fileSize = 4;
        known[0].addedMs = 1000;   // first seen long ago

        TrackList again = LocalLibrary::scan(s, known);
        QVERIFY(again[0].scanned);   // unchanged
        QCOMPARE(again[0].addedMs, qint64(1000));

        writeFile(a, "AAAA");   // new tags, same size
        {
            QFile f(a);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(old.addSecs(60), QFileDevice::FileModificationTime));
        }
        again = LocalLibrary::scan(s, known);
        QVERIFY(!again[0].scanned);                  // read again
        QCOMPARE(again[0].uid, known[0].uid);
        QCOMPARE(again[0].addedMs, qint64(1000));    // still added back then
        QCOMPARE(again[0].modifiedMs, old.addSecs(60).toMSecsSinceEpoch());
    }

    // "Newest first" is a timeline down to the second (13:11, 13:10, 13:10,
    // never 13:10 after 13:11); equal times keep album order.
    void sortByDateAdded()
    {
        auto track = [](const QString &path, int no, qint64 addedSec) {
            Track t = Track::fromPath(path);
            t.album = QFileInfo(path).dir().dirName();
            t.trackNo = no;
            t.addedMs = addedSec * 1000;
            t.modifiedMs = (1000 - addedSec) * 1000;
            return t;
        };
        TrackList tracks = {
            track(QStringLiteral("/m/Old/01.mp3"), 1, 600),
            track(QStringLiteral("/m/Old/02.mp3"), 2, 600),        // same second as 01
            track(QStringLiteral("/m/New/02.mp3"), 2, 3010),       // 0:50:10
            track(QStringLiteral("/m/New/01.mp3"), 1, 3065),       // 0:51:05
            track(QStringLiteral("/m/New/03.mp3"), 3, 2995),       // 0:49:55
            track(QStringLiteral("/m/Old/bonus.mp3"), 9, 4200),
        };
        auto names = [](const TrackList &list) {
            QStringList out;
            for (const Track &t : list)
                out << QFileInfo(t.path).dir().dirName() + QLatin1Char('/') + t.fileName();
            return out;
        };
        TrackSort::sort(tracks, TrackSort::Key::Added, Qt::DescendingOrder);
        QCOMPARE(names(tracks), (QStringList{"Old/bonus.mp3", "New/01.mp3", "New/02.mp3", "New/03.mp3",
                                             "Old/01.mp3", "Old/02.mp3"}));
        for (int i = 1; i < tracks.size(); ++i)
            QVERIFY(tracks[i - 1].addedMs >= tracks[i].addedMs);
        TrackSort::sort(tracks, TrackSort::Key::Added, Qt::AscendingOrder);
        QCOMPARE(names(tracks), (QStringList{"Old/01.mp3", "Old/02.mp3", "New/03.mp3", "New/02.mp3",
                                             "New/01.mp3", "Old/bonus.mp3"}));
        TrackSort::sort(tracks, TrackSort::Key::Modified, Qt::DescendingOrder);
        QCOMPARE(names(tracks).first(), QStringLiteral("Old/01.mp3"));
        QCOMPARE(names(tracks).last(), QStringLiteral("Old/bonus.mp3"));
    }

    // Moving or renaming inside the library is not "adding": same date, tags kept.
    void movedFileKeepsDateAndTags()
    {
        QTemporaryDir music;
        writeFile(music.filePath(QStringLiteral("Inbox/song.mp3")), "same bytes");
        writeFile(music.filePath(QStringLiteral("Inbox/other.mp3")), "other one!");   // same size, other mtime
        {
            QFile f(music.filePath(QStringLiteral("Inbox/other.mp3")));
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(QDateTime::currentDateTime().addDays(-3), QFileDevice::FileModificationTime));
        }
        LocalFilesSettings s;
        s.folders = {music.path()};
        TrackList known = LocalLibrary::scan(s, {});
        QCOMPARE(known.size(), 2);
        for (Track &t : known) {
            t.scanned = true;
            t.fileSize = QFileInfo(t.path).size();
            t.title = QStringLiteral("tagged ") + t.fileName();
            t.addedMs = 5000;   // long ago
            t.missing = t.fileName() == QLatin1String("song.mp3");   // failed to play after it was moved
        }
        QDir().mkpath(music.filePath(QStringLiteral("Artist/Album")));
        QVERIFY(QFile::rename(music.filePath(QStringLiteral("Inbox/song.mp3")),
                              music.filePath(QStringLiteral("Artist/Album/01 Song.mp3"))));

        const TrackList t = LocalLibrary::scan(s, known);
        QCOMPARE(t.size(), 2);
        const Track &moved = t[0].fileName() == QLatin1String("01 Song.mp3") ? t[0] : t[1];
        QCOMPARE(moved.fileName(), QStringLiteral("01 Song.mp3"));
        QCOMPARE(moved.addedMs, qint64(5000));
        QCOMPARE(moved.title, QStringLiteral("tagged song.mp3"));   // not read again
        QVERIFY(moved.scanned);
        QVERIFY(!moved.missing);   // found at its new place

        // A really new file is new, even with a size seen before.
        writeFile(music.filePath(QStringLiteral("Inbox/new.mp3")), "same bytes");
        const TrackList t2 = LocalLibrary::scan(s, t);
        for (const Track &x : t2) {
            if (x.fileName() == QLatin1String("new.mp3"))
                QVERIFY(x.addedMs > 5000 && !x.scanned);
        }
    }

    // A file renamed in place keeps its entry and its row (e.g. sorted by
    // date added); the list must still take the new path.
    void sameListNoticesRename()
    {
        QTemporaryDir music;
        writeFile(music.filePath(QStringLiteral("a.mp3")), "aaaa");
        writeFile(music.filePath(QStringLiteral("b.mp3")), "bbbb!");
        LocalFilesSettings s;
        s.folders = {music.path()};
        TrackList shown = LocalLibrary::scan(s, {});
        for (Track &t : shown) {
            t.scanned = true;
            t.fileSize = QFileInfo(t.path).size();
        }
        QVERIFY(LocalLibrary::sameList(shown, LocalLibrary::scan(s, shown)));
        QVERIFY(QFile::rename(music.filePath(QStringLiteral("b.mp3")), music.filePath(QStringLiteral("c.mp3"))));
        const TrackList after = LocalLibrary::scan(s, shown);
        QCOMPARE(after[1].uid, shown[1].uid);   // same entry, same row
        QVERIFY(!LocalLibrary::sameList(shown, after));
    }

    void followsChangesOnDisk()
    {
        QTemporaryDir music;
        writeFile(music.filePath(QStringLiteral("Album/a.mp3")), "x");
        LocalFiles lf;
        LocalFilesSettings s;
        s.folders = {music.path()};
        s.watch = true;
        lf.setSettings(s);

        QSignalSpy scanned(&lf, &LocalFiles::scanned);
        QSignalSpy changed(&lf, &LocalFiles::changedOnDisk);
        lf.rescan({});
        QVERIFY(scanned.wait(5000));
        QCOMPARE(scanned.takeFirst().at(0).value<TrackList>().size(), 1);

        writeFile(music.filePath(QStringLiteral("Album/b.mp3")), "x");   // a new file in a subfolder
        QVERIFY(changed.wait(6000));
        lf.rescan({});
        QVERIFY(scanned.wait(5000));
        QCOMPARE(scanned.takeFirst().at(0).value<TrackList>().size(), 2);

        const TrackList saved = LocalLibrary::scan(s, {});
        lf.saveCache(saved);
        const TrackList loaded = lf.loadCache();
        QCOMPARE(loaded.size(), 2);
        QCOMPARE(loaded[0].addedMs, saved[0].addedMs);   // dates survive restarts
        QCOMPARE(loaded[0].modifiedMs, saved[0].modifiedMs);
        QVERIFY(loaded[0].addedMs > 0);
    }
};

QTEST_GUILESS_MAIN(TestLocalFiles)
#include "test_localfiles.moc"
