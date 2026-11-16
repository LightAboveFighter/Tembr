#pragma once

#include "core/Track.h"
#include "core/TrackSort.h"

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
// (same size and mtime), or whose tags are not read yet, are reused as they
// are; a changed file keeps its uid but comes back unscanned with its new
// size; the rest are new and unscanned. Every track gets its file dates; a
// known track keeps the date it was added, also when its file was moved or
// renamed inside the folders (recognised by the same size and mtime).
// `dirs` receives the folders walked. Blocking.
TrackList scan(const LocalFilesSettings &settings, const TrackList &known, QStringList *dirs = nullptr);

// Whether a scan result brings nothing new for the list shown: the same
// entries at the same paths (a moved file keeps its entry) with the same
// dates, in the same order, and no file changed.
bool sameList(const TrackList &shown, const TrackList &scanned);

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
    void setSettings(const LocalFilesSettings &s);   // saves; rescan() is up to the caller

    // The list as it was last saved; safe on any thread.
    static QString cacheFile();
    static TrackList loadCache(const QString &file);
    void saveCache(const TrackList &tracks) const;

    // Starts a background scan; `known` are the tracks shown now (their tags
    // are kept). A scan already running is followed by another one.
    void rescan(const TrackList &known);
    bool isScanning() const { return m_watcher.isRunning(); }

    // The order scans come back in (sorted on the scan's thread). `sorted`
    // false: folder order. Each change counts up sortGeneration().
    void setSort(bool sorted, TrackSort::Key key, Qt::SortOrder order);
    int sortGeneration() const { return m_sortGeneration; }

signals:
    // `sortGeneration`: the sort the tracks are in (see setSort()).
    void scanned(const TrackList &tracks, int sortGeneration);
    void scanningChanged(bool scanning);
    // A folder changed, or a rescan was asked for while one ran: call
    // rescan() with the tracks shown now.
    void changedOnDisk();

private:
    void watchDirs(const QStringList &dirs);

    LocalFilesSettings m_settings;
    QFutureWatcher<QPair<TrackList, QStringList>> m_watcher;
    QFileSystemWatcher m_fsWatcher;
    QTimer m_debounce;
    bool m_rescanPending = false;
    bool m_sorted = false;
    TrackSort::Key m_sortKey = TrackSort::Key::Title;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    int m_sortGeneration = 0;
    int m_scanGeneration = 0;   // of the scan running
};
