#include "ui/PlaylistPage.h"

#include "core/ColumnFit.h"
#include "core/MediaCollector.h"
#include "ui/PlaylistFilterProxy.h"
#include "ui/PlaylistModel.h"

#include <QDragEnterEvent>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QMimeData>
#include <QPointer>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSettings>
#include <QStyle>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

// --- PlaylistView ------------------------------------------------------------

namespace {

// Free text: may be narrower than its longest value (elided) or wider.
bool isFlexible(int column)
{
    switch (column) {
    case PlaylistModel::ColTitle:
    case PlaylistModel::ColArtist:
    case PlaylistModel::ColAlbum:
    case PlaylistModel::ColGenre:
    case PlaylistModel::ColPath:
        return true;
    }
    return false;   // #, track no., year, length, format, dates: bounded
}

// Widths the user set by dragging, shared by all tabs and kept between runs.
QHash<int, int> &columnUserWidths()
{
    static QHash<int, int> widths = [] {
        QHash<int, int> out;
        const QVariantMap saved = QSettings().value(QStringLiteral("columns/userWidths")).toMap();
        for (auto it = saved.cbegin(); it != saved.cend(); ++it)
            out.insert(it.key().toInt(), it.value().toInt());
        return out;
    }();
    return widths;
}

void saveUserWidths()
{
    QVariantMap out;
    const QHash<int, int> &w = columnUserWidths();
    for (auto it = w.cbegin(); it != w.cend(); ++it)
        out.insert(QString::number(it.key()), it.value());
    QSettings().setValue(QStringLiteral("columns/userWidths"), out);
}

} // namespace

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
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionHidden(PlaylistModel::ColPath, true);
    header->setSectionHidden(PlaylistModel::ColAdded, true);
    header->setSectionHidden(PlaylistModel::ColModified, true);
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

    // Column widths follow the content and the window.
    m_fitTimer.setSingleShot(true);
    m_fitTimer.setInterval(120);   // tags arrive track by track while scanning
    connect(&m_fitTimer, &QTimer::timeout, this, [this] {
        // After a drag the other columns stay as they are (the table may
        // scroll) until a column is shown or hidden or the window resized;
        // new content alone (the next track, a pause, tags) does not move them.
        if (m_keepLayout && !m_layoutWanted)
            return;
        fitColumns();
    });
    for (auto sig : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
        connect(m_model, sig, this, [this] { scheduleFit(true); });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] { scheduleFit(true); });
    connect(m_model, &QAbstractItemModel::dataChanged, this, [this] { scheduleFit(true); });
    connect(header, &QHeaderView::sectionCountChanged, this, [this] {
        m_layoutWanted = true;
        scheduleFit(true);
    });
    connect(header, &QHeaderView::sectionResized, this, [this, header](int col, int old, int size) {
        if (m_applying)
            return;
        if (m_dragging && old > 0 && size > 0 && !header->isSectionHidden(col)) {   // not shown just now
            // Dragged by the user: only this column changes; the others are
            // laid out again when a column is shown or hidden.
            columnUserWidths().insert(col, size);
            saveUserWidths();
            m_keepLayout = true;
        } else {
            // A hidden column forgets its dragged width: shown again, it gets
            // the width from its content like a new one. (Hiding resizes the
            // section to 0 before it counts as hidden.)
            if ((size == 0 || header->isSectionHidden(col)) && columnUserWidths().remove(col))
                saveUserWidths();
            if (old == 0 && size > 0 && isVisible()) {
                // Shown from the header menu: laid out at once, not first at
                // the header's default width. (Hidden columns are not measured.)
                measure(col);
                fitColumns();
                return;
            }
            m_layoutWanted = true;
            scheduleFit(true);   // hidden, shown in another tab, layout restored, ...
        }
    });
    // Double click on a column edge: back to the width from the content.
    connect(header, &QHeaderView::sectionHandleDoubleClicked, this, [this](int col) {
        columnUserWidths().remove(col);
        saveUserWidths();
        scheduleFit(false);
    });
    m_view->viewport()->installEventFilter(this);
    header->viewport()->installEventFilter(this);
}

void PlaylistPage::addPaths(const QStringList &paths, int sourceRow, bool playFirst)
{
    if (paths.isEmpty() || m_model->isReadOnly())
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
    watcher->setFuture(QtConcurrent::run([paths] { return MediaCollector::collect(paths); }));
}

bool PlaylistPage::isReadOnly() const
{
    return m_model->isReadOnly();
}

void PlaylistPage::setReadOnly(bool readOnly)
{
    m_model->setReadOnly(readOnly);
    m_view->setAcceptDrops(!readOnly);
    m_view->viewport()->setAcceptDrops(!readOnly);
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
    if (m_model->isReadOnly())
        return;
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

// --- Column widths -------------------------------------------------------------

void PlaylistPage::resetColumnWidths()
{
    columnUserWidths().clear();
    saveUserWidths();
}

void PlaylistPage::scheduleFit(bool contentChanged)
{
    if (contentChanged)
        m_needsDirty = true;
    else
        m_layoutWanted = true;
    m_fitTimer.start();
}

bool PlaylistPage::eventFilter(QObject *watched, QEvent *e)
{
    // Cached needs: cheap, so follow the window at once. Width only: a
    // horizontal scroll bar coming or going (after dragging a column wider)
    // changes the height and must not lay the other columns out again.
    if (watched == m_view->viewport() && e->type() == QEvent::Resize && !m_needs.isEmpty()
        && static_cast<QResizeEvent *>(e)->oldSize().width() != static_cast<QResizeEvent *>(e)->size().width())
        fitColumns();
    if (watched == m_view->horizontalHeader()->viewport()) {
        // Left button only: a right click opens the column menu, which takes
        // the release, and would leave this set while columns are shown.
        if (e->type() == QEvent::MouseButtonPress)
            m_dragging = static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton;
        else if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::ContextMenu)
            m_dragging = false;
    }
    return QWidget::eventFilter(watched, e);
}

void PlaylistPage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    scheduleFit(false);   // widths may have been dragged in another tab
}

// Widest value of each column (or of just `column`). Big lists are sampled
// (evenly, plus the current track): bounded columns look alike all the way
// down, and free text is elided anyway.
void PlaylistPage::measure(int column)
{
    constexpr int kMaxRows = 4000;
    const QFontMetrics fm(m_view->font());
    QFont bold = m_view->font();
    bold.setBold(true);
    const QFontMetrics bm(bold);
    // Style sheet padding (4 + 4) and the delegate's text margins (frame margin + 1 a side).
    const int padding = 8 + 2 * (m_view->style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, m_view) + 1) + 2;
    const int icon = m_view->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, m_view) + 4;
    QHeaderView *header = m_view->horizontalHeader();

    if (m_needs.size() != PlaylistModel::ColumnCount)
        column = -1;   // nothing measured yet
    const int first = column < 0 ? 0 : column;
    const int last = column < 0 ? PlaylistModel::ColumnCount - 1 : column;
    if (column < 0)
        m_needs.fill(0, PlaylistModel::ColumnCount);
    for (int col = first; col <= last; ++col)
        m_needs[col] = header->sectionSizeHint(col);

    const int rows = m_model->rowCount();
    const int step = qMax(1, rows / kMaxRows);
    const int current = m_model->currentRow();
    auto measureRow = [&](int row) {
        const QFontMetrics &m = row == current ? bm : fm;
        for (int col = first; col <= last; ++col) {
            if (header->isSectionHidden(col))
                continue;
            const QString text = m_model->data(m_model->index(row, col), Qt::DisplayRole).toString();
            int w = m.horizontalAdvance(text) + padding;
            if (col == PlaylistModel::ColTitle)
                w += icon;   // play/pause mark of the current track
            m_needs[col] = qMax(m_needs[col], w);
        }
    };
    for (int row = 0; row < rows; row += step)
        measureRow(row);
    if (current >= 0)
        measureRow(current);
    if (column < 0)
        m_needsDirty = false;
}

void PlaylistPage::fitColumns()
{
    m_fitTimer.stop();
    if (m_needsDirty || m_needs.size() != PlaylistModel::ColumnCount)
        measure();
    QHeaderView *header = m_view->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);   // a restored layout may say Stretch

    QVector<int> visible;
    QVector<ColumnFit::Column> cols;
    const QHash<int, int> &user = columnUserWidths();
    for (int col = 0; col < PlaylistModel::ColumnCount; ++col) {
        if (header->isSectionHidden(col))
            continue;
        visible.append(col);
        ColumnFit::Column c;
        if (user.contains(col)) {
            c.need = user.value(col);   // as the user left it
            c.fixed = true;
        } else {
            c.need = m_needs[col];
            c.flexible = isFlexible(col);
            // The title comes first: a quarter of the table, if it needs that much.
            const int floor = col == PlaylistModel::ColTitle ? m_view->viewport()->width() / 4 : 60;
            c.minimum = qMin(c.need, qMax(floor, header->sectionSizeHint(col)));
        }
        cols.append(c);
    }
    const QVector<int> widths = ColumnFit::widths(cols, m_view->viewport()->width());
    m_applying = true;
    for (int i = 0; i < visible.size(); ++i) {
        if (header->sectionSize(visible[i]) != widths[i])
            header->resizeSection(visible[i], widths[i]);
    }
    m_applying = false;
    m_layoutWanted = false;
    m_keepLayout = false;
}
