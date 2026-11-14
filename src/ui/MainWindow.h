#pragma once

#include "audio/AudioEngine.h"
#include "core/Track.h"
#include "ui/SettingsDialog.h"

#include <QFuture>
#include <QImage>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QSlider;
class QSplitter;
class QSystemTrayIcon;
class QTabWidget;
class QToolButton;
class LocalFiles;
class MprisService;
class NowPlayingPanel;
class TrackOsd;
class PlaylistPage;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Files given on the command line: added to the current playlist and played.
    void openPaths(const QStringList &paths);
    // "play-pause", "play", "pause", "stop", "next", "previous",
    // "volume-up", "volume-down", "show" — from MPRIS, tray, `tembr --next`.
    void handleCommand(const QString &command);

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

    PlaylistPage *currentPage() const;
    PlaylistPage *editablePage();   // current page unless read-only, else another (created if needed)
    bool refuseReadOnly(PlaylistPage *page);
    void createLocalPage();
    void applyLocalScan(const TrackList &tracks);
    void showLocalFilesSettings();
    PlaylistPage *addPage(const QString &name);
    void closePage(int index);
    void renamePage(int index);

    void playRow(PlaylistPage *page, int row);
    void playOrResume();
    void playNext();
    void playPrevious();
    void changeVolume(int delta);
    void showTrackInfo();
    void updateNowPlaying();
    void updateNavigation();
    void updateStats();
    void applySearch();
    void showContextMenu(const QPoint &globalPos);
    void showHeaderMenu(const QPoint &globalPos);

    void addFiles();
    void addFolder();
    void openPlaylists();
    void savePlaylist();
    void findMovedFiles();
    void importFromAimp();
    void rememberMoves(const QList<QPair<QString, QString>> &moves);
    void offerToFindMoved(int missing, int total);

    // Playlists, tab layout and player state; with the Local files cache
    // (large) only when the player closes. Writes every file before
    // removing ones no longer used, so a failure never loses playlists.
    enum class SaveScope { Playlists, Everything };
    void saveSession(SaveScope scope = SaveScope::Everything);
    void scheduleSave();   // soon after a change, so a crash or kill loses little
    void restoreSession();

    AudioEngine *m_engine;
    MprisService *m_mpris;
    LocalFiles *m_localFiles;
    PlaylistPage *m_localPage = nullptr;   // "Local files": read-only, follows the music folders
    QPointer<PlaylistPage> m_playingPage;
    AppSettings m_settings;
    bool m_quitting = false;

    QSplitter *m_splitter = nullptr;
    NowPlayingPanel *m_nowPlaying = nullptr;
    QTabWidget *m_tabs = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_searchField = nullptr;
    QSlider *m_seek = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_position = nullptr;
    QLabel *m_duration = nullptr;
    QLabel *m_volumeIcon = nullptr;
    QLabel *m_stats = nullptr;
    QToolButton *m_playButton = nullptr;

    QAction *m_actPlay = nullptr;
    QAction *m_actStop = nullptr;
    QAction *m_actPrev = nullptr;
    QAction *m_actNext = nullptr;
    QAction *m_actShow = nullptr;
    QAction *m_actQuit = nullptr;

    QSystemTrayIcon *m_tray = nullptr;
    TrackOsd *m_osd = nullptr;   // a window of its own (no parent)
    QMenu *m_trayMenu = nullptr;
    QAction *m_trayTitle = nullptr;

    QByteArray m_headerState;   // column layout shared by all tabs
    QString m_coverPath;        // track whose cover is shown
    QImage m_coverImage;
    QString m_artFile;          // cover exported for MPRIS clients
    int m_failStreak = 0;       // consecutive unplayable tracks skipped
    QFuture<QString> m_hotkeyJob;   // background registration at startup
    bool m_hotkeysReleased = false;
    QTimer *m_autosave = nullptr;
    bool m_finalSaveDone = false;   // saved on close: not again on quit
};
