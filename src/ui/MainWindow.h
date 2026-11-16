#pragma once

#include "audio/AudioEngine.h"
#include "core/PlaylistIO.h"
#include "core/Track.h"
#include "ui/SettingsDialog.h"

#include <QFuture>
#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

#include <functional>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QSplitter;
class QSystemTrayIcon;
class LocalFiles;
class MprisService;
class NowPlayingPanel;
class PlaybackController;
class PlaylistModel;
class PlaylistPanes;
class TrackOsd;
class TransportBar;
class PlaylistPage;

// What is read from disk at startup, on a worker thread.
struct SessionSnapshot
{
    TrackList localFiles;            // the Local files cache
    QList<PlaylistData> playlists;   // the user's playlists, in their order
    QStringList keptAside;           // copies of playlist files that could not be read in full
};

struct MainWindowOptions
{
    // MPRIS on the session bus, global hotkeys (gsettings) and the tray icon.
    // Off in tests: they would take the bus name of a running player and
    // overwrite the desktop's shortcuts.
    bool desktopIntegration = true;
    // Reads the session; runs on a worker thread. Empty: the files on disk.
    std::function<SessionSnapshot()> loadSession;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const MainWindowOptions &options = {}, QWidget *parent = nullptr);
    ~MainWindow() override;

    // The playlists of the last session are read in the background; until
    // they are in, the playlists cannot be used and nothing is saved.
    bool isSessionLoaded() const { return m_sessionLoaded; }

    // Files given on the command line: added to the current playlist and played.
    // Before the session is loaded: once it is, into the playlist restored.
    void openPaths(const QStringList &paths);
    // "play-pause", "play", "pause", "stop", "next", "previous",
    // "volume-up", "volume-down", "show" — from MPRIS, tray, `tembr --next`.
    void handleCommand(const QString &command);

    // Local files and the user's playlists side by side (also for tests).
    PlaylistPanes *panes() const { return m_panes; }

signals:
    void sessionLoaded();

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void buildActions();
    void buildUi();
    void refreshIcons();
    void applySettings(const AppSettings &s);
    void setupTray(bool enabled);
    void showSettings();
    void showAndRaise();
    void toggleVisible();
    void quit();
    void registerHotkeys();
    void releaseHotkeys();

    // The list of the active pane (the one worked in last): Local files or
    // the playlist shown.
    PlaylistPage *activePage() const;
    // Where added files go: the active list unless it is Local files, else
    // the playlist shown; with no playlists at all, a new one.
    PlaylistPage *editablePage();
    bool refuseReadOnly(PlaylistPage *page);
    PlaylistPage *createPage(const QString &name);   // with its connections, not placed yet
    void createLocalPage();
    void applyLocalScan(const TrackList &tracks, int sortGeneration);
    void syncLocalSort();   // m_localSortColumn/Order to LocalFiles
    void showLocalFilesSettings();
    // column < 0: by folder, as found on disk (AIMP's grouping by %FileDir).
    void setLocalSort(int column, Qt::SortOrder order);
    PlaylistPage *addPage(const QString &name);   // a user playlist, at the end of the list
    void bringUp(PlaylistPage *page);   // shown in the right pane, which becomes active
    QString newPlaylistName() const;
    void closePage(PlaylistPage *page);
    void renamePage(PlaylistPage *page);

    // Play/pause; stopped: the row in focus, else the current track, else the first one.
    void playOrResume();
    PlaylistPage *pageOf(PlaylistModel *model) const;
    void showTrackInfo(PlaylistPage *page = nullptr);   // nullptr: the active list
    void updateNowPlaying();
    void loadCover(const Track &t);
    void setCover(int job, const QImage &image, const QString &artFile);
    void updateNavigation();
    void updateStats();
    void applySearch();
    void setPageQuery(PlaylistPage *page, const QString &text, int field);
    void showContextMenu(PlaylistPage *page, const QPoint &globalPos);
    // Copies the selected tracks of `from` to the end of `to` (nullptr: a new
    // playlist, named by the user).
    void sendSelected(PlaylistPage *from, PlaylistPage *to);
    void showHeaderMenu(PlaylistPage *page, const QPoint &globalPos);

    void addFiles();
    void addFolder();
    void openPlaylists();
    void savePlaylist();
    void findMovedFiles();
    // Deletes the files of the selected tracks from the device (to the trash,
    // after asking), then drops them from this playlist.
    void deleteSelectedFiles(PlaylistPage *page = nullptr);   // nullptr: the active list
    void forgetDeletedFiles(PlaylistPage *from, const QStringList &paths, bool fromAllPlaylists);
    void importFromAimp();
    void rememberMoves(const QList<QPair<QString, QString>> &moves);
    void offerToFindMoved(int missing, int total);

    // Playlists, which one is shown and player state; with the Local files cache
    // (large) only when the player closes. Writes every file before
    // removing ones no longer used, so a failure never loses playlists.
    enum class SaveScope { Playlists, Everything };
    void saveSession(SaveScope scope = SaveScope::Everything);
    void scheduleSave();   // soon after a change, so a crash or kill loses little
    void restoreSession();   // settings at once, playlists in the background
    void applySession(const SessionSnapshot &session);
    void rescanLocal();      // Local files from the folders (after the session is loaded)

    MainWindowOptions m_options;
    bool m_sessionLoaded = false;
    QList<QStringList> m_pendingOpen;     // openPaths() before the session was loaded
    QStringList m_pendingCommands;        // play, next, ... before it was loaded
    QList<QPointer<QAction>> m_needSession;   // menu items that work on playlists

    AudioEngine *m_engine;
    PlaybackController *m_playback;   // what plays; the window shows it
    MprisService *m_mpris;
    LocalFiles *m_localFiles;
    PlaylistPage *m_localPage = nullptr;   // "Local files": read-only, follows the music folders
    AppSettings m_settings;
    bool m_quitting = false;

    QSplitter *m_splitter = nullptr;
    NowPlayingPanel *m_nowPlaying = nullptr;
    PlaylistPanes *m_panes = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_searchField = nullptr;
    TransportBar *m_transport = nullptr;
    QLabel *m_stats = nullptr;

    QAction *m_actPlay = nullptr;
    QAction *m_actStop = nullptr;
    QAction *m_actPrev = nullptr;
    QAction *m_actNext = nullptr;
    QAction *m_actShow = nullptr;
    QAction *m_actQuit = nullptr;
    QAction *m_actNewPlaylist = nullptr;
    QAction *m_actOpenPlaylists = nullptr;
    QAction *m_actRename = nullptr;
    QAction *m_actClose = nullptr;

    QSystemTrayIcon *m_tray = nullptr;
    TrackOsd *m_osd = nullptr;   // a window of its own (no parent)
    QMenu *m_trayMenu = nullptr;
    QAction *m_trayTitle = nullptr;

    QByteArray m_headerState;   // column layout shared by all lists
    // The query each list is filtered by ("" none): filtering 100k tracks
    // takes a while, so a list is filtered again only when its query changes.
    QHash<const PlaylistPage *, QString> m_pageQueries;
    int m_headerColumns = 0;    // columns m_headerState knows (newer ones start hidden)
    // Order of Local files, kept across rescans and restarts.
    int m_localSortColumn = -1;
    Qt::SortOrder m_localSortOrder = Qt::AscendingOrder;
    bool m_localResort = false;   // sorted by tags before all were read: sort again once they are
    QString m_coverPath;        // track whose cover is shown
    QImage m_coverImage;
    QString m_artFile;          // cover exported for MPRIS clients
    int m_coverJob = 0;         // the cover read last; older results are dropped
    QFuture<QStringList> m_saveJob;   // autosave being written
    QFuture<QString> m_hotkeyJob;   // background registration at startup
    bool m_hotkeysReleased = false;
    QTimer *m_autosave = nullptr;
    bool m_finalSaveDone = false;   // saved on close: not again on quit
};
