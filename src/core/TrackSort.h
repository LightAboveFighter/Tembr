#pragma once

#include "core/Track.h"

#include <QVector>

// Sorting of playlists (as in AIMP, it reorders the playlist itself).
namespace TrackSort {

enum class Key { Title, Artist, Album, TrackNo, Year, Genre, Duration, Format, Path, Added, Modified };

// New order of `tracks`: element i is the old row of the track that goes to
// row i. Stable; ties (same time, same tag) keep albums together and in
// track order.
QVector<int> permutation(const TrackList &tracks, Key key, Qt::SortOrder order);

void sort(TrackList &tracks, Key key, Qt::SortOrder order);

} // namespace TrackSort
