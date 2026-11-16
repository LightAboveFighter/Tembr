#include "core/PathResolver.h"
#include "core/PlaylistIO.h"
#include "core/Relocator.h"
#include "core/TextDecode.h"

#include <QDir>

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class TestPlaylistIO : public QObject
{
    Q_OBJECT

    QTemporaryDir m_wine;   // fake, empty Wine prefix: tests must not see ~/.wine

private slots:
    void initTestCase()
    {
        QVERIFY(m_wine.isValid());
        qputenv("WINEPREFIX", m_wine.path().toUtf8());
    }

    void wineDrives()
    {
        QTemporaryDir prefix;
        QTemporaryDir music;
        QDir(prefix.path()).mkpath(QStringLiteral("dosdevices"));
        QVERIFY(QFile::link(QStringLiteral("/"), prefix.filePath(QStringLiteral("dosdevices/z:"))));
        QVERIFY(QFile::link(music.path(), prefix.filePath(QStringLiteral("dosdevices/d:"))));
        qputenv("WINEPREFIX", prefix.path().toUtf8());

        const PathResolver r(QStringLiteral("/pl"));
        QCOMPARE(r.resolve(QStringLiteral("Z:\\home\\me\\Кино.mp3")), QStringLiteral("/home/me/Кино.mp3"));
        QCOMPARE(r.resolve(QStringLiteral("Z:/home/me/a.mp3")), QStringLiteral("/home/me/a.mp3"));
        QCOMPARE(r.resolve(QStringLiteral("d:\\Rock\\a.mp3")), QDir(music.path()).canonicalPath() + QStringLiteral("/Rock/a.mp3"));
        QCOMPARE(r.resolve(QStringLiteral("Q:\\x.mp3")), QStringLiteral("Q:/x.mp3"));   // unknown drive: kept
        QCOMPARE(PathResolver::toWindows(QStringLiteral("/home/me/a b.mp3")), QStringLiteral("Z:\\home\\me\\a b.mp3"));

        qputenv("WINEPREFIX", m_wine.path().toUtf8());
    }

    void remapsForMovedMusic()
    {
        QTemporaryDir newRoot;
        QDir(newRoot.path()).mkpath(QStringLiteral("Music/Rock"));
        QFile f(newRoot.filePath(QStringLiteral("Music/Rock/song.mp3")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        // "find moved files": index the new folder, match by name, learn the folder move.
        Track t = Track::fromPath(QStringLiteral("/gone/Music/Rock/song.mp3"));
        const Relocator::Index idx = Relocator::build(newRoot.path());
        QCOMPARE(idx.files, 1);
        const QString found = Relocator::find(t, idx);
        QCOMPARE(found, f.fileName());
        const auto remaps = Relocator::deriveRemaps({{t.path, found}});
        QCOMPARE(remaps.size(), 1);
        QCOMPARE(remaps[0].first, QStringLiteral("/gone/Music/Rock"));

        // Next imports apply the learned move automatically.
        PathResolver::setRemaps(remaps);
        QCOMPARE(PathResolver(QStringLiteral("/")).resolve(QStringLiteral("/gone/Music/Rock/song.mp3")), f.fileName());
        PathResolver::setRemaps({});
    }

    void decodeUtf8()
    {
        QCOMPARE(decodeText(QStringLiteral("Кино — Группа крови").toUtf8()), QStringLiteral("Кино — Группа крови"));
    }

    void decodeCp1251Fallback()
    {
        // "Привет Ёж" in Windows-1251
        const QByteArray cp1251("\xCF\xF0\xE8\xE2\xE5\xF2 \xA8\xE6");
        QCOMPARE(decodeText(cp1251), QStringLiteral("Привет Ёж"));
    }

    void decodeUtf16Bom()
    {
        const QString s = QStringLiteral("Name=Мой плейлист");
        QByteArray bytes("\xFF\xFE", 2);
        bytes.append(reinterpret_cast<const char *>(s.utf16()), s.size() * 2);
        QCOMPARE(decodeText(bytes), s);
    }

    void m3u()
    {
        const QString text = QStringLiteral(
            "#EXTM3U\r\n#PLAYLIST:Rock\r\n#EXTINF:245,Queen - Love of My Life\r\nmusic/queen.mp3\r\n"
            "..\\other\\track.flac\r\nhttp://radio.example/stream\r\nC:\\Music\\win.mp3\r\n");
        QString name;
        const TrackList t = PlaylistIO::parseM3u(text, QStringLiteral("/home/u/lists"), &name);
        QCOMPARE(name, QStringLiteral("Rock"));
        QCOMPARE(t.size(), 4);
        QCOMPARE(t[0].path, QStringLiteral("/home/u/lists/music/queen.mp3"));
        QCOMPARE(t[0].artist, QStringLiteral("Queen"));
        QCOMPARE(t[0].title, QStringLiteral("Love of My Life"));
        QCOMPARE(t[0].durationMs, 245000);
        QCOMPARE(t[1].path, QStringLiteral("/home/u/other/track.flac"));
        QCOMPARE(t[2].path, QStringLiteral("http://radio.example/stream"));
        QVERIFY(t[2].isUrl());
        QCOMPARE(t[3].path, QStringLiteral("C:/Music/win.mp3"));
        QVERIFY(t[0].uid != t[1].uid);
    }

    void pls()
    {
        const QString text = QStringLiteral("[playlist]\nFile2=b.ogg\nFile1=/abs/a.mp3\nTitle1=A - Song\nLength1=61\n"
                                            "NumberOfEntries=2\nVersion=2\n");
        const TrackList t = PlaylistIO::parsePls(text, QStringLiteral("/base"));
        QCOMPARE(t.size(), 2);
        QCOMPARE(t[0].path, QStringLiteral("/abs/a.mp3"));
        QCOMPARE(t[0].title, QStringLiteral("Song"));
        QCOMPARE(t[0].durationMs, 61000);
        QCOMPARE(t[1].path, QStringLiteral("/base/b.ogg"));
    }

    void cue()
    {
        const QString text = QStringLiteral(
            "REM GENRE Rock\nREM DATE 1973\nPERFORMER \"Pink Floyd\"\nTITLE \"The Dark Side of the Moon\"\n"
            "FILE \"album.flac\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"Speak to Me\"\n    INDEX 01 00:00:00\n"
            "  TRACK 02 AUDIO\n    TITLE \"Breathe\"\n    PERFORMER \"PF\"\n    INDEX 00 01:06:00\n    INDEX 01 01:07:30\n"
            "  TRACK 03 AUDIO\n    INDEX 01 03:50:15\n");
        QString album;
        const TrackList t = PlaylistIO::parseCue(text, QStringLiteral("/m/dsotm/album.cue"), &album);
        QCOMPARE(album, QStringLiteral("The Dark Side of the Moon"));
        QCOMPARE(t.size(), 3);
        QCOMPARE(t[0].path, QStringLiteral("/m/dsotm/album.flac"));
        QCOMPARE(t[0].cueSheet, QStringLiteral("/m/dsotm/album.cue"));
        QCOMPARE(t[0].artist, QStringLiteral("Pink Floyd"));
        QCOMPARE(t[0].genre, QStringLiteral("Rock"));
        QCOMPARE(t[0].year, QStringLiteral("1973"));
        QCOMPARE(t[0].startMs, 0);
        QCOMPARE(t[0].endMs, 67400);                       // 1:07 + 30/75 s
        QCOMPARE(t[1].artist, QStringLiteral("PF"));
        QCOMPARE(t[1].startMs, 67400);
        QCOMPARE(t[1].endMs, 230200);                      // 3:50 + 15/75 s
        QCOMPARE(t[1].durationMs, 230200 - 67400);
        QCOMPARE(t[2].title, QStringLiteral("Track 03"));
        QCOMPARE(t[2].endMs, 0);                           // to the end of the file
        QCOMPARE(t[2].trackNo, 3);
    }

    // Many albums have "01 - Intro.mp3": the folders decide, never a guess.
    void relocateAmbiguousNames()
    {
        QTemporaryDir lib;
        auto make = [&](const QString &rel, int size) {
            const QString p = lib.filePath(rel);
            QDir().mkpath(QFileInfo(p).absolutePath());
            QFile f(p);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(size, 'x'));
        };
        make(QStringLiteral("A/Album1/01 - Intro.mp3"), 10);
        make(QStringLiteral("B/Album1/01 - Intro.mp3"), 20);
        make(QStringLiteral("B/Album2/01 - Intro.mp3"), 30);
        make(QStringLiteral("C/Bones.mp3"), 40);
        make(QStringLiteral("C/bones.mp3"), 50);
        make(QStringLiteral("D/unique.mp3"), 1);
        const Relocator::Index idx = Relocator::build(lib.path());
        QCOMPARE(idx.files, 6);
        auto find = [&](const QString &path, qint64 size = 0) {
            Track t = Track::fromPath(path);
            t.fileSize = size;
            const QString found = Relocator::find(t, idx);
            return found.isEmpty() ? QString() : found.mid(lib.path().size() + 1);
        };
        QCOMPARE(find(QStringLiteral("/old/B/Album1/01 - Intro.mp3")), QStringLiteral("B/Album1/01 - Intro.mp3"));
        QCOMPARE(find(QStringLiteral("/old/X/Album2/01 - Intro.mp3")), QStringLiteral("B/Album2/01 - Intro.mp3"));
        QCOMPARE(find(QStringLiteral("/old/Z/Album9/01 - Intro.mp3")), QString());   // ambiguous: left missing
        QCOMPARE(find(QStringLiteral("/old/Z/Album9/01 - Intro.mp3"), 10), QStringLiteral("A/Album1/01 - Intro.mp3"));
        QCOMPARE(find(QStringLiteral("/old/C/bones.mp3")), QStringLiteral("C/bones.mp3"));          // exact case
        QCOMPARE(find(QStringLiteral("/elsewhere/unique.mp3")), QStringLiteral("D/unique.mp3"));
        QCOMPARE(find(QStringLiteral("/old/none.mp3")), QString());
    }

    void aimppl4()
    {
        // Lines as written by AIMP 5 under Wine (fields shortened to the known ones).
        const QString text = QStringLiteral(
            "#-----SUMMARY-----#\r\nID={E5FE0A1E}\r\nName=Любимое\r\nNameIsAutoSet=0\r\n\r\n"
            "#-----SETTINGS-----#\r\nFlags=2047\r\nFormatMainLine=%IF(%Artist,%Artist - %Title,%Title)\r\n\r\n"
            "#-----CONTENT-----#\r\n-Z:\\home\\me\\Music\r\n"
            "Z:\\home\\me\\Music\\Кипелов - Я Свободен.mp3|Я Свободен|Кипелов|Альбом|Кипелов|Rock|1997|3|1|Комп||"
            "320|4098|44100|433659|17733673|0|1|3||||0||MP3|строка1\\nстрока2|\r\n"
            "Z:/home/me/Music/a.ogg|||||||||||192|2|48000|1000|5|0|1|4||||0||||\r\n"
            "'\x01\x02mimetypeapplication/vnd.oasis.opendocument.textPK\r\n"   // junk seen in a real file
            "E:\\Music\\b.mp3|B|||||0||||||0|0|0|255000|0|0|1|0||||0||||\r\n");
        QString name;
        const TrackList t = PlaylistIO::parseAimppl4(text, QStringLiteral("/pl"), &name);
        QCOMPARE(name, QStringLiteral("Любимое"));
        QCOMPARE(t.size(), 3);
        QCOMPARE(t[0].path, QStringLiteral("/home/me/Music/Кипелов - Я Свободен.mp3"));
        QCOMPARE(t[0].title, QStringLiteral("Я Свободен"));
        QCOMPARE(t[0].artist, QStringLiteral("Кипелов"));
        QCOMPARE(t[0].album, QStringLiteral("Альбом"));
        QCOMPARE(t[0].albumArtist, QStringLiteral("Кипелов"));
        QCOMPARE(t[0].genre, QStringLiteral("Rock"));
        QCOMPARE(t[0].year, QStringLiteral("1997"));
        QCOMPARE(t[0].trackNo, 3);
        QCOMPARE(t[0].discNo, 1);
        QCOMPARE(t[0].composer, QStringLiteral("Комп"));
        QCOMPARE(t[0].bitrateKbps, 320);
        QCOMPARE(t[0].channels, 2);
        QCOMPARE(t[0].sampleRate, 44100);
        QCOMPARE(t[0].durationMs, 433659);
        QCOMPARE(t[0].fileSize, 17733673);
        QCOMPARE(t[0].codec, QStringLiteral("MP3"));
        QCOMPARE(t[0].comment, QStringLiteral("строка1\nстрока2"));
        QVERIFY(t[0].scanned);
        QCOMPARE(t[1].path, QStringLiteral("/home/me/Music/a.ogg"));
        QCOMPARE(t[1].codec, QStringLiteral("OGG"));      // from the extension
        QCOMPARE(t[2].path, QStringLiteral("E:/Music/b.mp3"));   // drive unknown in this prefix
        QVERIFY(t[2].year.isEmpty());                     // "0" means no year
    }

    void aimppl4RoundTrip()
    {
        QTemporaryDir dir;
        Track a = Track::fromPath(QStringLiteral("/home/me/Music/x y.mp3"));
        a.title = QStringLiteral("Title|with pipe");
        a.artist = QStringLiteral("Ария");
        a.year = QStringLiteral("1987");
        a.trackNo = 5;
        a.durationMs = 200000;
        a.fileSize = 1234;
        a.bitrateKbps = 256;
        a.sampleRate = 44100;
        a.channels = 2;
        a.codec = QStringLiteral("mp3");
        a.comment = QStringLiteral("a\nb");
        Track b = Track::fromPath(QStringLiteral("/home/me/Other/y.flac"));
        b.durationMs = 1000;

        const QString file = dir.filePath(QStringLiteral("p.aimppl4"));
        QVERIFY(PlaylistIO::save(file, {QStringLiteral("Мой"), {a, b}}));

        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray raw = f.readAll();
        QVERIFY(raw.startsWith("\xFF\xFE"));
        const QString text = decodeText(raw);
        QVERIFY(text.contains(QStringLiteral("\r\n#-----CONTENT-----#\r\n-Z:\\home\\me\\Music\r\n")));
        QVERIFY(text.contains(QStringLiteral("\r\n-Z:\\home\\me\\Other\r\n")));
        QVERIFY(text.contains(QStringLiteral("ContentDuration=201000")));
        const QString line = text.section(QStringLiteral("\r\n"), 20, 20);
        QCOMPARE(line.count(QLatin1Char('|')), 26);   // 27 fields, as AIMP writes

        PlaylistData back;
        QVERIFY(PlaylistIO::load(file, back));
        QCOMPARE(back.name, QStringLiteral("Мой"));
        QCOMPARE(back.tracks.size(), 2);
        QCOMPARE(back.tracks[0].path, a.path);
        QCOMPARE(back.tracks[0].title, QStringLiteral("Title/with pipe"));
        QCOMPARE(back.tracks[0].artist, a.artist);
        QCOMPARE(back.tracks[0].trackNo, 5);
        QCOMPARE(back.tracks[0].comment, a.comment);
        QCOMPARE(back.tracks[0].codec, QStringLiteral("MP3"));
        QVERIFY(back.tracks[0].missing);              // the file doesn't exist
    }

    void xspfRelativePercentEncoded()
    {
        const QByteArray xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\">"
            "<title>Phone</title><trackList>"
            "<track><location>../Music/%D0%9A%D0%B8%D0%BD%D0%BE/My%20Song.mp3</location></track>"
            "<track><location>file:///storage/emulated/0/Music/a%20b.mp3</location></track>"
            "</trackList></playlist>";
        const TrackList t = PlaylistIO::parseXspf(xml, QStringLiteral("/phone/Playlists"));
        QCOMPARE(t.size(), 2);
        QCOMPARE(t[0].path, QStringLiteral("/phone/Music/Кино/My Song.mp3"));
        QCOMPARE(t[1].path, QStringLiteral("/storage/emulated/0/Music/a b.mp3"));
    }

    void customEntryResolver()
    {
        QStringList seen;
        const auto resolve = [&](const QString &e) {
            seen << e;
            return QStringLiteral("mapped:") + e;
        };
        QString name;
        const TrackList t = PlaylistIO::parse("#EXTM3U\n#PLAYLIST:P\nMusic/a.mp3\n/sdcard/b.mp3\n", QStringLiteral("M3U8"),
                                              resolve, &name);
        QCOMPARE(name, QStringLiteral("P"));
        QCOMPARE(seen, (QStringList{QStringLiteral("Music/a.mp3"), QStringLiteral("/sdcard/b.mp3")}));
        QCOMPARE(t[1].path, QStringLiteral("mapped:/sdcard/b.mp3"));
        QVERIFY(PlaylistIO::parse("x", QStringLiteral("cue"), resolve).isEmpty());
    }

    void xspfRoundTripKeepsEverything()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        Track a = Track::fromPath(dir.filePath(QStringLiteral("Кино/Звезда по имени Солнце.mp3")));
        a.title = QStringLiteral("Звезда по имени Солнце");
        a.artist = QStringLiteral("Кино");
        a.year = QStringLiteral("1989");
        a.genre = QStringLiteral("Rock");
        a.durationMs = 225000;
        a.codec = QStringLiteral("mp3");
        a.bitrateKbps = 320;
        a.scanned = true;
        Track b = Track::fromPath(QStringLiteral("/m/album.flac"));
        b.cueSheet = QStringLiteral("/m/album.cue");
        b.startMs = 67400;
        b.endMs = 230200;
        b.trackNo = 2;

        const QString file = dir.filePath(QStringLiteral("s.xspf"));
        QString error;
        QVERIFY2(PlaylistIO::save(file, {QStringLiteral("Сессия"), {a, b}}, &error), qPrintable(error));

        PlaylistData back;
        QVERIFY(PlaylistIO::load(file, back, &error));
        QCOMPARE(back.name, QStringLiteral("Сессия"));
        QCOMPARE(back.tracks.size(), 2);
        QCOMPARE(back.tracks[0].path, a.path);
        QCOMPARE(back.tracks[0].title, a.title);
        QCOMPARE(back.tracks[0].year, a.year);
        QCOMPARE(back.tracks[0].bitrateKbps, 320);
        QVERIFY(back.tracks[0].scanned);
        QCOMPARE(back.tracks[1].cueSheet, b.cueSheet);
        QCOMPARE(back.tracks[1].startMs, b.startMs);
        QCOMPARE(back.tracks[1].endMs, b.endMs);
        QVERIFY(!back.tracks[1].scanned);
    }

    void m3uWritesCueSheetOnce()
    {
        QTemporaryDir dir;
        Track c1 = Track::fromPath(QStringLiteral("/m/album.flac"));
        c1.cueSheet = QStringLiteral("/m/album.cue");
        Track c2 = c1;
        c2.startMs = 1000;
        Track plain = Track::fromPath(QStringLiteral("/m/single.mp3"));
        plain.title = QStringLiteral("Single");
        plain.durationMs = 60400;

        const QString file = dir.filePath(QStringLiteral("p.m3u8"));
        QVERIFY(PlaylistIO::save(file, {QStringLiteral("P"), {c1, c2, plain}}));
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        QCOMPARE(text.count(QStringLiteral("/m/album.cue")), 1);
        QVERIFY(text.contains(QStringLiteral("#EXTINF:60,Single\n/m/single.mp3")));
    }

    void cueFileOnDiskInCp1251()
    {
        QTemporaryDir dir;
        QFile audio(dir.filePath(QStringLiteral("a.flac")));
        QVERIFY(audio.open(QIODevice::WriteOnly));
        audio.close();
        QFile cue(dir.filePath(QStringLiteral("a.cue")));
        QVERIFY(cue.open(QIODevice::WriteOnly));
        // TITLE "Кино", FILE references a .wav that was re-encoded to .flac
        cue.write("TITLE \"\xCA\xE8\xED\xEE\"\r\nFILE \"a.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n");
        cue.close();

        PlaylistData data;
        QVERIFY(PlaylistIO::load(cue.fileName(), data));
        QCOMPARE(data.name, QStringLiteral("Кино"));
        QCOMPARE(data.tracks.size(), 1);
        QCOMPARE(data.tracks[0].path, audio.fileName());
    }

    // A tag with half of a UTF-16 surrogate pair (AIMP files carry such junk)
    // used to make the whole UTF-16 playlist read as empty.
    void loneSurrogateKeepsThePlaylist()
    {
        QString text = QStringLiteral("#EXTM3U\r\n#EXTINF:10,Bad @\r\n/m/a.mp3\r\n/m/b.mp3\r\n");
        text[text.indexOf(QLatin1Char('@'))] = QChar(0xD800);
        QByteArray bytes("\xFF\xFE", 2);
        bytes.append(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("p.m3u")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        f.close();

        PlaylistData data;
        QVERIFY(PlaylistIO::load(f.fileName(), data));
        QCOMPARE(data.tracks.size(), 2);
        QCOMPARE(data.tracks[0].title, QStringLiteral("Bad ") + QChar(QChar::ReplacementCharacter));
    }

    void loneSurrogateInXspf()
    {
        QTemporaryDir dir;
        Track a = Track::fromPath(QStringLiteral("/m/a.mp3"));
        a.title = QStringLiteral("x") + QChar(0xDC00);
        Track b = Track::fromPath(QStringLiteral("/m/b.mp3"));
        const QString file = dir.filePath(QStringLiteral("s.xspf"));
        QVERIFY(PlaylistIO::save(file, {QStringLiteral("n") + QChar(0xD800), {a, b}}));
        PlaylistData back;
        QString error;
        QVERIFY2(PlaylistIO::load(file, back, &error), qPrintable(error));
        QCOMPARE(back.tracks.size(), 2);
    }

    void damagedXspfIsReported()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("s.xspf"));
        QVERIFY(PlaylistIO::save(file, {QStringLiteral("S"), {Track::fromPath(QStringLiteral("/m/a.mp3")),
                                                              Track::fromPath(QStringLiteral("/m/b.mp3"))}}));
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray xml = f.readAll();
        f.close();
        xml.insert(xml.indexOf("/m/b.mp3"), "<broken");   // damage the second track
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(xml);
        f.close();

        PlaylistData back;
        QString error;
        QVERIFY(!PlaylistIO::load(file, back, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(back.tracks.size(), 1);   // what came before the damage
    }

    // Tags are untrusted: a line break in one must not add an entry.
    void lineBreaksInTagsStayInTheirLine()
    {
        QTemporaryDir dir;
        Track t = Track::fromPath(QStringLiteral("/m/a.mp3"));
        t.title = QStringLiteral("line1\n/etc/passwd\r\nhttp://evil.example/x");
        Track odd = Track::fromPath(QStringLiteral("/m/new\nline.mp3"));
        for (const char *ext : {"m3u8", "pls"}) {
            const QString file = dir.filePath(QStringLiteral("p.") + QLatin1String(ext));
            QVERIFY(PlaylistIO::save(file, {QStringLiteral("P\nQ"), {t, odd}}));
            PlaylistData back;
            QVERIFY(PlaylistIO::load(file, back));
            QCOMPARE(back.tracks.size(), 1);
            QCOMPARE(back.tracks[0].path, t.path);
        }
    }

    // A Windows path that was not found stays one in the session file.
    void unresolvedWindowsPathSurvivesXspf()
    {
        QTemporaryDir dir;
        Track t;
        t.path = QStringLiteral("D:/Music/a.mp3");
        t.uid = Track::newUid();
        const QString file = dir.filePath(QStringLiteral("s.xspf"));
        QVERIFY(PlaylistIO::save(file, {QStringLiteral("S"), {t}}));
        PlaylistData back;
        QVERIFY(PlaylistIO::load(file, back));
        QCOMPARE(back.tracks.size(), 1);
        QVERIFY2(!back.tracks[0].path.startsWith(QLatin1String("/D:")), qPrintable(back.tracks[0].path));
    }
};

QTEST_GUILESS_MAIN(TestPlaylistIO)
#include "test_playlistio.moc"
