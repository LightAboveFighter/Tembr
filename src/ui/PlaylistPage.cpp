#include "ui/PlaylistPage.h"

#include "core/ColumnFit.h"
#include "core/MediaCollector.h"
#include "ui/DropChooser.h"
#include "ui/PlaylistFilterProxy.h"
#include "ui/PlaylistModel.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QMimeData>
#include <QPointer>
#include <QMouseEvent>
#include <QPainter>
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

static PlaylistView::DragRunner &dragRunner()
{
    static PlaylistView::DragRunner runner;
    return runner;
}

void PlaylistView::setDragRunner(DragRunner runner)
{
    dragRunner() = std::move(runner);
}

void PlaylistView::runDrag(QDrag *drag)
{
    if (dragRunner())
        dragRunner()(drag);
    else
        drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
}

int PlaylistView::insertionRow(const QPoint &pos) const
{
    const QModelIndex idx = indexAt(pos);
    if (!idx.isValid())
        return pos.y() < 0 ? 0 : model()->rowCount();   // below the last row: at the end
    return idx.row() + (pos.y() > visualRect(idx).center().y() ? 1 : 0);
}

int PlaylistView::sourceInsertionRow(int proxyRow) const
{
    auto *proxy = qobject_cast<QSortFilterProxyModel *>(model());
    const int shown = proxy->rowCount();
    if (proxyRow >= 0 && proxyRow < shown)
        return proxy->mapToSource(proxy->index(proxyRow, 0)).row();
    if (shown > 0)
        return proxy->mapToSource(proxy->index(shown - 1, 0)).row() + 1;
    return proxy->sourceModel()->rowCount();
}

// A press on the selection may start a drag of it, so it must not start a
// selection by dragging (which would replace the selection). The selection
// change a click makes (only the row clicked stays selected) comes on the
// release, as before: QAbstractItemView leaves a selected row alone on press.
void PlaylistView::mousePressEvent(QMouseEvent *e)
{
    const QModelIndex idx = indexAt(e->position().toPoint());
    m_dragCandidate = e->button() == Qt::LeftButton && e->modifiers() == Qt::NoModifier && idx.isValid()
                      && selectionModel()->isRowSelected(idx.row(), {});
    m_pressPos = e->position().toPoint();
    QTableView::mousePressEvent(e);
}

void PlaylistView::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragCandidate && (e->buttons() & Qt::LeftButton)) {
        if ((e->position().toPoint() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            m_dragCandidate = false;
            emit dragRequested();   // the drag ends with the button's release
        }
        return;
    }
    m_dragCandidate = false;
    QTableView::mouseMoveEvent(e);
}

void PlaylistView::mouseReleaseEvent(QMouseEvent *e)
{
    m_dragCandidate = false;
    QTableView::mouseReleaseEvent(e);
}

bool PlaylistView::isOwnDrag(const QMimeData *mime) const
{
    const auto *tracks = qobject_cast<const TrackDragData *>(mime);
    return tracks && tracks->source() && tracks->source() == parentWidget();
}

bool PlaylistView::isOnSelection(const QPoint &pos) const
{
    const QModelIndex idx = indexAt(pos);
    return idx.isValid() && selectionModel()->isRowSelected(idx.row(), {});
}

// Local files (read-only) take nothing: acceptDrops() is off for them.
void PlaylistView::dragEnterEvent(QDragEnterEvent *e)
{
    const QMimeData *mime = e->mimeData();
    if (!acceptDrops() || !(mime->hasUrls() || qobject_cast<const TrackDragData *>(mime))) {
        e->ignore();
        return;
    }
    e->acceptProposedAction();
    showDropAt(e->position().toPoint(), isOwnDrag(mime));
}

void PlaylistView::dragMoveEvent(QDragMoveEvent *e)
{
    const QMimeData *mime = e->mimeData();
    if (!acceptDrops() || !(mime->hasUrls() || qobject_cast<const TrackDragData *>(mime))) {
        e->ignore();
        return;
    }
    const QPoint pos = e->position().toPoint();
    const bool own = isOwnDrag(mime);
    showDropAt(pos, own);
    // Near the top or bottom edge the list scrolls (QAbstractItemView's own
    // auto scroll; it stops once the pointer leaves the margin).
    if (hasAutoScroll()
        && (pos.y() < autoScrollMargin() || pos.y() > viewport()->height() - autoScrollMargin()))
        startAutoScroll();
    if (own && isOnSelection(pos)) {
        e->ignore();   // dropped onto itself: nothing would happen
        return;
    }
    if (mime->hasUrls()) {
        e->acceptProposedAction();
    } else {
        e->setDropAction(own ? Qt::MoveAction : Qt::CopyAction);
        e->accept();
    }
}

void PlaylistView::dragLeaveEvent(QDragLeaveEvent *e)
{
    hideDrop();
    stopAutoScroll();
    QTableView::dragLeaveEvent(e);
}

void PlaylistView::dropEvent(QDropEvent *e)
{
    hideDrop();
    stopAutoScroll();
    const QMimeData *mime = e->mimeData();
    if (!acceptDrops()) {
        e->ignore();
        return;
    }
    const QPoint pos = e->position().toPoint();
    if (const auto *tracks = qobject_cast<const TrackDragData *>(mime)) {
        const bool own = isOwnDrag(mime);
        e->setDropAction(own ? Qt::MoveAction : Qt::CopyAction);
        e->accept();
        emit tracksDropped(tracks, insertionRow(pos), isOnSelection(pos), viewport()->mapToGlobal(pos));
        return;
    }
    if (!mime->hasUrls()) {
        e->ignore();
        return;
    }
    QStringList paths;
    for (const QUrl &url : mime->urls())
        paths << (url.isLocalFile() ? url.toLocalFile() : url.toString());
    e->acceptProposedAction();
    emit pathsDropped(paths, sourceInsertionRow(insertionRow(pos)));
}

void PlaylistView::showDropAt(const QPoint &pos, bool own)
{
    if (m_dropShown && m_dropPos == pos && m_dropOwn == own)
        return;
    m_dropShown = true;
    m_dropPos = pos;
    m_dropOwn = own;
    viewport()->update();
}

void PlaylistView::hideDrop()
{
    if (!m_dropShown)
        return;
    m_dropShown = false;
    viewport()->update();
}

// The drop line between rows, in the accent colour. The row is found from
// the pointer at paint time, so it follows the list as it auto scrolls.
void PlaylistView::paintEvent(QPaintEvent *e)
{
    QTableView::paintEvent(e);
    if (!m_dropShown || (m_dropOwn && isOnSelection(m_dropPos)))
        return;
    const int row = insertionRow(m_dropPos);
    const int rows = model()->rowCount();
    int y = 0;
    if (row < rows)
        y = rowViewportPosition(row);
    else if (rows > 0)
        y = rowViewportPosition(rows - 1) + rowHeight(rows - 1) - 1;
    QPainter p(viewport());
    p.setPen(QPen(palette().color(QPalette::Highlight), 2));
    p.drawLine(0, y, viewport()->width(), y);
}

void PlaylistView::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Delete) {
        emit deletePressed();
        return;
    }
    QTableView::keyPressEvent(e);
}

// --- TrackDragData -----------------------------------------------------------

TrackDragData::TrackDragData(PlaylistPage *source, const QList<quint64> &uids)
    : m_source(source)
    , m_uids(uids)
{
    setData(mimeType(), {});   // hasFormat() for drop targets that only look at formats
}

QString TrackDragData::mimeType()
{
    return QStringLiteral("application/x-tembr-tracks");
}

namespace {

// Rows of the tracks still in `model`, in list order.
QList<int> rowsOfUids(const PlaylistModel *model, const QList<quint64> &uids)
{
    QList<int> rows;
    rows.reserve(uids.size());
    for (quint64 uid : uids) {
        const int row = model->rowOfUid(uid);
        if (row >= 0)
            rows.append(row);
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

QString tracksText(int n)
{
    const int mod10 = n % 10, mod100 = n % 100;
    const char *word = mod10 == 1 && mod100 != 11                            ? "трек"
                       : mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14) ? "трека"
                                                                               : "треков";
    return QStringLiteral("%1 %2").arg(n).arg(QString::fromUtf8(word));
}

} // namespace

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
    connect(m_view, &PlaylistView::dragRequested, this, &PlaylistPage::startTrackDrag);
    connect(m_view, &PlaylistView::tracksDropped, this, &PlaylistPage::dropTracks);
    connect(m_view, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &p) { emit contextMenuRequested(m_view->viewport()->mapToGlobal(p)); });
    // The totals take a pass over the whole list: once per burst of changes
    // (a removal of scattered rows, tags arriving under a filter), not once
    // per run of rows.
    m_statsTimer.setSingleShot(true);
    m_statsTimer.setInterval(0);
    connect(&m_statsTimer, &QTimer::timeout, this, &PlaylistPage::statsChanged);
    const auto stats = [this] { m_statsTimer.start(); };
    connect(m_model, &PlaylistModel::contentsChanged, this, stats);
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, stats);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, stats);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, stats);
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, stats);

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
    // Changed rows (tags read, the current track) can only make columns
    // wider: measured alone. A full measure (90 ms at 100k rows) after
    // every batch of tags kept the window busy while scanning.
    connect(m_model, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &tl, const QModelIndex &br) {
        if (m_needsDirty || m_needs.size() != PlaylistModel::ColumnCount) {
            scheduleFit(true);
            return;
        }
        if (growNeeds(tl.row(), br.row()))
            m_fitTimer.start();
    });
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
            // Hidden: what the content needs is the same, only the layout
            // changes. QHeaderView hides its hidden sections again after
            // every filter change (layoutChanged), which re-measured 100k
            // rows (~50 ms) per keystroke in every list on screen.
            scheduleFit(size > 0);   // shown in another tab, layout restored, ...
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

// The selection, by uid, with a small label "N треков" under the pointer:
// no rendering of the rows (10k selected would take a while), no copies.
void PlaylistPage::startTrackDrag()
{
    const QList<int> rows = selectedSourceRows();
    if (rows.isEmpty())
        return;
    QList<quint64> uids;
    uids.reserve(rows.size());
    for (int row : rows)
        uids.append(m_model->track(row).uid);

    auto *drag = new QDrag(m_view);
    drag->setMimeData(new TrackDragData(this, uids));
    const QString text = tracksText(int(uids.size()));
    const QFontMetrics fm = m_view->fontMetrics();
    const qreal dpr = m_view->devicePixelRatioF();
    const QSize size(fm.horizontalAdvance(text) + 16, fm.height() + 8);
    QPixmap pixmap(size * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    {
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Highlight));
        p.drawRoundedRect(QRectF(QPointF(0, 0), size), 6, 6);
        p.setPen(palette().color(QPalette::HighlightedText));
        p.drawText(QRect(QPoint(0, 0), size), Qt::AlignCenter, text);
    }
    drag->setPixmap(pixmap);
    drag->setHotSpot(QPoint(-12, -12));   // beside the pointer, not under it
    PlaylistView::runDrag(drag);
    drag->deleteLater();
}

// Dropped here. From this list: moved there (onto its own selection:
// nothing). From the other list: copied, at the start, there or at the end
// as chosen in a round popup; into an empty playlist at once.
void PlaylistPage::dropTracks(const TrackDragData *data, int proxyRow, bool onSelection, const QPoint &globalPos)
{
    if (isReadOnly())
        return;
    emit dropReceived();
    PlaylistPage *from = data->source();
    if (!from)
        return;
    const int sourceRow = m_view->sourceInsertionRow(proxyRow);
    if (from == this) {
        if (!onSelection)
            moveTracks(data->uids(), sourceRow);
        return;
    }
    if (m_model->rowCount() == 0) {
        receiveTracks(from, data->uids(), -1);   // start, here and end are all the same
        return;
    }
    // After the drop is done: it is still inside the drag's own event loop,
    // which ends with the button's release.
    QTimer::singleShot(0, this, [this, source = QPointer<PlaylistPage>(from), uids = data->uids(), sourceRow, globalPos] {
        auto *chooser = new DropChooser(this);
        connect(chooser, &DropChooser::chosen, this, [this, source, uids, sourceRow](DropChooser::Place place) {
            if (!source)
                return;
            const int row = place == DropChooser::Place::Start ? 0
                            : place == DropChooser::Place::End ? -1
                                                               : sourceRow;
            receiveTracks(source, uids, row);
        });
        chooser->popup(globalPos);
    });
}

int PlaylistPage::receiveTracks(PlaylistPage *from, const QList<quint64> &uids, int sourceRow)
{
    if (!from || isReadOnly())
        return 0;
    const TrackList copies = from->model()->copiesOf(rowsOfUids(from->model(), uids));
    if (copies.isEmpty())
        return 0;
    const int row = sourceRow < 0 || sourceRow > m_model->rowCount() ? m_model->rowCount() : sourceRow;
    m_model->insertTracks(row, copies);
    // Selected where they went, so it can be seen.
    QList<int> rows;
    rows.reserve(copies.size());
    for (int r = row; r < row + copies.size(); ++r)
        rows.append(r);
    m_view->selectionModel()->clearSelection();
    selectSourceRows(rows);
    const QModelIndex first = m_proxy->mapFromSource(m_model->index(row, 0));
    if (first.isValid()) {
        m_view->selectionModel()->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
        m_view->scrollTo(first);
    }
    emit tracksReceived(int(copies.size()));
    return int(copies.size());
}

// Moved within this list; they stay selected. The selection is set again as
// one range: carried through the layout change, 10k scattered selected rows
// were remapped one by one.
void PlaylistPage::moveTracks(const QList<quint64> &uids, int sourceRow)
{
    const QList<int> rows = rowsOfUids(m_model, uids);
    if (rows.isEmpty())
        return;
    QItemSelectionModel *sel = m_view->selectionModel();
    sel->clearSelection();
    const int first = m_model->moveTracks(rows, sourceRow);
    if (first < 0) {   // already there
        selectSourceRows(rows);
        return;
    }
    QList<int> moved;
    moved.reserve(rows.size());
    for (int r = first; r < first + rows.size(); ++r)
        moved.append(r);
    selectSourceRows(moved);
    m_view->horizontalHeader()->setSortIndicatorShown(false);   // no longer in that order
}

// Rows of the whole list, ascending; those the search hides are left out.
// Selected as runs of rows: one range for a block, not one per row.
void PlaylistPage::selectSourceRows(const QList<int> &sortedRows)
{
    QList<int> rows;
    rows.reserve(sortedRows.size());
    for (int r : sortedRows) {
        const QModelIndex idx = m_proxy->mapFromSource(m_model->index(r, 0));
        if (idx.isValid())
            rows.append(idx.row());
    }
    std::sort(rows.begin(), rows.end());
    QItemSelection selection;
    const int lastColumn = m_proxy->columnCount() - 1;
    for (qsizetype i = 0; i < rows.size();) {
        qsizetype j = i;
        while (j + 1 < rows.size() && rows[j + 1] <= rows[j] + 1)
            ++j;
        selection.append(QItemSelectionRange(m_proxy->index(rows[i], 0), m_proxy->index(rows[j], lastColumn)));
        i = j + 1;
    }
    if (!selection.isEmpty())
        m_view->selectionModel()->select(selection, QItemSelectionModel::Select);
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

// Rows are selected whole (SelectRows), so the ranges give them directly;
// QItemSelectionModel::selectedRows() checks every row against every range,
// which is quadratic for a scattered selection.
static QList<int> selectedProxyRows(const QItemSelectionModel *sel)
{
    QList<int> rows;
    for (const QItemSelectionRange &r : sel->selection()) {
        for (int row = r.top(); row <= r.bottom(); ++row)
            rows.append(row);
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

QList<int> PlaylistPage::selectedSourceRows() const
{
    QList<int> rows;
    for (int row : selectedProxyRows(m_view->selectionModel()))
        rows.append(m_proxy->mapToSource(m_proxy->index(row, 0)).row());
    std::sort(rows.begin(), rows.end());
    return rows;
}

void PlaylistPage::replaceTracks(const TrackList &tracks)
{
    // The reset drops the selection and the current index, and a rescan can
    // land at any moment, also while the user is picking tracks to send or
    // delete. Remember them by uid (a rescan keeps the uids of known files),
    // since rows move when files come and go.
    QItemSelectionModel *sel = m_view->selectionModel();
    const TrackList &old = m_model->tracks();
    auto uidAt = [&](const QModelIndex &proxyIdx) -> quint64 {
        const QModelIndex src = m_proxy->mapToSource(proxyIdx);
        return src.isValid() ? old.at(src.row()).uid : 0;
    };
    const quint64 topUid = uidAt(m_view->indexAt(QPoint(4, 4)));
    const quint64 currentUid = uidAt(m_view->currentIndex());
    const QList<int> proxyRows = selectedProxyRows(sel);
    QList<quint64> selectedUids;
    selectedUids.reserve(proxyRows.size());
    for (int row : proxyRows)
        selectedUids.append(uidAt(m_proxy->index(row, 0)));

    m_model->setTracks(tracks);

    auto proxyIndexOf = [this](quint64 uid) -> QModelIndex {
        const int row = uid ? m_model->rowOfUid(uid) : -1;
        return row < 0 ? QModelIndex() : m_proxy->mapFromSource(m_model->index(row, 0));
    };
    // Rows still there (and not filtered out), as contiguous runs: a whole
    // list selected with Ctrl+A comes back as one range in one select().
    QList<int> rows;
    rows.reserve(selectedUids.size());
    for (quint64 uid : std::as_const(selectedUids)) {
        const QModelIndex idx = proxyIndexOf(uid);
        if (idx.isValid())
            rows.append(idx.row());
    }
    std::sort(rows.begin(), rows.end());
    QItemSelection selection;
    const int lastColumn = m_proxy->columnCount() - 1;
    for (qsizetype i = 0; i < rows.size();) {
        qsizetype j = i;
        while (j + 1 < rows.size() && rows[j + 1] <= rows[j] + 1)
            ++j;
        selection.append(QItemSelectionRange(m_proxy->index(rows[i], 0), m_proxy->index(rows[j], lastColumn)));
        i = j + 1;
    }
    // NoUpdate: the current track alone is not a selection (the user may
    // have Ctrl+clicked it off).
    const QModelIndex current = proxyIndexOf(currentUid);
    if (current.isValid())
        sel->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
    if (!selection.isEmpty())
        sel->select(selection, QItemSelectionModel::Select);

    // Keep the list where the user was looking.
    const QModelIndex top = proxyIndexOf(topUid);
    if (top.isValid())
        m_view->scrollTo(top, QAbstractItemView::PositionAtTop);
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
    const QList<int> proxyRows = selectedProxyRows(m_view->selectionModel());
    const int nextProxyRow = proxyRows.isEmpty() ? m_proxy->rowCount() : proxyRows.first();
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
    m_statsTimer.start();
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
    QHeaderView *header = m_view->horizontalHeader();

    if (m_needs.size() != PlaylistModel::ColumnCount)
        column = -1;   // nothing measured yet
    const int first = column < 0 ? 0 : column;
    const int last = column < 0 ? PlaylistModel::ColumnCount - 1 : column;
    if (column < 0)
        m_needs.fill(0, PlaylistModel::ColumnCount);
    for (int col = first; col <= last; ++col)
        m_needs[col] = header->sectionSizeHint(col);

    measureRows(first, last, 0, m_model->rowCount() - 1, kMaxRows);
    if (column < 0)
        m_needsDirty = false;
}

// Widens m_needs[first..last] to what rows top..bottom need (at most
// `maxRows` of them, evenly spread, and always the current track). True if a
// column got wider.
bool PlaylistPage::measureRows(int first, int last, int top, int bottom, int maxRows)
{
    const QFontMetrics fm(m_view->font());
    QFont bold = m_view->font();
    bold.setBold(true);
    const QFontMetrics bm(bold);
    // Style sheet padding (4 + 4) and the delegate's text margins (frame margin + 1 a side).
    const int padding = 8 + 2 * (m_view->style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, m_view) + 1) + 2;
    const int icon = m_view->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, m_view) + 4;
    QHeaderView *header = m_view->horizontalHeader();
    const int current = m_model->currentRow();
    bool wider = false;
    auto measureRow = [&](int row) {
        const QFontMetrics &m = row == current ? bm : fm;
        for (int col = first; col <= last; ++col) {
            if (header->isSectionHidden(col))
                continue;
            const QString text = m_model->data(m_model->index(row, col), Qt::DisplayRole).toString();
            int w = m.horizontalAdvance(text) + padding;
            if (col == PlaylistModel::ColTitle)
                w += icon;   // play/pause mark of the current track
            if (w > m_needs[col]) {
                m_needs[col] = w;
                wider = true;
            }
        }
    };
    const int rows = bottom - top + 1;
    const int step = qMax(1, rows / maxRows);
    for (int row = top; row <= bottom; row += step)
        measureRow(row);
    if (current >= top && current <= bottom)
        measureRow(current);
    return wider;
}

bool PlaylistPage::growNeeds(int top, int bottom)
{
    return measureRows(0, PlaylistModel::ColumnCount - 1, top, bottom, 1000);
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
