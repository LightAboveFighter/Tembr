#include "core/Relocator.h"

#include "core/MediaCollector.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QMap>

#include <algorithm>

namespace Relocator {

Index build(const QString &root)
{
    Index idx;
    QDirIterator it(root, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString name = it.fileName();
        if (!MediaCollector::isAudioFile(name))
            continue;
        idx.byName[name.toLower()].append(path);
        ++idx.files;
    }
    return idx;
}

QString find(const Track &t, const Index &index)
{
    const QFileInfo old(t.path);
    const QStringList candidates = index.byName.value(old.fileName().toLower());
    if (candidates.size() <= 1)
        return candidates.value(0);
    if (t.fileSize > 0) {
        for (const QString &c : candidates) {
            if (QFileInfo(c).size() == t.fileSize)
                return c;
        }
    }
    const QString parentName = old.dir().dirName();
    for (const QString &c : candidates) {
        if (QFileInfo(c).dir().dirName() == parentName)
            return c;
    }
    return candidates.first();
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
