#include "ui/PlaylistModel.h"
#include "ui/PlaylistPage.h"

#include <QApplication>
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
};

QTEST_MAIN(TestPlaylistPage)
#include "test_playlistpage.moc"
