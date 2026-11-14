#include "core/FileTrash.h"

#include <QFile>
#include <QFileInfo>

namespace FileTrash {

Result moveToTrash(const QStringList &paths)
{
    Result r;
    for (const QString &p : paths) {
        if (!QFileInfo::exists(p) || QFile::moveToTrash(p))
            r.done << p;   // already gone counts as done
        else
            r.failed << p;
    }
    return r;
}

Result removeForever(const QStringList &paths)
{
    Result r;
    for (const QString &p : paths) {
        if (!QFileInfo::exists(p) || QFile::remove(p))
            r.done << p;
        else
            r.failed << p;
    }
    return r;
}

} // namespace FileTrash
