#include "migration/PhoneImport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

namespace Migration {

namespace {

// A path inside a storage: ".." can never climb out of it (nor out of the
// folder tracks are copied to, which keeps the same relative path).
QString insideStorage(const QString &path)
{
    QString rel = QDir::cleanPath(path);
    while (rel.startsWith(QLatin1Char('/')))
        rel.remove(0, 1);
    while (rel == QLatin1String("..") || rel.startsWith(QLatin1String("../")))
        rel.remove(0, qMin(3, int(rel.size())));
    return rel == QLatin1String(".") ? QString() : rel;
}

int storageByKey(const QList<PhoneStorage> &storages, const QString &key)
{
    for (int i = 0; i < storages.size(); ++i) {
        if (key == QLatin1String("primary") ? storages[i].primary : storages[i].key.compare(key, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

// "/storage/emulated/0/Music/a.mp3" -> (primary, "Music/a.mp3")
bool mapAbsolute(const QString &path, const QList<PhoneStorage> &storages, PhoneRef &ref)
{
    static const QRegularExpression primary(
        QStringLiteral("^/(?:storage/emulated/\\d+|sdcard|mnt/sdcard|storage/self/primary|data/media/\\d+)(?:/(.*))?$"));
    static const QRegularExpression volume(
        QStringLiteral("^/(?:storage|mnt/media_rw)/([0-9A-Fa-f]{4}-[0-9A-Fa-f]{4})(?:/(.*))?$"));
    if (auto m = primary.match(path); m.hasMatch()) {
        ref.storage = storageByKey(storages, QStringLiteral("primary"));
        ref.rel = insideStorage(m.captured(1));
        return true;
    }
    if (auto m = volume.match(path); m.hasMatch()) {
        ref.storage = storageByKey(storages, m.captured(1));   // -1 over MTP: try every storage
        ref.rel = insideStorage(m.captured(2));
        return true;
    }
    return false;
}

// Where a phone file may go below the copy folder, best first. The phone's
// folder layout is kept, minus the top "Music/" (copying into ~/Music should
// not give ~/Music/Music/...); that may meet a file from the storage root, so
// next the path with "Music/", then numbered names: "x (2).mp3".
class CopyTargets
{
public:
    explicit CopyTargets(const QString &rel)
    {
        const bool music = rel.startsWith(QLatin1String("Music/"), Qt::CaseInsensitive);
        m_first = music ? rel.mid(6) : rel;
        if (music)
            m_second = rel;
    }

    // Empty when there are no more.
    QString next()
    {
        ++m_n;
        if (m_n == 1)
            return m_first;
        if (m_n == 2 && !m_second.isEmpty())
            return m_second;
        const int number = m_second.isEmpty() ? m_n : m_n - 1;
        if (number > 999)
            return {};
        const int slash = int(m_first.lastIndexOf(QLatin1Char('/')));
        const QString dir = m_first.left(slash + 1);
        const QString file = m_first.mid(slash + 1);
        const int dot = int(file.lastIndexOf(QLatin1Char('.')));
        if (dot <= 0)
            return dir + file + QStringLiteral(" (%1)").arg(number);
        return dir + file.left(dot) + QStringLiteral(" (%1)").arg(number) + file.mid(dot);
    }

private:
    QString m_first, m_second;
    int m_n = 0;
};

QString claimKey(const QString &localPath)
{
    // Lower case: on a FAT or NTFS drive x.mp3 and X.mp3 are one file.
    return localPath.toLower();
}

QString sourceKey(int storage, const QString &rel)
{
    return QString::number(storage) + QLatin1Char(':') + rel;
}


// Copies the file of `ref` below `folder` (or finds it copied before) and
// returns the local path; empty if it could not be.
QString copyFromPhone(PhoneSource &source, const PhoneRef &ref, const QString &folderIn, PhoneCopyClaims &claims,
                      PhoneImportStats &stats)
{
    const QString folder = QDir::cleanPath(folderIn);
    const QString prefix = folder.endsWith(QLatin1Char('/')) ? folder : folder + QLatin1Char('/');
    if (ref.rel.isEmpty()) {
        stats.errors << QStringLiteral("%1: недопустимый путь, не копирую").arg(ref.raw);
        return {};
    }
    const bool adb = source.kind() == PhoneSource::Kind::Adb;
    const int storageCount = int(source.storages().size());

    // The expected storage first, then the others: which MTP storage is
    // "/storage/emulated/0" is a guess, and SD cards have no id over MTP.
    QList<int> candidates;
    if (ref.storage >= 0)
        candidates << ref.storage;
    if (ref.storage < 0 || !adb) {
        for (int s = 0; s < storageCount; ++s) {
            if (s != ref.storage)
                candidates << s;
        }
    }

    // Which storage holds the file and its size: looked up when needed.
    // Over ADB every question is a process started, so it is asked only when
    // a file is already in the way; a plain copy needs no question there.
    int foundStorage = -1;
    qint64 foundSize = -1;
    bool looked = false;
    const auto lookUp = [&] {
        if (looked)
            return foundStorage >= 0;
        looked = true;
        for (int s : std::as_const(candidates)) {
            const qint64 size = source.fileSize(s, ref.rel);
            if (size >= 0) {
                foundStorage = s;
                foundSize = size;
                break;
            }
        }
        return foundStorage >= 0;
    };
    if (!adb && !lookUp())
        return {};   // not on the phone
    // Over ADB with a known storage the source is known without asking.
    const auto knownKey = [&]() -> QString {
        if (foundStorage >= 0)
            return sourceKey(foundStorage, ref.rel);
        if (adb && ref.storage >= 0)
            return sourceKey(ref.storage, ref.rel);
        return lookUp() ? sourceKey(foundStorage, ref.rel) : QString();
    };

    CopyTargets targets(ref.rel);
    for (QString target = targets.next(); !target.isEmpty(); target = targets.next()) {
        const QString dest = QDir::cleanPath(QDir(folder).filePath(target));
        if (!dest.startsWith(prefix)) {
            stats.errors << QStringLiteral("%1: недопустимый путь, не копирую").arg(ref.raw);
            return {};
        }
        const auto claim = claims.constFind(claimKey(dest));
        if (claim != claims.cend()) {
            if (*claim == knownKey()) {
                ++stats.copied;   // copied for an earlier entry of this import
                return dest;
            }
            continue;   // another phone file is there
        }
        const QFileInfo existing(dest);
        if (existing.exists()) {
            // Copied by an earlier import, or someone else's file: the size tells.
            if (existing.isFile() && lookUp() && existing.size() == foundSize) {
                claims.insert(claimKey(dest), sourceKey(foundStorage, ref.rel));
                ++stats.copied;
                return dest;
            }
            if (adb && looked && foundStorage < 0)
                return {};   // not on the phone
            continue;
        }

        // Into "<name>.part", renamed when complete: a copy cut short (adb
        // timed out, the cable came out) is never taken later for one
        // "copied before".
        const QString part = dest + QStringLiteral(".part");
        QString copyError;
        const QList<int> from = foundStorage >= 0 ? QList<int>{foundStorage} : candidates;
        for (int s : from) {
            QFile::remove(part);   // left by an earlier attempt
            if (!source.copyTo(s, ref.rel, part, &copyError))
                continue;
            const qint64 size = QFileInfo(part).size();
            if (size <= 0 || (foundStorage >= 0 && size != foundSize))
                continue;
            if (!QFile::rename(part, dest))
                continue;
            claims.insert(claimKey(dest), sourceKey(s, ref.rel));
            ++stats.copied;
            return dest;
        }
        QFile::remove(part);
        if (!copyError.isEmpty())
            stats.errors << QStringLiteral("%1: %2").arg(ref.rel, copyError);
        return {};
    }
    stats.errors << QStringLiteral("%1: нет свободного имени для копии").arg(ref.rel);
    return {};
}

} // namespace

PhoneImportStats &PhoneImportStats::operator+=(const PhoneImportStats &o)
{
    tracks += o.tracks;
    matched += o.matched;
    copied += o.copied;
    missing += o.missing;
    streams += o.streams;
    errors += o.errors;
    return *this;
}

PhoneRef mapPhoneEntry(const QString &entryIn, const QList<PhoneStorage> &storages, int playlistStorage,
                       const QString &playlistDir)
{
    PhoneRef ref;
    ref.raw = entryIn;
    QString entry = entryIn.trimmed();

    if (entry.startsWith(QLatin1String("content://"), Qt::CaseInsensitive)) {
        // SAF: content://com.android.externalstorage.documents/document/primary%3AMusic%2Fa.mp3
        const QUrl url(entry);
        const QString path = url.path(QUrl::FullyEncoded);
        const int at = path.lastIndexOf(QLatin1String("/document/"));
        const QString docId = at >= 0 ? QUrl::fromPercentEncoding(path.mid(at + 10).toUtf8()) : QString();
        const int colon = docId.indexOf(QLatin1Char(':'));
        if (url.host() != QLatin1String("com.android.externalstorage.documents") || colon < 0) {
            ref.kind = PhoneRef::Unmappable;   // e.g. content://media/external/audio/media/123
            return ref;
        }
        ref.storage = storageByKey(storages, docId.left(colon));
        ref.rel = insideStorage(docId.mid(colon + 1));
        return ref;
    }
    if (entry.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
        entry = QUrl(entry).toLocalFile();
    else if (entry.contains(QLatin1String("://"))) {
        ref.kind = PhoneRef::Url;
        ref.rel = entry;
        return ref;
    }

    entry.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (entry.startsWith(QLatin1Char('/'))) {
        if (!mapAbsolute(QDir::cleanPath(entry), storages, ref)) {
            ref.storage = -1;          // some other absolute path: try it on every storage
            ref.rel = insideStorage(entry);
        }
        return ref;
    }
    // Relative to the playlist (what AIMP for Android exports).
    ref.storage = playlistStorage;
    ref.rel = insideStorage(playlistDir.isEmpty() ? entry : playlistDir + QLatin1Char('/') + entry);
    return ref;
}

bool importPhonePlaylist(PhoneSource &source, const PhonePlaylistFile &file, const PhoneImportOptions &options,
                         PlaylistData &out, PhoneImportStats &stats, const PhoneProgress &progress, QString *error)
{
    QString readError;
    const QByteArray data = source.readFile(file.storage, file.path, &readError);
    if (data.isEmpty()) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(file.path, readError);
        return false;
    }
    const QList<PhoneStorage> storages = source.storages();
    const QString dir = QFileInfo(file.path).path() == QLatin1String(".") ? QString() : QFileInfo(file.path).path();

    // The parser stores an index into `refs` as the track path.
    QList<PhoneRef> refs;
    const auto resolve = [&](const QString &entry) {
        refs.append(mapPhoneEntry(entry, storages, file.storage, dir));
        return QString::number(refs.size() - 1);
    };
    QString name;
    TrackList tracks = PlaylistIO::parse(data, QFileInfo(file.path).suffix(), resolve, &name);
    out.name = name.isEmpty() ? QFileInfo(file.path).completeBaseName() : name;
    out.tracks.clear();

    PhoneCopyClaims ownClaims;
    PhoneCopyClaims *claims = options.claims ? options.claims : &ownClaims;
    PhoneImportStats local;
    for (int i = 0; i < tracks.size(); ++i) {
        Track t = tracks[i];
        const PhoneRef ref = refs.value(t.path.toInt());
        ++local.tracks;
        if (progress && !progress(i, int(tracks.size()), ref.rel)) {
            if (error)
                *error = QStringLiteral("отменено");
            return false;
        }

        if (ref.kind == PhoneRef::Url) {
            t.path = ref.rel;
            ++local.streams;
            out.tracks.append(t);
            continue;
        }
        if (ref.kind == PhoneRef::Unmappable) {
            t.path = ref.raw;
            t.missing = true;
            ++local.missing;
            out.tracks.append(t);
            continue;
        }

        // 1) Already on this computer?
        QString localPath;
        if (options.library) {
            Track probe = t;
            probe.path = QLatin1Char('/') + ref.rel;
            localPath = Relocator::find(probe, *options.library);
            if (!localPath.isEmpty())
                ++local.matched;
        }
        // 2) Copy it from the phone.
        if (localPath.isEmpty() && !options.copyMissingTo.isEmpty())
            localPath = copyFromPhone(source, ref, options.copyMissingTo, *claims, local);
        if (localPath.isEmpty()) {
            t.path = source.phonePath(ref.storage, ref.rel);
            t.missing = true;
            ++local.missing;
        } else {
            t.path = localPath;
            t.missing = false;
            t.scanned = false;   // read the real tags from the file
        }
        out.tracks.append(t);
    }
    if (progress)
        progress(int(tracks.size()), int(tracks.size()), QString());
    stats += local;
    return true;
}

} // namespace Migration
