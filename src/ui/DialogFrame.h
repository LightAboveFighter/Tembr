#pragma once

#include <QDialog>

// A dialog that resizes like an ordinary window: the edge being dragged
// moves, the opposite one stays, and growing (e.g. "Details") goes right and
// down. GNOME attaches modal dialogs to their window and keeps them centred
// on it, so there every resize moved both edges. As a utility window it stays
// above its window, modal and out of the taskbar, but is not attached.
// For dialogs the user resizes; small fixed ones (questions) keep GNOME's way.
inline void resizesLikeAWindow(QDialog *dialog)
{
    dialog->setWindowFlags(Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint);
}
