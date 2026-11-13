#pragma once

#include "core/Track.h"

#include <QImage>
#include <QList>
#include <QPair>

namespace MetadataReader {

// Fills tags and technical info of `t` from its file (thread-safe; used from
// a thread pool). For CUE tracks the tags from the sheet are kept and only
// missing ones are taken from the file.
Track read(Track t);

// Embedded cover art, or cover.jpg/folder.jpg/... next to the file.
QImage readCover(const QString &path);

// Every tag present in the file, for the "track info" dialog.
QList<QPair<QString, QString>> readAllTags(const QString &path);

} // namespace MetadataReader
