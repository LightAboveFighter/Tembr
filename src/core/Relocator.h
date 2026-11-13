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
    QHash<QString, QStringList> byName;   // lower-case file name -> full paths
    int files = 0;
};

Index build(const QString &root);   // blocking, run off the GUI thread

// New location of a missing track, or an empty string. With several files of
// the same name, prefers the same size, then the same parent folder name.
QString find(const Track &t, const Index &index);

// Folder-level moves (old folder -> new folder), most frequent first.
QList<PathResolver::Remap> deriveRemaps(const QList<QPair<QString, QString>> &moves);

} // namespace Relocator
