#pragma once

#include "core/TrackQuery.h"

#include <QSortFilterProxyModel>
#include <QTimer>

// Search filter over a PlaylistModel. Sorting is forwarded to the source
// model, so clicking a column header reorders the playlist itself.
//
// Changed rows (tags arriving while scanning) are not re-filtered one by one:
// each run of rows that starts to match made QSortFilterProxyModel rebuild
// its mapping, a pass over the list per run. While a query is active the
// whole filter runs again instead, at most every half second.
class PlaylistFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit PlaylistFilterProxy(QObject *parent = nullptr);
    void setSourceModel(QAbstractItemModel *source) override;

    void setQuery(const QString &text, TrackQuery::Field defaultField);
    bool isFiltering() const { return !m_query.isEmpty(); }

    void sort(int column, Qt::SortOrder order) override;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    TrackQuery m_query;
    QTimer m_refilter;
};
