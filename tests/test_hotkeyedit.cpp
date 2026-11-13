#include "ui/HotkeyEdit.h"

#include <QTest>

class TestHotkeyEdit : public QObject
{
    Q_OBJECT

private slots:
    void numpadDigitKeepsKeypadFlag()
    {
        HotkeyEdit edit;
        QTest::keyClick(&edit, Qt::Key_4, Qt::KeypadModifier);
        QCOMPARE(edit.keySequence(), QKeySequence(QKeyCombination(Qt::KeypadModifier, Qt::Key_4)));
        QCOMPARE(edit.text(), QStringLiteral("Numpad 4"));

        QTest::keyClick(&edit, Qt::Key_4);
        QCOMPARE(edit.keySequence(), QKeySequence(Qt::Key_4));
        QCOMPARE(edit.text(), QStringLiteral("4"));
    }

    void modifiersAndClearing()
    {
        HotkeyEdit edit;
        QTest::keyClick(&edit, Qt::Key_Right, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(edit.keySequence(), QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::AltModifier, Qt::Key_Right)));
        QTest::keyClick(&edit, Qt::Key_Escape);                       // leave, keep the value
        QVERIFY(!edit.keySequence().isEmpty());
        QTest::keyClick(&edit, Qt::Key_Backspace);                    // clear
        QVERIFY(edit.keySequence().isEmpty());
        QTest::keyClick(&edit, Qt::Key_Delete, Qt::KeypadModifier);   // numpad Del is a key, not "clear"
        QCOMPARE(edit.keySequence(), QKeySequence(QKeyCombination(Qt::KeypadModifier, Qt::Key_Delete)));
    }

    void modifierAloneDoesNotBind()
    {
        HotkeyEdit edit(QKeySequence(Qt::Key_F5));
        QTest::keyPress(&edit, Qt::Key_Control, Qt::ControlModifier);
        QCOMPARE(edit.keySequence(), QKeySequence(Qt::Key_F5));
        QTest::keyRelease(&edit, Qt::Key_Control);
        QCOMPARE(edit.text(), QStringLiteral("F5"));
    }
};

QTEST_MAIN(TestHotkeyEdit)
#include "test_hotkeyedit.moc"
