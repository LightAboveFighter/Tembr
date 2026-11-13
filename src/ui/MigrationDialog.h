#pragma once

#include "core/PlaylistIO.h"
#include "migration/AimpProfiles.h"
#include "migration/PhoneSources.h"

#include <QDialog>

#include <atomic>
#include <memory>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QTreeWidget;

struct MigrationResult
{
    QList<PlaylistData> playlists;
    int activeIndex = -1;                       // tab to select
    int playingIndex = -1;                      // playlist AIMP was playing…
    int playingRow = -1;                        // …and its track
    bool replaceSameName = false;               // replace tabs with the same name
    QList<QPair<QString, QString>> moves;       // files found elsewhere: learn the folder moves
    QString summary;
};

// "Import from AIMP": AIMP for Windows on this computer, or playlists from
// an Android phone connected over USB. Lists the playlists, then imports
// the checked ones in the background (finding moved music, copying tracks
// from the phone).
class MigrationDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MigrationDialog(QWidget *parent = nullptr);
    ~MigrationDialog() override;

    MigrationResult result() const { return m_result; }

public slots:
    void reject() override;   // Esc / window close: cancels a running import first

private:
    struct Item
    {
        QString name;
        int tracks = -1;
        QString where;
        bool checked = true;
        int aimpIndex = -1;                     // into m_session.playlists
        Migration::PhonePlaylistFile phoneFile;
    };

    bool phoneMode() const;
    void refreshSources();
    void findPlaylists();
    void showItems(const QList<Item> &items);
    void startImport();
    void setBusy(bool busy, const QString &text = {});
    void updateButtons();

    QRadioButton *m_fromAimp;
    QRadioButton *m_fromPhone;
    QComboBox *m_aimpProfiles;
    QComboBox *m_phones;
    QPushButton *m_refresh;
    QPushButton *m_pickFolder;
    QLabel *m_notes;
    QCheckBox *m_includeOld;
    QLineEdit *m_library;
    QCheckBox *m_copy;
    QLineEdit *m_copyTo;
    QTreeWidget *m_list;
    QCheckBox *m_replace;
    QProgressBar *m_progress;
    QLabel *m_status;
    QPushButton *m_find;
    QPushButton *m_import;
    QPushButton *m_close;

    QList<Migration::AimpProfile> m_profiles;
    Migration::AimpSession m_session;
    QList<Migration::PhoneSourcePtr> m_phoneSources;
    QList<Item> m_items;
    MigrationResult m_result;
    bool m_busy = false;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};
