#pragma once

#include <QStringList>

// Deleting music files from the device.
namespace FileTrash {

struct Result
{
    QStringList done;     // gone from where they were
    QStringList failed;   // still there
};

// Into the desktop trash (freedesktop.org spec: ~/.local/share/Trash, or
// .Trash-<uid> at the top of another drive), so it can be undone.
Result moveToTrash(const QStringList &paths);
// For drives without a trash (no rights to create one, some network shares).
Result removeForever(const QStringList &paths);

} // namespace FileTrash
