#include "core/TrackSort.h"

#include <QCollator>

#include <algorithm>
#include <numeric>

namespace TrackSort {

QVector<int> permutation(const TrackList &tracks, Key key, Qt::SortOrder order)
{
    QCollator coll;
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    auto str = [&](const QString &a, const QString &b) { return coll.compare(a, b); };
    auto num = [](qint64 a, qint64 b) { return a < b ? -1 : (a > b ? 1 : 0); };
    auto title = [](const Track &t) { return t.title.isEmpty() ? t.fileName() : t.title; };
    auto discTrack = [](const Track &t) { return qint64(t.discNo) * 10000 + t.trackNo; };

    auto primary = [&](int ia, int ib) -> int {
        const Track &a = tracks.at(ia);
        const Track &b = tracks.at(ib);
        switch (key) {
        case Key::Title: return str(title(a), title(b));
        case Key::Artist: return str(a.artist, b.artist);
        case Key::Album: return str(a.album, b.album);
        case Key::TrackNo: return num(discTrack(a), discTrack(b));
        case Key::Year: return str(a.year, b.year);
        case Key::Genre: return str(a.genre, b.genre);
        case Key::Duration: return num(a.durationMs, b.durationMs);
        case Key::Format: {
            const int c = str(a.codec, b.codec);
            return c != 0 ? c : num(a.bitrateKbps, b.bitrateKbps);
        }
        case Key::Path: return str(a.path, b.path);
        // Exact times: the list reads as a timeline (copying an album in
        // another order than its tracks shows that order).
        case Key::Added: return num(a.addedMs, b.addedMs);
        case Key::Modified: return num(a.modifiedMs, b.modifiedMs);
        }
        return 0;
    };
    auto tieBreak = [&](const Track &a, const Track &b) -> int {
        if (int c = str(a.albumArtist.isEmpty() ? a.artist : a.albumArtist,
                        b.albumArtist.isEmpty() ? b.artist : b.albumArtist))
            return c;
        if (int c = str(a.album, b.album))
            return c;
        if (int c = num(discTrack(a), discTrack(b)))
            return c;
        if (int c = str(a.path, b.path))
            return c;
        return num(a.startMs, b.startMs);
    };

    QVector<int> perm(tracks.size());
    std::iota(perm.begin(), perm.end(), 0);
    std::stable_sort(perm.begin(), perm.end(), [&](int ia, int ib) {
        if (int c = primary(ia, ib))
            return order == Qt::AscendingOrder ? c < 0 : c > 0;
        return tieBreak(tracks.at(ia), tracks.at(ib)) < 0;
    });
    return perm;
}

void sort(TrackList &tracks, Key key, Qt::SortOrder order)
{
    const QVector<int> perm = permutation(tracks, key, order);
    TrackList sorted;
    sorted.reserve(tracks.size());
    for (int i : perm)
        sorted.append(tracks.at(i));
    tracks = std::move(sorted);
}

} // namespace TrackSort
