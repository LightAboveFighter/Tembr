#include "ui/PlaylistFilterProxy.h"

#include "ui/PlaylistModel.h"

void PlaylistFilterProxy::setQuery(const QString &text, TrackQuery::Field defaultField)
{
    m_query = TrackQuery::parse(text, defaultField);
    // One layoutChanged instead of a signal for every run of rows that comes
    // or goes (with scattered hits: thousands, each redrawn and counted).
    invalidate();
}

void PlaylistFilterProxy::sort(int column, Qt::SortOrder order)
{
    if (sourceModel())
        sourceModel()->sort(column, order);
}

bool PlaylistFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex &) const
{
    if (m_query.isEmpty())
        return true;
    const auto *model = static_cast<const PlaylistModel *>(sourceModel());
    return m_query.matches(model->track(sourceRow));
}
