#pragma once

#include "core/Track.h"
#include "core/TrackSort.h"

#include <QAbstractTableModel>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QTimer>

// Tracks of one playlist. Sorting reorders the playlist itself (as in AIMP),
// filtering is done on top of it by PlaylistFilterProxy. Tags are read in the
// background after tracks are inserted.
class PlaylistModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column {
        ColIndex,
        ColTitle,
        ColArtist,
        ColAlbum,
        ColTrackNo,
        ColYear,
        ColGenre,
        ColDuration,
        ColFormat,
        ColPath,
        ColAdded,      // date the file appeared on this device
        ColModified,   // file modification date
        ColumnCount
    };

    explicit PlaylistModel(QObject *parent = nullptr);
    ~PlaylistModel() override;

    QString name() const { return m_name; }
    void setName(const QString &name) { m_name = name; }

    // Read-only playlists (Local files) refuse insert/remove/relocate/clear;
    // their content is replaced as a whole with setTracks().
    bool isReadOnly() const { return m_readOnly; }
    void setReadOnly(bool readOnly) { m_readOnly = readOnly; }
    void setTracks(const TrackList &tracks);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void sort(int column, Qt::SortOrder order) override;
    // Sort key of a column; false for the "#" column.
    static bool sortKey(int column, TrackSort::Key *key);

    const TrackList &tracks() const { return m_tracks; }
    const Track &track(int row) const { return m_tracks.at(row); }
    int rowOfUid(quint64 uid) const { return m_rowByUid.value(uid, -1); }

    // row < 0 appends. Unscanned local files get their tags read in the background.
    void insertTracks(int row, const TrackList &tracks);
    // Copies of rows for another playlist (sent there, dragged there): with
    // their tags, so nothing is read again, and without their uids, so the
    // playlist they go to gives each its own (the same track sent twice is
    // two entries). Not the playing / missing marks of this list.
    TrackList copiesOf(const QList<int> &rows) const;
    void removeRowList(QList<int> rows);
    void removeMissing();
    void clear();
    void setMissing(int row, bool missing);
    // Points tracks at their new location (after "find moved files").
    void relocate(const QList<QPair<int, QString>> &moves);
    int missingCount() const;

    // The track the player is on (bold + play/pause icon). uid 0 = none.
    quint64 currentUid() const { return m_currentUid; }
    int currentRow() const { return rowOfUid(m_currentUid); }
    void setCurrent(quint64 uid);

    // The current track stays current when its row goes away (removed from
    // the playlist, its file deleted, Local files rescanned): it keeps
    // playing and showing, and the player goes on from where it was.
    const Track *currentTrack() const;   // nullptr: none
    bool isCurrentDetached() const { return m_currentUid != 0 && currentRow() < 0; }
    // Rows to go to from the current track, -1 at the ends. For a removed
    // one: the first track that came after (before) it and is still here,
    // else the track now in its place; so it does not depend on what else
    // was removed or how the list was reordered meanwhile.
    int nextRow() const;
    int previousRow() const;
    void setPaused(bool paused);

    qint64 totalDurationMs() const;
    bool isScanning() const { return !m_watchers.isEmpty(); }

signals:
    void contentsChanged();   // tracks added/removed/updated: refresh totals
    void currentDetached();   // the current track's row went away (see currentTrack())
    void sorted(int column, Qt::SortOrder order);

private:
    void startScan(const TrackList &tracks);
    void removeScattered(const QList<int> &descendingRows);
    void applyScanned();          // the tags read so far (m_scanned)
    void rebuildIndex();
    void rememberCurrent();   // before rows go away
    void checkCurrent();      // after: detach if its row is gone
    void emitRowChanged(int row);

    QString m_name;
    bool m_readOnly = false;
    TrackList m_tracks;
    QHash<quint64, int> m_rowByUid;
    quint64 m_currentUid = 0;
    Track m_detached;                 // copy of the current track once its row is gone
    QVector<quint64> m_after;         // uids after / before it when it went away (nearest first)
    QVector<quint64> m_before;
    int m_detachedSlot = -1;          // its row then
    bool m_checkCurrent = false;      // rememberCurrent() took a snapshot
    bool m_paused = false;
    QList<QFutureWatcher<Track> *> m_watchers;
    TrackList m_scanned;          // tags read, not applied yet
    QTimer m_applyTimer;
};
