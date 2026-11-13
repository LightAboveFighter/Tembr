#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QString>

// System-wide hotkeys that work while the window is minimised or hidden.
//
// Wayland does not let applications grab keys, so the shortcut is registered
// with the desktop instead: on GNOME (X11 or Wayland) as a custom keybinding
// (Settings → Keyboard → Custom Shortcuts) that runs "tembr --next" etc.;
// the command forwards to the running player over D-Bus. On other desktops
// the same commands can be bound by hand.
namespace GlobalHotkeys {

enum class Action { PlayPause, Stop, Next, Previous, VolumeUp, VolumeDown, Show };

struct ActionInfo
{
    Action action;
    QString id;        // "play-pause": settings key, CLI flag (--play-pause), D-Bus command
    QString title;     // for the UI
};

QList<ActionInfo> actions();
QString idOf(Action a);

using Bindings = QHash<Action, QKeySequence>;
Bindings load();                  // from QSettings
void save(const Bindings &b);

// The desktop integration that can register keys for us.
bool desktopSupported();          // GNOME with gsettings
QString desktopName();

QString command(Action a);        // "/path/to/tembr --next"
QString gtkAccelerator(const QKeySequence &seq);   // "<Control><Alt>Right"; empty if unsupported

// Registers (or removes, for empty sequences) the bindings with GNOME.
// Returns an error message, or an empty string on success.
QString applyToDesktop(const Bindings &b);

} // namespace GlobalHotkeys
