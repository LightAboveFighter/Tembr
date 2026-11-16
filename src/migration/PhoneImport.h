#pragma once

#include "core/PlaylistIO.h"
#include "core/Relocator.h"
#include "migration/PhoneSources.h"

#include <QHash>

#include <functional>

namespace Migration {

// Where a playlist entry points on the phone.
struct PhoneRef
{
    enum Kind { File, Url, Unmappable };
    Kind kind = File;
    int storage = -1;   // index in storages(); -1 = unknown volume, try all
    QString rel;        // path relative to the storage root
    QString raw;        // the entry as written in the playlist
};

// Understands what Android playlists contain: paths relative to the
// playlist, /storage/emulated/0/…, /sdcard/…, /storage/<SD id>/…,
// file:// and SAF content://…/document/primary%3AMusic%2F… URIs.
PhoneRef mapPhoneEntry(const QString &entry, const QList<PhoneStorage> &storages, int playlistStorage,
                       const QString &playlistDir);

// Files placed in the copy folder during one import: local path (lower
// case) -> the phone file it holds. Shared by the playlists of one import, so
// that two phone files never end up as one local file, even with the same size.
using PhoneCopyClaims = QHash<QString, QString>;

struct PhoneImportOptions
{
    const Relocator::Index *library = nullptr;   // music on this computer, matched by file name (and size)
    // Copy tracks not found locally into this folder (empty = don't), keeping
    // the phone's folders below its top "Music/" folder. Two phone files that
    // would land in one place (Music/x.mp3 and x.mp3, the same path on two
    // storages) get different ones: the path with "Music/", then "x (2).mp3".
    // A file already there is taken only if its size is that of the phone
    // file (across imports a file of the same name and size is taken for
    // the same: no hashes, which over MTP would read every file twice).
    QString copyMissingTo;
    PhoneCopyClaims *claims = nullptr;   // null: claims of this playlist only
};

struct PhoneImportStats
{
    int tracks = 0;
    int matched = 0;    // found on this computer
    int copied = 0;     // copied from the phone
    int missing = 0;
    int streams = 0;
    QStringList errors;

    PhoneImportStats &operator+=(const PhoneImportStats &o);
};

// done/total over the tracks of the playlist; return false to cancel.
using PhoneProgress = std::function<bool(int done, int total, const QString &current)>;

// Reads a playlist from the phone and turns it into a playlist of local
// files. Tracks found neither locally nor copied keep their phone path and
// are marked missing ("Find moved files" can fix them later).
bool importPhonePlaylist(PhoneSource &source, const PhonePlaylistFile &file, const PhoneImportOptions &options,
                         PlaylistData &out, PhoneImportStats &stats, const PhoneProgress &progress = {},
                         QString *error = nullptr);

} // namespace Migration
