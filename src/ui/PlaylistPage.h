#pragma once

#include "core/TrackQuery.h"

#include <QTableView>
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

    // Expands folders/playlists in the background, then inserts at sourceRow.
    void addPaths(const QStringList &paths, int sourceRow = -1, bool playFirst = false);

    QList<int> selectedSourceRows() const;
    int focusedSourceRow() const;   // current or first selected row, -1 if none
    void removeSelected();
    void setQuery(const QString &text, TrackQuery::Field field);
    void scrollToSourceRow(int row);

signals:
    void trackActivated(int sourceRow);
    void tracksAdded(int firstRow, bool playFirst);
    void statsChanged();
    void contextMenuRequested(const QPoint &globalPos);

private:
    PlaylistModel *m_model;
    PlaylistFilterProxy *m_proxy;
    PlaylistView *m_view;
};
