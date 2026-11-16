#include "core/FileTrash.h"

#include <QFile>
#include <QFileInfo>

namespace FileTrash {

namespace {

// A file that is already gone counts as done.
template<class Remove>
Result removeEach(const QStringList &paths, Remove remove)
{
    Result r;
    for (const QString &p : paths) {
        if (!QFileInfo::exists(p) || remove(p))
            r.done << p;
        else
            r.failed << p;
    }
    return r;
}

} // namespace

Result moveToTrash(const QStringList &paths)
{
    return removeEach(paths, [](const QString &p) { return QFile::moveToTrash(p); });
}

Result removeForever(const QStringList &paths)
{
    return removeEach(paths, [](const QString &p) { return QFile::remove(p); });
}

} // namespace FileTrash
