#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

// Turns playlist entries into local paths.
//
//  * file:// URLs, relative paths, Windows separators;
//  * Windows drive paths written by AIMP running under Wine ("Z:\home\me\a.mp3"):
//    drives are looked up in $WINEPREFIX/dosdevices (default ~/.wine), so Z: -> /,
//    C: -> drive_c, and "e::" device links -> the partition's mount point;
//  * "remaps": folders the music was moved from/to (learned by "find moved
//    files"), applied when the file is not at its original place.
class PathResolver
{
public:
    explicit PathResolver(const QString &baseDir = {});

    QString resolve(const QString &entry) const;

    // "Z:/home/x.mp3" -> "/home/x.mp3"; empty when the drive is unknown.
    QString fromWindows(const QString &winPath) const;
    // "/home/x.mp3" -> "Z:\home\x.mp3" (how Wine programs see Linux paths).
    static QString toWindows(const QString &path);
    static bool isWindowsAbsolute(const QString &path);

    using Remap = QPair<QString, QString>;   // from folder, to folder
    static void setRemaps(const QList<Remap> &remaps);
    static QList<Remap> remaps();
    static QString applyRemaps(const QString &path);   // only if `path` doesn't exist

private:
    QString driveRoot(QChar letter) const;

    QString m_baseDir;
    mutable QHash<QChar, QString> m_driveCache;
};
