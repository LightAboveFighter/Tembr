#pragma once

#include "core/PlaylistIO.h"

#include <QList>
#include <QStringList>

// The open playlists of a session as files: <dir>/000.xspf, 001.xspf, ...
namespace SessionFiles {

// Writes each playlist atomically. Files left from before are removed only
// when every one was written. Returns "name: error" for those that failed.
QStringList save(const QString &dir, const QList<PlaylistData> &playlists);

struct Loaded
{
    QList<PlaylistData> playlists;   // in file order
    QStringList keptAside;           // copies of files that could not be read in full
};

// Reads every NNN.xspf there (also past a stale count: a crash between
// writing the files and the settings leaves more of them). The next save
// replaces these files, so a damaged one is first copied aside; what could be
// read from it is still loaded, and one that gave nothing is left out.
Loaded load(const QString &dir);

} // namespace SessionFiles
