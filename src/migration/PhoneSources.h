#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

// An Android phone connected over USB (or any folder with a copy of its
// storage). AIMP for Android keeps its playlists in private app storage, so
// what we can reach are playlists exported by it ("Export playlist" writes
// XSPF with relative paths) or any other m3u/m3u8/pls/xspf on the phone.
namespace Migration {

struct PhoneStorage
{
    QString key;        // "primary" (internal shared storage) or an SD card volume id ("1234-ABCD")
    QString name;       // for the UI
    QString root;       // ADB: "/sdcard", "/storage/1234-ABCD"; MTP/folder: local mount path
    bool primary = false;
};

struct PhonePlaylistFile
{
    int storage = 0;    // index in storages()
    QString path;       // relative to the storage root: "Music/Playlists/Rock.xspf"
};

class PhoneSource
{
public:
    enum class Kind { Adb, Mtp, Folder };

    virtual ~PhoneSource() = default;
    virtual Kind kind() const = 0;
    virtual QString label() const = 0;
    virtual QList<PhoneStorage> storages() const = 0;

    // Playlist files (m3u, m3u8, pls, xspf, aimppl4). Android/ app folders are
    // skipped (players and browsers cache HLS .m3u8 there), except AIMP's own.
    virtual QList<PhonePlaylistFile> findPlaylists(QString *error = nullptr) = 0;
    virtual QByteArray readFile(int storage, const QString &rel, QString *error = nullptr) = 0;
    virtual bool exists(int storage, const QString &rel) = 0;
    virtual bool copyTo(int storage, const QString &rel, const QString &localPath, QString *error = nullptr) = 0;

    // "/sdcard/Music/a.mp3": how the path looks on the phone.
    QString phonePath(int storage, const QString &rel) const;
};

using PhoneSourcePtr = std::shared_ptr<PhoneSource>;

struct PhoneDiscovery
{
    QList<PhoneSourcePtr> sources;
    QStringList notes;   // why a connected phone can't be used yet (USB debugging not allowed, ...)
};

// Phones over ADB (if `adb` is installed; $TEMBR_ADB overrides the binary)
// and over MTP as mounted by GNOME/gvfs ("File transfer" mode).
PhoneDiscovery discoverPhones();

// A folder: the phone mounted some other way, an SD card in a reader,
// or a copy of the phone's storage.
PhoneSourcePtr folderSource(const QString &path);

// gvfs MTP mounts below `gvfsDir` (for tests; default /run/user/<uid>/gvfs).
QList<PhoneSourcePtr> mtpSources(const QString &gvfsDir, QStringList *notes = nullptr);

} // namespace Migration
