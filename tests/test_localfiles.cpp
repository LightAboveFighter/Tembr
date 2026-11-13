#include "core/LocalFiles.h"

#include <QDir>
#include <QFile>
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
        QVERIFY(!t[2].scanned);
        QVERIFY(t[2].uid != known[0].uid && t[2].uid != known[1].uid);
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

        lf.saveCache(LocalLibrary::scan(s, {}));
        QCOMPARE(lf.loadCache().size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestLocalFiles)
#include "test_localfiles.moc"
