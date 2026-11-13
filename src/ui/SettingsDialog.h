#pragma once

#include "platform/GlobalHotkeys.h"
#include "ui/Theme.h"

#include <QDialog>
#include <QHash>

class QCheckBox;
class QComboBox;
class QKeySequenceEdit;

struct AppSettings
{
    Theme::Kind theme = Theme::Kind::Dark;
    bool trayIcon = true;
    bool closeToTray = false;
    GlobalHotkeys::Bindings hotkeys;

    static AppSettings load();
    void save() const;
};

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    SettingsDialog(const AppSettings &current, QWidget *parent = nullptr);
    AppSettings values() const;

private:
    QComboBox *m_theme;
    QCheckBox *m_tray;
    QCheckBox *m_closeToTray;
    QHash<GlobalHotkeys::Action, QKeySequenceEdit *> m_keys;
};

// Creates ~/.local/share/applications/tembr.desktop (+ icon) for the running
// binary: app menu entry, "Open with", and the icon/name GNOME shows in its
// media controls. Returns an error message or an empty string.
QString installDesktopEntry();
