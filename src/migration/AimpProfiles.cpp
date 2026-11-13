#include "migration/AimpProfiles.h"

#include "core/PathResolver.h"
#include "core/TextDecode.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>

namespace Migration {

namespace {

QString tildeHome(const QString &path, const QString &home)
{
    return path.startsWith(home + QLatin1Char('/')) ? QLatin1Char('~') + path.mid(home.size()) : path;
}

QStringList subdirs(const QString &dir)
{
    QStringList out;
    for (const QString &name : QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        out << dir + QLatin1Char('/') + name;
    return out;
}

// AIMP folders inside one Wine prefix (a prefix has drive_c/).
void collectFromPrefix(const QString &prefix, const QString &kind, const QString &home, QList<AimpProfile> &out,
                       QSet<QString> &seen)
{
    const QString driveC = prefix + QStringLiteral("/drive_c");
    if (!QFileInfo(driveC).isDir())
        return;
    QStringList candidates;
    for (const QString &user : subdirs(driveC + QStringLiteral("/users"))) {
        candidates << user + QStringLiteral("/AppData/Roaming/AIMP")
                   << user + QStringLiteral("/Application Data/AIMP");
    }
    // Portable AIMP keeps its profile next to the program.
    for (const QString &pf : {QStringLiteral("/Program Files"), QStringLiteral("/Program Files (x86)")}) {
        for (const QString &app : subdirs(driveC + pf)) {
            if (QFileInfo(app).fileName().startsWith(QLatin1String("AIMP"), Qt::CaseInsensitive))
                candidates << app + QStringLiteral("/Profile");
        }
    }
    for (const QString &c : std::as_const(candidates)) {
        const QString canonical = QFileInfo(c).canonicalFilePath();
        if (canonical.isEmpty() || seen.contains(canonical) || !QFileInfo(canonical + QStringLiteral("/PLS")).isDir())
            continue;
        seen.insert(canonical);
        QString label = QStringLiteral("%1: %2").arg(kind, tildeHome(prefix, home));
        if (c.contains(QLatin1String("/Program Files")))
            label += QStringLiteral(" (portable)");
        out.append({label, canonical});
    }
}

struct Summary
{
    QString name, id;
    int files = 0;
    int cursor = -1;
};

Summary readSummary(const QString &file)
{
    Summary s;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return s;
    // The summary is at the top; 16 KiB is plenty even in UTF-16.
    const QString text = decodeText(f.read(16 * 1024));
    bool inSummary = false;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("#-----"))) {
            if (inSummary)
                break;
            inSummary = line.contains(QLatin1String("SUMMARY"), Qt::CaseInsensitive);
            continue;
        }
        if (!inSummary)
            continue;
        const QString key = line.section(QLatin1Char('='), 0, 0);
        const QString value = line.section(QLatin1Char('='), 1);
        if (key == QLatin1String("Name"))
            s.name = value;
        else if (key == QLatin1String("ID"))
            s.id = value.toUpper();
        else if (key == QLatin1String("ContentFiles"))
            s.files = value.toInt();
        else if (key == QLatin1String("PlaybackCursor"))
            s.cursor = value.toInt();
    }
    return s;
}

// Minimal INI reader: AIMP.ini is UTF-16, which QSettings can't read.
QHash<QString, QHash<QString, QString>> readIni(const QString &file)
{
    QHash<QString, QHash<QString, QString>> out;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QString section;
    for (const QString &raw : decodeText(f.readAll()).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2);
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq > 0)
            out[section].insert(line.left(eq), line.mid(eq + 1));
    }
    return out;
}

} // namespace

QList<AimpProfile> findAimpProfiles(const QString &homeIn)
{
    const QString home = homeIn.isEmpty() ? QDir::homePath() : homeIn;
    QList<AimpProfile> out;
    QSet<QString> seen;

#ifdef Q_OS_WIN
    for (const QString &dir : {qEnvironmentVariable("APPDATA") + QStringLiteral("/AIMP")}) {
        const QString canonical = QFileInfo(dir).canonicalFilePath();
        if (!canonical.isEmpty() && QFileInfo(canonical + QStringLiteral("/PLS")).isDir() && !seen.contains(canonical)) {
            seen.insert(canonical);
            out.append({QStringLiteral("Windows"), canonical});
        }
    }
#endif

    const QString envPrefix = qEnvironmentVariable("WINEPREFIX");
    if (!envPrefix.isEmpty())
        collectFromPrefix(envPrefix, QStringLiteral("Wine"), home, out, seen);
    collectFromPrefix(home + QStringLiteral("/.wine"), QStringLiteral("Wine"), home, out, seen);

    const QList<QPair<QString, QString>> containers = {
        {home + QStringLiteral("/.local/share/wineprefixes"), QStringLiteral("Wine")},
        {home + QStringLiteral("/.PlayOnLinux/wineprefix"), QStringLiteral("PlayOnLinux")},
        {home + QStringLiteral("/.local/share/bottles/bottles"), QStringLiteral("Bottles")},
        {home + QStringLiteral("/.var/app/com.usebottles.bottles/data/bottles/bottles"), QStringLiteral("Bottles")},
        {home + QStringLiteral("/Games"), QStringLiteral("Lutris")},
    };
    for (const auto &c : containers) {
        for (const QString &prefix : subdirs(c.first))
            collectFromPrefix(prefix, c.second, home, out, seen);
    }
    for (const QString &steam : {home + QStringLiteral("/.steam/steam/steamapps/compatdata"),
                                 home + QStringLiteral("/.local/share/Steam/steamapps/compatdata")}) {
        for (const QString &app : subdirs(steam))
            collectFromPrefix(app + QStringLiteral("/pfx"), QStringLiteral("Proton"), home, out, seen);
    }
    return out;
}

AimpSession readAimpSession(const AimpProfile &profile)
{
    AimpSession session;
    QList<AimpPlaylistInfo> all;
    const QFileInfoList files =
        QDir(profile.plsDir()).entryInfoList({QStringLiteral("*.aimppl4")}, QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &fi : files) {
        const Summary s = readSummary(fi.absoluteFilePath());
        AimpPlaylistInfo info;
        info.file = fi.absoluteFilePath();
        info.name = s.name.isEmpty() ? fi.completeBaseName() : s.name;
        info.id = s.id;
        info.trackCount = s.files;
        info.playbackCursor = s.cursor;
        info.modified = fi.lastModified();
        all.append(info);
    }

    const auto ini = readIni(profile.iniPath());
    const auto storage = ini.value(QStringLiteral("Playlist.Storage"));
    const int count = storage.value(QStringLiteral("Count")).toInt();

    QList<AimpPlaylistInfo> open, closed;
    if (count > 0) {
        for (int i = 1; i <= count; ++i) {
            const QString item = storage.value(QStringLiteral("Item%1").arg(i));
            const auto it = std::find_if(all.begin(), all.end(), [&](const AimpPlaylistInfo &p) {
                return QFileInfo(p.file).fileName().compare(item, Qt::CaseInsensitive) == 0;
            });
            if (it == all.end())
                continue;
            it->open = true;
            open.append(*it);
            all.erase(it);
        }
        closed = all;
    } else {
        // No tab list: the newest playlist of each name.
        std::stable_sort(all.begin(), all.end(),
                         [](const AimpPlaylistInfo &a, const AimpPlaylistInfo &b) { return a.modified > b.modified; });
        QSet<QString> names;
        for (AimpPlaylistInfo p : std::as_const(all)) {
            p.open = !names.contains(p.name);
            names.insert(p.name);
            (p.open ? open : closed).append(p);
        }
        std::sort(open.begin(), open.end(), [](const AimpPlaylistInfo &a, const AimpPlaylistInfo &b) {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
    }
    std::sort(closed.begin(), closed.end(), [](const AimpPlaylistInfo &a, const AimpPlaylistInfo &b) {
        const int c = a.name.compare(b.name, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.modified > b.modified;
    });
    session.playlists = open + closed;

    const auto manager = ini.value(QStringLiteral("Playlist.Manager"));
    const QString active = manager.value(QStringLiteral("ActiveStorage")).toUpper();
    const QString playing = manager.value(QStringLiteral("PlayingStorage")).toUpper();
    for (int i = 0; i < session.playlists.size(); ++i) {
        const QString &id = session.playlists[i].id;
        if (id.isEmpty())
            continue;
        if (id == active)
            session.activeIndex = i;
        if (id == playing)
            session.playingIndex = i;
    }
    return session;
}

QStringList aimpLibraryFolders(const AimpProfile &profile, QStringList *excluded)
{
    const auto opts = readIni(profile.iniPath()).value(QStringLiteral("MusicLibrary.TAIMPMLLocalDataStorage.SearchOptions"));
    const PathResolver resolver;
    auto list = [&](const QString &countKey, const QString &itemKey) {
        QStringList out;
        const int n = opts.value(countKey).toInt();
        for (int i = 0; i < n; ++i) {
            QString p = resolver.resolve(opts.value(itemKey + QString::number(i)));
            while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
                p.chop(1);
            if (!p.isEmpty() && !out.contains(p))
                out << p;
        }
        return out;
    };
    if (excluded)
        *excluded = list(QStringLiteral("ExcludedPathCount"), QStringLiteral("ExcludedPath"));
    return list(QStringLiteral("SearchPathCount"), QStringLiteral("SearchPath"));
}

QList<int> groupedByName(const AimpSession &session, bool includeClosed)
{
    // Group key -> members; the group sits where its first open member is.
    QHash<QString, QList<int>> groups;
    QHash<QString, int> groupPos;
    for (int i = 0; i < session.playlists.size(); ++i) {
        const AimpPlaylistInfo &p = session.playlists[i];
        if (!p.open && !includeClosed)
            continue;
        const QString key = p.name.toLower();
        groups[key].append(i);
        if (p.open && !groupPos.contains(key))
            groupPos.insert(key, i);
    }
    QStringList keys = groups.keys();
    std::sort(keys.begin(), keys.end(), [&](const QString &a, const QString &b) {
        const bool ao = groupPos.contains(a), bo = groupPos.contains(b);
        if (ao != bo)
            return ao;                                    // groups with an open tab first
        if (ao)
            return groupPos.value(a) < groupPos.value(b); // in AIMP's tab order
        return a < b;
    });
    QList<int> order;
    for (const QString &key : std::as_const(keys)) {
        QList<int> members = groups.value(key);
        std::stable_sort(members.begin(), members.end(), [&](int a, int b) {
            return session.playlists[a].modified > session.playlists[b].modified;   // newest first
        });
        order += members;
    }
    return order;
}

bool loadAimpPlaylist(const AimpPlaylistInfo &info, PlaylistData &out, QString *error)
{
    if (!PlaylistIO::load(info.file, out, error))
        return false;
    if (!info.name.isEmpty())
        out.name = info.name;
    return true;
}

} // namespace Migration
