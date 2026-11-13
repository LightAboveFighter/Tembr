#include "migration/AimpProfiles.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

void writeUtf16(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("\xFF\xFE", 2);
    f.write(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
}

QString aimpPlaylist(const QString &name, const QString &id, const QStringList &paths, int cursor = -1)
{
    QString s = QStringLiteral("#-----SUMMARY-----#\r\nID=%1\r\nName=%2\r\nContentFiles=%3\r\nPlaybackCursor=%4\r\n\r\n"
                               "#-----SETTINGS-----#\r\nFlags=2047\r\n\r\n#-----CONTENT-----#\r\n")
                    .arg(id, name)
                    .arg(paths.size())
                    .arg(cursor);
    for (const QString &p : paths)
        s += p + QStringLiteral("|Title|Artist|||||||||320|2|44100|1000|10|0|1|0||||0||MP3||\r\n");
    return s;
}

} // namespace

class TestMigration : public QObject
{
    Q_OBJECT

private slots:
    void findsWineAndBottlesProfiles()
    {
        QTemporaryDir home;
        const QString wine = home.filePath(QStringLiteral(".wine/drive_c/users/me/AppData/Roaming/AIMP"));
        QDir().mkpath(wine + QStringLiteral("/PLS"));
        // "Application Data" is a link to AppData/Roaming in Wine: must not be listed twice.
        QVERIFY(QFile::link(home.filePath(QStringLiteral(".wine/drive_c/users/me/AppData/Roaming")),
                            home.filePath(QStringLiteral(".wine/drive_c/users/me/Application Data"))));
        QDir().mkpath(home.filePath(QStringLiteral(".local/share/bottles/bottles/Music/drive_c/users/me/AppData/Roaming/AIMP/PLS")));
        QDir().mkpath(home.filePath(QStringLiteral(".wine/drive_c/Program Files/AIMP5/Profile/PLS")));
        QDir().mkpath(home.filePath(QStringLiteral(".local/share/bottles/bottles/Empty/drive_c/users/me")));

        qunsetenv("WINEPREFIX");
        const auto profiles = Migration::findAimpProfiles(home.path());
        QStringList labels;
        for (const auto &p : profiles)
            labels << p.label;
        QCOMPARE(profiles.size(), 3);
        QVERIFY(labels.contains(QStringLiteral("Wine: ~/.wine")));
        QVERIFY(labels.contains(QStringLiteral("Wine: ~/.wine (portable)")));
        QVERIFY(labels.contains(QStringLiteral("Bottles: ~/.local/share/bottles/bottles/Music")));
    }

    // AIMP.ini lists the open tabs; stale copies come last and are marked closed.
    void sessionFollowsAimpTabs()
    {
        QTemporaryDir dir;
        const Migration::AimpProfile profile{QStringLiteral("test"), dir.path()};
        writeUtf16(profile.plsDir() + QStringLiteral("/SD.aimppl4"),
                   aimpPlaylist(QStringLiteral("SD"), QStringLiteral("{OLD}"), {QStringLiteral("Z:\\a.mp3")}));
        writeUtf16(profile.plsDir() + QStringLiteral("/SD (2).aimppl4"),
                   aimpPlaylist(QStringLiteral("SD"), QStringLiteral("{sd-new}"),
                                {QStringLiteral("Z:\\a.mp3"), QStringLiteral("Z:\\BatMetal.mp3")}, 1));
        writeUtf16(profile.plsDir() + QStringLiteral("/ACDC.aimppl4"),
                   aimpPlaylist(QStringLiteral("ACDC"), QStringLiteral("{ACDC}"), {QStringLiteral("Z:\\b.mp3")}));
        writeUtf16(profile.iniPath(),
                   QStringLiteral("[System]\r\nX=1\r\n\r\n[Playlist.Manager]\r\nActiveStorage={ACDC}\r\n"
                                  "PlayingStorage={SD-NEW}\r\n\r\n[Playlist.Storage]\r\nCount=2\r\n"
                                  "Item1=SD (2).aimppl4\r\nItem2=ACDC.aimppl4\r\n"));

        const auto s = Migration::readAimpSession(profile);
        QCOMPARE(s.playlists.size(), 3);
        QCOMPARE(QFileInfo(s.playlists[0].file).fileName(), QStringLiteral("SD (2).aimppl4"));
        QCOMPARE(s.playlists[0].trackCount, 2);
        QCOMPARE(s.playlists[0].playbackCursor, 1);
        QVERIFY(s.playlists[0].open);
        QCOMPARE(s.playlists[1].name, QStringLiteral("ACDC"));
        QVERIFY(!s.playlists[2].open);
        QCOMPARE(QFileInfo(s.playlists[2].file).fileName(), QStringLiteral("SD.aimppl4"));
        QCOMPARE(s.activeIndex, 1);
        QCOMPARE(s.playingIndex, 0);

        PlaylistData data;
        QVERIFY(Migration::loadAimpPlaylist(s.playlists[0], data));
        QCOMPARE(data.name, QStringLiteral("SD"));
        QCOMPARE(data.tracks.size(), 2);
    }

    void withoutIniNewestOfEachNameIsOpen()
    {
        QTemporaryDir dir;
        const Migration::AimpProfile profile{QStringLiteral("test"), dir.path()};
        const QString older = profile.plsDir() + QStringLiteral("/SD.aimppl4");
        const QString newer = profile.plsDir() + QStringLiteral("/SD (2).aimppl4");
        // Name order puts "SD (2)" first; make it the *older* one so the test
        // proves that modification time decides.
        writeUtf16(newer, aimpPlaylist(QStringLiteral("SD"), QStringLiteral("{2}"), {}));
        writeUtf16(older, aimpPlaylist(QStringLiteral("SD"), QStringLiteral("{1}"), {}));
        QFile f(newer);
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.setFileTime(QDateTime::currentDateTime().addDays(-10), QFileDevice::FileModificationTime));
        f.close();

        const auto s = Migration::readAimpSession(profile);
        QCOMPARE(s.playlists.size(), 2);
        QCOMPARE(QFileInfo(s.playlists[0].file).fileName(), QStringLiteral("SD.aimppl4"));
        QVERIFY(s.playlists[0].open);
        QVERIFY(!s.playlists[1].open);
    }
};

QTEST_GUILESS_MAIN(TestMigration)
#include "test_migration.moc"
