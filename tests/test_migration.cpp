#include "migration/AimpProfiles.h"
#include "migration/PhoneImport.h"
#include "migration/PhoneSources.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

namespace {

void writeUtf16(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("\xFF\xFE", 2);
    f.write(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
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

    void aimpLibraryFolders()
    {
        QTemporaryDir dir, emptyPrefix;
        qputenv("WINEPREFIX", emptyPrefix.path().toUtf8());   // no dosdevices: Z: is "/"
        const Migration::AimpProfile profile{QStringLiteral("test"), dir.path()};
        writeUtf16(profile.iniPath(),
                   QStringLiteral("[MusicLibrary.TAIMPMLLocalDataStorage.SearchOptions]\r\nSearchPathCount=2\r\n"
                                  "SearchPath0=Z:\\media\\Music\\\r\nSearchPath1=Z:\\home\\me\\Аудио\\\r\n"
                                  "ExcludedPathCount=1\r\nExcludedPath0=Z:\\media\\Music\\Podcasts\\\r\n"));
        QStringList excluded;
        QCOMPARE(Migration::aimpLibraryFolders(profile, &excluded),
                 (QStringList{QStringLiteral("/media/Music"), QStringLiteral("/home/me/Аудио")}));
        QCOMPARE(excluded, QStringList{QStringLiteral("/media/Music/Podcasts")});
        qunsetenv("WINEPREFIX");
    }

    // Another phone: storage names we have no pattern for (Turkish; the card sorts
    // first), no Music folder, and a track
    // the playlist says is in internal storage that actually is on the card.
    void otherPhoneLayout()
    {
        QTemporaryDir gvfs, copyTo;
        const QString dev = gvfs.filePath(QStringLiteral("mtp:host=SAMSUNG_Android_R58M"));
        const QString sd = dev + QStringLiteral("/Bellek kartı");
        const QString internal = dev + QStringLiteral("/Dahili paylaşılan depolama");
        for (const char *d : {"Alarms", "Ringtones", "Download", "Android"})
            QDir().mkpath(internal + QLatin1Char('/') + QLatin1String(d));
        QDir().mkpath(sd + QStringLiteral("/Android"));
        writeFile(sd + QStringLiteral("/Lieder/Band - Song.mp3"), "on the card");
        // No "Music" folder anywhere; playlist in Documents.
        writeFile(internal + QStringLiteral("/Documents/AIMP/Mix.m3u"),
                  "#EXTM3U\n/storage/emulated/0/Lieder/Band - Song.mp3\n");

        const auto sources = Migration::mtpSources(gvfs.path());
        QCOMPARE(sources.size(), 1);
        const auto storages = sources[0]->storages();
        const auto primary = std::find_if(storages.cbegin(), storages.cend(), [](const auto &s) { return s.primary; });
        QCOMPARE(primary->name, QStringLiteral("Dahili paylaşılan depolama"));   // by its system folders

        const auto files = sources[0]->findPlaylists();
        QCOMPARE(files.size(), 1);
        QCOMPARE(files[0].path, QStringLiteral("Documents/AIMP/Mix.m3u"));
        Migration::PhoneImportOptions opt;
        opt.copyMissingTo = copyTo.path();
        PlaylistData data;
        Migration::PhoneImportStats stats;
        QVERIFY(Migration::importPhonePlaylist(*sources[0], files[0], opt, data, stats));
        QCOMPARE(stats.copied, 1);
        QCOMPARE(data.tracks[0].path, copyTo.filePath(QStringLiteral("Lieder/Band - Song.mp3")));
    }

    void phoneEntries()
    {
        using Migration::PhoneRef;
        const QList<Migration::PhoneStorage> st = {
            {QStringLiteral("primary"), QStringLiteral("Internal"), QStringLiteral("/sdcard"), true},
            {QStringLiteral("1234-ABCD"), QStringLiteral("SD"), QStringLiteral("/storage/1234-ABCD"), false},
        };
        auto map = [&](const QString &e) { return Migration::mapPhoneEntry(e, st, 0, QStringLiteral("Music/Playlists ")); };

        PhoneRef r = map(QStringLiteral("/storage/emulated/0/Music/Кино - Звезда.mp3"));
        QCOMPARE(r.storage, 0);
        QCOMPARE(r.rel, QStringLiteral("Music/Кино - Звезда.mp3"));
        QCOMPARE(map(QStringLiteral("/sdcard/Music/a.mp3")).rel, QStringLiteral("Music/a.mp3"));
        r = map(QStringLiteral("/storage/1234-abcd/Music/b.mp3"));
        QCOMPARE(r.storage, 1);
        QCOMPARE(r.rel, QStringLiteral("Music/b.mp3"));
        r = map(QStringLiteral("../a.mp3"));                       // relative to "Music/Playlists "
        QCOMPARE(r.storage, 0);
        QCOMPARE(r.rel, QStringLiteral("Music/a.mp3"));
        r = map(QStringLiteral("content://com.android.externalstorage.documents/document/1234-ABCD%3AMusic%2Fc.mp3"));
        QCOMPARE(r.storage, 1);
        QCOMPARE(r.rel, QStringLiteral("Music/c.mp3"));
        QCOMPARE(map(QStringLiteral("content://media/external/audio/media/42")).kind, PhoneRef::Unmappable);
        QCOMPARE(map(QStringLiteral("https://radio.example/stream")).kind, PhoneRef::Url);
        QCOMPARE(map(QStringLiteral("file:///storage/emulated/0/Music/d%20e.mp3")).rel, QStringLiteral("Music/d e.mp3"));
    }

    // A phone (or its copy) as a folder: playlist in "Music/Playlists " (with the
    // trailing space seen on a real phone), absolute and relative entries.
    void importFromPhoneFolder()
    {
        QTemporaryDir phone, library, copyTo;
        writeFile(phone.filePath(QStringLiteral("Music/known.mp3")), "phone copy");
        writeFile(phone.filePath(QStringLiteral("Music/only-on-phone.mp3")), "PHONE-ONLY");
        writeFile(phone.filePath(QStringLiteral("Music/Playlists /Mix.m3u8")),
                  "#EXTM3U\n#EXTINF:10,A - Known\n/storage/emulated/0/Music/known.mp3\n"
                  "#EXTINF:20,B - Only\n../only-on-phone.mp3\n"
                  "/storage/emulated/0/Music/gone.mp3\nhttp://radio.example/live\n");
        writeFile(phone.filePath(QStringLiteral("Android/data/com.other/cache/hls.m3u8")), "#EXTM3U\n");
        writeFile(phone.filePath(QStringLiteral("Android/data/com.aimp.player/files/Exported.xspf")),
                  "<playlist xmlns=\"http://xspf.org/ns/0/\"><trackList><track><location>../../../../Music/known.mp3"
                  "</location></track></trackList></playlist>");
        writeFile(phone.filePath(QStringLiteral("DCIM/x.m3u")), "#EXTM3U\n");
        writeFile(library.filePath(QStringLiteral("Rock/known.mp3")), "pc copy");

        const auto source = Migration::folderSource(phone.path());
        QList<Migration::PhonePlaylistFile> files = source->findPlaylists();
        QStringList paths;
        for (const auto &f : files)
            paths << f.path;
        paths.sort();
        QCOMPARE(paths, (QStringList{QStringLiteral("Android/data/com.aimp.player/files/Exported.xspf"),
                                     QStringLiteral("Music/Playlists /Mix.m3u8")}));

        const Relocator::Index index = Relocator::build(library.path());
        Migration::PhoneImportOptions opt;
        opt.library = &index;
        opt.copyMissingTo = copyTo.path();
        const auto mix = std::find_if(files.cbegin(), files.cend(),
                                      [](const auto &f) { return f.path.endsWith(QLatin1String("Mix.m3u8")); });
        PlaylistData data;
        Migration::PhoneImportStats stats;
        int lastDone = -1;
        QVERIFY(Migration::importPhonePlaylist(*source, *mix, opt, data, stats,
                                               [&](int done, int, const QString &) { lastDone = done; return true; }));
        QCOMPARE(data.name, QStringLiteral("Mix"));
        QCOMPARE(data.tracks.size(), 4);
        QCOMPARE(data.tracks[0].path, library.filePath(QStringLiteral("Rock/known.mp3")));   // matched, not copied
        QCOMPARE(data.tracks[1].path, copyTo.filePath(QStringLiteral("only-on-phone.mp3")));   // no Music/Music
        QFile copied(data.tracks[1].path);
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), QByteArray("PHONE-ONLY"));
        QVERIFY(data.tracks[2].missing);
        QCOMPARE(data.tracks[2].path, QStringLiteral("/sdcard/Music/gone.mp3"));
        QCOMPARE(data.tracks[3].path, QStringLiteral("http://radio.example/live"));
        QCOMPARE(stats.matched, 1);
        QCOMPARE(stats.copied, 1);
        QCOMPARE(stats.missing, 1);
        QCOMPARE(stats.streams, 1);
        QCOMPARE(lastDone, 4);

        // Cancelling stops the import.
        PlaylistData cancelled;
        Migration::PhoneImportStats st2;
        QVERIFY(!Migration::importPhonePlaylist(*source, *mix, opt, cancelled, st2,
                                                [](int, int, const QString &) { return false; }));
    }

    void gvfsMtpMounts()
    {
        QTemporaryDir gvfs;
        QDir().mkpath(gvfs.filePath(QStringLiteral("mtp:host=Xiaomi_Redmi_Note_13_Pro_5G_4c311b75/Внутренний общий накопитель")));
        QDir().mkpath(gvfs.filePath(QStringLiteral("mtp:host=Locked_Phone_1")));
        QDir().mkpath(gvfs.filePath(QStringLiteral("smb-share:server=nas")));
        QStringList notes;
        const auto sources = Migration::mtpSources(gvfs.path(), &notes);
        QCOMPARE(sources.size(), 1);
        QCOMPARE(sources[0]->kind(), Migration::PhoneSource::Kind::Mtp);
        QVERIFY(sources[0]->label().startsWith(QStringLiteral("Xiaomi Redmi Note 13 Pro 5G")));
        QVERIFY(sources[0]->storages().first().primary);
        QCOMPARE(notes.size(), 1);
        QVERIFY(notes.first().startsWith(QStringLiteral("Locked Phone 1")));
    }

    // ADB through a fake `adb` that runs the commands on a local copy of the
    // phone: checks command lines and shell quoting, not just parsing.
    void adbPhone()
    {
        QTemporaryDir root, copyTo;
        writeFile(root.filePath(QStringLiteral("sdcard/Music/It's mine.mp3")), "song");
        writeFile(root.filePath(QStringLiteral("sdcard/Music/Playlists/Best.xspf")),
                  "<playlist xmlns=\"http://xspf.org/ns/0/\"><title>Best</title><trackList>"
                  "<track><location>../It's%20mine.mp3</location></track>"
                  "<track><location>/storage/1234-ABCD/Music/sd.mp3</location></track>"
                  "</trackList></playlist>");
        writeFile(root.filePath(QStringLiteral("sdcard/Android/data/com.google.chrome/x.m3u8")), "#EXTM3U\n");
        writeFile(root.filePath(QStringLiteral("storage/1234-ABCD/Music/sd.mp3")), "sd card song");
        QDir().mkpath(root.filePath(QStringLiteral("storage/emulated")));

        const QString adb = root.filePath(QStringLiteral("fake-adb"));
        // (no raw string: moc would read the "#!" line as a directive)
        writeFile(adb,
                  "#!/bin/sh\n"
                  "ROOT=\"$FAKE_PHONE_ROOT\"\n"
                  "if [ \"$1\" = \"devices\" ]; then\n"
                  "  printf 'List of devices attached\\nSER123         device usb:1-1 product:x model:Pixel_7 device:panther\\nBAD456  unauthorized usb:1-2\\n\\n'\n"
                  "  exit 0\n"
                  "fi\n"
                  "[ \"$1\" = \"-s\" ] && shift 2\n"
                  "cmd=\"$1\"; shift\n"
                  "map() { printf '%s' \"$1\" | sed \"s#/sdcard#$ROOT/sdcard#g; s#/storage/#$ROOT/storage/#g; s#^ls /storage\\$#ls $ROOT/storage#\"; }\n"
                  "case \"$cmd\" in\n"
                  "  shell) sh -c \"$(map \"$1\")\" | sed \"s#$ROOT##g\" ;;\n"
                  "  exec-out) sh -c \"$(map \"$1\")\" ;;\n"
                  "  pull) cp \"$(map \"$1\")\" \"$2\" ;;\n"
                  "esac\n"
                  "\n");
        QFile(adb).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("TEMBR_ADB", adb.toUtf8());
        qputenv("FAKE_PHONE_ROOT", root.path().toUtf8());

        // (a real phone over MTP may be listed too; take the ADB one)
        const Migration::PhoneDiscovery d = Migration::discoverPhones();
        Migration::PhoneSourcePtr phone;
        for (const auto &s : d.sources) {
            if (s->kind() == Migration::PhoneSource::Kind::Adb)
                phone = s;
        }
        QVERIFY(phone);
        QVERIFY(phone->label().startsWith(QStringLiteral("Pixel 7")));
        QVERIFY(std::any_of(d.notes.cbegin(), d.notes.cend(), [](const QString &n) { return n.startsWith(QStringLiteral("BAD456")); }));
        QCOMPARE(phone->storages().size(), 2);
        QCOMPARE(phone->storages()[1].key, QStringLiteral("1234-ABCD"));

        const auto files = phone->findPlaylists();
        QCOMPARE(files.size(), 1);   // Android/ skipped
        QCOMPARE(files[0].path, QStringLiteral("Music/Playlists/Best.xspf"));

        Migration::PhoneImportOptions opt;
        opt.copyMissingTo = copyTo.path();
        PlaylistData data;
        Migration::PhoneImportStats stats;
        QString error;
        QVERIFY2(Migration::importPhonePlaylist(*phone, files[0], opt, data, stats, {}, &error), qPrintable(error));
        QCOMPARE(data.name, QStringLiteral("Best"));
        QCOMPARE(stats.copied, 2);
        QCOMPARE(data.tracks[0].path, copyTo.filePath(QStringLiteral("It's mine.mp3")));
        QCOMPARE(data.tracks[1].path, copyTo.filePath(QStringLiteral("sd.mp3")));
        QFile sd(data.tracks[1].path);
        QVERIFY(sd.open(QIODevice::ReadOnly));
        QCOMPARE(sd.readAll(), QByteArray("sd card song"));
        qunsetenv("TEMBR_ADB");
    }
};

QTEST_GUILESS_MAIN(TestMigration)
#include "test_migration.moc"
