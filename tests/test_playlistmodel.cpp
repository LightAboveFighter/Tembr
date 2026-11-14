#include "ui/PlaylistModel.h"

#include <QApplication>
#include <QSignalSpy>
#include <QTest>

namespace {

TrackList make(const QStringList &names)
{
    TrackList out;
    for (const QString &n : names) {
        Track t = Track::fromPath(QStringLiteral("/m/") + n + QStringLiteral(".mp3"));
        t.uid = Track::newUid();
        t.title = n;
        t.scanned = true;   // no background tag reading
        out << t;
    }
    return out;
}

QString title(const PlaylistModel &m, int row)
{
    return row >= 0 ? m.track(row).title : QStringLiteral("<end>");
}

QList<int> rowsOf(const PlaylistModel &m, const QStringList &names)
{
    QList<int> rows;
    for (int r = 0; r < m.rowCount(); ++r) {
        if (names.contains(m.track(r).title))
            rows << r;
    }
    return rows;
}

TrackList without(const TrackList &list, const QStringList &names)
{
    TrackList out;
    for (const Track &t : list) {
        if (!names.contains(t.title))
            out << t;
    }
    return out;
}

} // namespace

// The track that plays when its row goes away (deleted from the disk,
// removed, Local files rescanned): still current, still shown, and the
// player goes on with what came after it.
class TestPlaylistModel : public QObject
{
    Q_OBJECT

private slots:
    void removedCurrentGoesOnWithItsNext()
    {
        PlaylistModel m;
        m.insertTracks(-1, make({"A", "B", "C", "D", "E"}));
        m.setCurrent(m.track(2).uid);   // C
        QSignalSpy detached(&m, &PlaylistModel::currentDetached);

        m.removeRowList(rowsOf(m, {"C"}));
        QCOMPARE(detached.count(), 1);
        QVERIFY(m.isCurrentDetached());
        QVERIFY(m.currentTrack());
        QCOMPARE(m.currentTrack()->title, QStringLiteral("C"));   // still shown
        QCOMPARE(m.currentRow(), -1);
        QCOMPARE(title(m, m.nextRow()), QStringLiteral("D"));
        QCOMPARE(title(m, m.previousRow()), QStringLiteral("B"));

        // More goes away later: still the nearest survivor after it.
        m.removeRowList(rowsOf(m, {"D", "B"}));
        QCOMPARE(detached.count(), 1);   // told once
        QCOMPARE(title(m, m.nextRow()), QStringLiteral("E"));
        QCOMPARE(title(m, m.previousRow()), QStringLiteral("A"));

        // Playing the next one ends it.
        m.setCurrent(m.track(m.nextRow()).uid);
        QVERIFY(!m.isCurrentDetached());
        QCOMPARE(m.currentTrack()->title, QStringLiteral("E"));
    }

    void otherRowsRemovedLeaveItAlone()
    {
        PlaylistModel m;
        m.insertTracks(-1, make({"A", "B", "C"}));
        m.setCurrent(m.track(1).uid);
        QSignalSpy detached(&m, &PlaylistModel::currentDetached);
        m.removeRowList(rowsOf(m, {"A"}));
        QCOMPARE(detached.count(), 0);
        QVERIFY(!m.isCurrentDetached());
        QCOMPARE(title(m, m.nextRow()), QStringLiteral("C"));
    }

    // Local files: the whole list is replaced, maybe in another order.
    void rescannedListKeepsTheOldNext()
    {
        PlaylistModel m;
        m.setReadOnly(true);
        const TrackList all = make({"A", "B", "C", "D", "E"});
        m.setTracks(all);
        m.setCurrent(all[2].uid);   // C

        TrackList rescanned = without(all, {"C"});
        std::reverse(rescanned.begin(), rescanned.end());   // E D B A: sorted another way
        m.setTracks(rescanned);
        QVERIFY(m.isCurrentDetached());
        QCOMPARE(m.currentTrack()->title, QStringLiteral("C"));
        QCOMPARE(title(m, m.nextRow()), QStringLiteral("D"));       // what came after C, wherever it is now
        QCOMPARE(title(m, m.previousRow()), QStringLiteral("B"));
    }

    // Nothing after it is left: the track now in its place, if any; else the end.
    void noSurvivorAfterIt()
    {
        PlaylistModel m;
        m.setReadOnly(true);
        const TrackList all = make({"A", "B", "C", "D"});
        m.setTracks(all);
        m.setCurrent(all[1].uid);   // B

        TrackList rescanned = {all[0]};
        rescanned << make({"New 1", "New 2"});   // C and D gone, new files in their place
        m.setTracks(rescanned);
        QCOMPARE(title(m, m.nextRow()), QStringLiteral("New 1"));   // row 1, where B was

        m.setTracks({all[0]});
        QCOMPARE(m.nextRow(), -1);   // end of the playlist
        QCOMPARE(title(m, m.previousRow()), QStringLiteral("A"));

        m.setTracks({});
        QCOMPARE(m.nextRow(), -1);
        QCOMPARE(m.previousRow(), -1);
        QCOMPARE(m.currentTrack()->title, QStringLiteral("B"));   // still shown
    }

    void lastTrackRemoved()
    {
        PlaylistModel m;
        m.insertTracks(-1, make({"A", "B"}));
        m.setCurrent(m.track(1).uid);
        m.removeRowList({1});
        QCOMPARE(m.nextRow(), -1);
        QCOMPARE(title(m, m.previousRow()), QStringLiteral("A"));
    }
};

QTEST_MAIN(TestPlaylistModel)
#include "test_playlistmodel.moc"
