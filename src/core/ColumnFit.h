#pragma once

#include <QVector>

// Widths of table columns. Every column first gets what its widest value
// needs. Room to spare is shared by all of them in proportion to that, so
// the table fills the window and keeps its look: a column twice as wide as
// another stays twice as wide. Short of room, columns whose content has a
// known bound (track number, year, length, dates, format) keep their need
// and the free-text columns (title, artist, album, ...) give way: one
// needing less than an even share keeps what it needs, the others split
// what is left evenly, never below their minimum (which then comes out of
// the other free columns; only when all are at their minimum is the table
// wider than the window). A width the user set by hand stays as it is.
namespace ColumnFit {

struct Column
{
    int need = 0;            // width of the widest content (and the header)
    bool flexible = false;   // free text: may get less than `need`, or more
    int minimum = 0;         // a flexible column never gets less
    bool fixed = false;      // set by the user: exactly `need`, no share of the spare
};

QVector<int> widths(const QVector<Column> &columns, int available);

} // namespace ColumnFit
