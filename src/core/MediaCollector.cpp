#include "core/MediaCollector.h"

#include "core/PlaylistIO.h"

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace MediaCollector {

namespace {

void sortNatural(QFileInfoList &list)
{
    QCollator coll;
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(list.begin(), list.end(), [&](const QFileInfo &a, const QFileInfo &b) {
        return coll.compare(a.fileName(), b.fileName()) < 0;
    });
}

bool isExcluded(const QString &dir, const QStringList &excluded)
{
    for (const QString &e : excluded) {
        if (dir == e || dir.startsWith(e + QLatin1Char('/')))
            return true;
    }
    return false;
}

void collectDir(const QString &dirPath, TrackList &out, QSet<QString> &visited, const QStringList &excluded,
                QStringList *visitedDirs)
{
    const QString canonical = QFileInfo(dirPath).canonicalFilePath();
    if (canonical.isEmpty() || visited.contains(canonical))
        return;   // symlink loop
    if (isExcluded(QFileInfo(dirPath).absoluteFilePath(), excluded) || isExcluded(canonical, excluded))
        return;
    visited.insert(canonical);
    if (visitedDirs)
        visitedDirs->append(QFileInfo(dirPath).absoluteFilePath());

    const QDir dir(dirPath);
    QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::Readable);
    QFileInfoList subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
    sortNatural(files);
    sortNatural(subdirs);

    // CUE sheets first: the files they reference are not listed on their own.
    QSet<QString> coveredByCue;
    TrackList cueTracks;
    for (const QFileInfo &fi : std::as_const(files)) {
        if (fi.suffix().compare(QLatin1String("cue"), Qt::CaseInsensitive) != 0)
            continue;
        const TrackList tracks = PlaylistIO::loadCue(fi.absoluteFilePath());
        for (const Track &t : tracks)
            coveredByCue.insert(QFileInfo(t.path).absoluteFilePath());
        cueTracks += tracks;
    }
    out += cueTracks;

    for (const QFileInfo &fi : std::as_const(files)) {
        if (isAudioFile(fi.fileName()) && !coveredByCue.contains(fi.absoluteFilePath()))
            out.append(Track::fromPath(fi.absoluteFilePath()));
    }
    for (const QFileInfo &fi : std::as_const(subdirs))
        collectDir(fi.absoluteFilePath(), out, visited, excluded, visitedDirs);
}

} // namespace

QStringList audioExtensions()
{
    return {QStringLiteral("mp3"),  QStringLiteral("flac"), QStringLiteral("ogg"),  QStringLiteral("oga"),
            QStringLiteral("opus"), QStringLiteral("m4a"),  QStringLiteral("m4b"),  QStringLiteral("mp4"),
            QStringLiteral("aac"),  QStringLiteral("alac"), QStringLiteral("wav"),  QStringLiteral("wv"),
            QStringLiteral("ape"),  QStringLiteral("wma"),  QStringLiteral("mpc"),  QStringLiteral("tta"),
            QStringLiteral("aif"),  QStringLiteral("aiff"), QStringLiteral("dsf"),  QStringLiteral("dff"),
            QStringLiteral("ac3"),  QStringLiteral("dts"),  QStringLiteral("mka"),  QStringLiteral("webm"),
            QStringLiteral("spx"),  QStringLiteral("mp2"),  QStringLiteral("mpga"), QStringLiteral("caf")};
}

bool isAudioFile(const QString &path)
{
    static const QStringList exts = audioExtensions();
    return exts.contains(QFileInfo(path).suffix().toLower());
}

TrackList collect(const QStringList &paths, const QStringList &excludedIn, QStringList *visitedDirs)
{
    QStringList excluded;
    for (const QString &e : excludedIn)
        excluded << QDir::cleanPath(QFileInfo(e).absoluteFilePath());
    TrackList out;
    QSet<QString> visited;
    for (const QString &p : paths) {
        if (p.contains(QLatin1String("://")) && !p.startsWith(QLatin1String("file:"))) {
            out.append(Track::fromPath(p));   // stream URL
            continue;
        }
        const QFileInfo fi(p);
        if (fi.isDir()) {
            collectDir(fi.absoluteFilePath(), out, visited, excluded, visitedDirs);
        } else if (PlaylistIO::isPlaylistFile(p)) {
            PlaylistData data;
            PlaylistIO::load(fi.absoluteFilePath(), data);   // a damaged one still gives what could be read
            out += data.tracks;
        } else if (isAudioFile(p)) {
            out.append(Track::fromPath(fi.absoluteFilePath()));
        }
    }
    return out;
}

} // namespace MediaCollector
