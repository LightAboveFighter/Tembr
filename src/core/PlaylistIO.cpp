#include "core/PlaylistIO.h"

#include "core/PathResolver.h"
#include "core/TextDecode.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace PlaylistIO {

// AIMP's own format (no public spec; layout taken from files written by AIMP 5):
// UTF-16LE with BOM, CRLF, three sections. In #-----CONTENT-----#, "-<folder>"
// lines are group headers and every track is one '|'-separated line.
namespace Aimp {
enum Field {
    Path, Title, Artist, Album, AlbumArtist, Genre, Year, TrackNo, DiscNo, Composer, Publisher,
    Bitrate, Channels, SampleRate, Duration, FileSize, Bpm, Flag17, Index,
    Format = 24, Comment = 25, FieldCount = 27
};
} // namespace Aimp

namespace {

const QString kMetaNs = QStringLiteral("tembr:");

QString suffixOf(const QString &file)
{
    return QFileInfo(file).suffix().toLower();
}

QString resolvePath(const QString &entry, const QString &baseDir)
{
    return PathResolver(baseDir).resolve(entry);
}

EntryResolver localResolver(const QString &baseDir)
{
    return [resolver = PathResolver(baseDir)](const QString &entry) { return resolver.resolve(entry); };
}

// XSPF locations are URIs. Relative ones are still percent-encoded
// ("Music/My%20Song.mp3", as AIMP for Android writes them): decode those.
QString xspfLocation(const QString &raw)
{
    const QString loc = raw.trimmed();
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    if (loc.contains(QLatin1Char('%')) && !scheme.match(loc).hasMatch())
        return QUrl::fromPercentEncoding(loc.toUtf8());
    return loc;
}

// Non-empty lines, whatever the line ends. No regular expression: one
// invalid character would make it return the whole text as one line.
QStringList splitLines(const QString &text)
{
    QStringList out;
    qsizetype start = 0;
    for (qsizetype i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text.at(i) == QLatin1Char('\n') || text.at(i) == QLatin1Char('\r')) {
            if (i > start)
                out << text.mid(start, i - start);
            start = i + 1;
        }
    }
    return out;
}

void splitArtistTitle(const QString &s, Track &t)
{
    const int sep = s.indexOf(QLatin1String(" - "));
    if (sep > 0) {
        t.artist = s.left(sep).trimmed();
        t.title = s.mid(sep + 3).trimmed();
    } else {
        t.title = s.trimmed();
    }
}

QString unquote(QString s)
{
    s = s.trimmed();
    if (s.size() >= 2 && s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"')))
        s = s.mid(1, s.size() - 2);
    return s;
}

// Replaces entries pointing at a .cue sheet with the sheet's tracks.
TrackList expandCueEntries(const TrackList &in)
{
    TrackList out;
    for (const Track &t : in) {
        if (!t.isCueTrack() && suffixOf(t.path) == QLatin1String("cue") && QFileInfo::exists(t.path))
            out += loadCue(t.path);
        else
            out.append(t);
    }
    return out;
}

// What to write for each playlist position: a regular track, or (once) the
// .cue sheet a run of consecutive CUE tracks came from.
struct Entry
{
    const Track *track = nullptr;
    QString cue;
};

QList<Entry> entriesForSave(const TrackList &tracks)
{
    QList<Entry> out;
    QString lastCue;
    for (const Track &t : tracks) {
        if (t.isCueTrack()) {
            if (t.cueSheet != lastCue)
                out.append(Entry{nullptr, t.cueSheet});
            lastCue = t.cueSheet;
        } else {
            out.append(Entry{&t, {}});
            lastCue.clear();
        }
    }
    return out;
}

// One line of a line-based format: tags are untrusted, and a line break in
// one would add an entry of its own to the playlist.
QString oneLine(QString s)
{
    s.replace(QLatin1Char('\r'), QLatin1Char(' '));
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return s;
}

bool hasLineBreak(const QString &s)
{
    return s.contains(QLatin1Char('\n')) || s.contains(QLatin1Char('\r'));
}

QString extinfTitle(const Track &t)
{
    if (t.title.isEmpty())
        return oneLine(QFileInfo(t.path).completeBaseName());
    return oneLine(t.artist.isEmpty() ? t.title : t.artist + QStringLiteral(" - ") + t.title);
}

bool writeFile(const QString &file, const QByteArray &bytes, QString *error)
{
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(bytes);
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

QByteArray writeM3u(const PlaylistData &data)
{
    QString s = QStringLiteral("#EXTM3U\n");
    if (!data.name.isEmpty())
        s += QStringLiteral("#PLAYLIST:") + oneLine(data.name) + QLatin1Char('\n');
    for (const Entry &e : entriesForSave(data.tracks)) {
        if (!e.track) {
            if (!hasLineBreak(e.cue))
                s += e.cue + QLatin1Char('\n');
            continue;
        }
        const Track &t = *e.track;
        if (hasLineBreak(t.path))
            continue;   // cannot be written as a line (a file name with a line break)
        const qint64 secs = t.durationMs > 0 ? (t.durationMs + 500) / 1000 : -1;
        s += QStringLiteral("#EXTINF:%1,%2\n%3\n").arg(secs).arg(extinfTitle(t), t.path);
    }
    return s.toUtf8();
}

QByteArray writePls(const PlaylistData &data)
{
    const QList<Entry> entries = entriesForSave(data.tracks);
    QString s = QStringLiteral("[playlist]\n");
    int n = 0;
    for (const Entry &e : entries) {
        if (hasLineBreak(e.track ? e.track->path : e.cue))
            continue;   // cannot be written as a line
        ++n;
        if (!e.track) {
            s += QStringLiteral("File%1=%2\n").arg(n).arg(e.cue);
            continue;
        }
        const Track &t = *e.track;
        s += QStringLiteral("File%1=%2\nTitle%1=%3\nLength%1=%4\n")
                 .arg(n)
                 .arg(t.path, extinfTitle(t))
                 .arg(t.durationMs > 0 ? (t.durationMs + 500) / 1000 : -1);
    }
    s += QStringLiteral("NumberOfEntries=%1\nVersion=2\n").arg(n);
    return s.toUtf8();
}

QString locationFor(const QString &path)
{
    // URLs, and paths that are not absolute Linux paths (Windows ones left by
    // a playlist from AIMP that were not found), are kept as they are: as a
    // file:// URL "D:/a.mp3" would come back as "/D:/a.mp3".
    if (path.contains(QLatin1String("://")) || !path.startsWith(QLatin1Char('/')))
        return replaceLoneSurrogates(path);
    return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
}

QByteArray writeXspf(const PlaylistData &data)
{
    QByteArray out;
    QXmlStreamWriter w(&out);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(2);
    w.writeStartDocument();
    w.writeStartElement(QStringLiteral("playlist"));
    w.writeAttribute(QStringLiteral("version"), QStringLiteral("1"));
    w.writeDefaultNamespace(QStringLiteral("http://xspf.org/ns/0/"));
    if (!data.name.isEmpty())
        w.writeTextElement(QStringLiteral("title"), replaceLoneSurrogates(data.name));
    w.writeStartElement(QStringLiteral("trackList"));

    auto text = [&](const char *name, const QString &v) {
        if (!v.isEmpty())
            w.writeTextElement(QLatin1String(name), replaceLoneSurrogates(v));
    };
    auto meta = [&](const char *key, const QString &v) {
        if (v.isEmpty() || v == QLatin1String("0"))
            return;
        w.writeStartElement(QStringLiteral("meta"));
        w.writeAttribute(QStringLiteral("rel"), kMetaNs + QLatin1String(key));
        w.writeCharacters(replaceLoneSurrogates(v));
        w.writeEndElement();
    };

    for (const Track &t : data.tracks) {
        w.writeStartElement(QStringLiteral("track"));
        text("location", locationFor(t.path));
        text("title", t.title);
        text("creator", t.artist);
        text("album", t.album);
        if (t.trackNo > 0)
            text("trackNum", QString::number(t.trackNo));
        if (t.durationMs > 0)
            text("duration", QString::number(t.durationMs));
        meta("cue", t.cueSheet);
        meta("start", QString::number(t.startMs));
        meta("end", QString::number(t.endMs));
        meta("albumartist", t.albumArtist);
        meta("genre", t.genre);
        meta("year", t.year);
        meta("composer", t.composer);
        meta("comment", t.comment);
        meta("disc", QString::number(t.discNo));
        meta("codec", t.codec);
        meta("bitrate", QString::number(t.bitrateKbps));
        meta("samplerate", QString::number(t.sampleRate));
        meta("channels", QString::number(t.channels));
        meta("filesize", QString::number(t.fileSize));
        meta("added", QString::number(t.addedMs));
        meta("modified", QString::number(t.modifiedMs));
        meta("scanned", t.scanned ? QStringLiteral("1") : QString());
        w.writeEndElement();
    }
    w.writeEndElement(); // trackList
    w.writeEndElement(); // playlist
    w.writeEndDocument();
    return out;
}

QString aimpValue(QString v)
{
    v.replace(QLatin1Char('|'), QLatin1Char('/'));
    v.replace(QLatin1String("\r\n"), QLatin1String("\\n"));
    v.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    return v;
}

QByteArray writeAimppl4(const PlaylistData &data)
{
    qint64 totalMs = 0, totalSize = 0;
    for (const Track &t : data.tracks) {
        totalMs += t.durationMs;
        totalSize += t.fileSize;
    }
    QStringList lines;
    lines << QStringLiteral("#-----SUMMARY-----#")
          << QStringLiteral("ID=") + QUuid::createUuid().toString().toUpper()
          << QStringLiteral("Name=") + data.name
          << QStringLiteral("NameIsAutoSet=0")
          << QStringLiteral("ContentDuration=%1").arg(totalMs)
          << QStringLiteral("ContentFiles=%1").arg(data.tracks.size())
          << QStringLiteral("ContentSize=%1").arg(totalSize)
          << QStringLiteral("PlaybackCursor=-1")
          << QStringLiteral("PlaybackCursorAutoBookmark=0")
          << QStringLiteral("Shuffled=0")
          << QStringLiteral("UserReordered=1")
          << QString()
          << QStringLiteral("#-----SETTINGS-----#")
          << QStringLiteral("Flags=2047")
          << QStringLiteral("FormatMainLine=%IF(%Artist,%Artist - %Title,%Title)")
          << QStringLiteral("FormatSecondLine=%FileFormat :: %SampleRate, %BitRate, %FileSize")
          << QStringLiteral("GroupFormatLine=%FileDir")
          << QString()
          << QStringLiteral("#-----CONTENT-----#");

    QString lastDir;
    int index = 0;
    for (const Track &t : data.tracks) {
        const QString winPath = PathResolver::toWindows(t.path);
        const QString dir = winPath.left(winPath.lastIndexOf(QLatin1Char('\\')));
        if (dir != lastDir && !t.isUrl())
            lines << QLatin1Char('-') + dir;
        lastDir = dir;

        QStringList f(Aimp::FieldCount);
        auto num = [](qint64 v) { return v > 0 ? QString::number(v) : QString(); };
        f[Aimp::Path] = winPath;
        f[Aimp::Title] = aimpValue(t.title);
        f[Aimp::Artist] = aimpValue(t.artist);
        f[Aimp::Album] = aimpValue(t.album);
        f[Aimp::AlbumArtist] = aimpValue(t.albumArtist);
        f[Aimp::Genre] = aimpValue(t.genre);
        f[Aimp::Year] = aimpValue(t.year);
        f[Aimp::TrackNo] = num(t.trackNo);
        f[Aimp::DiscNo] = num(t.discNo);
        f[Aimp::Composer] = aimpValue(t.composer);
        f[Aimp::Bitrate] = QString::number(t.bitrateKbps);
        f[Aimp::Channels] = QString::number(t.channels);
        f[Aimp::SampleRate] = QString::number(t.sampleRate);
        f[Aimp::Duration] = QString::number(t.durationMs);
        f[Aimp::FileSize] = QString::number(t.fileSize);
        f[Aimp::Bpm] = QStringLiteral("0");
        f[Aimp::Flag17] = QStringLiteral("1");
        f[Aimp::Index] = QString::number(index++);
        f[22] = QStringLiteral("0");
        f[Aimp::Format] = t.codec.toUpper();
        f[Aimp::Comment] = aimpValue(t.comment);
        lines << f.join(QLatin1Char('|'));
    }

    const QString text = lines.join(QLatin1String("\r\n")) + QLatin1String("\r\n");
    QByteArray out("\xFF\xFE", 2);
    out.append(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
    return out;
}

const char *const kAudioFallbackExts[] = {"flac", "ape", "wv", "wav", "mp3", "ogg", "m4a", "tta", "opus"};

// CUE sheets often name "album.wav" while the real file was re-encoded to FLAC.
QString findAudioForCue(const QString &path)
{
    if (QFileInfo::exists(path) || path.contains(QLatin1String("://")))
        return path;
    const QFileInfo fi(path);
    for (const char *ext : kAudioFallbackExts) {
        const QString candidate = fi.dir().filePath(fi.completeBaseName() + QLatin1Char('.') + QLatin1String(ext));
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return path;
}

// "mm:ss:ff", 75 frames per second.
qint64 cueTimeToMs(const QString &s)
{
    const QStringList p = s.trimmed().split(QLatin1Char(':'));
    if (p.size() != 3)
        return 0;
    const qint64 frames = (p[0].toLongLong() * 60 + p[1].toLongLong()) * 75 + p[2].toLongLong();
    return frames * 1000 / 75;
}

} // namespace

QStringList readableFormats()
{
    return {QStringLiteral("m3u"), QStringLiteral("m3u8"), QStringLiteral("pls"),
            QStringLiteral("xspf"), QStringLiteral("cue"), QStringLiteral("aimppl4")};
}

QStringList writableFormats()
{
    return {QStringLiteral("m3u8"), QStringLiteral("m3u"), QStringLiteral("pls"), QStringLiteral("xspf"),
            QStringLiteral("aimppl4")};
}

bool isPlaylistFile(const QString &path)
{
    return readableFormats().contains(suffixOf(path));
}

TrackList parseM3u(const QString &text, const QString &baseDir, QString *name)
{
    return parseM3u(text, localResolver(baseDir), name);
}

TrackList parseM3u(const QString &text, const EntryResolver &resolve, QString *name)
{
    TrackList out;
    QString pendingTitle;
    qint64 pendingDuration = 0;
    const QStringList lines = splitLines(text);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("#EXTINF:"), Qt::CaseInsensitive)) {
            const QString rest = line.mid(8);
            const int comma = rest.indexOf(QLatin1Char(','));
            const qint64 secs = rest.left(comma).trimmed().toLongLong();
            pendingDuration = secs > 0 ? secs * 1000 : 0;
            pendingTitle = comma >= 0 ? rest.mid(comma + 1) : QString();
        } else if (line.startsWith(QLatin1String("#PLAYLIST:"), Qt::CaseInsensitive)) {
            if (name)
                *name = line.mid(10).trimmed();
        } else if (!line.startsWith(QLatin1Char('#'))) {
            Track t = Track::fromPath(resolve(line));
            if (!pendingTitle.isEmpty())
                splitArtistTitle(pendingTitle, t);
            t.durationMs = pendingDuration;
            pendingTitle.clear();
            pendingDuration = 0;
            out.append(t);
        }
    }
    return out;
}

TrackList parsePls(const QString &text, const QString &baseDir)
{
    return parsePls(text, localResolver(baseDir));
}

TrackList parsePls(const QString &text, const EntryResolver &resolve)
{
    QMap<int, Track> byIndex;
    static const QRegularExpression re(QStringLiteral("^(File|Title|Length)(\\d+)\\s*=(.*)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    for (const QString &raw : splitLines(text)) {
        const auto m = re.match(raw.trimmed());
        if (!m.hasMatch())
            continue;
        const QString key = m.captured(1).toLower();
        const int idx = m.captured(2).toInt();
        const QString value = m.captured(3).trimmed();
        Track &t = byIndex[idx];
        if (key == QLatin1String("file"))
            t.path = resolve(value);
        else if (key == QLatin1String("title"))
            splitArtistTitle(value, t);
        else if (value.toLongLong() > 0)
            t.durationMs = value.toLongLong() * 1000;
    }
    TrackList out;
    for (Track t : std::as_const(byIndex)) {
        if (t.path.isEmpty())
            continue;
        t.uid = Track::newUid();
        out.append(t);
    }
    return out;
}

TrackList parseXspf(const QByteArray &xml, const QString &baseDir, QString *name)
{
    return parseXspf(xml, localResolver(baseDir), name);
}

TrackList parseXspf(const QByteArray &xml, const EntryResolver &resolve, QString *name)
{
    return parseXspfChecked(xml, resolve, name, nullptr);
}

TrackList parseXspfChecked(const QByteArray &xml, const EntryResolver &resolve, QString *name, QString *error)
{
    TrackList out;
    QXmlStreamReader r(xml);
    Track cur;
    bool inTrack = false;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            const auto tag = r.name();
            if (tag == QLatin1String("track")) {
                cur = Track();
                inTrack = true;
            } else if (!inTrack) {
                if (tag == QLatin1String("title") && name)
                    *name = r.readElementText().trimmed();
            } else if (tag == QLatin1String("location")) {
                cur.path = resolve(xspfLocation(r.readElementText()));
            } else if (tag == QLatin1String("title")) {
                cur.title = r.readElementText().trimmed();
            } else if (tag == QLatin1String("creator")) {
                cur.artist = r.readElementText().trimmed();
            } else if (tag == QLatin1String("album")) {
                cur.album = r.readElementText().trimmed();
            } else if (tag == QLatin1String("trackNum")) {
                cur.trackNo = r.readElementText().toInt();
            } else if (tag == QLatin1String("duration")) {
                cur.durationMs = r.readElementText().toLongLong();
            } else if (tag == QLatin1String("meta")) {
                const QString rel = r.attributes().value(QLatin1String("rel")).toString();
                const QString v = r.readElementText();
                if (!rel.startsWith(kMetaNs))
                    continue;
                const QString key = rel.mid(kMetaNs.size());
                if (key == QLatin1String("cue")) cur.cueSheet = v;
                else if (key == QLatin1String("start")) cur.startMs = v.toLongLong();
                else if (key == QLatin1String("end")) cur.endMs = v.toLongLong();
                else if (key == QLatin1String("albumartist")) cur.albumArtist = v;
                else if (key == QLatin1String("genre")) cur.genre = v;
                else if (key == QLatin1String("year")) cur.year = v;
                else if (key == QLatin1String("composer")) cur.composer = v;
                else if (key == QLatin1String("comment")) cur.comment = v;
                else if (key == QLatin1String("disc")) cur.discNo = v.toInt();
                else if (key == QLatin1String("codec")) cur.codec = v;
                else if (key == QLatin1String("bitrate")) cur.bitrateKbps = v.toInt();
                else if (key == QLatin1String("samplerate")) cur.sampleRate = v.toInt();
                else if (key == QLatin1String("channels")) cur.channels = v.toInt();
                else if (key == QLatin1String("filesize")) cur.fileSize = v.toLongLong();
                else if (key == QLatin1String("added")) cur.addedMs = v.toLongLong();
                else if (key == QLatin1String("modified")) cur.modifiedMs = v.toLongLong();
                else if (key == QLatin1String("scanned")) cur.scanned = (v == QLatin1String("1"));
            }
        } else if (r.isEndElement() && r.name() == QLatin1String("track")) {
            inTrack = false;
            if (!cur.path.isEmpty()) {
                cur.uid = Track::newUid();
                out.append(cur);
            }
        }
    }
    if (r.hasError() && error)
        *error = QStringLiteral("line %1: %2").arg(r.lineNumber()).arg(r.errorString());
    return out;
}


namespace {

// A content line must start with a path; AIMP files sometimes carry junk
// (e.g. bytes of an unrelated binary file) that would otherwise become tracks.
bool looksLikeAimpTrackLine(const QString &line)
{
    const QString path = line.section(QLatin1Char('|'), 0, 0);
    if (path.isEmpty())
        return false;
    for (QChar c : path) {
        if (c.category() == QChar::Other_Control || c == QChar::ReplacementCharacter)
            return false;
    }
    return PathResolver::isWindowsAbsolute(path) || path.startsWith(QLatin1Char('/'))
           || path.startsWith(QLatin1String("\\\\")) || path.contains(QLatin1String("://"));
}

QString aimpField(const QStringList &f, int i)
{
    return i < f.size() ? f.at(i).trimmed() : QString();
}

} // namespace

TrackList parseAimppl4(const QString &text, const QString &baseDir, QString *name)
{
    return parseAimppl4(text, localResolver(baseDir), name);
}

TrackList parseAimppl4(const QString &text, const EntryResolver &resolve, QString *name)
{
    TrackList out;
    enum class Section { None, Summary, Settings, Content } section = Section::None;
    for (const QString &raw : splitLines(text)) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("#-----"))) {
            const QString s = line.toUpper();
            if (s.contains(QLatin1String("SUMMARY")))
                section = Section::Summary;
            else if (s.contains(QLatin1String("SETTINGS")))
                section = Section::Settings;
            else if (s.contains(QLatin1String("CONTENT")))
                section = Section::Content;
            else
                section = Section::None;
            continue;
        }
        if (section == Section::Summary) {
            if (name && line.startsWith(QLatin1String("Name="), Qt::CaseInsensitive))
                *name = line.mid(5).trimmed();
            continue;
        }
        if (section != Section::Content || line.startsWith(QLatin1Char('-')) || !looksLikeAimpTrackLine(line))
            continue;

        const QStringList f = line.split(QLatin1Char('|'));
        Track t = Track::fromPath(resolve(f.at(Aimp::Path)));
        t.title = aimpField(f, Aimp::Title);
        t.artist = aimpField(f, Aimp::Artist);
        t.album = aimpField(f, Aimp::Album);
        t.albumArtist = aimpField(f, Aimp::AlbumArtist);
        t.genre = aimpField(f, Aimp::Genre);
        t.year = aimpField(f, Aimp::Year);
        if (t.year == QLatin1String("0"))
            t.year.clear();
        t.trackNo = aimpField(f, Aimp::TrackNo).toInt();
        t.discNo = aimpField(f, Aimp::DiscNo).toInt();
        t.composer = aimpField(f, Aimp::Composer);
        t.bitrateKbps = aimpField(f, Aimp::Bitrate).toInt();
        t.channels = aimpField(f, Aimp::Channels).toInt() & 0xFF;   // high bits are flags
        t.sampleRate = aimpField(f, Aimp::SampleRate).toInt();
        t.durationMs = aimpField(f, Aimp::Duration).toLongLong();
        t.fileSize = aimpField(f, Aimp::FileSize).toLongLong();
        t.codec = aimpField(f, Aimp::Format);
        if (t.codec.isEmpty() && !t.isUrl())
            t.codec = QFileInfo(t.path).suffix().toUpper();
        t.comment = aimpField(f, Aimp::Comment).replace(QLatin1String("\\n"), QLatin1String("\n"));
        // AIMP has already read the tags; don't scan the file again.
        t.scanned = t.durationMs > 0;
        out.append(t);
    }
    return out;
}

TrackList parseCue(const QString &text, const QString &cueFile, QString *albumTitle)
{
    const QString baseDir = QFileInfo(cueFile).absolutePath();
    QString gPerformer, gTitle, gGenre, gDate, currentFile;
    TrackList tracks;
    Track *cur = nullptr;

    for (const QString &raw : splitLines(text)) {
        const QString line = raw.trimmed();
        const int sp = line.indexOf(QLatin1Char(' '));
        const QString cmd = (sp < 0 ? line : line.left(sp)).toUpper();
        const QString rest = sp < 0 ? QString() : line.mid(sp + 1).trimmed();

        if (cmd == QLatin1String("REM")) {
            const int sp2 = rest.indexOf(QLatin1Char(' '));
            const QString key = rest.left(sp2).toUpper();
            const QString val = unquote(rest.mid(sp2 + 1));
            if (key == QLatin1String("GENRE"))
                (cur ? cur->genre : gGenre) = val;
            else if (key == QLatin1String("DATE"))
                (cur ? cur->year : gDate) = val;
        } else if (cmd == QLatin1String("PERFORMER")) {
            (cur ? cur->artist : gPerformer) = unquote(rest);
        } else if (cmd == QLatin1String("TITLE")) {
            (cur ? cur->title : gTitle) = unquote(rest);
        } else if (cmd == QLatin1String("SONGWRITER")) {
            if (cur)
                cur->composer = unquote(rest);
        } else if (cmd == QLatin1String("FILE")) {
            QString name;
            if (rest.startsWith(QLatin1Char('"')))
                name = rest.mid(1, rest.indexOf(QLatin1Char('"'), 1) - 1);
            else
                name = rest.left(rest.lastIndexOf(QLatin1Char(' ')));
            currentFile = findAudioForCue(resolvePath(name, baseDir));
            cur = nullptr;
        } else if (cmd == QLatin1String("TRACK")) {
            Track t = Track::fromPath(currentFile);
            t.cueSheet = QFileInfo(cueFile).absoluteFilePath();
            t.trackNo = rest.section(QLatin1Char(' '), 0, 0).toInt();
            t.startMs = -1;   // until INDEX 01 is seen
            tracks.append(t);
            cur = &tracks.last();
        } else if (cmd == QLatin1String("INDEX") && cur) {
            const QString num = rest.section(QLatin1Char(' '), 0, 0);
            const qint64 ms = cueTimeToMs(rest.section(QLatin1Char(' '), 1, 1));
            if (num.toInt() == 1 || (num.toInt() == 0 && cur->startMs < 0))
                cur->startMs = ms;
        }
    }

    for (int i = 0; i < tracks.size(); ++i) {
        Track &t = tracks[i];
        t.startMs = qMax<qint64>(0, t.startMs);
        if (i + 1 < tracks.size() && tracks[i + 1].path == t.path)
            t.endMs = qMax<qint64>(0, tracks[i + 1].startMs);
        if (t.endMs > t.startMs)
            t.durationMs = t.endMs - t.startMs;
        if (t.artist.isEmpty()) t.artist = gPerformer;
        if (t.album.isEmpty()) t.album = gTitle;
        if (t.albumArtist.isEmpty()) t.albumArtist = gPerformer;
        if (t.genre.isEmpty()) t.genre = gGenre;
        if (t.year.isEmpty()) t.year = gDate;
        if (t.title.isEmpty()) t.title = QStringLiteral("Track %1").arg(t.trackNo, 2, 10, QLatin1Char('0'));
    }
    if (albumTitle)
        *albumTitle = gTitle;
    return tracks;
}

TrackList loadCue(const QString &cueFile, QString *albumTitle)
{
    QFile f(cueFile);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return parseCue(decodeText(f.readAll()), cueFile, albumTitle);
}

TrackList parse(const QByteArray &data, const QString &format, const EntryResolver &resolve, QString *name)
{
    const QString ext = format.toLower();
    if (ext == QLatin1String("m3u") || ext == QLatin1String("m3u8"))
        return parseM3u(decodeText(data), resolve, name);
    if (ext == QLatin1String("pls"))
        return parsePls(decodeText(data), resolve);
    if (ext == QLatin1String("xspf"))
        return parseXspf(data, resolve, name);
    if (ext == QLatin1String("aimppl4"))
        return parseAimppl4(decodeText(data), resolve, name);
    return {};
}

bool load(const QString &file, PlaylistData &out, QString *error)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    const QByteArray bytes = f.readAll();
    const QString baseDir = QFileInfo(file).absolutePath();
    const QString ext = suffixOf(file);
    out.name = QFileInfo(file).completeBaseName();

    TrackList tracks;
    QString damage;
    if (ext == QLatin1String("m3u") || ext == QLatin1String("m3u8"))
        tracks = parseM3u(decodeText(bytes), baseDir, &out.name);
    else if (ext == QLatin1String("pls"))
        tracks = parsePls(decodeText(bytes), baseDir);
    else if (ext == QLatin1String("xspf"))
        tracks = parseXspfChecked(bytes, localResolver(baseDir), &out.name, &damage);
    else if (ext == QLatin1String("aimppl4"))
        tracks = parseAimppl4(decodeText(bytes), baseDir, &out.name);
    else if (ext == QLatin1String("cue")) {
        QString album;
        tracks = parseCue(decodeText(bytes), file, &album);
        if (!album.isEmpty())
            out.name = album;
    } else {
        if (error)
            *error = QStringLiteral("Unsupported playlist format: .%1").arg(ext);
        return false;
    }
    out.tracks = expandCueEntries(tracks);
    for (Track &t : out.tracks) {
        if (!t.isUrl())
            t.missing = !QFileInfo::exists(t.path);
    }
    if (!damage.isEmpty()) {
        if (error)
            *error = QStringLiteral("Damaged playlist file (%1)").arg(damage);
        return false;   // `out` keeps what could be read
    }
    return true;
}

bool save(const QString &file, const PlaylistData &data, QString *error)
{
    const QString ext = suffixOf(file);
    if (ext == QLatin1String("m3u") || ext == QLatin1String("m3u8"))
        return writeFile(file, writeM3u(data), error);
    if (ext == QLatin1String("pls"))
        return writeFile(file, writePls(data), error);
    if (ext == QLatin1String("xspf"))
        return writeFile(file, writeXspf(data), error);
    if (ext == QLatin1String("aimppl4"))
        return writeFile(file, writeAimppl4(data), error);
    if (error)
        *error = QStringLiteral("Saving to .%1 is not supported").arg(ext);
    return false;
}

} // namespace PlaylistIO
