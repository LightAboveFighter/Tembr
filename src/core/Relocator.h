#pragma once

#include "core/PathResolver.h"
#include "core/Track.h"

#include <QHash>
#include <QStringList>

// Finds files that were moved: indexes a folder by file name and matches
// missing tracks against it.
namespace Relocator {

struct Index
{
    static constexpr int kSuffixDirs = 3;

    QHash<QString, QStringList> byName;   // lower-case file name -> full paths
    // bySuffix[k-1]: "album/01 - intro.mp3" (k = 1), "artist/album/01 - intro.mp3" (k = 2), ...
    // used to tell apart files with the same name in different folders.
    QHash<QString, QStringList> bySuffix[kSuffixDirs];
    int files = 0;
};

// Indexes audio files under the folders (blocking: run off the GUI thread).
Index build(const QString &root);
Index build(const QStringList &roots);

// New location of a missing track, or an empty string. Several files with
// the same name are narrowed down by the folders the track was in (album,
// then artist, ...), then by file size and exact letter case. If it is still
// ambiguous nothing is returned: a wrong song is worse than a missing one.
QString find(const Track &t, const Index &index);

// Points missing local tracks of `tracks` at files found in `index`.
// Appends (old path, new path) to `moves` if given; returns how many.
int relocateMissing(TrackList &tracks, const Index &index, QList<QPair<QString, QString>> *moves = nullptr);

// Folder-level moves (old folder -> new folder), most frequent first.
QList<PathResolver::Remap> deriveRemaps(const QList<QPair<QString, QString>> &moves);

} // namespace Relocator
