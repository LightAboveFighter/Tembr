#pragma once

#include "core/TrackQuery.h"

#include <QHash>
#include <QTableView>
#include <QTimer>
#include <QVector>
#include <QWidget>

class PlaylistModel;
class PlaylistFilterProxy;

// Table that accepts files/folders dropped from a file manager.
class PlaylistView : public QTableView
{
    Q_OBJECT
public:
    explicit PlaylistView(QWidget *parent = nullptr);

signals:
    void pathsDropped(const QStringList &paths, int sourceRow);   // -1 = append
    void deletePressed();

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
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

protected:
    bool eventFilter(QObject *watched, QEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void scheduleFit(bool contentChanged);
    void measure(int column = -1);

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
