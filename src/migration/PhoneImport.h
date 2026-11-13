#pragma once

#include "core/PlaylistIO.h"
#include "core/Relocator.h"
#include "migration/PhoneSources.h"

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

struct PhoneImportOptions
{
    const Relocator::Index *library = nullptr;   // music on this computer, matched by file name (and size)
    QString copyMissingTo;                       // copy tracks not found locally into this folder; empty = don't
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
