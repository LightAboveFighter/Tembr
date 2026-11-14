#pragma once

#include <QDialog>

class QCheckBox;
class QPushButton;
class QTreeWidget;

// "Delete from disk" confirmation: what is deleted, the files as a Name /
// Folder table behind "Details", and whether to also drop the tracks from
// every playlist that has them. Cancel is the default button.
class DeleteFilesDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Kind { ToTrash, Forever };   // Forever: the trash could not take them

    DeleteFilesDialog(Kind kind, const QStringList &paths, int skippedCueTracks, QWidget *parent = nullptr);

    // Also remove the tracks from all playlists (else other playlists only
    // mark them missing). Remembered for next time when accepted.
    bool fromAllPlaylists() const;

    QTreeWidget *table() const { return m_table; }   // tests
    QPushButton *detailsButton() const { return m_details; }

private:
    void updateSizeLimits();
    int tableHeight(int rows) const;   // header, rows and frame

    QTreeWidget *m_table;
    QPushButton *m_details;
    QCheckBox *m_allPlaylists = nullptr;
    QSize m_tableFull;   // the list with every row and full texts
    int m_rowHeight = 0;
};
