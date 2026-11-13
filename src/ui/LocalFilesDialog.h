#pragma once

#include "core/LocalFiles.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QListWidget;

// Settings of "Local files", the same as AIMP's music library has:
// folders (with subfolders), excluded folders, following changes.
class LocalFilesDialog : public QDialog
{
    Q_OBJECT
public:
    LocalFilesDialog(const LocalFilesSettings &current, QWidget *parent = nullptr);
    LocalFilesSettings values() const;

private:
    void fill(const LocalFilesSettings &s);

    QListWidget *m_folders;
    QListWidget *m_excluded;
    QCheckBox *m_watch;
    QLabel *m_aimpNote;
};
