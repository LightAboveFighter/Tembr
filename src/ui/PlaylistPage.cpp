#include "ui/PlaylistPage.h"

#include "core/MediaCollector.h"
#include "ui/PlaylistFilterProxy.h"
#include "ui/PlaylistModel.h"

#include <QDragEnterEvent>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QMimeData>
#include <QPointer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

// --- PlaylistView ------------------------------------------------------------

PlaylistView::PlaylistView(QWidget *parent)
    : QTableView(parent)
{
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setAlternatingRowColors(true);
    setShowGrid(false);
    setWordWrap(false);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DropOnly);
    setDropIndicatorShown(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    verticalHeader()->hide();
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 6);
    horizontalHeader()->setSectionsMovable(true);
    horizontalHeader()->setHighlightSections(false);
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
}

void PlaylistView::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
    else
        QTableView::dragEnterEvent(e);
}

void PlaylistView::dragMoveEvent(QDragMoveEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
    else
        QTableView::dragMoveEvent(e);
}

void PlaylistView::dropEvent(QDropEvent *e)
{
    if (!e->mimeData()->hasUrls()) {
        QTableView::dropEvent(e);
        return;
    }
    QStringList paths;
    for (const QUrl &url : e->mimeData()->urls())
        paths << (url.isLocalFile() ? url.toLocalFile() : url.toString());

    int row = -1;
    const QPoint pos = e->position().toPoint();
    const QModelIndex idx = indexAt(pos);
    if (idx.isValid()) {
        auto *proxy = qobject_cast<QSortFilterProxyModel *>(model());
        row = proxy->mapToSource(idx).row();
        if (pos.y() > visualRect(idx).center().y())
            ++row;
    }
    e->acceptProposedAction();
    emit pathsDropped(paths, row);
}

void PlaylistView::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Delete) {
        emit deletePressed();
        return;
    }
    QTableView::keyPressEvent(e);
}

// --- PlaylistPage ------------------------------------------------------------

PlaylistPage::PlaylistPage(const QString &name, QWidget *parent)
    : QWidget(parent)
    , m_model(new PlaylistModel(this))
    , m_proxy(new PlaylistFilterProxy(this))
    , m_view(new PlaylistView(this))
{
    m_model->setName(name);
    m_proxy->setSourceModel(m_model);
    m_view->setModel(m_proxy);
    m_view->setSortingEnabled(true);
    m_view->horizontalHeader()->setSortIndicatorShown(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);

    auto *header = m_view->horizontalHeader();
    header->resizeSection(PlaylistModel::ColIndex, 44);
    header->setSectionResizeMode(PlaylistModel::ColTitle, QHeaderView::Stretch);
    header->resizeSection(PlaylistModel::ColArtist, 160);
    header->resizeSection(PlaylistModel::ColAlbum, 160);
    header->resizeSection(PlaylistModel::ColTrackNo, 44);
    header->resizeSection(PlaylistModel::ColYear, 52);
    header->resizeSection(PlaylistModel::ColGenre, 90);
    header->resizeSection(PlaylistModel::ColDuration, 60);
    header->resizeSection(PlaylistModel::ColFormat, 130);
    header->setSectionHidden(PlaylistModel::ColPath, true);
    // Sorting reorders the playlist, so a lingering arrow would be misleading
    // once tracks are added; show it only right after a click.
    connect(header, &QHeaderView::sortIndicatorChanged, this, [header] { header->setSortIndicatorShown(true); });
    connect(m_model, &PlaylistModel::rowsInserted, this, [header] { header->setSortIndicatorShown(false); });

    connect(m_view, &QTableView::activated, this, [this](const QModelIndex &idx) {
        emit trackActivated(m_proxy->mapToSource(idx).row());
    });
    connect(m_view, &PlaylistView::pathsDropped, this,
            [this](const QStringList &paths, int row) { addPaths(paths, row); });
    connect(m_view, &PlaylistView::deletePressed, this, &PlaylistPage::removeSelected);
    connect(m_view, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &p) { emit contextMenuRequested(m_view->viewport()->mapToGlobal(p)); });
    connect(m_model, &PlaylistModel::contentsChanged, this, &PlaylistPage::statsChanged);
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, &PlaylistPage::statsChanged);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, &PlaylistPage::statsChanged);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, &PlaylistPage::statsChanged);
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, &PlaylistPage::statsChanged);
}

void PlaylistPage::addPaths(const QStringList &paths, int sourceRow, bool playFirst)
{
    if (paths.isEmpty())
        return;
    auto *watcher = new QFutureWatcher<TrackList>(this);
    connect(watcher, &QFutureWatcher<TrackList>::finished, this, [this, watcher, sourceRow, playFirst] {
        const TrackList tracks = watcher->result();
        watcher->deleteLater();
        const int row = (sourceRow < 0 || sourceRow > m_model->rowCount()) ? m_model->rowCount() : sourceRow;
        m_model->insertTracks(row, tracks);
        if (!tracks.isEmpty())
            emit tracksAdded(row, playFirst);
    });
    watcher->setFuture(QtConcurrent::run(&MediaCollector::collect, paths));
}

QList<int> PlaylistPage::selectedSourceRows() const
{
    QList<int> rows;
    for (const QModelIndex &idx : m_view->selectionModel()->selectedRows())
        rows.append(m_proxy->mapToSource(idx).row());
    std::sort(rows.begin(), rows.end());
    return rows;
}

int PlaylistPage::focusedSourceRow() const
{
    const QModelIndex cur = m_view->currentIndex();
    if (cur.isValid() && m_view->selectionModel()->isRowSelected(cur.row(), {}))
        return m_proxy->mapToSource(cur).row();
    const QList<int> rows = selectedSourceRows();
    return rows.isEmpty() ? -1 : rows.first();
}

void PlaylistPage::removeSelected()
{
    const QList<int> rows = selectedSourceRows();
    if (rows.isEmpty())
        return;
    int nextProxyRow = m_proxy->rowCount();
    for (const QModelIndex &idx : m_view->selectionModel()->selectedRows())
        nextProxyRow = std::min(nextProxyRow, idx.row());
    m_model->removeRowList(rows);
    const int count = m_proxy->rowCount();
    if (count > 0) {
        const QModelIndex idx = m_proxy->index(std::min(nextProxyRow, count - 1), 0);
        m_view->setCurrentIndex(idx);
    }
}

void PlaylistPage::setQuery(const QString &text, TrackQuery::Field field)
{
    m_proxy->setQuery(text, field);
    emit statsChanged();
}

void PlaylistPage::scrollToSourceRow(int row)
{
    const QModelIndex idx = m_proxy->mapFromSource(m_model->index(row, 0));
    if (idx.isValid())
        m_view->scrollTo(idx, QAbstractItemView::PositionAtCenter);
}
