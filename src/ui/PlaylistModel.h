#pragma once

#include "core/Track.h"

#include <QAbstractTableModel>
#include <QFutureWatcher>
#include <QHash>
#include <QList>

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
        ColumnCount
    };

    explicit PlaylistModel(QObject *parent = nullptr);
    ~PlaylistModel() override;

    QString name() const { return m_name; }
    void setName(const QString &name) { m_name = name; }

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void sort(int column, Qt::SortOrder order) override;

    const TrackList &tracks() const { return m_tracks; }
    const Track &track(int row) const { return m_tracks.at(row); }
    int rowOfUid(quint64 uid) const { return m_rowByUid.value(uid, -1); }

    // row < 0 appends. Unscanned local files get their tags read in the background.
    void insertTracks(int row, const TrackList &tracks);
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
    void setPaused(bool paused);

    qint64 totalDurationMs() const;
    bool isScanning() const { return !m_watchers.isEmpty(); }

signals:
    void contentsChanged();   // tracks added/removed/updated: refresh totals

private:
    void startScan(const TrackList &tracks);
    void applyScanned(const Track &t);
    void rebuildIndex();
    void emitRowChanged(int row);

    QString m_name;
    TrackList m_tracks;
    QHash<quint64, int> m_rowByUid;
    quint64 m_currentUid = 0;
    bool m_paused = false;
    QList<QFutureWatcher<Track> *> m_watchers;
};
