#pragma once

#include <QFileInfo>

// File dates for "date added" / "date modified", in ms since the epoch.
namespace FileTimes {

qint64 modifiedMs(const QFileInfo &fi);

// When the file appeared on this device: its creation time on the disk.
// Copying keeps the mtime of the original but gets a new creation time, so
// this is the moment the track landed here. Linux exposes it as the birth
// time (statx) on most file systems; NTFS mounted with ntfs-3g hides it in
// the "system.ntfs_crtime" attribute instead. Without either, the inode
// change time is the closest thing. 0 if the file cannot be read.
qint64 addedMs(const QFileInfo &fi);

} // namespace FileTimes
