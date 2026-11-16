// Dragging tracks with the mouse: reordering inside a playlist, copying into
// the other list through the round chooser. QDrag::exec() cannot run
// offscreen, so the drag itself is replaced (PlaylistView::setDragRunner) and
// drops are sent as constructed QDropEvents carrying the drag's own data.

#include "ui/DropChooser.h"
#include "ui/PlaylistFilterProxy.h"
#include "ui/PlaylistModel.h"
#include "ui/PlaylistPage.h"

#include <QApplication>
#include <QDrag>
#include <QElapsedTimer>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <memory>

namespace {

TrackList named(const QStringList &titles)
{
    TrackList out;
    for (const QString &title : titles) {
        Track t = Track::fromPath(QStringLiteral("/m/%1.mp3").arg(title));
        t.title = title;
        t.scanned = true;   // no tag reading
        t.uid = Track::newUid();
        out << t;
    }
    return out;
}

QString order(const PlaylistPage &page)
{
    QStringList out;
    for (const Track &t : page.model()->tracks())
        out << t.title;
    return out.join(QLatin1Char(' '));
}

QStringList selectedTitles(const PlaylistPage &page)
{
    QStringList out;
    for (int row : page.selectedSourceRows())
        out << page.model()->track(row).title;
    return out;
}

void selectRows(PlaylistPage &page, std::initializer_list<int> proxyRows)
{
    for (int row : proxyRows)
        page.view()->selectionModel()->select(page.proxy()->index(row, 0),
                                              QItemSelectionModel::Select | QItemSelectionModel::Rows);
}

// Middle of a row (a drop there goes before it), in viewport coordinates.
QPoint rowPoint(const PlaylistPage &page, int proxyRow)
{
    return page.view()->visualRect(page.proxy()->index(proxyRow, 1)).center();
}

QPoint belowRows(const PlaylistPage &page)
{
    const int last = page.proxy()->rowCount() - 1;
    const QRect r = last >= 0 ? page.view()->visualRect(page.proxy()->index(last, 1)) : QRect(4, 0, 10, 0);
    return QPoint(r.center().x(), r.bottom() + 8);
}

// The drag of `from`'s selection, as the page makes it (the runner keeps
// its data instead of running it).
std::unique_ptr<TrackDragData> dragOf(PlaylistPage &from)
{
    std::unique_ptr<TrackDragData> data;
    PlaylistView::setDragRunner([&data](QDrag *drag) {
        auto *tracks = qobject_cast<TrackDragData *>(drag->mimeData());
        data = std::make_unique<TrackDragData>(tracks->source(), tracks->uids());
    });
    emit from.view()->dragRequested();
    PlaylistView::setDragRunner({});
    return data;
}

// Sent as the window system would: enter, move, drop. True if the target
// took the drop.
bool dropAt(PlaylistPage &to, const QMimeData *data, const QPoint &pos)
{
    QWidget *viewport = to.view()->viewport();
    QDragEnterEvent enter(pos, Qt::CopyAction | Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &enter);
    if (!enter.isAccepted())
        return false;
    QDragMoveEvent move(pos, Qt::CopyAction | Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &move);
    QDropEvent drop(pos, Qt::CopyAction | Qt::MoveAction, data, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &drop);
    return drop.isAccepted();
}

// A move with the left button held (QTest::mouseMove of Qt 6.2 sends none).
void dragMouseTo(QWidget *viewport, const QPoint &pos, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent move(QEvent::MouseMove, pos, viewport->mapToGlobal(pos), Qt::NoButton, Qt::LeftButton, modifiers);
    QApplication::sendEvent(viewport, &move);
}

DropChooser *chooserOf(PlaylistPage &page)
{
    for (DropChooser *c : page.findChildren<DropChooser *>()) {
        if (c->isVisible())
            return c;
    }
    return nullptr;
}

} // namespace

class TestDragTracks : public QObject
{
    Q_OBJECT

    std::unique_ptr<PlaylistPage> m_a;   // a playlist
    std::unique_ptr<PlaylistPage> m_b;   // another one (the other pane)

    void show(PlaylistPage &page, const QPoint &at)
    {
        page.resize(500, 400);
        page.move(at);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("tembr-test"));
        QCoreApplication::setApplicationName(QStringLiteral("tembr-test-drag"));
        QSettings().clear();
    }
    void cleanupTestCase() { QSettings().clear(); }

    void init()
    {
        m_a = std::make_unique<PlaylistPage>(QStringLiteral("A"));
        m_b = std::make_unique<PlaylistPage>(QStringLiteral("B"));
        m_a->model()->insertTracks(-1, named({"a0", "a1", "a2", "a3", "a4", "a5"}));
        m_b->model()->insertTracks(-1, named({"b0", "b1", "b2"}));
        show(*m_a, QPoint(0, 0));
        show(*m_b, QPoint(520, 0));
    }

    void cleanup()
    {
        PlaylistView::setDragRunner({});
        m_a.reset();
        m_b.reset();
    }

    // Pressing on a selected row and moving drags the selection; pressing
    // on another row and moving selects rows, as before; a click on a
    // selected row leaves only that row selected.
    void pressOnSelectionDragsElsewhereSelects()
    {
        PlaylistView *view = m_a->view();
        QWidget *vp = view->viewport();
        selectRows(*m_a, {1, 2});
        QStringList dragged;
        PlaylistView::setDragRunner([&](QDrag *drag) {
            for (quint64 uid : qobject_cast<TrackDragData *>(drag->mimeData())->uids())
                dragged << m_a->model()->track(m_a->model()->rowOfUid(uid)).title;
        });
        QSignalSpy requested(view, &PlaylistView::dragRequested);

        QTest::mousePress(vp, Qt::LeftButton, {}, rowPoint(*m_a, 1));
        dragMouseTo(vp, rowPoint(*m_a, 1) + QPoint(0, 2));   // not far yet
        QCOMPARE(requested.count(), 0);
        dragMouseTo(vp, rowPoint(*m_a, 4));
        QCOMPARE(requested.count(), 1);
        QCOMPARE(dragged, (QStringList{"a1", "a2"}));
        QTest::mouseRelease(vp, Qt::LeftButton, {}, rowPoint(*m_a, 4));
        QCOMPARE(selectedTitles(*m_a), (QStringList{"a1", "a2"}));   // the drag left it alone

        // From a row that is not selected: a selection by dragging.
        QTest::mousePress(vp, Qt::LeftButton, {}, rowPoint(*m_a, 4));
        dragMouseTo(vp, rowPoint(*m_a, 5));
        QTest::mouseRelease(vp, Qt::LeftButton, {}, rowPoint(*m_a, 5));
        QCOMPARE(requested.count(), 1);
        QCOMPARE(selectedTitles(*m_a), (QStringList{"a4", "a5"}));

        // A click on one of the selected rows: only it stays selected.
        QTest::mouseClick(vp, Qt::LeftButton, {}, rowPoint(*m_a, 5));
        QCOMPARE(requested.count(), 1);
        QCOMPARE(selectedTitles(*m_a), (QStringList{"a5"}));

        // With Ctrl, a press on the selection toggles as before: no drag.
        selectRows(*m_a, {2});
        QTest::mousePress(vp, Qt::LeftButton, Qt::ControlModifier, rowPoint(*m_a, 2));
        dragMouseTo(vp, rowPoint(*m_a, 0), Qt::ControlModifier);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::ControlModifier, rowPoint(*m_a, 0));
        QCOMPARE(requested.count(), 1);
    }

    // Released over the tracks dragged: nothing changes.
    void dropOntoTheSelectionDoesNothing()
    {
        selectRows(*m_a, {1, 2, 4});
        auto data = dragOf(*m_a);
        QSignalSpy layout(m_a->model(), &QAbstractItemModel::layoutChanged);
        QVERIFY(dropAt(*m_a, data.get(), rowPoint(*m_a, 2)));
        QCOMPARE(order(*m_a), QStringLiteral("a0 a1 a2 a3 a4 a5"));
        QCOMPARE(layout.count(), 0);
        QCOMPARE(selectedTitles(*m_a), (QStringList{"a1", "a2", "a4"}));
        QVERIFY(!chooserOf(*m_a));
    }

    // Released elsewhere in the same playlist: moved there, still selected,
    // the playing track still current; no chooser.
    void dropElsewhereInTheSamePlaylistReorders()
    {
        m_a->model()->setCurrent(m_a->model()->track(1).uid);   // a1 plays
        selectRows(*m_a, {1, 3});
        auto data = dragOf(*m_a);
        QSignalSpy layout(m_a->model(), &QAbstractItemModel::layoutChanged);
        QVERIFY(dropAt(*m_a, data.get(), rowPoint(*m_a, 5)));   // before a5
        QCOMPARE(order(*m_a), QStringLiteral("a0 a2 a4 a1 a3 a5"));
        QCOMPARE(layout.count(), 1);
        QCOMPARE(selectedTitles(*m_a), (QStringList{"a1", "a3"}));
        QCOMPARE(m_a->model()->currentTrack()->title, QStringLiteral("a1"));
        QCOMPARE(m_a->model()->track(m_a->model()->nextRow()).title, QStringLiteral("a3"));

        // Below the last row: to the end. Above the first: to the start.
        auto again = dragOf(*m_a);
        QVERIFY(dropAt(*m_a, again.get(), belowRows(*m_a)));
        QCOMPARE(order(*m_a), QStringLiteral("a0 a2 a4 a5 a1 a3"));
        auto up = dragOf(*m_a);
        QVERIFY(dropAt(*m_a, up.get(), rowPoint(*m_a, 0)));
        QCOMPARE(order(*m_a), QStringLiteral("a1 a3 a0 a2 a4 a5"));
        QVERIFY(!chooserOf(*m_a));
    }

    // Under a search the drop row is a row of what is shown: the tracks go
    // before the track shown there; the hidden ones keep their places.
    void reorderUnderASearch()
    {
        m_a->model()->clear();
        m_a->model()->insertTracks(-1, named({"x0", "hidden1", "x2", "hidden3", "x4", "x5"}));
        m_a->setQuery(QStringLiteral("x"), TrackQuery::Field::Title);
        QCOMPARE(m_a->proxy()->rowCount(), 4);   // x0 x2 x4 x5
        selectRows(*m_a, {0});                   // x0
        auto data = dragOf(*m_a);
        QVERIFY(dropAt(*m_a, data.get(), rowPoint(*m_a, 3)));   // before x5
        QCOMPARE(order(*m_a), QStringLiteral("hidden1 x2 hidden3 x4 x0 x5"));
        QCOMPARE(selectedTitles(*m_a), (QStringList{"x0"}));

        auto end = dragOf(*m_a);
        QVERIFY(dropAt(*m_a, end.get(), belowRows(*m_a)));   // after the last shown
        QCOMPARE(order(*m_a), QStringLiteral("hidden1 x2 hidden3 x4 x5 x0"));
    }

    // Into the other list: copied, after asking where. Each zone, clicked
    // at its centre: at the start, before the row pointed at, at the end.
    void dropIntoAnotherListAsksWhere_data()
    {
        QTest::addColumn<int>("place");
        QTest::addColumn<QString>("expected");
        QTest::newRow("start") << int(DropChooser::Place::Start) << "a1 a3 b0 b1 b2";
        QTest::newRow("here") << int(DropChooser::Place::Here) << "b0 a1 a3 b1 b2";
        QTest::newRow("end") << int(DropChooser::Place::End) << "b0 b1 b2 a1 a3";
    }

    void dropIntoAnotherListAsksWhere()
    {
        QFETCH(int, place);
        QFETCH(QString, expected);
        selectRows(*m_a, {1, 3});
        auto data = dragOf(*m_a);
        QSignalSpy received(m_b.get(), &PlaylistPage::tracksReceived);
        QSignalSpy dropped(m_b.get(), &PlaylistPage::dropReceived);
        QVERIFY(dropAt(*m_b, data.get(), rowPoint(*m_b, 1)));   // before b1
        QCOMPARE(dropped.count(), 1);
        QCOMPARE(order(*m_b), QStringLiteral("b0 b1 b2"));   // not yet: the chooser asks
        QTRY_VERIFY(chooserOf(*m_b));
        DropChooser *chooser = chooserOf(*m_b);
        const QPoint at = chooser->centerOf(DropChooser::Place(place));
        QCOMPARE(chooser->placeAt(at), DropChooser::Place(place));
        QTest::mouseClick(chooser, Qt::LeftButton, {}, at);
        QCOMPARE(order(*m_b), expected);
        QCOMPARE(received.count(), 1);
        QCOMPARE(received.first().first().toInt(), 2);
        QTRY_VERIFY(!chooserOf(*m_b));
        QCOMPARE(order(*m_a), QStringLiteral("a0 a1 a2 a3 a4 a5"));   // copies: the source as it was
        QCOMPARE(selectedTitles(*m_b), (QStringList{"a1", "a3"}));   // where they went
        for (const Track &t : m_b->model()->tracks())
            QVERIFY(m_a->model()->rowOfUid(t.uid) < 0);   // uids of their own
    }

    void chooserKeys()
    {
        for (const auto &[key, expected] : {std::pair{Qt::Key_Up, "a2 b0 b1 b2"}, std::pair{Qt::Key_Return, "a2 b0 a2 b1 b2"},
                                            std::pair{Qt::Key_Down, "a2 b0 a2 b1 b2 a2"}}) {
            m_a->view()->selectionModel()->clearSelection();
            selectRows(*m_a, {2});
            auto data = dragOf(*m_a);
            QVERIFY(dropAt(*m_b, data.get(), rowPoint(*m_b, 2)));   // before b1 once a2 is first
            QTRY_VERIFY(chooserOf(*m_b));
            QTest::keyClick(chooserOf(*m_b), key);
            QCOMPARE(order(*m_b), QString::fromLatin1(expected));
            QTRY_VERIFY(!chooserOf(*m_b));
        }
    }

    // Esc or a click outside the circle: nothing is copied.
    void chooserCancelled()
    {
        selectRows(*m_a, {0});
        auto data = dragOf(*m_a);
        QSignalSpy received(m_b.get(), &PlaylistPage::tracksReceived);
        QVERIFY(dropAt(*m_b, data.get(), rowPoint(*m_b, 1)));
        QTRY_VERIFY(chooserOf(*m_b));
        QTest::keyClick(chooserOf(*m_b), Qt::Key_Escape);
        QTRY_VERIFY(!chooserOf(*m_b));

        QVERIFY(dropAt(*m_b, data.get(), rowPoint(*m_b, 1)));
        QTRY_VERIFY(chooserOf(*m_b));
        DropChooser *chooser = chooserOf(*m_b);
        QCOMPARE(chooser->placeAt(QPointF(1, 1)), DropChooser::Place::None);   // a corner of the square
        QTest::mouseClick(chooser, Qt::LeftButton, {}, QPoint(1, 1));
        QTRY_VERIFY(!chooserOf(*m_b));

        QVERIFY(dropAt(*m_b, data.get(), rowPoint(*m_b, 1)));
        QTRY_VERIFY(chooserOf(*m_b));
        // In another window, through the window system as a real click comes
        // (QTest's QWidget overload would hand it to that widget directly).
        QWidget *vp = m_a->view()->viewport();
        QTest::mouseClick(m_a->windowHandle(), Qt::LeftButton, {}, vp->mapTo(m_a.get(), rowPoint(*m_a, 4)));
        QTRY_VERIFY(!chooserOf(*m_b));

        QCOMPARE(order(*m_b), QStringLiteral("b0 b1 b2"));
        QCOMPARE(received.count(), 0);
    }

    // An empty playlist: start, here and end are the same, so no question.
    void dropIntoAnEmptyPlaylistAtOnce()
    {
        m_b->model()->clear();
        selectRows(*m_a, {4, 5});
        auto data = dragOf(*m_a);
        QVERIFY(dropAt(*m_b, data.get(), QPoint(20, 20)));
        QCOMPARE(order(*m_b), QStringLiteral("a4 a5"));
        QTest::qWait(20);
        QVERIFY(!chooserOf(*m_b));
    }

    // Local files take nothing (neither tracks nor files), but give tracks.
    void localFilesTakeNothing()
    {
        PlaylistPage local(QStringLiteral("Local"));
        local.setReadOnly(true);
        local.model()->setTracks(named({"l0", "l1", "l2"}));
        show(local, QPoint(0, 420));
        selectRows(*m_a, {0});
        auto data = dragOf(*m_a);
        QVERIFY(!dropAt(local, data.get(), rowPoint(local, 1)));   // the drag is refused on entering
        QCOMPARE(order(local), QStringLiteral("l0 l1 l2"));

        // Within itself: no reordering.
        selectRows(local, {0});
        auto own = dragOf(local);
        QVERIFY(own);
        QVERIFY(!dropAt(local, own.get(), belowRows(local)));
        QCOMPARE(order(local), QStringLiteral("l0 l1 l2"));
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(QStringLiteral("/m/new.mp3"))});
        QVERIFY(!dropAt(local, &files, rowPoint(local, 1)));

        // Out of it, into a playlist.
        QVERIFY(dropAt(*m_b, own.get(), belowRows(*m_b)));
        QTRY_VERIFY(chooserOf(*m_b));
        QTest::keyClick(chooserOf(*m_b), Qt::Key_Down);
        QCOMPARE(order(*m_b), QStringLiteral("b0 b1 b2 l0"));
    }

    // Files from a file manager still go where they are dropped.
    void filesFromAFileManager()
    {
        QSignalSpy paths(m_a->view(), &PlaylistView::pathsDropped);
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(QStringLiteral("/m/new.mp3"))});
        QVERIFY(dropAt(*m_a, &files, rowPoint(*m_a, 2)));
        QCOMPARE(paths.count(), 1);
        QCOMPARE(paths.first().at(0).toStringList(), QStringList{QStringLiteral("/m/new.mp3")});
        QCOMPARE(paths.first().at(1).toInt(), 2);   // before a2
        QVERIFY(dropAt(*m_a, &files, belowRows(*m_a)));
        QCOMPARE(paths.last().at(1).toInt(), 6);   // at the end
    }

    // 10k selected tracks of a 100k playlist: the drag starts at once (no
    // copies, no rendering), a move and a copy are linear.
    void tenThousandOfAHundredThousand()
    {
        QStringList titles;
        for (int i = 0; i < 100000; ++i)
            titles << QStringLiteral("t%1").arg(i);
        const TrackList big = named(titles);
        m_a->model()->clear();
        m_a->model()->insertTracks(-1, big);
        m_b->model()->clear();
        m_b->model()->insertTracks(-1, named(titles));
        QItemSelection every;
        const int last = m_a->proxy()->columnCount() - 1;
        for (int row = 5; row < 100000; row += 10)
            every.append(QItemSelectionRange(m_a->proxy()->index(row, 0), m_a->proxy()->index(row, last)));
        m_a->view()->selectionModel()->select(every, QItemSelectionModel::Select);
        m_a->view()->scrollTo(m_a->proxy()->index(50000, 0), QAbstractItemView::PositionAtTop);
        m_b->view()->scrollTo(m_b->proxy()->index(50000, 0), QAbstractItemView::PositionAtTop);
        QTest::qWait(50);

        QElapsedTimer timer;
        timer.start();
        auto data = dragOf(*m_a);
        const qint64 startMs = timer.elapsed();
        QCOMPARE(data->uids().size(), 10000);

        // Copied into the other 100k list, before the row pointed at.
        QVERIFY(dropAt(*m_b, data.get(), rowPoint(*m_b, 50002)));
        QTRY_VERIFY(chooserOf(*m_b));
        timer.restart();
        QTest::keyClick(chooserOf(*m_b), Qt::Key_Return);
        const qint64 copyMs = timer.elapsed();
        QCOMPARE(m_b->model()->rowCount(), 110000);
        QCOMPARE(m_b->model()->track(50002).title, QStringLiteral("t5"));

        // Moved within the first list (a row that is not selected).
        timer.restart();
        QVERIFY(dropAt(*m_a, data.get(), rowPoint(*m_a, 50002)));
        const qint64 moveMs = timer.elapsed();
        QCOMPARE(m_a->model()->track(45002).title, QStringLiteral("t5"));
        QCOMPARE(m_a->selectedSourceRows().size(), 10000);
        QCOMPARE(m_a->view()->selectionModel()->selection().size(), 1);   // one block now

        qInfo("10000 selected of 100000: drag start %lld ms, copy into another 100k list %lld ms, "
              "move within the list %lld ms",
              static_cast<long long>(startMs), static_cast<long long>(copyMs), static_cast<long long>(moveMs));
        QVERIFY(startMs < 1000);
        QVERIFY(copyMs < 3000);
        QVERIFY(moveMs < 3000);
    }
};

QTEST_MAIN(TestDragTracks)
#include "test_dragtracks.moc"
