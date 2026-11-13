#pragma once

#include "core/Track.h"

#include <QStringList>

namespace MediaCollector {

QStringList audioExtensions();
bool isAudioFile(const QString &path);

// Expands files, folders (recursively) and playlist files into tracks, in
// natural order ("2.mp3" before "10.mp3"). Inside a folder, a .cue sheet
// replaces the audio file it describes. Blocking: call it off the GUI thread
// for big folders.
TrackList collect(const QStringList &paths);

} // namespace MediaCollector
