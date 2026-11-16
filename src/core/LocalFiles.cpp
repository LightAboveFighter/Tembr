#include "core/LocalFiles.h"

#include "core/FileTimes.h"
#include "core/MediaCollector.h"
#include "core/PathResolver.h"
#include "core/PlaylistIO.h"
#include "core/TextDecode.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent>

namespace {

// Two sheets for one audio file give tracks with the same file and start.
QString trackKey(const Track &t)
{
    return t.isCueTrack() ? t.cueSheet + QLatin1Char('|') + t.path + QLatin1Char('#') + QString::number(t.startMs)
                          : t.path;
}

QStringList cleanFolders(const QStringList &in)
{
    QStringList out;
    for (QString p : in) {
        p = QDir::cleanPath(p.trimmed());
        if (!p.isEmpty() && p != QLatin1String(".") && !out.contains(p))
            out << p;
    }
    return out;
}

// AIMP.ini of AIMP for Windows: natively, or in the usual Wine prefix.
QStringList aimpIniCandidates()
{
    QStringList out;
#ifdef Q_OS_WIN
    out << qEnvironmentVariable("APPDATA") + QStringLiteral("/AIMP/AIMP.ini");
#endif
    const QString prefix = qEnvironmentVariable("WINEPREFIX", QDir::homePath() + QStringLiteral("/.wine"));
    for (const QString &user : QDir(prefix + QStringLiteral("/drive_c/users")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        for (const char *sub : {"AppData/Roaming/AIMP/AIMP.ini", "Application Data/AIMP/AIMP.ini"})
            out << QStringLiteral("%1/drive_c/users/%2/%3").arg(prefix, user, QLatin1String(sub));
    }
    return out;
}

QHash<QString, QString> iniSection(const QString &file, const QString &section)
{
    QHash<QString, QString> out;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    bool inSection = false;
    for (const QString &raw : decodeText(f.readAll()).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inSection = line == QLatin1Char('[') + section + QLatin1Char(']');
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (inSection && eq > 0)
            out.insert(line.left(eq), line.mid(eq + 1));
    }
    return out;
}

} // namespace

// --- Settings ------------------------------------------------------------------

LocalFilesSettings LocalFilesSettings::load(bool *firstRun)
{
    QSettings s;
    const bool configured = s.contains(QStringLiteral("localFiles/folders"));
    if (firstRun)
        *firstRun = !configured;
    LocalFilesSettings out;
    if (configured) {
        out.folders = s.value(QStringLiteral("localFiles/folders")).toStringList();
        out.excluded = s.value(QStringLiteral("localFiles/excluded")).toStringList();
        out.watch = s.value(QStringLiteral("localFiles/watch"), true).toBool();
        return out;
    }
    if (fromAimp(out))
        return out;
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (QFileInfo(music).isDir())
        out.folders << music;
    return out;
}

void LocalFilesSettings::save() const
{
    QSettings s;
    s.setValue(QStringLiteral("localFiles/folders"), folders);
    s.setValue(QStringLiteral("localFiles/excluded"), excluded);
    s.setValue(QStringLiteral("localFiles/watch"), watch);
}

bool LocalFilesSettings::fromAimp(LocalFilesSettings &out, QString *source, const QString &iniPath)
{
    const QStringList candidates = iniPath.isEmpty() ? aimpIniCandidates() : QStringList{iniPath};
    const PathResolver resolver;
    for (const QString &ini : candidates) {
        if (!QFileInfo(ini).isFile())
            continue;
        const auto opts = iniSection(ini, QStringLiteral("MusicLibrary.TAIMPMLLocalDataStorage.SearchOptions"));
        auto list = [&](const QString &count, const QString &item) {
            QStringList paths;
            for (int i = 0; i < opts.value(count).toInt(); ++i)
                paths << resolver.resolve(opts.value(item + QString::number(i)));
            return cleanFolders(paths);
        };
        const QStringList folders = list(QStringLiteral("SearchPathCount"), QStringLiteral("SearchPath"));
        if (folders.isEmpty())
            continue;
        out.folders = folders;
        out.excluded = list(QStringLiteral("ExcludedPathCount"), QStringLiteral("ExcludedPath"));
        out.watch = true;   // AIMP follows folder changes unless switched off
        if (source)
            *source = ini;
        return true;
    }
    return false;
}

// --- Scanning ------------------------------------------------------------------

TrackList LocalLibrary::scan(const LocalFilesSettings &settings, const TrackList &known, QStringList *dirs)
{
    QHash<QString, Track> byKey;
    for (const Track &t : known)
        byKey.insert(trackKey(t), t);

    // A folder that is not there, or empty, is most likely a drive that is
    // not mounted: its tracks stay, marked missing, with their dates and tags.
    QStringList folders, unavailable;
    for (const QString &f : cleanFolders(settings.folders)) {
        if (QFileInfo(f).isDir() && !QDir(f).isEmpty())
            folders << f;
        else
            unavailable << f;
    }
    TrackList found = MediaCollector::collect(folders, cleanFolders(settings.excluded), dirs);

    // Files that vanished from where they were: one appearing elsewhere with
    // the same size and mtime was moved or renamed. It keeps its date added
    // (where the date comes from the inode change time, a move would reset it)
    // and its tags.
    QSet<QString> present;
    for (const Track &t : found)
        present.insert(trackKey(t));
    QHash<QPair<qint64, qint64>, int> gone;   // (size, mtime) -> index in known, -1 if not unique
    for (int i = 0; i < known.size(); ++i) {
        const Track &k = known[i];
        if (k.isCueTrack() || !k.scanned || k.fileSize <= 0 || k.modifiedMs <= 0 || present.contains(trackKey(k)))
            continue;
        const auto key = qMakePair(k.fileSize, k.modifiedMs);
        gone.insert(key, gone.contains(key) ? -1 : i);
    }

    TrackList out;
    out.reserve(found.size());
    for (Track &t : found) {
        const QFileInfo fi(t.path);
        const qint64 modified = FileTimes::modifiedMs(fi);
        const auto it = byKey.constFind(trackKey(t));
        if (it == byKey.constEnd()) {
            const int moved = t.isCueTrack() ? -1 : gone.value(qMakePair(fi.size(), modified), -1);
            if (moved >= 0) {
                gone.insert(qMakePair(fi.size(), modified), -1);   // one new place per file
                Track m = known[moved];
                m.path = t.path;
                m.missing = false;   // marked when its old path failed to play: here it is
                out.append(m);
                continue;
            }
            t.addedMs = FileTimes::addedMs(fi);   // new
            t.modifiedMs = modified;
            out.append(t);
            continue;
        }
        // Retagging may keep the size, so the mtime counts too (0: cache
        // written before dates were kept).
        const bool unchanged = !it->missing && (it->isCueTrack() || it->fileSize == fi.size())
                               && (it->modifiedMs == 0 || it->modifiedMs == modified);
        if (!it->scanned || unchanged) {
            out.append(*it);   // tags not read yet (the list is reading them), or unchanged: keep them
        } else {
            // Changed (or back after being missing): same entry, tags read
            // again. The new size marks it as changed for sameList().
            t.fileSize = fi.size();
            out.append(t);
        }
        Track &kept = out.last();
        kept.uid = it->uid;
        kept.addedMs = it->addedMs > 0 ? it->addedMs : FileTimes::addedMs(fi);   // first seen stays
        kept.modifiedMs = modified;
    }

    for (const QString &root : std::as_const(unavailable)) {
        const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
        for (const Track &k : known) {
            if (k.path.startsWith(prefix) && !present.contains(trackKey(k))) {
                out.append(k);
                out.last().missing = true;
            }
        }
    }
    return out;
}

bool LocalLibrary::sameList(const TrackList &shown, const TrackList &scanned)
{
    if (shown.size() != scanned.size())
        return false;
    for (int i = 0; i < shown.size(); ++i) {
        const Track &n = scanned.at(i);
        const Track &o = shown.at(i);
        if (n.uid != o.uid || n.path != o.path || n.missing != o.missing || n.addedMs != o.addedMs
            || n.modifiedMs != o.modifiedMs
            || (!n.scanned && n.fileSize > 0))
            return false;
    }
    return true;
}

// --- LocalFiles ----------------------------------------------------------------

LocalFiles::LocalFiles(QObject *parent)
    : QObject(parent)
{
    bool firstRun = false;
    m_settings = LocalFilesSettings::load(&firstRun);
    if (firstRun && !m_settings.folders.isEmpty())
        m_settings.save();   // keep AIMP's folders even if AIMP goes away later
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(2000);   // copying an album touches a folder many times
    connect(&m_fsWatcher, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_debounce, &QTimer::timeout, this, &LocalFiles::changedOnDisk);
    connect(&m_watcher, &QFutureWatcher<QPair<TrackList, QStringList>>::finished, this, [this] {
        const auto result = m_watcher.result();
        watchDirs(result.second);
        emit scanned(result.first, m_scanGeneration);
        if (m_rescanPending) {
            // Asked for while this scan ran: rescan from what is shown now,
            // with this result applied (an older list would give the files
            // found by this scan new uids).
            m_rescanPending = false;
            emit changedOnDisk();
        }
        if (!m_watcher.isRunning())
            emit scanningChanged(false);
    });
}

// A scan still running works on its own copies and is not waited for: it can
// take minutes on a big or slow drive. See main() for the end of the process.
LocalFiles::~LocalFiles() = default;

void LocalFiles::setSettings(const LocalFilesSettings &s)
{
    m_settings = s;
    m_settings.save();
}

QString LocalFiles::cacheFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/local-files.xspf");
}

TrackList LocalFiles::loadCache(const QString &file)
{
    PlaylistData data;
    PlaylistIO::load(file, data);   // damaged: what could be read; the rescan reads the rest
    return data.tracks;
}

void LocalFiles::saveCache(const TrackList &tracks) const
{
    const QString file = cacheFile();
    QDir().mkpath(QFileInfo(file).absolutePath());
    PlaylistIO::save(file, {QStringLiteral("Локальные файлы"), tracks});
}

void LocalFiles::rescan(const TrackList &known)
{
    if (m_watcher.isRunning()) {
        m_rescanPending = true;
        return;
    }
    emit scanningChanged(true);
    const LocalFilesSettings s = m_settings;
    m_scanGeneration = m_sortGeneration;
    m_watcher.setFuture(QtConcurrent::run([s, known, sorted = m_sorted, key = m_sortKey, order = m_sortOrder] {
        QStringList dirs;
        TrackList tracks = LocalLibrary::scan(s, known, &dirs);
        if (sorted)
            TrackSort::sort(tracks, key, order);   // here, not on the GUI thread
        return qMakePair(tracks, dirs);
    }));
}

void LocalFiles::setSort(bool sorted, TrackSort::Key key, Qt::SortOrder order)
{
    m_sorted = sorted;
    m_sortKey = key;
    m_sortOrder = order;
    ++m_sortGeneration;
}

void LocalFiles::watchDirs(const QStringList &dirs)
{
    if (!m_fsWatcher.directories().isEmpty())
        m_fsWatcher.removePaths(m_fsWatcher.directories());
    if (!m_settings.watch || dirs.isEmpty())
        return;
    // inotify watches are a limited resource (8192 by default on many systems).
    constexpr int kMaxWatched = 4000;
    m_fsWatcher.addPaths(dirs.mid(0, kMaxWatched));
}
