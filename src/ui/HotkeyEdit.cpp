#include "ui/HotkeyEdit.h"

#include "platform/GlobalHotkeys.h"

#include <QKeyEvent>

namespace {

constexpr Qt::KeyboardModifiers kChordModifiers =
    Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier;

bool isModifierKey(int key)
{
    switch (key) {
    case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Alt: case Qt::Key_Meta:
    case Qt::Key_AltGr: case Qt::Key_Super_L: case Qt::Key_Super_R: case Qt::Key_Hyper_L:
    case Qt::Key_Hyper_R: case Qt::Key_CapsLock: case Qt::Key_NumLock: case 0: case Qt::Key_unknown:
        return true;
    default:
        return false;
    }
}

QString modifiersText(Qt::KeyboardModifiers mods)
{
    QString s;
    if (mods & Qt::ControlModifier) s += QStringLiteral("Ctrl+");
    if (mods & Qt::AltModifier) s += QStringLiteral("Alt+");
    if (mods & Qt::ShiftModifier) s += QStringLiteral("Shift+");
    if (mods & Qt::MetaModifier) s += QStringLiteral("Super+");
    return s;
}

} // namespace

HotkeyEdit::HotkeyEdit(const QKeySequence &seq, QWidget *parent)
    : QLineEdit(parent)
{
    setReadOnly(true);
    setPlaceholderText(QStringLiteral("Нажмите сочетание…"));
    setContextMenuPolicy(Qt::NoContextMenu);
    setKeySequence(seq);
}

void HotkeyEdit::setKeySequence(const QKeySequence &seq)
{
    m_seq = seq.isEmpty() ? QKeySequence() : QKeySequence(seq[0]);
    setText(GlobalHotkeys::displayText(m_seq));
    emit keySequenceChanged(m_seq);
}

bool HotkeyEdit::event(QEvent *e)
{
    // Grab everything while focused: Tab, and keys that are window shortcuts
    // (Space, X, Ctrl+, ...) would otherwise never reach keyPressEvent.
    if (e->type() == QEvent::ShortcutOverride) {
        e->accept();
        return true;
    }
    if (e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            keyPressEvent(ke);
            return true;
        }
    }
    return QLineEdit::event(e);
}

void HotkeyEdit::keyPressEvent(QKeyEvent *e)
{
    int key = e->key();
    Qt::KeyboardModifiers mods = e->modifiers() & (kChordModifiers | Qt::KeypadModifier);

    if (isModifierKey(key)) {
        setText(modifiersText(mods) + QStringLiteral("…"));
        return;
    }
    const bool plain = !(mods & kChordModifiers);
    if (plain && !(mods & Qt::KeypadModifier)) {
        if (key == Qt::Key_Escape) {
            setText(GlobalHotkeys::displayText(m_seq));
            clearFocus();
            return;
        }
        if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
            clearSequence();
            return;
        }
    }
    if (key == Qt::Key_Backtab) {
        key = Qt::Key_Tab;
        mods |= Qt::ShiftModifier;
    }
    setKeySequence(QKeySequence(QKeyCombination(mods, Qt::Key(key))));
}

void HotkeyEdit::keyReleaseEvent(QKeyEvent *e)
{
    // Modifiers released without a key: show the stored combination again.
    if (isModifierKey(e->key()))
        setText(GlobalHotkeys::displayText(m_seq));
}

void HotkeyEdit::focusOutEvent(QFocusEvent *e)
{
    setText(GlobalHotkeys::displayText(m_seq));
    QLineEdit::focusOutEvent(e);
}
