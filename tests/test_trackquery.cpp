#include "core/TrackQuery.h"
#include "core/TrackSort.h"

#include <QCollator>
#include <QRandomGenerator>
#include <QTest>

#include <algorithm>
#include <numeric>

namespace {

Track make(const QString &artist, const QString &title, const QString &album, const QString &year,
           const QString &genre = {})
{
    Track t = Track::fromPath(QStringLiteral("/music/%1/%2.flac").arg(artist, title));
    t.artist = artist;
    t.title = title;
    t.album = album;
    t.year = year;
    t.genre = genre;
    t.codec = QStringLiteral("flac");
    return t;
}

} // namespace

class TestTrackQuery : public QObject
{
    Q_OBJECT

private:
    const Track queen = make(QStringLiteral("Queen"), QStringLiteral("Love of My Life"),
                             QStringLiteral("A Night at the Opera"), QStringLiteral("1975"), QStringLiteral("Rock"));
    const Track queenLive = make(QStringLiteral("Queen"), QStringLiteral("Love of My Life (Live)"),
                                 QStringLiteral("Live Killers"), QStringLiteral("1979"));
    const Track kino = make(QStringLiteral("Кино"), QStringLiteral("Ёлка"), QStringLiteral("Чёрный альбом"),
                            QStringLiteral("1990"), QStringLiteral("Рок"));

private slots:
    void emptyMatchesAll()
    {
        const auto q = TrackQuery::parse(QStringLiteral("   "));
        QVERIFY(q.isEmpty());
        QVERIFY(q.matches(queen));
    }

    void freeTextAnyField()
    {
        const auto q = TrackQuery::parse(QStringLiteral("opera"));
        QVERIFY(q.matches(queen));
        QVERIFY(!q.matches(queenLive));
    }

    void fieldPrefixAndQuotes()
    {
        QVERIFY(TrackQuery::parse(QStringLiteral("album:\"night at the\"")).matches(queen));
        QVERIFY(!TrackQuery::parse(QStringLiteral("artist:opera")).matches(queen));
        QVERIFY(TrackQuery::parse(QStringLiteral("artist:queen title:love")).matches(queenLive));
    }

    void yearRangeAndNegation()
    {
        const auto q = TrackQuery::parse(QStringLiteral("year:1970-1977 -live"));
        QVERIFY(q.matches(queen));
        QVERIFY(!q.matches(queenLive));
        QVERIFY(!q.matches(kino));
    }

    void russianAliasesAndYo()
    {
        QVERIFY(TrackQuery::parse(QStringLiteral("исполнитель:кино")).matches(kino));
        QVERIFY(TrackQuery::parse(QStringLiteral("елка")).matches(kino));          // ё == е
        QVERIFY(TrackQuery::parse(QStringLiteral("альбом:ЧЕРНЫЙ")).matches(kino));
    }

    void defaultFieldFromPicker()
    {
        const auto q = TrackQuery::parse(QStringLiteral("rock"), TrackQuery::Field::Genre);
        QVERIFY(q.matches(queen));
        QVERIFY(!q.matches(queenLive));
    }

    void unknownPrefixIsPlainText()
    {
        Track t = make(QStringLiteral("AC/DC"), QStringLiteral("x:y"), {}, {});
        QVERIFY(TrackQuery::parse(QStringLiteral("x:y")).matches(t));
    }

    void incompletePrefixIgnored()
    {
        QVERIFY(TrackQuery::parse(QStringLiteral("artist:")).isEmpty());
    }

    // Sorting with collation keys gives the order QCollator::compare gives:
    // numbers by value, case ignored, Cyrillic, ties by album artist etc.
    void sortKeysOrderAsCompare()
    {
        const QStringList words = {QStringLiteral("Track 2"), QStringLiteral("track 10"), QStringLiteral("Track 1"),
                                   QStringLiteral("Ёлка"), QStringLiteral("елка"), QStringLiteral("Яблоко"),
                                   QStringLiteral("apple"), QStringLiteral("Apple"), QStringLiteral(""),
                                   QStringLiteral("Zebra 9"), QStringLiteral("zebra 09"), QStringLiteral("Über")};
        QRandomGenerator rng(42);
        TrackList tracks;
        for (int i = 0; i < 400; ++i) {
            Track t = Track::fromPath(QStringLiteral("/m/%1/%2.mp3").arg(words[rng.bounded(words.size())]).arg(i));
            t.title = words[rng.bounded(words.size())];
            t.artist = words[rng.bounded(words.size())];
            t.album = words[rng.bounded(words.size())];
            t.discNo = rng.bounded(2);
            t.trackNo = rng.bounded(5);
            tracks << t;
        }
        QCollator coll;
        coll.setNumericMode(true);
        coll.setCaseSensitivity(Qt::CaseInsensitive);
        auto title = [](const Track &t) { return t.title.isEmpty() ? t.fileName() : t.title; };
        auto dt = [](const Track &t) { return qint64(t.discNo) * 10000 + t.trackNo; };
        for (auto key : {TrackSort::Key::Title, TrackSort::Key::Artist, TrackSort::Key::Path}) {
            for (auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
                QVector<int> ref(tracks.size());
                std::iota(ref.begin(), ref.end(), 0);
                std::stable_sort(ref.begin(), ref.end(), [&](int ia, int ib) {
                    const Track &a = tracks[ia], &b = tracks[ib];
                    const int c = key == TrackSort::Key::Title  ? coll.compare(title(a), title(b))
                                  : key == TrackSort::Key::Artist ? coll.compare(a.artist, b.artist)
                                                                  : coll.compare(a.path, b.path);
                    if (c)
                        return order == Qt::AscendingOrder ? c < 0 : c > 0;
                    int t = coll.compare(a.albumArtist.isEmpty() ? a.artist : a.albumArtist,
                                         b.albumArtist.isEmpty() ? b.artist : b.albumArtist);
                    if (!t) t = coll.compare(a.album, b.album);
                    if (!t) t = dt(a) < dt(b) ? -1 : dt(a) > dt(b);
                    if (!t) t = coll.compare(a.path, b.path);
                    return t < 0;
                });
                QCOMPARE(TrackSort::permutation(tracks, key, order), ref);
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestTrackQuery)
#include "test_trackquery.moc"
