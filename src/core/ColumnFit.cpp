#include "core/ColumnFit.h"

#include <QPair>

#include <algorithm>
#include <numeric>

namespace ColumnFit {

namespace {

// One round: needs, the spare in proportion, or short of room the free
// columns giving way. Minimums are left to widths().
QVector<int> share(const QVector<Column> &columns, int available)
{
    QVector<int> out(columns.size());
    QVector<int> flex, grow;   // grow: sized from the content, not by the user
    int rest = available;
    for (int i = 0; i < columns.size(); ++i) {
        if (columns[i].flexible && !columns[i].fixed) {
            flex.append(i);
        } else {
            out[i] = columns[i].need;
            rest -= out[i];
        }
        if (!columns[i].fixed)
            grow.append(i);
    }

    // Spread `amount` evenly over `cols`, the pixels that do not divide to the first ones.
    auto spread = [&](const QVector<int> &cols, int amount) {
        const int n = cols.size();
        for (int k = 0; k < n; ++k)
            out[cols[k]] += amount / n + (k < amount % n ? 1 : 0);
    };
    // Spread `amount` over `cols` in proportion to their needs; the pixels
    // the rounding leaves go to the largest fractions.
    auto spreadByNeed = [&](const QVector<int> &cols, int amount) {
        qint64 total = 0;
        for (int i : cols)
            total += columns[i].need;
        if (total <= 0) {
            spread(cols, amount);
            return;
        }
        QVector<QPair<qint64, int>> fractions;   // remainder, column
        int given = 0;
        for (int i : cols) {
            const qint64 share = qint64(amount) * columns[i].need;
            out[i] += int(share / total);
            given += int(share / total);
            fractions.append({share % total, i});
        }
        std::stable_sort(fractions.begin(), fractions.end(),
                         [](const auto &a, const auto &b) { return a.first > b.first; });
        for (int k = 0; k < amount - given; ++k)
            ++out[fractions[k % fractions.size()].second];
    };

    int needed = 0;
    for (int i : flex)
        needed += columns[i].need;
    if (needed <= rest) {   // everything fits: needs, plus the spare to all in proportion
        for (int i : flex)
            out[i] = columns[i].need;
        if (rest > needed && !grow.isEmpty())
            spreadByNeed(grow, rest - needed);
        return out;
    }

    // Short of room: the smallest needs are met first while they are under
    // an even share; the others split what is left.
    std::sort(flex.begin(), flex.end(), [&](int a, int b) { return columns[a].need < columns[b].need; });
    int k = 0;
    while (k < flex.size()) {
        const int share = qMax(0, rest) / int(flex.size() - k);
        if (columns[flex[k]].need > share)
            break;
        out[flex[k]] = columns[flex[k]].need;
        rest -= columns[flex[k]].need;
        ++k;
    }
    const QVector<int> shortOf = flex.mid(k);
    for (int i : shortOf)
        out[i] = 0;
    spread(shortOf, qMax(0, rest));
    return out;
}

} // namespace

QVector<int> widths(const QVector<Column> &columnsIn, int available)
{
    // A free column given less than its minimum keeps the minimum, and the
    // others share what is left (so the title's floor is taken from the
    // other text columns, not added on top). Only when every free column is
    // at its minimum may the table get wider than the window and scroll.
    QVector<Column> columns = columnsIn;
    for (;;) {
        const QVector<int> out = share(columns, available);
        bool raised = false;
        for (int i = 0; i < columns.size(); ++i) {
            Column &c = columns[i];
            if (c.flexible && !c.fixed && out[i] < c.minimum) {
                c.flexible = false;   // from now on exactly its minimum
                c.need = c.minimum;
                raised = true;
            }
        }
        if (!raised)
            return out;
    }
}

} // namespace ColumnFit
