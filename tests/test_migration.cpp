#include "migration/AimpProfiles.h"
#include "migration/PhoneImport.h"
#include "migration/PhoneSources.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QSet>
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


// A fake `adb` that runs the commands on a local copy of the phone below
// `root` (sdcard/, storage/<id>/) and logs each command to root/calls.log.
void installFakeAdb(const QString &root)
{
    const QString adb = root + QStringLiteral("/fake-adb");
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
              "echo \"$cmd\" >> \"$ROOT/calls.log\"\n"
              "map() { printf '%s' \"$1\" | sed \"s#/sdcard#$ROOT/sdcard#g; s#/storage/#$ROOT/storage/#g; s#^ls /storage\\$#ls $ROOT/storage#\"; }\n"
              "case \"$cmd\" in\n"
              "  shell) sh -c \"$(map \"$1\")\" | sed \"s#$ROOT##g\" ;;\n"
              "  exec-out) sh -c \"$(map \"$1\")\" ;;\n"
              "  pull) cp \"$(map \"$1\")\" \"$2\" ;;\n"
              "esac\n"
              "\n");
    QFile(adb).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("TEMBR_ADB", adb.toUtf8());
    qputenv("FAKE_PHONE_ROOT", root.toUtf8());
}

Migration::PhoneSourcePtr adbPhoneOf(Migration::PhoneDiscovery *discovery)
{
    *discovery = Migration::discoverPhones();
    for (const auto &s : std::as_const(discovery->sources)) {
        if (s->kind() == Migration::PhoneSource::Kind::Adb)
            return s;
    }
    return nullptr;
}

// Commands the fake adb ran since the log was last cleared, by kind.
QMap<QString, int> adbCalls(const QString &root, bool clear = true)
{
    QMap<QString, int> out;
    QFile log(root + QStringLiteral("/calls.log"));
    if (log.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : log.readAll().split('\n')) {
            if (!line.isEmpty())
                ++out[QString::fromUtf8(line)];
        }
    }
    if (clear)
        QFile::remove(log.fileName());
    return out;
}

QByteArray contents(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A source that counts the files it copies.
struct CountingSource : Migration::PhoneSource
{
    Migration::PhoneSourcePtr inner;
    int copies = 0;
    explicit CountingSource(Migration::PhoneSourcePtr s) : inner(std::move(s)) {}
    Kind kind() const override { return inner->kind(); }
    QString label() const override { return inner->label(); }
    QList<Migration::PhoneStorage> storages() const override { return inner->storages(); }
    QList<Migration::PhonePlaylistFile> findPlaylists(QString *e) override { return inner->findPlaylists(e); }
    QByteArray readFile(int s, const QString &rel, QString *e) override { return inner->readFile(s, rel, e); }
    qint64 fileSize(int s, const QString &rel) override { return inner->fileSize(s, rel); }
    bool copyTo(int s, const QString &rel, const QString &local, QString *e) override
    {
        ++copies;
        return inner->copyTo(s, rel, local, e);
    }
};

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

    // Versions of a playlist stand together, newest first; groups in tab order.
    void oldCopiesGroupedByName()
    {
        QTemporaryDir dir;
        const Migration::AimpProfile profile{QStringLiteral("test"), dir.path()};
        auto add = [&](const QString &file, const QString &name, int daysAgo) {
            const QString path = profile.plsDir() + QLatin1Char('/') + file;
            writeUtf16(path, aimpPlaylist(name, QLatin1Char('{') + file + QLatin1Char('}'), {}));
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(QDateTime::currentDateTime().addDays(-daysAgo), QFileDevice::FileModificationTime));
        };
        add(QStringLiteral("Rock.aimppl4"), QStringLiteral("Rock"), 300);
        add(QStringLiteral("Rock (2).aimppl4"), QStringLiteral("Rock"), 10);     // open
        add(QStringLiteral("Rock (3).aimppl4"), QStringLiteral("Rock"), 100);
        add(QStringLiteral("Jazz.aimppl4"), QStringLiteral("Jazz"), 5);          // open, first tab
        add(QStringLiteral("Jazz (2).aimppl4"), QStringLiteral("Jazz"), 1);      // newer, but closed
        add(QStringLiteral("Blues.aimppl4"), QStringLiteral("Blues"), 50);       // closed only
        writeUtf16(profile.iniPath(), QStringLiteral("[Playlist.Storage]\r\nCount=2\r\nItem1=Jazz.aimppl4\r\n"
                                                     "Item2=Rock (2).aimppl4\r\n"));
        const auto s = Migration::readAimpSession(profile);
        QStringList files;
        for (int i : Migration::groupedByName(s))
            files << QFileInfo(s.playlists[i].file).fileName();
        QCOMPARE(files, (QStringList{QStringLiteral("Jazz (2).aimppl4"), QStringLiteral("Jazz.aimppl4"),
                                     QStringLiteral("Rock (2).aimppl4"), QStringLiteral("Rock (3).aimppl4"),
                                     QStringLiteral("Rock.aimppl4"), QStringLiteral("Blues.aimppl4")}));
        files.clear();
        for (int i : Migration::groupedByName(s, false))
            files << QFileInfo(s.playlists[i].file).fileName();
        QCOMPARE(files, (QStringList{QStringLiteral("Jazz.aimppl4"), QStringLiteral("Rock (2).aimppl4")}));
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

    // A copy cut short is not taken later for one made before.
    void interruptedCopyIsNotKept()
    {
        struct Flaky : Migration::PhoneSource
        {
            QString root;
            bool fail = true;
            Kind kind() const override { return Kind::Folder; }
            QString label() const override { return QStringLiteral("flaky"); }
            QList<Migration::PhoneStorage> storages() const override
            {
                Migration::PhoneStorage s;
                s.name = QStringLiteral("Internal");
                s.root = root;
                s.primary = true;
                return {s};
            }
            QList<Migration::PhonePlaylistFile> findPlaylists(QString *) override { return {}; }
            QByteArray readFile(int, const QString &rel, QString *) override
            {
                QFile f(root + QLatin1Char('/') + rel);
                return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
            }
            qint64 fileSize(int, const QString &rel) override
            {
                const QFileInfo fi(root + QLatin1Char('/') + rel);
                return fi.isFile() ? fi.size() : -1;
            }
            bool copyTo(int, const QString &rel, const QString &localPath, QString *error) override
            {
                QDir().mkpath(QFileInfo(localPath).absolutePath());
                QFile out(localPath);
                if (!out.open(QIODevice::WriteOnly))
                    return false;
                const QByteArray all = readFile(0, rel, nullptr);
                out.write(fail ? all.left(3) : all);   // like adb pull killed halfway
                if (fail && error)
                    *error = QStringLiteral("timeout");
                return !fail;
            }
        };

        QTemporaryDir phone, copyTo;
        writeFile(phone.filePath(QStringLiteral("Music/a.mp3")), "the whole song");
        writeFile(phone.filePath(QStringLiteral("Music/list.m3u")), "a.mp3\n");
        Flaky src;
        src.root = phone.path();
        Migration::PhoneImportOptions opt;
        opt.copyMissingTo = copyTo.path();
        const Migration::PhonePlaylistFile file{0, QStringLiteral("Music/list.m3u")};

        PlaylistData data;
        Migration::PhoneImportStats stats;
        QVERIFY(Migration::importPhonePlaylist(src, file, opt, data, stats));
        QCOMPARE(stats.copied, 0);
        QVERIFY(data.tracks[0].missing);
        QVERIFY(!QFileInfo::exists(copyTo.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(!QFileInfo::exists(copyTo.filePath(QStringLiteral("a.mp3.part"))));

        src.fail = false;   // next time it works
        data = {};
        stats = {};
        QVERIFY(Migration::importPhonePlaylist(src, file, opt, data, stats));
        QCOMPARE(stats.copied, 1);
        QFile copied(copyTo.filePath(QStringLiteral("a.mp3")));
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), QByteArray("the whole song"));
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

        // ".." never climbs out of the storage, whatever form the entry has.
        QCOMPARE(map(QStringLiteral("content://com.android.externalstorage.documents/document/primary%3A..%2F..%2Fetc%2Fx.mp3")).rel,
                 QStringLiteral("etc/x.mp3"));
        QCOMPARE(map(QStringLiteral("content://com.android.externalstorage.documents/document/primary%3AMusic%2F..%2F..%2Fa.mp3")).rel,
                 QStringLiteral("a.mp3"));
        QCOMPARE(map(QStringLiteral("/sdcard/../../etc/x.mp3")).rel, QStringLiteral("etc/x.mp3"));
        QCOMPARE(map(QStringLiteral("../../../../x.mp3")).rel, QStringLiteral("x.mp3"));
    }

    // A playlist written to escape: the entry points outside the phone folder,
    // and copying must neither read from nor write outside the given folders.
    void importStaysInsideFolders()
    {
        QTemporaryDir root;
        const QDir r(root.path());
        writeFile(r.filePath(QStringLiteral("src/loot/payload.mp3")), "outside the phone");
        writeFile(r.filePath(QStringLiteral("src/phone/Download/list.m3u8")),
                  "#EXTM3U\ncontent://com.android.externalstorage.documents/document/primary%3A..%2Floot%2Fpayload.mp3\n");
        QDir().mkpath(r.filePath(QStringLiteral("pc/Music")));

        const auto source = Migration::folderSource(r.filePath(QStringLiteral("src/phone")));
        Migration::PhoneImportOptions opt;
        opt.copyMissingTo = r.filePath(QStringLiteral("pc/Music"));
        PlaylistData data;
        Migration::PhoneImportStats stats;
        QVERIFY(Migration::importPhonePlaylist(*source, {0, QStringLiteral("Download/list.m3u8")}, opt, data, stats));
        QCOMPARE(stats.copied, 0);
        QCOMPARE(stats.missing, 1);
        QVERIFY(!QFileInfo::exists(r.filePath(QStringLiteral("pc/loot/payload.mp3"))));
        QVERIFY(!QFileInfo::exists(r.filePath(QStringLiteral("pc/Music/loot/payload.mp3"))));
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

        installFakeAdb(root.path());

        // (a real phone over MTP may be listed too; take the ADB one)
        Migration::PhoneDiscovery d;
        const Migration::PhoneSourcePtr phone = adbPhoneOf(&d);
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
        adbCalls(root.path());
        QVERIFY2(Migration::importPhonePlaylist(*phone, files[0], opt, data, stats, {}, &error), qPrintable(error));
        // Nothing in the way: the playlist read and one pull per track, no
        // questions about the files (each is a process started).
        const QMap<QString, int> calls = adbCalls(root.path());
        QCOMPARE(calls.value(QStringLiteral("exec-out")), 1);
        QCOMPARE(calls.value(QStringLiteral("pull")), 2);
        QCOMPARE(calls.value(QStringLiteral("shell")), 0);
        QCOMPARE(data.name, QStringLiteral("Best"));
        QCOMPARE(stats.copied, 2);
        QCOMPARE(data.tracks[0].path, copyTo.filePath(QStringLiteral("It's mine.mp3")));
        QCOMPARE(data.tracks[1].path, copyTo.filePath(QStringLiteral("sd.mp3")));
        QFile sd(data.tracks[1].path);
        QVERIFY(sd.open(QIODevice::ReadOnly));
        QCOMPARE(sd.readAll(), QByteArray("sd card song"));
        qunsetenv("TEMBR_ADB");
    }

    // Music/x.mp3 and x.mp3 at the storage root: the top "Music/" is dropped
    // when copying, so both would be <folder>/x.mp3. Each gets its own file,
    // also when the two have the same size; a second import copies nothing
    // and gives the same paths.
    void phoneFilesNeverShareACopy_data()
    {
        QTest::addColumn<QByteArray>("inMusic");
        QTest::addColumn<QByteArray>("atRoot");
        QTest::newRow("different sizes") << QByteArray("from Music") << QByteArray("from the root folder");
        QTest::newRow("same size") << QByteArray("AAAA") << QByteArray("BBBB");
    }
    void phoneFilesNeverShareACopy()
    {
        QFETCH(QByteArray, inMusic);
        QFETCH(QByteArray, atRoot);
        QTemporaryDir phone, copyTo;
        writeFile(phone.filePath(QStringLiteral("Music/x.mp3")), inMusic);
        writeFile(phone.filePath(QStringLiteral("x.mp3")), atRoot);
        writeFile(phone.filePath(QStringLiteral("list.m3u8")), "#EXTM3U\nMusic/x.mp3\nx.mp3\nMusic/x.mp3\n");
        CountingSource src(Migration::folderSource(phone.path()));
        Migration::PhoneImportOptions opt;
        opt.copyMissingTo = copyTo.path();

        QStringList first;
        for (int run = 0; run < 2; ++run) {
            PlaylistData data;
            Migration::PhoneImportStats stats;
            QVERIFY(Migration::importPhonePlaylist(src, {0, QStringLiteral("list.m3u8")}, opt, data, stats));
            QCOMPARE(data.tracks.size(), 3);
            QCOMPARE(stats.copied, 3);
            QCOMPARE(contents(data.tracks[0].path), inMusic);
            QCOMPARE(contents(data.tracks[1].path), atRoot);
            QCOMPARE(data.tracks[2].path, data.tracks[0].path);   // the same phone file again
            QStringList paths;
            for (const Track &t : std::as_const(data.tracks))
                paths << t.path;
            if (run == 0) {
                QCOMPARE(src.copies, 2);
                QCOMPARE(paths[0], copyTo.filePath(QStringLiteral("x.mp3")));
                QCOMPARE(paths[1], copyTo.filePath(QStringLiteral("x (2).mp3")));
                first = paths;
            } else {
                QCOMPARE(src.copies, 2);   // nothing copied again
                QCOMPARE(paths, first);
            }
        }
    }

    // The same path on two storages (internal and SD card), from two
    // playlists of one import: two files.
    void samePathOnTwoStorages()
    {
        QTemporaryDir gvfs, copyTo;
        const QString dev = gvfs.filePath(QStringLiteral("mtp:host=Phone"));
        const QString internal = dev + QStringLiteral("/Internal shared storage");
        const QString sd = dev + QStringLiteral("/SD card");
        writeFile(internal + QStringLiteral("/Music/a.mp3"), "internal");
        writeFile(internal + QStringLiteral("/Music/list.m3u"), "a.mp3\n");
        writeFile(sd + QStringLiteral("/Music/a.mp3"), "on the card");
        writeFile(sd + QStringLiteral("/Music/list.m3u"), "a.mp3\n");
        const auto sources = Migration::mtpSources(gvfs.path());
        QCOMPARE(sources.size(), 1);
        const auto files = sources[0]->findPlaylists();
        QCOMPARE(files.size(), 2);

        Migration::PhoneCopyClaims claims;
        Migration::PhoneImportOptions opt;
        opt.copyMissingTo = copyTo.path();
        opt.claims = &claims;
        QSet<QByteArray> got;
        QSet<QString> paths;
        for (const auto &f : files) {
            PlaylistData data;
            Migration::PhoneImportStats stats;
            QVERIFY(Migration::importPhonePlaylist(*sources[0], f, opt, data, stats));
            QCOMPARE(stats.copied, 1);
            got.insert(contents(data.tracks[0].path));
            paths.insert(data.tracks[0].path);
        }
        QCOMPARE(paths.size(), 2);
        QCOMPARE(got, (QSet<QByteArray>{"internal", "on the card"}));
    }

    // A file of someone else's in the copy folder is neither taken nor changed.
    void foreignFileIsLeftAlone()
    {
        QTemporaryDir phone, copyTo;
        writeFile(phone.filePath(QStringLiteral("Music/x.mp3")), "the phone's song");
        writeFile(phone.filePath(QStringLiteral("list.m3u")), "Music/x.mp3\n");
        writeFile(copyTo.filePath(QStringLiteral("x.mp3")), "already here, not from the phone");
        const auto src = Migration::folderSource(phone.path());
        Migration::PhoneImportOptions opt;
        opt.copyMissingTo = copyTo.path();
        PlaylistData data;
        Migration::PhoneImportStats stats;
        QVERIFY(Migration::importPhonePlaylist(*src, {0, QStringLiteral("list.m3u")}, opt, data, stats));
        QCOMPARE(data.tracks[0].path, copyTo.filePath(QStringLiteral("Music/x.mp3")));
        QCOMPARE(contents(data.tracks[0].path), QByteArray("the phone's song"));
        QCOMPARE(contents(copyTo.filePath(QStringLiteral("x.mp3"))), QByteArray("already here, not from the phone"));
    }

    // Over ADB: the same collision, a file in the way whose size is asked
    // with one shell command, and a re-import that copies nothing.
    void adbCollision()
    {
        QTemporaryDir root, copyTo;
        writeFile(root.filePath(QStringLiteral("sdcard/Music/x.mp3")), "from Music");
        writeFile(root.filePath(QStringLiteral("sdcard/x.mp3")), "from the root folder");
        writeFile(root.filePath(QStringLiteral("sdcard/list.m3u")), "Music/x.mp3\nx.mp3\n");
        QDir().mkpath(root.filePath(QStringLiteral("storage/emulated")));
        writeFile(copyTo.filePath(QStringLiteral("x.mp3")), "someone else's");
        installFakeAdb(root.path());
        Migration::PhoneDiscovery d;
        const Migration::PhoneSourcePtr phone = adbPhoneOf(&d);
        QVERIFY(phone);
        QCOMPARE(phone->fileSize(0, QStringLiteral("x.mp3")), qint64(20));
        QCOMPARE(phone->fileSize(0, QStringLiteral("nothing.mp3")), qint64(-1));
        QCOMPARE(phone->fileSize(0, QStringLiteral("Music")), qint64(-1));   // a folder

        Migration::PhoneImportOptions opt;
        opt.copyMissingTo = copyTo.path();
        QStringList first;
        for (int run = 0; run < 2; ++run) {
            adbCalls(root.path());
            PlaylistData data;
            Migration::PhoneImportStats stats;
            QString error;
            QVERIFY2(Migration::importPhonePlaylist(*phone, {0, QStringLiteral("list.m3u")}, opt, data, stats, {}, &error),
                     qPrintable(error));
            QCOMPARE(stats.copied, 2);
            QCOMPARE(contents(data.tracks[0].path), QByteArray("from Music"));
            QCOMPARE(contents(data.tracks[1].path), QByteArray("from the root folder"));
            const QMap<QString, int> calls = adbCalls(root.path());
            if (run == 0) {
                QCOMPARE(data.tracks[0].path, copyTo.filePath(QStringLiteral("Music/x.mp3")));
                QCOMPARE(data.tracks[1].path, copyTo.filePath(QStringLiteral("x (2).mp3")));
                QCOMPARE(calls.value(QStringLiteral("pull")), 2);
                first = {data.tracks[0].path, data.tracks[1].path};
            } else {
                QCOMPARE(calls.value(QStringLiteral("pull")), 0);
                QCOMPARE((QStringList{data.tracks[0].path, data.tracks[1].path}), first);
            }
        }
        QCOMPARE(contents(copyTo.filePath(QStringLiteral("x.mp3"))), QByteArray("someone else's"));
        qunsetenv("TEMBR_ADB");
    }
};

QTEST_GUILESS_MAIN(TestMigration)
#include "test_migration.moc"
