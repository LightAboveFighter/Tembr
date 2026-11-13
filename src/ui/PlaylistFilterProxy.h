#pragma once

#include "core/TrackQuery.h"

#include <QSortFilterProxyModel>

// Search filter over a PlaylistModel. Sorting is forwarded to the source
// model, so clicking a column header reorders the playlist itself.
class PlaylistFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setQuery(const QString &text, TrackQuery::Field defaultField);
    bool isFiltering() const { return !m_query.isEmpty(); }

    void sort(int column, Qt::SortOrder order) override;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    TrackQuery m_query;
};
