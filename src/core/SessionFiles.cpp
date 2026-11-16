#include "core/SessionFiles.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSet>

namespace SessionFiles {

QStringList save(const QString &dir, const QList<PlaylistData> &playlists)
{
    QDir().mkpath(dir);
    QSet<QString> written;
    QStringList failed;
    for (int i = 0; i < playlists.size(); ++i) {
        const QString name = QStringLiteral("%1.xspf").arg(i, 3, 10, QLatin1Char('0'));
        QString error;
        if (PlaylistIO::save(dir + QLatin1Char('/') + name, playlists[i], &error))
            written.insert(name);
        else
            failed << QStringLiteral("%1: %2").arg(playlists[i].name, error);
    }
    if (failed.isEmpty()) {
        for (const QString &old : QDir(dir).entryList({QStringLiteral("*.xspf")}, QDir::Files)) {
            if (!written.contains(old))
                QFile::remove(dir + QLatin1Char('/') + old);
        }
    }
    return failed;
}

Loaded load(const QString &dir)
{
    Loaded out;
    const QStringList files = QDir(dir).entryList({QStringLiteral("[0-9][0-9][0-9].xspf")}, QDir::Files, QDir::Name);
    for (const QString &name : files) {
        const QString file = dir + QLatin1Char('/') + name;
        PlaylistData data;
        if (!PlaylistIO::load(file, data)) {
            const QString kept = file + QDateTime::currentDateTime().toString(QStringLiteral(".yyyyMMdd-HHmmss"))
                                 + QStringLiteral(".damaged");
            if (QFile::copy(file, kept))
                out.keptAside << kept;
            if (data.tracks.isEmpty())
                continue;
        }
        out.playlists << data;
    }
    return out;
}

} // namespace SessionFiles
