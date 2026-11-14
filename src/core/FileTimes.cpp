#include "core/FileTimes.h"

#include <QDateTime>
#include <QFile>

#ifdef Q_OS_LINUX
#include <sys/xattr.h>
#endif

#include <cstring>

namespace {

qint64 msOf(const QDateTime &dt)
{
    return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
}

#ifdef Q_OS_LINUX
// ntfs-3g: creation time as a Windows FILETIME (100 ns since 1601, little endian).
qint64 ntfsCreationMs(const QString &path)
{
    unsigned char buf[8];
    const QByteArray local = QFile::encodeName(path);
    if (getxattr(local.constData(), "system.ntfs_crtime", buf, sizeof buf) != ssize_t(sizeof buf))
        return 0;
    quint64 ticks = 0;
    for (int i = 7; i >= 0; --i)
        ticks = (ticks << 8) | buf[i];
    constexpr qint64 kEpochDiffMs = 11644473600000LL;   // 1601-01-01 .. 1970-01-01
    const qint64 ms = qint64(ticks / 10000) - kEpochDiffMs;
    return ms > 0 ? ms : 0;
}
#endif

} // namespace

namespace FileTimes {

qint64 modifiedMs(const QFileInfo &fi)
{
    return msOf(fi.lastModified());
}

qint64 addedMs(const QFileInfo &fi)
{
    if (!fi.exists())
        return 0;
    if (const qint64 birth = msOf(fi.birthTime()))
        return birth;
#ifdef Q_OS_LINUX
    if (const qint64 ntfs = ntfsCreationMs(fi.filePath()))
        return ntfs;
#endif
    return msOf(fi.metadataChangeTime());
}

} // namespace FileTimes
