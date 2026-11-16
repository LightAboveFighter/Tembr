#pragma once

#include <QList>
#include <QPointer>
#include <QToolButton>
#include <QWidget>

class QAction;
class QLabel;
class QMenu;
class QSplitter;
class QStackedWidget;
class PlaylistPage;

// Header of the right pane: the name of the playlist shown; a click opens the
// list of all playlists to pick one from. The mouse wheel does nothing here:
// with 24 playlists a scroll over the header (meant for the list below) would
// silently swap the playlist under the user, as QTabBar and QComboBox do.
class PlaylistPicker : public QToolButton
{
    Q_OBJECT
public:
    explicit PlaylistPicker(QWidget *parent = nullptr);

protected:
    void wheelEvent(QWheelEvent *e) override;
};

// The two lists side by side: "Local files" on the left, always; on the right
// one of the user's playlists, picked from the header (or a placeholder when
// there are none). Pages are created by the window and kept here, so switching
// the playlist shown only raises another page of a stack.
//
// One of the two panes is "active": the one that had the keyboard focus last.
// Menu commands, the status bar and Play when stopped refer to it.
class PlaylistPanes : public QWidget
{
    Q_OBJECT
public:
    enum class Pane { Local, Playlist };

    explicit PlaylistPanes(QWidget *parent = nullptr);

    void setLocalPage(PlaylistPage *page);
    PlaylistPage *localPage() const { return m_local; }

    // The user's playlists in their order (as saved, as listed in the picker).
    const QList<PlaylistPage *> &playlists() const { return m_playlists; }
    int indexOf(PlaylistPage *page) const { return m_playlists.indexOf(page); }
    // Appended; shown only if no other playlist is.
    void addPlaylist(PlaylistPage *page);
    // Taken out (the caller deletes the page); a neighbour is shown instead.
    void removePlaylist(PlaylistPage *page);
    void movePlaylist(int from, int to);
    PlaylistPage *shownPlaylist() const { return m_shown; }   // nullptr: none
    void showPlaylist(PlaylistPage *page);
    void refreshNames();   // a playlist was renamed

    Pane activePane() const;
    // The page of the active pane: Local files, or the playlist shown.
    PlaylistPage *activePage() const;
    // The other list on screen, if there is one.
    PlaylistPage *otherPage() const;
    // Makes a pane active (the playlist pane with no playlist: Local files);
    // with `focus`, its list also takes the keyboard focus.
    void activate(Pane pane, bool focus = true);
    void activatePage(PlaylistPage *page, bool focus = true);

    // New / open playlists from the placeholder; rename and close act on the
    // playlist shown. The window's own actions, so the shortcuts show.
    void setPlaylistActions(QAction *create, QAction *open, QAction *rename, QAction *close);

    QSplitter *splitter() const { return m_splitter; }
    PlaylistPicker *picker() const { return m_picker; }
    QMenu *pickerMenu() const { return m_menu; }

    // The theme changed: the active pane's mark is drawn in its colours.
    void refreshActiveMark();

signals:
    void shownChanged(PlaylistPage *page);   // nullptr: no playlist (placeholder)
    void activeChanged(PlaylistPage *page);
    void orderChanged();
    // Files dropped on the free right pane (no playlists yet).
    void filesDropped(const QStringList &paths);
    // Tracks of a list (by uid) dropped there.
    void tracksDropped(PlaylistPage *source, const QList<quint64> &uids);

private:
    void fillMenu();
    void showPickerContextMenu(const QPoint &pos);
    void setActivePane(Pane pane);
    void focusChanged(QWidget *now);

    QSplitter *m_splitter;
    QWidget *m_localPane;
    QWidget *m_localHeader;
    QLabel *m_localTitle;
    QWidget *m_rightPane;
    QWidget *m_rightHeader;
    PlaylistPicker *m_picker;
    QMenu *m_menu;
    QStackedWidget *m_stack;
    QWidget *m_placeholder;

    PlaylistPage *m_local = nullptr;
    QList<PlaylistPage *> m_playlists;
    PlaylistPage *m_shown = nullptr;
    Pane m_active = Pane::Local;

    QPointer<QAction> m_create;
    QPointer<QAction> m_open;
    QPointer<QAction> m_rename;
    QPointer<QAction> m_close;
};
