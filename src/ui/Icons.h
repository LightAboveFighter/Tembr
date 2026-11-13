#pragma once

#include <QColor>
#include <QIcon>

// Vector icons drawn in code, so they follow the theme colours (the stock
// Qt media icons are dark and vanish on a black background).
namespace Icons {

enum class Kind { Play, Pause, Stop, Next, Previous, Volume, Note, Settings, Search };

QIcon make(Kind kind, const QColor &color);
// Orange disc with a dark glyph: the main play button.
QIcon accentButton(Kind kind);
// Application icon (taskbar, tray, window).
QIcon app();
QPixmap appPixmap(int size);

} // namespace Icons
