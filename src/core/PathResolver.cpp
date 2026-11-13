#include "core/PathResolver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QReadWriteLock>
#include <QRegularExpression>
#include <QUrl>

namespace {

QReadWriteLock g_remapLock;
QList<PathResolver::Remap> g_remaps;

QString winePrefix()
{
    const QString env = qEnvironmentVariable("WINEPREFIX");
    return env.isEmpty() ? QDir::homePath() + QStringLiteral("/.wine") : env;
}

// /proc/mounts escapes spaces etc. as octal: "\040".
QString unescapeMount(const QByteArray &s)
{
    QByteArray out;
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            bool ok = false;
            const int v = s.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out.append(char(v));
                i += 3;
                continue;
            }
        }
        out.append(s[i]);
    }
    return QFile::decodeName(out);
}

QString mountPointOf(const QString &device)
{
    const QString dev = QFileInfo(device).canonicalFilePath();
    if (dev.isEmpty())
        return {};
    QFile f(QStringLiteral("/proc/mounts"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : f.readAll().split('\n')) {
        const QList<QByteArray> parts = line.split(' ');
        if (parts.size() < 2 || !parts[0].startsWith('/'))
            continue;
        if (QFileInfo(unescapeMount(parts[0])).canonicalFilePath() == dev)
            return unescapeMount(parts[1]);
    }
    return {};
}

} // namespace

PathResolver::PathResolver(const QString &baseDir)
    : m_baseDir(baseDir)
{
}

bool PathResolver::isWindowsAbsolute(const QString &path)
{
    return path.size() >= 3 && path[0].isLetter() && path[0].unicode() < 128 && path[1] == QLatin1Char(':')
           && (path[2] == QLatin1Char('/') || path[2] == QLatin1Char('\\'));
}

QString PathResolver::driveRoot(QChar letter) const
{
    letter = letter.toLower();
    const auto it = m_driveCache.constFind(letter);
    if (it != m_driveCache.constEnd())
        return it.value();

    QString root;
    const QString dosdevices = winePrefix() + QStringLiteral("/dosdevices/");
    const QFileInfo dirLink(dosdevices + letter + QLatin1Char(':'));
    const QFileInfo devLink(dosdevices + letter + QStringLiteral("::"));
    if (dirLink.exists() && dirLink.isDir())
        root = dirLink.canonicalFilePath();
    else if (devLink.exists())
        root = mountPointOf(devLink.absoluteFilePath());
    else if (letter == QLatin1Char('z') && !QFileInfo::exists(dosdevices))
        root = QStringLiteral("/");   // Wine's default even without a prefix here
    m_driveCache.insert(letter, root);
    return root;
}

QString PathResolver::fromWindows(const QString &winPath) const
{
    if (!isWindowsAbsolute(winPath))
        return {};
    const QString root = driveRoot(winPath[0]);
    if (root.isEmpty())
        return {};
    QString rest = winPath.mid(3);
    rest.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return QDir::cleanPath(root + QLatin1Char('/') + rest);
}

QString PathResolver::toWindows(const QString &path)
{
    if (path.contains(QLatin1String("://")))
        return path;
    QString p = path;
    if (!isWindowsAbsolute(p))
        p = QStringLiteral("Z:") + p;
    p.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return p;
}

QString PathResolver::resolve(const QString &entryIn) const
{
    QString entry = entryIn.trimmed();
    if (entry.isEmpty())
        return {};
    if (entry.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
        return applyRemaps(QUrl(entry).toLocalFile());
    if (entry.contains(QLatin1String("://")))
        return entry;

    entry.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (isWindowsAbsolute(entry)) {
        const QString local = fromWindows(entry);
        return local.isEmpty() ? entry : applyRemaps(local);
    }
    if (entry.startsWith(QLatin1String("//")))
        return entry;   // UNC share: nothing sensible to map it to
    if (QDir::isRelativePath(entry))
        entry = QDir(m_baseDir).absoluteFilePath(entry);
    return applyRemaps(QDir::cleanPath(entry));
}

void PathResolver::setRemaps(const QList<Remap> &remaps)
{
    QWriteLocker lock(&g_remapLock);
    g_remaps = remaps;
}

QList<PathResolver::Remap> PathResolver::remaps()
{
    QReadLocker lock(&g_remapLock);
    return g_remaps;
}

QString PathResolver::applyRemaps(const QString &path)
{
    if (path.isEmpty() || QFileInfo::exists(path))
        return path;
    QReadLocker lock(&g_remapLock);
    for (const Remap &r : std::as_const(g_remaps)) {
        if (path.startsWith(r.first + QLatin1Char('/'))) {
            const QString candidate = r.second + path.mid(r.first.size());
            if (QFileInfo::exists(candidate))
                return candidate;
        }
    }
    return path;
}
