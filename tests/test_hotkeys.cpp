#include "platform/GlobalHotkeys.h"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QProcess>
#include <QTest>

class TestHotkeys : public QObject
{
    Q_OBJECT

private slots:
    void gtkAccelerators_data()
    {
        QTest::addColumn<QString>("qt");
        QTest::addColumn<QString>("gtk");
        QTest::newRow("ctrl+alt+right") << "Ctrl+Alt+Right" << "<Control><Alt>Right";
        QTest::newRow("meta+shift+p") << "Meta+Shift+P" << "<Shift><Super>p";
        QTest::newRow("ctrl+alt+space") << "Ctrl+Alt+Space" << "<Control><Alt>space";
        QTest::newRow("f9") << "F9" << "F9";
        QTest::newRow("ctrl+pgdown") << "Ctrl+PgDown" << "<Control>Page_Down";
        QTest::newRow("alt+comma") << "Alt+," << "<Alt>comma";
        QTest::newRow("media next") << "Media Next" << "XF86AudioNext";
        QTest::newRow("digit") << "Ctrl+Alt+5" << "<Control><Alt>5";
    }

    void gtkAccelerators()
    {
        QFETCH(QString, qt);
        QFETCH(QString, gtk);
        QCOMPARE(GlobalHotkeys::gtkAccelerator(QKeySequence::fromString(qt, QKeySequence::PortableText)), gtk);
    }

    // The bug: numpad 4 was registered as plain "4" and blocked typing 4 everywhere.
    void numpadIsNotTheDigitRow()
    {
        using GlobalHotkeys::gtkAccelerator;
        QCOMPARE(gtkAccelerator(QKeySequence(QKeyCombination(Qt::KeypadModifier, Qt::Key_4))), QStringLiteral("KP_4"));
        QCOMPARE(gtkAccelerator(QKeySequence(Qt::Key_4)), QStringLiteral("4"));
        QCOMPARE(gtkAccelerator(QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::KeypadModifier, Qt::Key_6))),
                 QStringLiteral("<Control>KP_6"));
        // NumLock off: numpad 4 / 5 arrive as Left / Clear.
        QCOMPARE(gtkAccelerator(QKeySequence(QKeyCombination(Qt::KeypadModifier, Qt::Key_Left))), QStringLiteral("KP_Left"));
        QCOMPARE(gtkAccelerator(QKeySequence(QKeyCombination(Qt::KeypadModifier, Qt::Key_Clear))), QStringLiteral("KP_Begin"));
        QCOMPARE(gtkAccelerator(QKeySequence(QKeyCombination(Qt::KeypadModifier, Qt::Key_Enter))), QStringLiteral("KP_Enter"));
    }

    void numpadSurvivesSettings()
    {
        const QKeySequence kp(QKeyCombination(Qt::KeypadModifier, Qt::Key_5));
        const QKeySequence back = QKeySequence::fromString(kp.toString(QKeySequence::PortableText), QKeySequence::PortableText);
        QCOMPARE(back, kp);
        QVERIFY(back[0].keyboardModifiers().testFlag(Qt::KeypadModifier));
        QCOMPARE(GlobalHotkeys::displayText(kp), QStringLiteral("Numpad 5"));
    }

    void unsafeBindings_data()
    {
        QTest::addColumn<QKeySequence>("seq");
        QTest::addColumn<bool>("safe");
        QTest::newRow("digit") << QKeySequence(Qt::Key_4) << false;
        QTest::newRow("letter") << QKeySequence(Qt::Key_A) << false;
        QTest::newRow("space") << QKeySequence(Qt::Key_Space) << false;
        QTest::newRow("shift+letter") << QKeySequence(QKeyCombination(Qt::ShiftModifier, Qt::Key_A)) << false;
        QTest::newRow("arrow") << QKeySequence(Qt::Key_Left) << false;
        QTest::newRow("numpad 4") << QKeySequence(QKeyCombination(Qt::KeypadModifier, Qt::Key_4)) << true;
        QTest::newRow("ctrl+4") << QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_4)) << true;
        QTest::newRow("alt+space") << QKeySequence(QKeyCombination(Qt::AltModifier, Qt::Key_Space)) << true;
        QTest::newRow("super+n") << QKeySequence(QKeyCombination(Qt::MetaModifier, Qt::Key_N)) << true;
        QTest::newRow("f8") << QKeySequence(Qt::Key_F8) << true;
        QTest::newRow("media next") << QKeySequence(Qt::Key_MediaNext) << true;
    }

    void unsafeBindings()
    {
        QFETCH(QKeySequence, seq);
        QFETCH(bool, safe);
        QCOMPARE(GlobalHotkeys::unsafeReason(seq).isEmpty(), safe);
    }

    // Settings saved by the old version ("4") are dropped on load, numpad kept.
    void loadDropsUnsafeBindings()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("tembr-test"));
        QCoreApplication::setApplicationName(QStringLiteral("tembr-test"));
        {
            QSettings s;
            s.clear();
            s.setValue(QStringLiteral("hotkeys/previous"), QStringLiteral("4"));
            s.setValue(QStringLiteral("hotkeys/next"),
                       QKeySequence(QKeyCombination(Qt::KeypadModifier, Qt::Key_6)).toString(QKeySequence::PortableText));
        }
        QStringList dropped;
        const auto b = GlobalHotkeys::load(&dropped);
        QCOMPARE(dropped.size(), 1);
        QVERIFY(dropped.first().startsWith(QStringLiteral("4 ")));
        QVERIFY(!b.contains(GlobalHotkeys::Action::Previous));
        QCOMPARE(GlobalHotkeys::gtkAccelerator(b.value(GlobalHotkeys::Action::Next)), QStringLiteral("KP_6"));
        QSettings().clear();
    }

    void emptyAndUnsupported()
    {
        QVERIFY(GlobalHotkeys::gtkAccelerator(QKeySequence()).isEmpty());
        QVERIFY(GlobalHotkeys::gtkAccelerator(QKeySequence(Qt::Key_Launch0)).isEmpty());
    }

    // Registers keys through the real `gsettings`, but into a throw-away keyfile
    // backend, never into the user's desktop settings.
    void gnomeKeybindingsIsolated()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("gsettings")).isEmpty())
            QSKIP("gsettings not installed");
        QTemporaryDir cfg;
        qputenv("GSETTINGS_BACKEND", "keyfile");
        qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());
        qputenv("XDG_CURRENT_DESKTOP", "ubuntu:GNOME");
        if (!GlobalHotkeys::desktopSupported())
            QSKIP("GNOME media-keys schema not installed");

        using GlobalHotkeys::Action;
        GlobalHotkeys::Bindings b;
        b.insert(Action::Next, QKeySequence(QStringLiteral("Ctrl+Alt+Right")));
        b.insert(Action::PlayPause, QKeySequence(QStringLiteral("Ctrl+Alt+Space")));
        b.insert(Action::Stop, QKeySequence(QKeyCombination(Qt::KeypadModifier, Qt::Key_5)));
        QCOMPARE(GlobalHotkeys::applyToDesktop(b), QString());

        auto keyfile = [&] {
            QFile f(cfg.filePath(QStringLiteral("glib-2.0/settings/keyfile")));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };
        QString kf = keyfile();
        QVERIFY2(kf.contains(QStringLiteral("custom-keybindings/tembr-next/'")), qPrintable(kf));
        QVERIFY(kf.contains(QStringLiteral("binding='<Control><Alt>Right'")));
        QVERIFY(kf.contains(QStringLiteral(" --next'")));
        QVERIFY(kf.contains(QStringLiteral("binding='<Control><Alt>space'")));
        QVERIFY(kf.contains(QStringLiteral("binding='KP_5'")));

        // Clearing a binding removes it from GNOME's list again.
        b.remove(Action::Next);
        QCOMPARE(GlobalHotkeys::applyToDesktop(b), QString());
        kf = keyfile();
        QVERIFY2(!kf.contains(QStringLiteral("custom-keybindings/tembr-next/'")), qPrintable(kf));
        QVERIFY(kf.contains(QStringLiteral("custom-keybindings/tembr-play-pause/'")));

        // On exit everything of ours is released.
        QCOMPARE(GlobalHotkeys::releaseFromDesktop(), QString());
        kf = keyfile();
        // (the keyfile backend keeps empty [section] headers of reset paths)
        QVERIFY2(!kf.contains(QStringLiteral("binding=")), qPrintable(kf));
        QVERIFY2(kf.contains(QStringLiteral("custom-keybindings=[]")) || kf.contains(QStringLiteral("custom-keybindings=@as []")),
                 qPrintable(kf));
    }

    void commandsUseCliFlags()
    {
        QVERIFY(GlobalHotkeys::command(GlobalHotkeys::Action::Next).endsWith(QStringLiteral(" --next")));
        QVERIFY(GlobalHotkeys::command(GlobalHotkeys::Action::PlayPause).endsWith(QStringLiteral(" --play-pause")));
    }

    // The program path is one argument whatever it holds (GNOME runs the
    // command through shell-style parsing): checked with a real shell.
    void shellQuoting()
    {
        const QStringList paths = {QStringLiteral("/usr/bin/tembr"), QStringLiteral("/opt/my apps/tembr"),
                                   QStringLiteral("/home/u/it's $HOME `x` \"q\" \\ ; & | * ? #/tembr"),
                                   QStringLiteral("/home/юзер/Музыка/tembr")};
        for (const QString &p : paths) {
            QProcess sh;
            sh.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("printf %s ") + GlobalHotkeys::shellQuote(p)});
            QVERIFY(sh.waitForFinished(5000));
            QCOMPARE(QString::fromUtf8(sh.readAllStandardOutput()), p);
        }
        QCOMPARE(GlobalHotkeys::shellQuote(QStringLiteral("/usr/bin/tembr")), QStringLiteral("/usr/bin/tembr"));
    }

    void desktopExecQuoting()
    {
        QCOMPARE(GlobalHotkeys::desktopExecQuote(QStringLiteral("/usr/bin/tembr")), QStringLiteral("/usr/bin/tembr"));
        QCOMPARE(GlobalHotkeys::desktopExecQuote(QStringLiteral("/opt/my apps/tembr")),
                 QStringLiteral("\"/opt/my apps/tembr\""));
        QCOMPARE(GlobalHotkeys::desktopExecQuote(QStringLiteral("/a/50%/tembr")), QStringLiteral("/a/50%%/tembr"));
        // " $ ` \ escaped inside the quotes, then every backslash once more for the value.
        QCOMPARE(GlobalHotkeys::desktopExecQuote(QStringLiteral("/a/$x\"b\\c")),
                 QStringLiteral("\"/a/\\\\$x\\\\\"b\\\\\\\\c\""));
    }
};

QTEST_GUILESS_MAIN(TestHotkeys)
#include "test_hotkeys.moc"
