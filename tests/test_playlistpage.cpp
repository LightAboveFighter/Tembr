#include "ui/PlaylistModel.h"
#include "ui/PlaylistFilterProxy.h"
#include "ui/PlaylistPage.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

class TestPlaylistPage : public QObject
{
    Q_OBJECT

    static TrackList tracks()
    {
        TrackList out;
        for (int i = 0; i < 20; ++i) {
            Track t = Track::fromPath(QStringLiteral("/m/Artist %1/Album/%2.mp3").arg(i).arg(i));
            t.title = QStringLiteral("A rather long title number %1 to need room").arg(i);
            t.artist = QStringLiteral("Artist with a long name %1").arg(i);
            t.album = QStringLiteral("Album %1").arg(i);
            t.year = QStringLiteral("1999");
            t.durationMs = 200000;
            t.scanned = true;
            t.uid = Track::newUid();
            out << t;
        }
        return out;
    }

    static QVector<int> sizes(QHeaderView *h)
    {
        QVector<int> out;
        for (int c = 0; c < h->count(); ++c)
            out << (h->isSectionHidden(c) ? -1 : h->sectionSize(c));
        return out;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("tembr-test"));
        QCoreApplication::setApplicationName(QStringLiteral("tembr-test-page"));
        QSettings().clear();
    }
    void cleanupTestCase() { QSettings().clear(); }

    // Dragging one column wider changes only that one; hiding a column
    // lays them all out again, and a hidden column comes back laid out
    // from its content, at once.
    void dragChangesOnlyThatColumn()
    {
        PlaylistPage page(QStringLiteral("Test"));
        page.resize(900, 400);
        page.show();
        page.model()->insertTracks(-1, tracks());
        QHeaderView *h = page.view()->horizontalHeader();
        QTest::qWait(300);   // the fit after the content
        const QVector<int> before = sizes(h);
        QVERIFY(before[PlaylistModel::ColTitle] > 0);

        // As a mouse drag on the header does it: button down, resize, up.
        QWidget *vp = h->viewport();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), vp->mapToGlobal(QPointF(5, 5)), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &press);
        h->resizeSection(PlaylistModel::ColYear, before[PlaylistModel::ColYear] + 150);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(5, 5), vp->mapToGlobal(QPointF(5, 5)), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &release);
        QTest::qWait(300);

        QVector<int> after = sizes(h);
        QCOMPARE(after[PlaylistModel::ColYear], before[PlaylistModel::ColYear] + 150);
        after[PlaylistModel::ColYear] = before[PlaylistModel::ColYear];
        QCOMPARE(after, before);   // nothing else moved
        int dragged = 0;
        for (int w : sizes(h))
            dragged += qMax(0, w);
        QVERIFY(dragged > page.view()->viewport()->width());   // so the table is wider now (scrolls)

        // New content alone does not undo it: the next track, a pause.
        const QVector<int> afterDrag = sizes(h);
        page.model()->setCurrent(page.model()->track(3).uid);
        page.model()->setPaused(true);
        QTest::qWait(300);
        QCOMPARE(sizes(h), afterDrag);

        // Hiding a column: the room is shared out again, the dragged width stays.
        h->setSectionHidden(PlaylistModel::ColAlbum, true);
        QTest::qWait(300);
        const QVector<int> hidden = sizes(h);
        QCOMPARE(hidden[PlaylistModel::ColYear], before[PlaylistModel::ColYear] + 150);
        int total = 0;
        for (int w : hidden)
            total += qMax(0, w);
        QCOMPARE(total, page.view()->viewport()->width());   // laid out again to fill the table

        // The dragged column hidden and shown again: back to its width from
        // the content, the dragged one is forgotten. From the column menu: a
        // right click on the header, whose release the menu takes.
        QMouseEvent rightPress(QEvent::MouseButtonPress, QPointF(5, 5), vp->mapToGlobal(QPointF(5, 5)), Qt::RightButton,
                               Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &rightPress);
        h->setSectionHidden(PlaylistModel::ColYear, true);
        QTest::qWait(300);
        h->setSectionHidden(PlaylistModel::ColYear, false);
        // At once at its width, not first at the header's default one.
        const QVector<int> shown = sizes(h);
        QTest::qWait(300);
        QCOMPARE(sizes(h), shown);
        QVERIFY(shown[PlaylistModel::ColYear] < before[PlaylistModel::ColYear] + 150);
        QVERIFY(shown[PlaylistModel::ColYear] >= h->sectionSizeHint(PlaylistModel::ColYear));
        total = 0;
        for (int w : sizes(h))
            total += qMax(0, w);
        QCOMPARE(total, page.view()->viewport()->width());

        PlaylistPage::resetColumnWidths();
    }

    // Under a search, a track whose data changes (tags read, file found
    // elsewhere) comes in or goes out with the next re-filter, not at once.
    void searchFollowsChangedTracks()
    {
        PlaylistPage page(QStringLiteral("P"));
        TrackList tracks;
        for (const char *n : {"a", "b", "c"}) {
            Track t = Track::fromPath(QStringLiteral("/m/%1.mp3").arg(QLatin1String(n)));
            t.scanned = true;
            tracks << t;
        }
        page.model()->insertTracks(-1, tracks);
        page.setQuery(QStringLiteral("moved"), TrackQuery::Field::Path);
        QCOMPARE(page.proxy()->rowCount(), 0);
        page.model()->relocate({{1, QStringLiteral("/m/moved/b.mp3")}});
        QTRY_COMPARE_WITH_TIMEOUT(page.proxy()->rowCount(), 1, 2000);
        QCOMPARE(page.proxy()->mapToSource(page.proxy()->index(0, 0)).row(), 1);
        page.model()->relocate({{1, QStringLiteral("/m/b.mp3")}});
        QTRY_COMPARE_WITH_TIMEOUT(page.proxy()->rowCount(), 0, 2000);
    }

    // A rescan of Local files replaces the whole list while the user may be
    // selecting tracks: the selection and the current track follow the
    // tracks (by uid), whatever rows they end up on.
    void replaceKeepsSelectionAndCurrent()
    {
        PlaylistPage page(QStringLiteral("Local"));
        page.resize(600, 300);
        page.show();
        const TrackList list = named({"a", "b", "c", "d", "e", "f"});
        page.model()->setTracks(list);
        select(page, {1, 2, 4});   // b, c, e
        setCurrent(page, 4);       // e

        // b (selected) is gone, e (selected, current) moves up, x is new.
        const Track x = named({"x"}).first();
        page.replaceTracks({list[0], list[4], x, list[2], list[3], list[5]});
        QCOMPARE(selectedUids(page), (QSet<quint64>{list[2].uid, list[4].uid}));
        QCOMPARE(currentUid(page), list[4].uid);
        QVERIFY(page.view()->selectionModel()->isRowSelected(1, {}));   // whole rows
        QVERIFY(page.view()->selectionModel()->isRowSelected(3, {}));

        // The current track gone: no current, the rest of the selection stays.
        page.replaceTracks({list[2], list[3], list[5]});
        QCOMPARE(selectedUids(page), (QSet<quint64>{list[2].uid}));
        QVERIFY(!page.view()->currentIndex().isValid());
    }

    void replaceKeepsSelectionUnderSearch()
    {
        PlaylistPage page(QStringLiteral("Local"));
        page.resize(600, 300);
        page.show();
        // Shown under the search: a, c, d, e (b is filtered out).
        const TrackList list = named({"a keep", "b other", "c keep", "d keep", "e keep"});
        page.model()->setTracks(list);
        page.setQuery(QStringLiteral("keep"), TrackQuery::Field::Title);
        QCOMPARE(page.proxy()->rowCount(), 4);
        select(page, {0, 1, 3});   // proxy rows: a, c, e
        setCurrent(page, 2);       // d

        // a (selected) is gone, e (selected) moves to the top, x is new and
        // shown, y is new and hidden, c (selected) moves down.
        const TrackList fresh = named({"x keep", "y other"});
        page.replaceTracks({list[1], list[4], fresh[0], fresh[1], list[3], list[2]});
        QCOMPARE(page.proxy()->rowCount(), 4);   // e, x, d, c
        QCOMPARE(selectedUids(page), (QSet<quint64>{list[2].uid, list[4].uid}));
        QCOMPARE(currentUid(page), list[3].uid);
        QItemSelectionModel *sel = page.view()->selectionModel();
        QVERIFY(sel->isRowSelected(0, {}));    // e
        QVERIFY(!sel->isRowSelected(1, {}));   // x
        QVERIFY(!sel->isRowSelected(2, {}));   // d: current, not selected
        QVERIFY(sel->isRowSelected(3, {}));    // c
    }

    // Local files may hold 100k tracks, all of them selected (Ctrl+A).
    void replaceKeepsBigSelectionCheaply()
    {
        constexpr int kCount = 100000;
        TrackList list;
        list.reserve(kCount);
        for (int i = 0; i < kCount; ++i) {
            Track t = Track::fromPath(QStringLiteral("/m/%1.mp3").arg(i));
            t.title = QStringLiteral("Title %1").arg(i);
            t.scanned = true;
            t.uid = Track::newUid();
            list << t;
        }
        PlaylistPage page(QStringLiteral("Local"));
        page.resize(600, 300);
        page.show();
        page.model()->setTracks(list);
        QItemSelectionModel *sel = page.view()->selectionModel();
        page.view()->selectAll();
        setCurrent(page, 500);

        // The first track gone, a new one at the end: every row moves.
        TrackList next = list.mid(1);
        next << named({"new"}).first();
        QElapsedTimer bare;
        bare.start();
        page.model()->setTracks(list);   // the reset alone, for comparison
        const qint64 resetMs = bare.elapsed();
        page.view()->selectAll();
        setCurrent(page, 500);

        QElapsedTimer timer;
        timer.start();
        page.replaceTracks(next);
        const qint64 allMs = timer.elapsed();
        QCOMPARE(sel->selection().size(), 1);   // one range, not 100k
        QCOMPARE(sel->selection().first().top(), 0);
        QCOMPARE(sel->selection().first().bottom(), kCount - 2);
        QCOMPARE(currentUid(page), list[500].uid);

        // Every other row selected: 50k ranges.
        page.model()->setTracks(list);
        QItemSelection every;
        for (int row = 0; row < kCount; row += 2)
            every.append(QItemSelectionRange(page.proxy()->index(row, 0),
                                             page.proxy()->index(row, page.proxy()->columnCount() - 1)));
        sel->select(every, QItemSelectionModel::Select);
        timer.restart();
        page.replaceTracks(next);
        const qint64 scatteredMs = timer.elapsed();
        QCOMPARE(sel->selection().size(), kCount / 2 - 1);   // the first one is gone

        qInfo("100k tracks: reset alone %lld ms; replaceTracks with all selected %lld ms, "
              "with every other row selected %lld ms",
              resetMs, allMs, scatteredMs);
        QVERIFY2(allMs < 2000, "keeping a full selection must stay linear");
        QVERIFY2(scatteredMs < 5000, "keeping a scattered selection must stay linear");
    }

private:
    static TrackList named(std::initializer_list<const char *> titles)
    {
        TrackList out;
        for (const char *title : titles) {
            Track t = Track::fromPath(QStringLiteral("/m/%1.mp3").arg(QLatin1String(title)));
            t.title = QLatin1String(title);
            t.scanned = true;
            t.uid = Track::newUid();
            out << t;
        }
        return out;
    }

    static void select(PlaylistPage &page, std::initializer_list<int> proxyRows)
    {
        for (int row : proxyRows)
            page.view()->selectionModel()->select(page.proxy()->index(row, 0),
                                                  QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }

    static void setCurrent(PlaylistPage &page, int proxyRow)
    {
        page.view()->selectionModel()->setCurrentIndex(page.proxy()->index(proxyRow, 0),
                                                       QItemSelectionModel::NoUpdate);
    }

    static QSet<quint64> selectedUids(PlaylistPage &page)
    {
        QSet<quint64> out;
        for (int row : page.selectedSourceRows())
            out.insert(page.model()->track(row).uid);
        return out;
    }

    static quint64 currentUid(PlaylistPage &page)
    {
        const QModelIndex idx = page.proxy()->mapToSource(page.view()->currentIndex());
        return idx.isValid() ? page.model()->track(idx.row()).uid : 0;
    }
};

QTEST_MAIN(TestPlaylistPage)
#include "test_playlistpage.moc"
