// Diagnoses playlists: how many entries resolve to existing files.
//
//   playlist_probe [--search <music folder>] [--resave <dir>] <playlist files...>
//
// --search  also tries to find missing files by name in that folder (what
//           "Find moved files" in the player does);
// --resave  writes every playlist back as .aimppl4 into <dir> (format check).

#include "core/PlaylistIO.h"
#include "core/Relocator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <cstdio>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    QString searchDir, resaveDir;
    for (int i = 0; i + 1 < args.size();) {
        if (args[i] == QLatin1String("--search")) {
            searchDir = args[i + 1];
            args.remove(i, 2);
        } else if (args[i] == QLatin1String("--resave")) {
            resaveDir = args[i + 1];
            args.remove(i, 2);
        } else {
            ++i;
        }
    }
    if (args.isEmpty()) {
        std::fprintf(stderr, "usage: playlist_probe [--search DIR] [--resave DIR] FILES...\n");
        return 2;
    }

    Relocator::Index index;
    if (!searchDir.isEmpty()) {
        index = Relocator::build(searchDir);
        std::printf("indexed %d audio files in %s\n", index.files, qPrintable(searchDir));
    }

    int total = 0, found = 0, relocatable = 0;
    for (const QString &file : std::as_const(args)) {
        PlaylistData data;
        QString error;
        if (!PlaylistIO::load(file, data, &error)) {
            std::printf("%-40s  ERROR %s\n", qPrintable(QFileInfo(file).fileName()), qPrintable(error));
            continue;
        }
        int ok = 0, reloc = 0;
        QString firstMissing;
        for (const Track &t : std::as_const(data.tracks)) {
            if (!t.missing) {
                ++ok;
            } else {
                if (firstMissing.isEmpty())
                    firstMissing = t.path;
                if (!searchDir.isEmpty() && !Relocator::find(t, index).isEmpty())
                    ++reloc;
            }
        }
        total += int(data.tracks.size());
        found += ok;
        relocatable += reloc;
        std::printf("%-40s %5lld tracks, %5d found, %5lld missing", qPrintable(data.name.left(40)),
                    qlonglong(data.tracks.size()), ok, qlonglong(data.tracks.size() - ok));
        if (!searchDir.isEmpty())
            std::printf(", %5d relocatable", reloc);
        std::printf("\n");
        if (!firstMissing.isEmpty())
            std::printf("    e.g. missing: %s\n", qPrintable(firstMissing));

        if (!resaveDir.isEmpty()) {
            QDir().mkpath(resaveDir);
            PlaylistIO::save(QDir(resaveDir).filePath(QFileInfo(file).completeBaseName() + QStringLiteral(".aimppl4")),
                             data, &error);
        }
    }
    std::printf("\nTOTAL %d tracks, %d found, %d missing", total, found, total - found);
    if (!searchDir.isEmpty())
        std::printf(", %d of the missing found by name in %s", relocatable, qPrintable(searchDir));
    std::printf("\n");
    return 0;
}
