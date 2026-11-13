#pragma once

#include "core/PlaylistIO.h"

#include <QDateTime>
#include <QList>
#include <QString>

// AIMP for Windows installed on this computer: natively (Windows) or under
// Wine (~/.wine, $WINEPREFIX, Bottles, PlayOnLinux, Lutris, Steam/Proton).
namespace Migration {

struct AimpProfile
{
    QString label;      // "Wine: ~/.wine", "Bottles: Music", "Windows", "Portable: D:/AIMP"
    QString dir;        // folder with AIMP.ini and PLS/
    QString plsDir() const { return dir + QStringLiteral("/PLS"); }
    QString iniPath() const { return dir + QStringLiteral("/AIMP.ini"); }
};

// Every AIMP profile that has a PLS folder, without duplicates.
// `home` is for tests; empty means the real home folder.
QList<AimpProfile> findAimpProfiles(const QString &home = {});

struct AimpPlaylistInfo
{
    QString file;           // full path of the .aimppl4
    QString name;           // Name= from the summary
    QString id;             // {GUID}
    int trackCount = 0;
    int playbackCursor = -1;
    QDateTime modified;
    bool open = false;      // shown as a tab in AIMP
};

struct AimpSession
{
    QList<AimpPlaylistInfo> playlists;   // open tabs in AIMP's order, then the rest
    int activeIndex = -1;                // tab selected in AIMP
    int playingIndex = -1;               // playlist AIMP was playing from
};

// Reads AIMP.ini ([Playlist.Storage], [Playlist.Manager]) and the playlist
// summaries. AIMP keeps old copies of playlists ("SD.aimppl4" next to the
// open "SD (2).aimppl4"); those come after the open ones with open == false.
// Without an AIMP.ini list, every playlist counts as open, and of several
// with the same name only the newest one.
AimpSession readAimpSession(const AimpProfile &profile);

// Folders of AIMP's music library ("Local files"): where AIMP looks for music,
// as local paths. `excluded` gets the excluded folders.
QStringList aimpLibraryFolders(const AimpProfile &profile, QStringList *excluded = nullptr);

// Loads one playlist; paths are resolved like any AIMP playlist (Wine
// drives, learned folder moves).
bool loadAimpPlaylist(const AimpPlaylistInfo &info, PlaylistData &out, QString *error = nullptr);

} // namespace Migration
