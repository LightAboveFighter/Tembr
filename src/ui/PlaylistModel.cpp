#include "ui/PlaylistModel.h"

#include "core/MetadataReader.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCollator>
#include <QFont>
#include <QPalette>
#include <QtConcurrent>

#include <algorithm>
#include <numeric>

PlaylistModel::PlaylistModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

PlaylistModel::~PlaylistModel()
{
    for (auto *w : std::as_const(m_watchers)) {
        w->cancel();
        w->waitForFinished();
    }
}

int PlaylistModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_tracks.size());
}

int PlaylistModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant PlaylistModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_tracks.size())
        return {};
    const Track &t = m_tracks.at(index.row());
    const int col = index.column();

    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case ColIndex: return index.row() + 1;
        case ColTitle: return t.title.isEmpty() ? t.fileName() : t.title;
        case ColArtist: return t.artist;
        case ColAlbum: return t.album;
        case ColTrackNo:
            if (t.trackNo <= 0)
                return {};
            return t.discNo > 1 ? QStringLiteral("%1.%2").arg(t.discNo).arg(t.trackNo) : QString::number(t.trackNo);
        case ColYear: return t.year;
        case ColGenre: return t.genre;
        case ColDuration: return formatDuration(t.durationMs);
        case ColFormat: return t.formatSummary();
        case ColPath: return t.path;
        }
        break;
    case Qt::ToolTipRole:
        return t.isCueTrack() ? t.path + QStringLiteral("\nCUE: ") + t.cueSheet : t.path;
    case Qt::TextAlignmentRole:
        if (col == ColIndex || col == ColTrackNo || col == ColDuration)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        break;
    case Qt::FontRole:
        if (t.uid == m_currentUid) {
            QFont f;
            f.setBold(true);
            return f;
        }
        break;
    case Qt::ForegroundRole:
        if (t.missing)
            return QApplication::palette().color(QPalette::Disabled, QPalette::Text);
        if (t.uid == m_currentUid)
            return Theme::accent();
        break;
    case Qt::DecorationRole:
        if (col == ColTitle && t.uid == m_currentUid) {
            static const QIcon play = Icons::make(Icons::Kind::Play, Theme::accent());
            static const QIcon pause = Icons::make(Icons::Kind::Pause, Theme::accent());
            return m_paused ? pause : play;
        }
        break;
    }
    return {};
}

QVariant PlaylistModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, role);
    switch (section) {
    case ColIndex: return QStringLiteral("#");
    case ColTitle: return QStringLiteral("Название");
    case ColArtist: return QStringLiteral("Исполнитель");
    case ColAlbum: return QStringLiteral("Альбом");
    case ColTrackNo: return QStringLiteral("№");
    case ColYear: return QStringLiteral("Год");
    case ColGenre: return QStringLiteral("Жанр");
    case ColDuration: return QStringLiteral("Время");
    case ColFormat: return QStringLiteral("Формат");
    case ColPath: return QStringLiteral("Путь");
    }
    return {};
}

void PlaylistModel::sort(int column, Qt::SortOrder order)
{
    if (column <= ColIndex || column >= ColumnCount || m_tracks.size() < 2)
        return;

    QCollator coll;
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    auto str = [&](const QString &a, const QString &b) { return coll.compare(a, b); };
    auto num = [](qint64 a, qint64 b) { return a < b ? -1 : (a > b ? 1 : 0); };

    auto primary = [&](const Track &a, const Track &b) -> int {
        switch (column) {
        case ColTitle: return str(a.title.isEmpty() ? a.fileName() : a.title, b.title.isEmpty() ? b.fileName() : b.title);
        case ColArtist: return str(a.artist, b.artist);
        case ColAlbum: return str(a.album, b.album);
        case ColTrackNo: return num(qint64(a.discNo) * 10000 + a.trackNo, qint64(b.discNo) * 10000 + b.trackNo);
        case ColYear: return str(a.year, b.year);
        case ColGenre: return str(a.genre, b.genre);
        case ColDuration: return num(a.durationMs, b.durationMs);
        case ColFormat: {
            const int c = str(a.codec, b.codec);
            return c != 0 ? c : num(a.bitrateKbps, b.bitrateKbps);
        }
        case ColPath: return str(a.path, b.path);
        }
        return 0;
    };
    // Ties keep albums together and in track order.
    auto tieBreak = [&](const Track &a, const Track &b) -> int {
        if (int c = str(a.albumArtist.isEmpty() ? a.artist : a.albumArtist,
                        b.albumArtist.isEmpty() ? b.artist : b.albumArtist))
            return c;
        if (int c = str(a.album, b.album))
            return c;
        if (int c = num(qint64(a.discNo) * 10000 + a.trackNo, qint64(b.discNo) * 10000 + b.trackNo))
            return c;
        if (int c = str(a.path, b.path))
            return c;
        return num(a.startMs, b.startMs);
    };

    QVector<int> perm(m_tracks.size());
    std::iota(perm.begin(), perm.end(), 0);
    std::stable_sort(perm.begin(), perm.end(), [&](int ia, int ib) {
        const Track &a = m_tracks.at(ia);
        const Track &b = m_tracks.at(ib);
        if (int c = primary(a, b))
            return order == Qt::AscendingOrder ? c < 0 : c > 0;
        return tieBreak(a, b) < 0;
    });

    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    QVector<int> newRow(perm.size());
    TrackList sorted;
    sorted.reserve(m_tracks.size());
    for (int i = 0; i < perm.size(); ++i) {
        newRow[perm[i]] = i;
        sorted.append(m_tracks.at(perm[i]));
    }
    const QModelIndexList from = persistentIndexList();
    QModelIndexList to;
    to.reserve(from.size());
    for (const QModelIndex &idx : from)
        to.append(index(newRow[idx.row()], idx.column()));
    changePersistentIndexList(from, to);
    m_tracks = std::move(sorted);
    rebuildIndex();
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void PlaylistModel::setTracks(const TrackList &tracks)
{
    beginResetModel();
    m_tracks = tracks;
    for (Track &t : m_tracks) {
        if (t.uid == 0)
            t.uid = Track::newUid();
    }
    rebuildIndex();
    endResetModel();

    TrackList toScan;
    for (const Track &t : std::as_const(m_tracks)) {
        if (!t.scanned && !t.isUrl())
            toScan.append(t);
    }
    startScan(toScan);
    emit contentsChanged();
}

void PlaylistModel::insertTracks(int row, const TrackList &tracks)
{
    if (tracks.isEmpty() || m_readOnly)
        return;
    if (row < 0 || row > m_tracks.size())
        row = int(m_tracks.size());

    TrackList toInsert = tracks;
    for (Track &t : toInsert) {
        if (t.uid == 0)
            t.uid = Track::newUid();
    }
    beginInsertRows({}, row, row + int(toInsert.size()) - 1);
    for (int i = 0; i < toInsert.size(); ++i)
        m_tracks.insert(row + i, toInsert.at(i));
    rebuildIndex();
    endInsertRows();

    TrackList toScan;
    for (const Track &t : std::as_const(toInsert)) {
        if (!t.scanned && !t.isUrl())
            toScan.append(t);
    }
    startScan(toScan);
    emit contentsChanged();
}

void PlaylistModel::removeRowList(QList<int> rows)
{
    if (m_readOnly)
        return;
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [this](int r) { return r < 0 || r >= m_tracks.size(); }),
               rows.end());
    // Remove contiguous runs at once (rows are in descending order).
    for (int i = 0; i < rows.size();) {
        int j = i;
        while (j + 1 < rows.size() && rows[j + 1] == rows[j] - 1)
            ++j;
        const int first = rows[j], last = rows[i];
        beginRemoveRows({}, first, last);
        m_tracks.remove(first, last - first + 1);
        endRemoveRows();
        i = j + 1;
    }
    rebuildIndex();
    emit contentsChanged();
}

void PlaylistModel::removeMissing()
{
    QList<int> rows;
    for (int i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks.at(i).missing)
            rows.append(i);
    }
    removeRowList(rows);
}

void PlaylistModel::clear()
{
    if (m_readOnly)
        return;
    beginResetModel();
    m_tracks.clear();
    m_rowByUid.clear();
    endResetModel();
    emit contentsChanged();
}

void PlaylistModel::setMissing(int row, bool missing)
{
    if (row < 0 || row >= m_tracks.size() || m_tracks[row].missing == missing)
        return;
    m_tracks[row].missing = missing;
    emitRowChanged(row);
}

void PlaylistModel::relocate(const QList<QPair<int, QString>> &moves)
{
    if (m_readOnly)
        return;
    TrackList toScan;
    for (const auto &m : moves) {
        if (m.first < 0 || m.first >= m_tracks.size())
            continue;
        Track &t = m_tracks[m.first];
        t.path = m.second;
        t.missing = false;
        if (t.durationMs <= 0) {   // never read successfully: read the tags now
            t.scanned = false;
            toScan.append(t);
        }
        emitRowChanged(m.first);
    }
    startScan(toScan);
    emit contentsChanged();
}

int PlaylistModel::missingCount() const
{
    return int(std::count_if(m_tracks.cbegin(), m_tracks.cend(), [](const Track &t) { return t.missing; }));
}

void PlaylistModel::setCurrent(quint64 uid)
{
    if (uid == m_currentUid)
        return;
    const int old = currentRow();
    m_currentUid = uid;
    emitRowChanged(old);
    emitRowChanged(currentRow());
}

void PlaylistModel::setPaused(bool paused)
{
    if (paused == m_paused)
        return;
    m_paused = paused;
    emitRowChanged(currentRow());
}

qint64 PlaylistModel::totalDurationMs() const
{
    qint64 total = 0;
    for (const Track &t : m_tracks)
        total += t.durationMs;
    return total;
}

void PlaylistModel::startScan(const TrackList &tracks)
{
    if (tracks.isEmpty())
        return;
    auto *watcher = new QFutureWatcher<Track>(this);
    connect(watcher, &QFutureWatcher<Track>::resultsReadyAt, this, [this, watcher](int begin, int end) {
        for (int i = begin; i < end; ++i)
            applyScanned(watcher->resultAt(i));
    });
    connect(watcher, &QFutureWatcher<Track>::finished, this, [this, watcher] {
        m_watchers.removeOne(watcher);
        watcher->deleteLater();
        emit contentsChanged();
    });
    m_watchers.append(watcher);
    watcher->setFuture(QtConcurrent::mapped(tracks, &MetadataReader::read));
}

void PlaylistModel::applyScanned(const Track &t)
{
    const int row = rowOfUid(t.uid);
    if (row < 0)
        return;   // removed while being scanned
    m_tracks[row] = t;
    emitRowChanged(row);
}

void PlaylistModel::rebuildIndex()
{
    m_rowByUid.clear();
    m_rowByUid.reserve(m_tracks.size());
    for (int i = 0; i < m_tracks.size(); ++i)
        m_rowByUid.insert(m_tracks.at(i).uid, i);
}

void PlaylistModel::emitRowChanged(int row)
{
    if (row >= 0 && row < m_tracks.size())
        emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}
