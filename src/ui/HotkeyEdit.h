#pragma once

#include <QKeySequence>
#include <QLineEdit>

// Records one key combination. Unlike QKeySequenceEdit it keeps
// Qt::KeypadModifier, so numpad 4 and the 4 above the letters are different
// keys (KP_4 vs 4 for the desktop).
//   Backspace/Delete without modifiers clears, Escape leaves the field.
class HotkeyEdit : public QLineEdit
{
    Q_OBJECT
public:
    explicit HotkeyEdit(const QKeySequence &seq = {}, QWidget *parent = nullptr);

    QKeySequence keySequence() const { return m_seq; }
    void setKeySequence(const QKeySequence &seq);
    void clearSequence() { setKeySequence({}); }

signals:
    void keySequenceChanged(const QKeySequence &seq);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;

private:
    QKeySequence m_seq;
};
