#include "core/TrackQuery.h"

#include <QTest>

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
};

QTEST_GUILESS_MAIN(TestTrackQuery)
#include "test_trackquery.moc"
