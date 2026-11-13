#pragma once

#include "core/Track.h"

#include <QString>
#include <QStringList>

#include <functional>

struct PlaylistData
{
    QString name;
    TrackList tracks;
};

namespace PlaylistIO {

// Extensions (lower-case, without dot).
QStringList readableFormats();   // m3u m3u8 pls xspf cue aimppl4
QStringList writableFormats();   // m3u8 m3u pls xspf aimppl4
bool isPlaylistFile(const QString &path);

// Loads any readable format, chosen by extension. Relative and Windows-style
// paths are resolved against the playlist's folder. Entries that point to a
// .cue sheet are expanded into its tracks.
bool load(const QString &file, PlaylistData &out, QString *error = nullptr);

// Saves in the format given by the extension. XSPF keeps everything (CUE
// ranges and scanned tags, via <meta>); it is also used for the session.
bool save(const QString &file, const PlaylistData &data, QString *error = nullptr);

// Parses a CUE sheet into virtual tracks.
TrackList loadCue(const QString &cueFile, QString *albumTitle = nullptr);

// Maps a raw playlist entry to the path stored in the Track. The default
// (PathResolver) makes local paths; migration uses its own for phone paths.
using EntryResolver = std::function<QString(const QString &entry)>;

// Parses playlist data of `format` ("m3u8", "xspf", ...; not "cue") with a
// custom path mapping. Unknown formats give an empty list.
TrackList parse(const QByteArray &data, const QString &format, const EntryResolver &resolve, QString *name = nullptr);

// Format-specific parsers, exposed for tests. `baseDir` resolves relative paths.
TrackList parseM3u(const QString &text, const QString &baseDir, QString *name = nullptr);
TrackList parsePls(const QString &text, const QString &baseDir);
TrackList parseXspf(const QByteArray &xml, const QString &baseDir, QString *name = nullptr);
TrackList parseAimppl4(const QString &text, const QString &baseDir, QString *name = nullptr);
TrackList parseM3u(const QString &text, const EntryResolver &resolve, QString *name = nullptr);
TrackList parsePls(const QString &text, const EntryResolver &resolve);
TrackList parseXspf(const QByteArray &xml, const EntryResolver &resolve, QString *name = nullptr);
TrackList parseAimppl4(const QString &text, const EntryResolver &resolve, QString *name = nullptr);
TrackList parseCue(const QString &text, const QString &cueFile, QString *albumTitle = nullptr);

} // namespace PlaylistIO
