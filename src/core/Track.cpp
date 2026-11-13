#include "core/Track.h"

#include <QFileInfo>
#include <QUrl>

#include <atomic>

bool Track::isUrl() const
{
    const int colon = path.indexOf(QLatin1String("://"));
    return colon > 1 && !path.startsWith(QLatin1String("file://"));
}

QString Track::fileName() const
{
    if (isUrl())
        return path;
    return QFileInfo(path).fileName();
}

QString Track::displayTitle() const
{
    if (title.isEmpty())
        return fileName();
    if (artist.isEmpty())
        return title;
    return artist + QStringLiteral(" — ") + title;
}

QString Track::formatSummary() const
{
    QStringList parts;
    if (!codec.isEmpty())
        parts << codec.toUpper();
    if (sampleRate > 0)
        parts << QStringLiteral("%1 kHz").arg(sampleRate / 1000.0, 0, 'g', 3);
    if (bitrateKbps > 0)
        parts << QStringLiteral("%1 kbps").arg(bitrateKbps);
    if (channels == 1)
        parts << QStringLiteral("mono");
    else if (channels > 2)
        parts << QStringLiteral("%1 ch").arg(channels);
    return parts.join(QStringLiteral(", "));
}

quint64 Track::newUid()
{
    static std::atomic<quint64> counter{0};
    return ++counter;
}

Track Track::fromPath(const QString &path)
{
    Track t;
    t.uid = newUid();
    t.path = path;
    return t;
}

QString formatDuration(qint64 ms)
{
    if (ms <= 0)
        return QString();
    qint64 s = (ms + 500) / 1000;
    const qint64 h = s / 3600;
    s %= 3600;
    const auto mm = QString::number(s / 60);
    const auto ss = QString::number(s % 60).rightJustified(2, QLatin1Char('0'));
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(mm.rightJustified(2, QLatin1Char('0')), ss);
    return mm + QLatin1Char(':') + ss;
}
