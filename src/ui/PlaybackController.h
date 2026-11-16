#pragma once

#include "core/Track.h"
#include "ui/PlaylistModel.h"   // QPointer needs the full type

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>

class AudioEngine;

// What plays: the playlist and track the player is on, going on to the next
// one, skipping files that do not open, volume. Knows playlists as models
// only, not as tabs or views: what the window shows (which row has the focus,
// scrolling to a row, titles, the tray) is up to the window, told by signals.
class PlaybackController : public QObject
{
    Q_OBJECT
public:
    explicit PlaybackController(AudioEngine *engine, QObject *parent = nullptr);

    AudioEngine *engine() const { return m_engine; }
    // The playlist the player is on (nullptr: none, or it is gone).
    PlaylistModel *playingModel() const { return m_model; }
    // Its current track; also one whose row was removed (it plays on).
    const Track *currentTrack() const;

    // Opens a row and plays it (start = false: only opens it).
    void playRow(PlaylistModel *model, int row, bool start = true);
    // Makes a row the current track without opening it: Play goes on from
    // there (a restored session, an import from AIMP). row < 0: none.
    void setCurrent(PlaylistModel *model, int row);
    // The model is about to go: what plays from it is unloaded.
    void release(PlaylistModel *model);

    // Playing: pause; paused: go on. Stopped: false, and the caller picks
    // what to start (the row in focus, the current track, ...).
    bool pauseOrResume();
    void pause();
    void stop();
    // In the playing playlist, or in `fallback` when there is none.
    void next(PlaylistModel *fallback = nullptr);
    // After 3 s into a track: back to its start.
    void previous(PlaylistModel *fallback = nullptr);
    // Files gone from the disk: if the current track was one of them, on at
    // once to the next one that was not (paused: chosen, not played).
    void skipDeleted(const QSet<QString> &gone);

    // Whether next() / previous() have somewhere to go (MPRIS buttons).
    bool hasNext(PlaylistModel *fallback) const;
    bool hasPrevious() const;

    int volume() const { return m_volume; }   // 0..100
    void setVolume(int volume);
    void changeVolume(int delta);

signals:
    // Another current track or none, or the current one changed: show it again.
    void currentChanged();
    // The current track's tags were read.
    void currentTagsChanged();
    // playRow(): a track was chosen to play (the title on screen).
    void trackStarted(const Track &track);
    // Its file is open: the length is known.
    void trackOpened(const Track &track);
    // Moved on by itself or by next/previous: the row to show.
    void followRow(PlaylistModel *model, int row);
    void navigationChanged();
    void message(const QString &text);
    void volumeChanged(int volume);

private:
    void watch(PlaylistModel *model);
    void opened();
    void openFailed(const QString &error, bool wasPlaying);

    AudioEngine *m_engine;
    QPointer<PlaylistModel> m_model;
    QList<QMetaObject::Connection> m_modelConnections;
    int m_failStreak = 0;   // consecutive unplayable tracks skipped
    int m_volume = 70;
};
