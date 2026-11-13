#include "core/Relocator.h"

#include "core/MediaCollector.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMap>

#include <algorithm>
#include <functional>

namespace Relocator {

namespace {

QStringList lowerParts(const QString &path)
{
    return QDir::cleanPath(path).toLower().split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

// The last `dirs` folders plus the file name: "album/01 - intro.mp3".
QString suffixKey(const QStringList &parts, int dirs)
{
    return parts.mid(parts.size() - dirs - 1).join(QLatin1Char('/'));
}

QStringList filtered(const QStringList &in, const std::function<bool(const QString &)> &keep)
{
    QStringList out;
    for (const QString &s : in) {
        if (keep(s))
            out << s;
    }
    return out;
}

} // namespace

Index build(const QString &root)
{
    return build(QStringList{root});
}

Index build(const QStringList &roots)
{
    Index idx;
    for (const QString &root : roots) {
        if (root.isEmpty())
            continue;
        QDirIterator it(root, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString name = it.fileName();
            if (!MediaCollector::isAudioFile(name))
                continue;
            idx.byName[name.toLower()].append(path);
            const QStringList parts = lowerParts(path);
            for (int k = 1; k <= Index::kSuffixDirs && k < parts.size(); ++k)
                idx.bySuffix[k - 1][suffixKey(parts, k)].append(path);
            ++idx.files;
        }
    }
    return idx;
}

QString find(const Track &t, const Index &index)
{
    const QFileInfo old(t.path);
    QStringList candidates = index.byName.value(old.fileName().toLower());
    if (candidates.size() <= 1)
        return candidates.value(0);

    // Same folders as before the move (from the album folder upwards).
    const QStringList parts = lowerParts(t.path);
    for (int k = 1; k <= Index::kSuffixDirs && k < parts.size(); ++k) {
        const QStringList narrowed = index.bySuffix[k - 1].value(suffixKey(parts, k));
        if (narrowed.isEmpty())
            break;
        candidates = narrowed;
        if (candidates.size() == 1)
            return candidates.first();
    }
    if (t.fileSize > 0) {
        const QStringList sameSize = filtered(candidates, [&](const QString &c) { return QFileInfo(c).size() == t.fileSize; });
        if (sameSize.size() == 1)
            return sameSize.first();
        if (!sameSize.isEmpty())
            candidates = sameSize;
    }
    const QStringList sameCase = filtered(candidates, [&](const QString &c) { return QFileInfo(c).fileName() == old.fileName(); });
    if (sameCase.size() == 1)
        return sameCase.first();
    return {};   // still ambiguous
}

QList<PathResolver::Remap> deriveRemaps(const QList<QPair<QString, QString>> &moves)
{
    QMap<PathResolver::Remap, int> counts;
    for (const auto &m : moves) {
        const QString from = QFileInfo(m.first).absolutePath();
        const QString to = QFileInfo(m.second).absolutePath();
        if (from != to)
            ++counts[{from, to}];
    }
    QList<PathResolver::Remap> out = counts.keys();
    std::stable_sort(out.begin(), out.end(),
                     [&](const PathResolver::Remap &a, const PathResolver::Remap &b) { return counts[a] > counts[b]; });
    return out;
}

} // namespace Relocator
