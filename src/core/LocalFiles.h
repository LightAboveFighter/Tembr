#pragma once

#include "core/Track.h"

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>

// "Local files": every track in the music folders, like the Local files of
// AIMP's music library. Same settings as there: folders searched with their
// subfolders, excluded folders, and following changes in the folders.
struct LocalFilesSettings
{
    QStringList folders;
    QStringList excluded;
    bool watch = true;

    bool operator==(const LocalFilesSettings &o) const
    {
        return folders == o.folders && excluded == o.excluded && watch == o.watch;
    }

    // Saved settings; on first use taken from AIMP if it is installed,
    // otherwise ~/Music (if it exists). `firstRun` tells which.
    static LocalFilesSettings load(bool *firstRun = nullptr);
    void save() const;

    // AIMP's music library settings ([MusicLibrary.TAIMPMLLocalDataStorage.SearchOptions]
    // in AIMP.ini), with Wine paths turned into local ones. `iniPath` overrides
    // where AIMP.ini is looked for (tests); `source` gets the file used.
    static bool fromAimp(LocalFilesSettings &out, QString *source = nullptr, const QString &iniPath = {});
};

namespace LocalLibrary {

// The tracks under the folders, grouped by folder in natural order (as AIMP
// groups Local files by %FileDir). Tracks of `known` whose file is unchanged
// (same size) are reused with their tags; the rest are new and unscanned.
// `dirs` receives the folders walked. Blocking.
TrackList scan(const LocalFilesSettings &settings, const TrackList &known, QStringList *dirs = nullptr);

} // namespace LocalLibrary

// Keeps the list up to date: background scans, a cache between runs, and
// rescans when files appear in or vanish from the folders.
class LocalFiles : public QObject
{
    Q_OBJECT
public:
    explicit LocalFiles(QObject *parent = nullptr);
    ~LocalFiles() override;

    LocalFilesSettings settings() const { return m_settings; }
    void setSettings(const LocalFilesSettings &s);   // saves, then rescans

    TrackList loadCache() const;
    void saveCache(const TrackList &tracks) const;

    // Starts a background scan; `known` are the tracks shown now (their tags
    // are kept). A scan already running is followed by another one.
    void rescan(const TrackList &known);
    bool isScanning() const { return m_watcher.isRunning(); }

signals:
    void scanned(const TrackList &tracks);
    void scanningChanged(bool scanning);
    void changedOnDisk();   // a folder changed: call rescan() with the current tracks

private:
    void watchDirs(const QStringList &dirs);

    LocalFilesSettings m_settings;
    QFutureWatcher<QPair<TrackList, QStringList>> m_watcher;
    QFileSystemWatcher m_fsWatcher;
    QTimer m_debounce;
    bool m_rescanPending = false;
    TrackList m_pendingKnown;
};
