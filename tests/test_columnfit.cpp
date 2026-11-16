#include "core/ColumnFit.h"

#include <QTest>

#include <numeric>

using ColumnFit::Column;

class TestColumnFit : public QObject
{
    Q_OBJECT

    static int sum(const QVector<int> &v) { return std::accumulate(v.begin(), v.end(), 0); }

private slots:
    // Bounded columns get exactly their content; a short free column too;
    // the long ones split the rest evenly.
    void shortOfRoom()
    {
        const QVector<Column> cols = {
            {40, false, 0},    // #
            {500, true, 60},   // title
            {300, true, 60},   // artist
            {90, true, 60},    // genre: fits in an even share
            {50, false, 0},    // length
            {125, false, 0},   // date modified
        };
        const QVector<int> w = ColumnFit::widths(cols, 800);
        QCOMPARE(w[0], 40);
        QCOMPARE(w[4], 50);
        QCOMPARE(w[5], 125);
        QCOMPARE(w[3], 90);
        QVERIFY(qAbs(w[1] - w[2]) <= 1);   // both short of room: the same (odd pixel aside)
        QCOMPARE(w[1] + w[2], 800 - 40 - 50 - 125 - 90);
        QCOMPARE(sum(w), 800);              // and the table is filled
    }

    void unevenRemainder()
    {
        const QVector<int> w = ColumnFit::widths({{500, true, 0}, {500, true, 0}, {500, true, 0}}, 301);
        QCOMPARE(sum(w), 301);
        QVERIFY(qAbs(w[0] - w[2]) <= 1);
    }

    // Room to spare: every need met, the spare to all columns in proportion
    // to their content, so each is wider by the same factor.
    void roomToSpare()
    {
        const QVector<Column> cols = {{40, false, 0}, {200, true, 0}, {100, true, 0}, {50, false, 0}};
        const QVector<int> w = ColumnFit::widths(cols, 1000);
        QCOMPARE(sum(w), 1000);
        for (int i = 0; i < cols.size(); ++i)
            QVERIFY2(qAbs(w[i] - cols[i].need * 1000.0 / 390) <= 1, qPrintable(QString::number(i)));
    }

    // A width set by hand stays as it is; the others share the spare.
    void userWidthStays()
    {
        Column dragged{120, false, 0};
        dragged.fixed = true;
        Column draggedText{300, true, 60};
        draggedText.fixed = true;
        const QVector<int> w = ColumnFit::widths({{50, false, 0}, dragged, {100, true, 0}, draggedText}, 1020);
        QCOMPARE(w[1], 120);
        QCOMPARE(w[3], 300);
        QCOMPARE(w[0], 50 + 150);
        QCOMPARE(w[2], 100 + 300);
    }

    // Only bounded columns: they grow too, the table is filled.
    void onlyBounded()
    {
        const QVector<int> w = ColumnFit::widths({{40, false, 0}, {60, false, 0}}, 300);
        QCOMPARE(w, (QVector<int>{120, 180}));
    }

    // The title's floor (a quarter of the table) comes out of the other text
    // columns: the table still fits the window, no scroll bar.
    void floorTakenFromOtherTextColumns()
    {
        const QVector<Column> cols = {
            {44, false, 0},     // #
            {400, true, 250},   // title, floor 1000 / 4
            {150, true, 60},    // artist
            {150, true, 60},    // album
            {40, false, 0},     // track no.
            {50, false, 0},     // year
            {100, true, 60},    // genre
            {60, false, 0},     // length
            {170, false, 0},    // format
        };
        const QVector<int> w = ColumnFit::widths(cols, 1000);
        QCOMPARE(sum(w), 1000);
        QCOMPARE(w[1], 250);
        QCOMPARE(w[6], 100);                 // fits an even share: its need
        QVERIFY(qAbs(w[2] - w[3]) <= 1);     // the rest split evenly
        for (int i : {0, 4, 5, 7, 8})
            QCOMPARE(w[i], cols[i].need);    // bounded columns keep theirs
    }

    // A window narrower than the bounded columns: those keep their width
    // (the table scrolls), free columns get their minimum.
    void tooNarrow()
    {
        const QVector<int> w = ColumnFit::widths({{300, false, 0}, {400, true, 60}, {300, false, 0}}, 500);
        QCOMPARE(w, (QVector<int>{300, 60, 300}));
    }
};

QTEST_GUILESS_MAIN(TestColumnFit)
#include "test_columnfit.moc"
