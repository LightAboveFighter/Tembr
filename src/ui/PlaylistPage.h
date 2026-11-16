#pragma once

#include "core/Track.h"
#include "core/TrackQuery.h"

#include <QHash>
#include <QMimeData>
#include <QPointer>
#include <QTableView>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <functional>

class QDrag;
class PlaylistModel;
class PlaylistFilterProxy;
class PlaylistPage;

// Tracks dragged with the mouse, inside Tembr only: the list they come from
// and which tracks, by uid (a rescan of Local files may land during the drag
// and move rows). Nothing is copied or serialised until they are dropped.
class TrackDragData : public QMimeData
{
    Q_OBJECT
public:
    TrackDragData(PlaylistPage *source, const QList<quint64> &uids);
    static QString mimeType();

    PlaylistPage *source() const { return m_source; }   // nullptr: closed meanwhile
    const QList<quint64> &uids() const { return m_uids; }   // in list order

private:
    QPointer<PlaylistPage> m_source;
    QList<quint64> m_uids;
};

// The table of a playlist. Takes files/folders dropped from a file manager
// and tracks dragged from a list; shows where a drop would land with a line
// between rows. Pressing on a selected row and moving drags the selection;
// pressing elsewhere and moving selects rows, as before.
class PlaylistView : public QTableView
{
    Q_OBJECT
public:
    explicit PlaylistView(QWidget *parent = nullptr);

    // Runs a drag of tracks: QDrag::exec by default. Tests put their own in
    // its place, since a drag cannot be run without a window system.
    using DragRunner = std::function<void(QDrag *drag)>;
    static void setDragRunner(DragRunner runner);   // empty: back to QDrag::exec
    static void runDrag(QDrag *drag);

    // The row (of the filtered view, 0..rowCount()) a drop at `pos` would
    // go before: the lower half of a row means after it.
    int insertionRow(const QPoint &pos) const;
    // The same place in the whole playlist: before the track shown at that
    // row, after the last one shown, or at the end of an empty view. Tracks
    // hidden by a search keep their places around it.
    int sourceInsertionRow(int proxyRow) const;

signals:
    void pathsDropped(const QStringList &paths, int sourceRow);
    void deletePressed();
    // A press on the selection moved past the drag distance.
    void dragRequested();
    // Tracks dropped here; `onSelection`: onto a selected row.
    void tracksDropped(const TrackDragData *data, int proxyRow, bool onSelection, const QPoint &globalPos);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    bool isOwnDrag(const QMimeData *mime) const;
    bool isOnSelection(const QPoint &pos) const;
    void showDropAt(const QPoint &pos, bool own);
    void hideDrop();

    bool m_dragCandidate = false;   // pressed on the selection, not moved far yet
    QPoint m_pressPos;
    bool m_dropShown = false;       // something is dragged over: the line is drawn
    bool m_dropOwn = false;         // ... and it is this list's own selection
    QPoint m_dropPos;               // where (viewport); the row is found when painting
};

// One playlist tab: model + search filter + view.
class PlaylistPage : public QWidget
{
    Q_OBJECT
public:
    explicit PlaylistPage(const QString &name, QWidget *parent = nullptr);

    PlaylistModel *model() const { return m_model; }
    PlaylistFilterProxy *proxy() const { return m_proxy; }
    PlaylistView *view() const { return m_view; }

    bool isReadOnly() const;
    void setReadOnly(bool readOnly);   // no drops, no deleting

    // Expands folders/playlists in the background, then inserts at sourceRow.
    void addPaths(const QStringList &paths, int sourceRow = -1, bool playFirst = false);
    // Copies of tracks of another list (by uid; see PlaylistModel::copiesOf)
    // inserted before `sourceRow` (-1: at the end), selected and shown.
    // Returns how many.
    int receiveTracks(PlaylistPage *from, const QList<quint64> &uids, int sourceRow);

    // Replaces the whole list (a model reset, as for a Local files rescan)
    // keeping what the user sees and has selected: the top visible track,
    // the selected tracks and the current one, found again by uid.
    void replaceTracks(const TrackList &tracks);

    QList<int> selectedSourceRows() const;
    int focusedSourceRow() const;   // current or first selected row, -1 if none
    void removeSelected();
    void setQuery(const QString &text, TrackQuery::Field field);
    void scrollToSourceRow(int row);

    // Column widths from the content (see core/ColumnFit). A column the user
    // drags keeps that width, in every tab, until resetColumnWidths().
    void fitColumns();
    static void resetColumnWidths();

signals:
    void trackActivated(int sourceRow);
    void tracksAdded(int firstRow, bool playFirst);
    void statsChanged();
    void contextMenuRequested(const QPoint &globalPos);
    void dropReceived();               // something was dropped here (this list is being worked in)
    void tracksReceived(int count);    // copies of another list's tracks were inserted

protected:
    bool eventFilter(QObject *watched, QEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void startTrackDrag();
    void dropTracks(const TrackDragData *data, int proxyRow, bool onSelection, const QPoint &globalPos);
    void moveTracks(const QList<quint64> &uids, int sourceRow);
    void selectSourceRows(const QList<int> &sortedRows);
    void scheduleFit(bool contentChanged);
    void measure(int column = -1);
    bool measureRows(int firstColumn, int lastColumn, int top, int bottom, int maxRows);
    bool growNeeds(int top, int bottom);   // after rows changed: wider only

    PlaylistModel *m_model;
    PlaylistFilterProxy *m_proxy;
    PlaylistView *m_view;
    QTimer m_fitTimer;
    QTimer m_statsTimer;           // statsChanged once per burst of changes
    QVector<int> m_needs;          // per column, from the content
    bool m_needsDirty = true;
    bool m_applying = false;       // our own resizing, not the user's
    bool m_dragging = false;       // the mouse is down on the header
    bool m_layoutWanted = false;   // a column shown or hidden, a tab shown, ...: lay out even after a drag
    bool m_keepLayout = false;     // a column was dragged: content changes alone leave the others
};
