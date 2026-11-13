#pragma once

#include <QColor>
#include <QString>

namespace Theme {

enum class Kind { Dark, System };

// Applies the style, palette and style sheet to the whole application.
// Can be called again at runtime to switch.
void apply(Kind kind);
Kind current();

QString toString(Kind kind);
Kind fromString(const QString &s);

QColor accent();      // orange
QColor iconColor();   // regular toolbar glyphs
QColor dimText();     // secondary labels

} // namespace Theme
