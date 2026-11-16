#include "core/TrackSort.h"

#include <QCollator>

#include <algorithm>
#include <numeric>
#include <optional>
#include <vector>

namespace TrackSort {

QVector<int> permutation(const TrackList &tracks, Key key, Qt::SortOrder order)
{
    QCollator coll;
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    auto num = [](qint64 a, qint64 b) { return a < b ? -1 : (a > b ? 1 : 0); };
    auto title = [](const Track &t) { return t.title.isEmpty() ? t.fileName() : t.title; };
    auto discTrack = [](const Track &t) { return qint64(t.discNo) * 10000 + t.trackNo; };

    // Collation keys, made once per track and string when first needed:
    // QCollator::compare() in every comparison did the work of a key each
    // time (Title at 100k: 0.6 s).
    enum Slot { Primary, TieArtist, TieAlbum, TiePath, SlotCount };
    std::vector<std::optional<QCollatorSortKey>> keys(size_t(tracks.size()) * SlotCount);
    auto text = [&](int i, Slot slot) -> QString {
        const Track &t = tracks.at(i);
        switch (slot) {
        case Primary:
            switch (key) {
            case Key::Title: return title(t);
            case Key::Artist: return t.artist;
            case Key::Album: return t.album;
            case Key::Year: return t.year;
            case Key::Genre: return t.genre;
            case Key::Format: return t.codec;
            case Key::Path: return t.path;
            default: return {};
            }
        case TieArtist: return t.albumArtist.isEmpty() ? t.artist : t.albumArtist;
        case TieAlbum: return t.album;
        case TiePath: return t.path;
        case SlotCount: break;
        }
        return {};
    };
    auto str = [&](int ia, int ib, Slot slot) {
        auto &ka = keys[size_t(ia) * SlotCount + slot];
        auto &kb = keys[size_t(ib) * SlotCount + slot];
        if (!ka)
            ka.emplace(coll.sortKey(text(ia, slot)));
        if (!kb)
            kb.emplace(coll.sortKey(text(ib, slot)));
        return ka->compare(*kb);
    };

    auto primary = [&](int ia, int ib) -> int {
        const Track &a = tracks.at(ia);
        const Track &b = tracks.at(ib);
        switch (key) {
        case Key::Title:
        case Key::Artist:
        case Key::Album:
        case Key::Year:
        case Key::Genre:
        case Key::Path:
            return str(ia, ib, Primary);
        case Key::TrackNo: return num(discTrack(a), discTrack(b));
        case Key::Duration: return num(a.durationMs, b.durationMs);
        case Key::Format: {
            const int c = str(ia, ib, Primary);
            return c != 0 ? c : num(a.bitrateKbps, b.bitrateKbps);
        }
        // Exact times: the list reads as a timeline (copying an album in
        // another order than its tracks shows that order).
        case Key::Added: return num(a.addedMs, b.addedMs);
        case Key::Modified: return num(a.modifiedMs, b.modifiedMs);
        }
        return 0;
    };
    auto tieBreak = [&](int ia, int ib) -> int {
        const Track &a = tracks.at(ia);
        const Track &b = tracks.at(ib);
        if (int c = str(ia, ib, TieArtist))
            return c;
        if (int c = str(ia, ib, TieAlbum))
            return c;
        if (int c = num(discTrack(a), discTrack(b)))
            return c;
        if (int c = str(ia, ib, TiePath))
            return c;
        return num(a.startMs, b.startMs);
    };

    QVector<int> perm(tracks.size());
    std::iota(perm.begin(), perm.end(), 0);
    std::stable_sort(perm.begin(), perm.end(), [&](int ia, int ib) {
        if (int c = primary(ia, ib))
            return order == Qt::AscendingOrder ? c < 0 : c > 0;
        return tieBreak(ia, ib) < 0;
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
