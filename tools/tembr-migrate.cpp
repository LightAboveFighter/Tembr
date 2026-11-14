// tembr-migrate: moves playlists from AIMP to Tembr (or to plain playlist files).
//
//   tembr-migrate aimp    [--profile N] [--all] [--library DIR] [--out DIR] [--format FMT]
//   tembr-migrate phones
//   tembr-migrate android [--device N | --folder PATH] [--library DIR] [--copy-to DIR]
//                         [--only NAME,...] [--out DIR] [--format FMT]
//
// Without --out nothing is written: the playlists are listed with how many of
// their tracks are found. FMT: xspf (default, keeps everything), m3u8, aimppl4.

#include "core/PathResolver.h"
#include "core/PlaylistIO.h"
#include "core/Relocator.h"
#include "migration/AimpProfiles.h"
#include "migration/PhoneImport.h"
#include "migration/PhoneSources.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

#include <algorithm>

#include <cstdio>

using namespace Migration;

namespace {

struct Args
{
    QString command;
    QHash<QString, QString> opts;   // --key value
    QSet<QString> flags;            // --key

    QString value(const char *key) const { return opts.value(QLatin1String(key)); }
    bool has(const char *key) const { return flags.contains(QLatin1String(key)) || opts.contains(QLatin1String(key)); }
};

Args parseArgs(const QStringList &argv)
{
    static const QSet<QString> withValue = {QStringLiteral("profile"), QStringLiteral("out"), QStringLiteral("format"),
                                            QStringLiteral("device"), QStringLiteral("folder"), QStringLiteral("library"),
                                            QStringLiteral("copy-to"), QStringLiteral("only")};
    Args a;
    for (int i = 1; i < argv.size(); ++i) {
        const QString arg = argv[i];
        if (!arg.startsWith(QLatin1String("--"))) {
            if (a.command.isEmpty())
                a.command = arg;
            continue;
        }
        const QString key = arg.mid(2);
        if (withValue.contains(key) && i + 1 < argv.size())
            a.opts.insert(key, argv[++i]);
        else
            a.flags.insert(key);
    }
    return a;
}

void out(const QString &s)
{
    std::fputs(s.toUtf8().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int usage()
{
    out(QStringLiteral(
        "tembr-migrate — перенос плейлистов из AIMP\n\n"
        "  tembr-migrate aimp    [--profile N] [--all] [--library DIR] [--out DIR] [--format FMT]\n"
        "      AIMP для Windows на этом компьютере (Wine, Bottles, ...). По умолчанию — открытые\n"
        "      вкладки AIMP в его порядке; --all добавляет старые копии из папки PLS;\n"
        "      --library DIR[;DIR] ищет по имени файлы, которых нет на старом месте\n"
        "      (по умолчанию — папки фонотеки AIMP).\n\n"
        "  tembr-migrate phones\n"
        "      Подключённые телефоны: «Передача файлов» (MTP) и USB-отладка (adb).\n\n"
        "  tembr-migrate android [--device N | --folder PATH] [--library DIR] [--copy-to DIR]\n"
        "                        [--only ИМЯ,ИМЯ] [--out DIR] [--format FMT]\n"
        "      Плейлисты с телефона. Треки ищутся по имени файла в --library (музыка на ПК);\n"
        "      ненайденные копируются с телефона в --copy-to.\n\n"
        "Без --out ничего не записывается, только отчёт. FMT: xspf (по умолчанию), m3u8, aimppl4.\n"
        "Перемещения папок, найденные плеером («Найти перемещённые файлы»), учитываются."));
    return 2;
}

QString safeFileName(QString name)
{
    static const QRegularExpression bad(QStringLiteral("[/\\\\:*?\"<>|]"));
    name.replace(bad, QStringLiteral("_"));
    name = name.trimmed();
    return name.isEmpty() ? QStringLiteral("playlist") : name;
}

bool writePlaylist(const QString &outDir, const QString &format, const PlaylistData &data, QString *written)
{
    QDir().mkpath(outDir);
    QString file = QDir(outDir).filePath(safeFileName(data.name) + QLatin1Char('.') + format);
    for (int n = 2; QFileInfo::exists(file); ++n)
        file = QDir(outDir).filePath(QStringLiteral("%1 (%2).%3").arg(safeFileName(data.name)).arg(n).arg(format));
    QString error;
    if (!PlaylistIO::save(file, data, &error)) {
        out(QStringLiteral("  ошибка записи %1: %2").arg(file, error));
        return false;
    }
    *written = file;
    return true;
}

// The same folder moves the player learned (shared settings).
void loadPlayerRemaps()
{
    QSettings s(QStringLiteral("tembr"), QStringLiteral("tembr"));
    const QStringList flat = s.value(QStringLiteral("paths/remaps")).toStringList();
    QList<PathResolver::Remap> remaps;
    for (int i = 0; i + 1 < flat.size(); i += 2)
        remaps.append({flat[i], flat[i + 1]});
    PathResolver::setRemaps(remaps);
}

// --library DIR[;DIR...], or else the folders of AIMP's music library.
Relocator::Index buildLibrary(const Args &a, const AimpProfile &profile)
{
    QStringList roots;
    if (a.has("library")) {
        for (const QString &d : a.value("library").split(QLatin1Char(';'), Qt::SkipEmptyParts))
            roots << d.trimmed();
    } else if (!profile.dir.isEmpty()) {
        roots = aimpLibraryFolders(profile);
        if (!roots.isEmpty())
            out(QStringLiteral("Музыка — папки фонотеки AIMP (другие: --library)."));
    }
    if (roots.isEmpty())
        return {};
    out(QStringLiteral("Индексирую музыку в %1…").arg(roots.join(QStringLiteral(", "))));
    Relocator::Index index = Relocator::build(roots);
    out(QStringLiteral("  %1 аудиофайлов").arg(index.files));
    return index;
}

int runAimp(const Args &a)
{
    const QList<AimpProfile> profiles = findAimpProfiles();
    if (profiles.isEmpty()) {
        out(QStringLiteral("AIMP не найден (искал в Wine, Bottles, PlayOnLinux, Lutris, Proton)."));
        return 1;
    }
    out(QStringLiteral("Профили AIMP:"));
    for (int i = 0; i < profiles.size(); ++i)
        out(QStringLiteral("  [%1] %2  —  %3").arg(i).arg(profiles[i].label, profiles[i].dir));
    const int index = a.value("profile").toInt();
    if (index < 0 || index >= profiles.size())
        return usage();

    const AimpSession session = readAimpSession(profiles[index]);
    const Relocator::Index library = buildLibrary(a, profiles[index]);
    const QString format = a.value("format").isEmpty() ? QStringLiteral("xspf") : a.value("format");
    out(QStringLiteral("\nПлейлисты профиля [%1]:").arg(index));
    int total = 0, found = 0;
    // With --all, versions of the same playlist stand together, newest first.
    for (int i : groupedByName(session, a.has("all"))) {
        const AimpPlaylistInfo &info = session.playlists[i];
        PlaylistData data;
        QString error;
        if (!loadAimpPlaylist(info, data, &error)) {
            out(QStringLiteral("  %1: %2").arg(info.name, error));
            continue;
        }
        const int relocated = library.files > 0 ? Relocator::relocateMissing(data.tracks, library) : 0;
        const int ok = int(std::count_if(data.tracks.cbegin(), data.tracks.cend(), [](const Track &t) { return !t.missing; }));
        total += int(data.tracks.size());
        found += ok;
        QString marks;
        if (i == session.activeIndex)
            marks += QStringLiteral(" [активный]");
        if (i == session.playingIndex)
            marks += QStringLiteral(" [играл]");
        if (!info.open)
            marks += QStringLiteral(" [старая копия: %1, %2]")
                         .arg(QFileInfo(info.file).fileName(), info.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        else if (a.has("all"))
            marks += QStringLiteral(" [%1]").arg(info.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        if (relocated > 0)
            marks += QStringLiteral(" (из них %1 найдено в --library)").arg(relocated);
        QString line = QStringLiteral("  %1  %2 треков, найдено %3%4").arg(info.name, -28).arg(data.tracks.size(), 5).arg(ok, 5).arg(marks);
        QString written;
        if (a.has("out") && writePlaylist(a.value("out"), format, data, &written))
            line += QStringLiteral("  ->  ") + written;
        out(line);
    }
    out(QStringLiteral("\nИтого: %1 треков, найдено на диске %2.").arg(total).arg(found));
    if (found < total)
        out(QStringLiteral("Ненайденные файлы можно разыскать в плеере: «Плейлист → Найти перемещённые файлы»."));
    return 0;
}

int runPhones()
{
    const PhoneDiscovery d = discoverPhones();
    if (d.sources.isEmpty())
        out(QStringLiteral("Телефоны не найдены. Подключите телефон кабелем и выберите режим USB «Передача файлов»."));
    for (int i = 0; i < d.sources.size(); ++i) {
        out(QStringLiteral("[%1] %2").arg(i).arg(d.sources[i]->label()));
        for (const PhoneStorage &s : d.sources[i]->storages())
            out(QStringLiteral("      %1%2  —  %3").arg(s.name, s.primary ? QStringLiteral(" (основная)") : QString(), s.root));
    }
    for (const QString &n : d.notes)
        out(QStringLiteral("! ") + n);
    return d.sources.isEmpty() ? 1 : 0;
}

int runAndroid(const Args &a)
{
    PhoneSourcePtr source;
    if (a.has("folder")) {
        source = folderSource(a.value("folder"));
    } else {
        const PhoneDiscovery d = discoverPhones();
        for (const QString &n : d.notes)
            out(QStringLiteral("! ") + n);
        const int index = a.value("device").toInt();
        if (index < 0 || index >= d.sources.size()) {
            out(QStringLiteral("Телефон не найден (см. tembr-migrate phones)."));
            return 1;
        }
        source = d.sources[index];
    }
    out(QStringLiteral("Источник: %1").arg(source->label()));

    QString error;
    out(QStringLiteral("Поиск плейлистов на телефоне…"));
    QList<PhonePlaylistFile> files = source->findPlaylists(&error);
    if (!error.isEmpty())
        out(QStringLiteral("! ") + error);
    const QStringList only = a.value("only").split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (!only.isEmpty()) {
        files.erase(std::remove_if(files.begin(), files.end(), [&](const PhonePlaylistFile &f) {
                        return !only.contains(QFileInfo(f.path).completeBaseName(), Qt::CaseInsensitive);
                    }),
                    files.end());
    }
    if (files.isEmpty()) {
        out(QStringLiteral("Плейлистов не найдено. В AIMP для Android: меню плейлиста → «Экспорт» (сохраните, например, в Music)."));
        return 1;
    }

    const QList<AimpProfile> profiles = findAimpProfiles();
    const Relocator::Index library = buildLibrary(a, profiles.isEmpty() ? AimpProfile() : profiles.first());
    PhoneImportOptions options;
    if (library.files > 0)
        options.library = &library;
    if (a.has("copy-to") && a.has("out"))
        options.copyMissingTo = a.value("copy-to");
    else if (a.has("copy-to"))
        out(QStringLiteral("(--copy-to без --out: пробный запуск, ничего не копирую)"));

    const QString format = a.value("format").isEmpty() ? QStringLiteral("xspf") : a.value("format");
    PhoneImportStats total;
    for (const PhonePlaylistFile &f : std::as_const(files)) {
        PlaylistData data;
        PhoneImportStats stats;
        if (!importPhonePlaylist(*source, f, options, data, stats, {}, &error)) {
            out(QStringLiteral("  %1: %2").arg(f.path, error));
            continue;
        }
        total += stats;
        QString line = QStringLiteral("  %1  %2 треков: на ПК %3, скопировано %4, нет %5")
                           .arg(data.name, -28)
                           .arg(stats.tracks, 5)
                           .arg(stats.matched, 5)
                           .arg(stats.copied)
                           .arg(stats.missing);
        QString written;
        if (a.has("out") && writePlaylist(a.value("out"), format, data, &written))
            line += QStringLiteral("  ->  ") + written;
        out(line);
        for (const QString &e : std::as_const(stats.errors).mid(0, 3))
            out(QStringLiteral("      ! ") + e);
    }
    out(QStringLiteral("\nИтого: %1 треков, на ПК найдено %2, скопировано %3, не найдено %4%5.")
            .arg(total.tracks)
            .arg(total.matched)
            .arg(total.copied)
            .arg(total.missing)
            .arg(total.streams ? QStringLiteral(", потоков %1").arg(total.streams) : QString()));
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const Args a = parseArgs(app.arguments());
    loadPlayerRemaps();
    if (a.command == QLatin1String("aimp"))
        return runAimp(a);
    if (a.command == QLatin1String("phones"))
        return runPhones();
    if (a.command == QLatin1String("android"))
        return runAndroid(a);
    return usage();
}
