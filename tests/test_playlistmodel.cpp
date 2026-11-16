#include "ui/PlaylistModel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QItemSelectionModel>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
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

    // Tags read in the background must not undo "find moved files": a result
    // for the old path, arriving after the track was pointed elsewhere, is
    // dropped (results reach the model only through the event loop).
    void lateScanResultKeepsTheNewPath()
    {
        PlaylistModel m;
        Track t = Track::fromPath(QStringLiteral("/nonexistent/old/a.mp3"));
        t.missing = true;
        t.durationMs = 1000;       // known length: relocating does not read the tags again
        m.insertTracks(-1, {t});   // starts reading tags of the old path
        m.relocate({{0, QStringLiteral("/nonexistent/new/a.mp3")}});
        QTRY_VERIFY(!m.isScanning());
        QCOMPARE(m.track(0).path, QStringLiteral("/nonexistent/new/a.mp3"));
    }

    // Choosing a sort counts even when there is nothing to reorder yet
    // (Local files remember it before the first scan is in).
    // Many scattered rows go in one pass; what refers to the rows left
    // (selection, current track, a filtered view) follows them.
    void scatteredRemovalKeepsTheRest()
    {
        QStringList names;
        for (int i = 0; i < 200; ++i)
            names << QStringLiteral("t%1").arg(i, 3, 10, QLatin1Char('0'));
        PlaylistModel m;
        m.insertTracks(-1, make(names));
        m.setCurrent(m.track(101).uid);
        QSortFilterProxyModel proxy;
        proxy.setSourceModel(&m);
        proxy.setFilterFixedString(QStringLiteral("t1"));
        proxy.setFilterKeyColumn(PlaylistModel::ColTitle);
        const int shownBefore = proxy.rowCount();
        const QPersistentModelIndex kept = m.index(101, 0), removed = m.index(100, 0);

        QList<int> even;
        for (int r = 0; r < 200; r += 2)
            even << r;
        m.removeRowList(even);   // 100 runs

        QCOMPARE(m.rowCount(), 100);
        for (int r = 0; r < m.rowCount(); ++r)
            QCOMPARE(m.track(r).title, names[2 * r + 1]);
        QVERIFY(kept.isValid());
        QCOMPARE(kept.row(), 50);
        QVERIFY(!removed.isValid());
        QCOMPARE(m.currentRow(), 50);
        QCOMPARE(m.rowOfUid(m.track(50).uid), 50);
        QCOMPARE(proxy.rowCount(), shownBefore / 2);
        for (int r = 0; r < proxy.rowCount(); ++r)
            QVERIFY(proxy.index(r, PlaylistModel::ColTitle).data().toString().startsWith(QLatin1String("t1")));
    }

    void insertInTheMiddle()
    {
        PlaylistModel m;
        m.insertTracks(-1, make({QStringLiteral("a"), QStringLiteral("d")}));
        m.insertTracks(1, make({QStringLiteral("b"), QStringLiteral("c")}));
        QCOMPARE(m.rowCount(), 4);
        for (int r = 0; r < 4; ++r)
            QCOMPARE(title(m, r), QString(QChar(u'a' + r)));
        QCOMPARE(m.rowOfUid(m.track(3).uid), 3);
    }

    void sortChoiceCountsForTinyLists()
    {
        PlaylistModel m;
        QSignalSpy sorted(&m, &PlaylistModel::sorted);
        m.sort(PlaylistModel::ColAdded, Qt::DescendingOrder);
        QCOMPARE(sorted.count(), 1);
        m.insertTracks(-1, make({QStringLiteral("a")}));
        m.sort(PlaylistModel::ColArtist, Qt::AscendingOrder);
        QCOMPARE(sorted.count(), 2);
        QCOMPARE(sorted.last().at(0).toInt(), int(PlaylistModel::ColArtist));
    }

    // Sent to another playlist: copies with their tags and new uids; the
    // same track sent twice is two entries, nothing is read again, and the
    // playlist they came from is as it was.
    void copiesForAnotherPlaylist()
    {
        PlaylistModel from, to;
        TrackList tracks;
        for (int i = 0; i < 10000; ++i) {
            Track t = Track::fromPath(QStringLiteral("/m/%1.mp3").arg(i));
            t.title = QStringLiteral("Song %1").arg(i);
            t.artist = QStringLiteral("Artist");
            t.durationMs = 1000;
            t.scanned = true;
            tracks << t;
        }
        from.insertTracks(-1, tracks);
        from.setCurrent(from.track(5).uid);
        const TrackList before = from.tracks();

        QList<int> all;
        for (int i = 0; i < from.rowCount(); ++i)
            all << i;
        QElapsedTimer timer;
        timer.start();
        to.insertTracks(-1, from.copiesOf(all));
        const qint64 ms = timer.elapsed();
        qInfo("10000 tracks copied into another playlist in %lld ms", static_cast<long long>(ms));
        QVERIFY(ms < 200);
        QCOMPARE(to.rowCount(), 10000);
        QVERIFY(!to.isScanning());
        QCOMPARE(to.track(7).title, QStringLiteral("Song 7"));
        QVERIFY(to.track(7).uid != from.track(7).uid);
        QCOMPARE(to.currentUid(), quint64(0));   // the playing mark stays where it was

        to.insertTracks(-1, from.copiesOf({3}));   // the same track again
        QCOMPARE(to.rowCount(), 10001);
        QVERIFY(to.track(10000).uid != to.track(3).uid);
        QCOMPARE(to.rowOfUid(to.track(3).uid), 3);
        QCOMPARE(to.rowOfUid(to.track(10000).uid), 10000);
        to.setCurrent(to.track(10000).uid);
        QCOMPARE(to.currentRow(), 10000);

        QCOMPARE(from.rowCount(), before.size());
        for (int i = 0; i < before.size(); i += 997)
            QCOMPARE(from.track(i).uid, before[i].uid);
        QCOMPARE(from.currentRow(), 5);
        QVERIFY(from.copiesOf({-1, 99999}).isEmpty());
    }

    // Dragged within the playlist: the tracks stand together where they were
    // dropped, in their order; the rest keep theirs.
    void moveTracksToStartMiddleEnd_data()
    {
        QTest::addColumn<QStringList>("moved");
        QTest::addColumn<int>("before");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<int>("first");
        QTest::newRow("scattered to the start") << QStringList{"C", "F"} << 0 << "CFABDEG" << 0;
        QTest::newRow("scattered to the end") << QStringList{"A", "D"} << 7 << "BCEFGAD" << 5;
        QTest::newRow("scattered into the middle") << QStringList{"A", "G", "C"} << 4 << "BDACGEF" << 2;
        QTest::newRow("down past themselves") << QStringList{"B", "C"} << 6 << "ADEFBCG" << 4;
        QTest::newRow("up") << QStringList{"E", "F"} << 1 << "AEFBCDG" << 1;
        QTest::newRow("before one of the moved") << QStringList{"B", "D", "F"} << 3 << "ACBDFEG" << 2;
        QTest::newRow("unsorted rows, out of range ignored") << QStringList{"F", "B"} << 99 << "ACDEGBF" << 5;
    }

    void moveTracksToStartMiddleEnd()
    {
        QFETCH(QStringList, moved);
        QFETCH(int, before);
        QFETCH(QString, expected);
        QFETCH(int, first);
        PlaylistModel m;
        m.insertTracks(-1, make({"A", "B", "C", "D", "E", "F", "G"}));
        QList<int> rows = rowsOf(m, moved);
        std::reverse(rows.begin(), rows.end());
        rows << -3 << 42;
        QSignalSpy layout(&m, &QAbstractItemModel::layoutChanged);
        QCOMPARE(m.moveTracks(rows, before), first);
        QString order;
        for (int r = 0; r < m.rowCount(); ++r)
            order += m.track(r).title;
        QCOMPARE(order, expected);
        QCOMPARE(layout.count(), 1);   // one change for any number of rows
        for (int r = 0; r < m.rowCount(); ++r)
            QCOMPARE(m.rowOfUid(m.track(r).uid), r);
    }

    // Dropped where they already stand: no change, nothing announced.
    void moveOntoItselfChangesNothing()
    {
        PlaylistModel m;
        const TrackList tracks = make({"A", "B", "C", "D", "E"});
        m.insertTracks(-1, tracks);
        QSignalSpy layout(&m, &QAbstractItemModel::layoutAboutToBeChanged);
        for (int before : {1, 2, 3, 4})   // B..D moved before B, C, D or E: where they are
            QCOMPARE(m.moveTracks({1, 2, 3}, before), -1);
        QCOMPARE(m.moveTracks({}, 0), -1);
        QCOMPARE(layout.count(), 0);
        for (int r = 0; r < tracks.size(); ++r)
            QCOMPARE(m.track(r).uid, tracks[r].uid);

        m.setReadOnly(true);   // Local files: never reordered
        QCOMPARE(m.moveTracks({4}, 0), -1);
        QCOMPARE(m.track(0).uid, tracks[0].uid);
    }

    // The playing track moves with the others and stays the current one;
    // next / previous follow the new order. The selection follows its tracks.
    void moveKeepsTheCurrentTrackAndSelection()
    {
        PlaylistModel m;
        m.insertTracks(-1, make({"A", "B", "C", "D", "E"}));
        m.setCurrent(m.track(1).uid);   // B plays
        QSortFilterProxyModel proxy;
        proxy.setSourceModel(&m);
        QItemSelectionModel sel(&proxy);
        sel.select(proxy.index(1, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        sel.select(proxy.index(3, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        sel.setCurrentIndex(proxy.index(3, 0), QItemSelectionModel::NoUpdate);

        QCOMPARE(m.moveTracks(rowsOf(m, {"B", "D"}), 5), 3);   // A C E B D
        QCOMPARE(m.currentTrack()->title, QStringLiteral("B"));
        QCOMPARE(m.currentRow(), 3);
        QVERIFY(!m.isCurrentDetached());
        QCOMPARE(title(m, m.nextRow()), QStringLiteral("D"));
        QCOMPARE(title(m, m.previousRow()), QStringLiteral("E"));
        QCOMPARE(m.data(m.index(3, PlaylistModel::ColIndex), Qt::DisplayRole).toInt(), 4);
        QVERIFY(sel.isRowSelected(3, {}));
        QVERIFY(sel.isRowSelected(4, {}));
        QVERIFY(!sel.isRowSelected(1, {}));
        QCOMPARE(m.track(proxy.mapToSource(sel.currentIndex()).row()).title, QStringLiteral("D"));
    }

    // 10k scattered tracks of a 100k playlist: linear, not one move per run.
    void moveManyScatteredTracksCheaply()
    {
        TrackList tracks;
        for (int i = 0; i < 100000; ++i) {
            Track t = Track::fromPath(QStringLiteral("/m/%1.mp3").arg(i));
            t.title = QString::number(i);
            t.scanned = true;
            tracks << t;
        }
        PlaylistModel m;
        m.insertTracks(-1, tracks);
        QList<int> rows;
        for (int r = 3; r < 100000; r += 10)
            rows << r;
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(m.moveTracks(rows, 50000), 45000);
        const qint64 ms = timer.elapsed();
        qInfo("10000 scattered tracks of 100000 moved in %lld ms", static_cast<long long>(ms));
        QVERIFY(ms < 1000);
        QCOMPARE(m.track(45000).title, QStringLiteral("3"));
        QCOMPARE(m.track(54999).title, QStringLiteral("99993"));
        QCOMPARE(m.track(44999).title, QStringLiteral("49999"));
        QCOMPARE(m.track(55000).title, QStringLiteral("50000"));
    }
};

QTEST_MAIN(TestPlaylistModel)
#include "test_playlistmodel.moc"
