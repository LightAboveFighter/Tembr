#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

// One playlist entry. For tracks coming from a CUE sheet, `path` is the real
// audio file and [startMs, endMs) is the range inside it (endMs == 0 means
// "until the end of the file").
struct Track
{
    quint64 uid = 0;            // stable id inside the running app (see newUid)

    QString path;               // local file path or a URL (http://...)
    QString cueSheet;           // .cue the track came from, if any
    qint64 startMs = 0;
    qint64 endMs = 0;

    // Tags
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    QString genre;
    QString year;
    QString composer;
    QString comment;
    int trackNo = 0;
    int discNo = 0;

    // Technical info
    qint64 durationMs = 0;
    QString codec;
    int bitrateKbps = 0;
    int sampleRate = 0;
    int channels = 0;
    qint64 fileSize = 0;
    // Dates, ms since the epoch (0 = unknown). `addedMs` is when the file
    // appeared on this device (see FileTimes::addedMs), `modifiedMs` its mtime.
    qint64 addedMs = 0;
    qint64 modifiedMs = 0;

    bool scanned = false;       // metadata has been read from the file
    bool missing = false;       // file could not be opened

    bool isUrl() const;
    bool isCueTrack() const { return !cueSheet.isEmpty(); }
    QString fileName() const;
    // "Artist - Title", or the file name when there are no tags.
    QString displayTitle() const;
    QString formatSummary() const;   // "FLAC, 44.1 kHz, 912 kbps"

    static quint64 newUid();
    static Track fromPath(const QString &path);
};

QString formatDuration(qint64 ms);

using TrackList = QList<Track>;
