#pragma once

#include "core/Track.h"

#include <QList>
#include <QPair>
#include <QString>

// Search query over track fields.
//
//   queen                     any field contains "queen"
//   artist:queen album:"a night at"
//   year:1970-1979 genre:rock
//   -live                     exclude matches
//
// Field names (English and Russian): title/t/название, artist/a/исполнитель,
// album/al/альбом, albumartist/aa, genre/g/жанр, year/y/год, path/file/p/путь,
// codec/format/f/формат, composer/композитор, comment/комментарий.
// Matching is case-insensitive and treats "ё" as "е".
class TrackQuery
{
public:
    enum class Field { Any, Title, Artist, Album, AlbumArtist, Genre, Year, Path, Codec, Composer, Comment };

    static TrackQuery parse(const QString &text, Field defaultField = Field::Any);

    bool isEmpty() const { return m_terms.isEmpty(); }
    bool matches(const Track &t) const;

    // Display names for a field picker in the UI.
    static QList<QPair<QString, Field>> fieldChoices();

private:
    struct Term
    {
        Field field = Field::Any;
        QString text;          // folded
        int yearFrom = 0;      // for year ranges
        int yearTo = 0;
        bool negate = false;
    };
    static bool termMatches(const Term &term, const Track &t);

    QList<Term> m_terms;
};
