#include "platform/GlobalHotkeys.h"

#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
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

        // Clearing a binding removes it from GNOME's list again.
        b.remove(Action::Next);
        QCOMPARE(GlobalHotkeys::applyToDesktop(b), QString());
        kf = keyfile();
        QVERIFY2(!kf.contains(QStringLiteral("custom-keybindings/tembr-next/'")), qPrintable(kf));
        QVERIFY(kf.contains(QStringLiteral("custom-keybindings/tembr-play-pause/'")));
    }

    void commandsUseCliFlags()
    {
        QVERIFY(GlobalHotkeys::command(GlobalHotkeys::Action::Next).endsWith(QStringLiteral(" --next")));
        QVERIFY(GlobalHotkeys::command(GlobalHotkeys::Action::PlayPause).endsWith(QStringLiteral(" --play-pause")));
    }
};

QTEST_GUILESS_MAIN(TestHotkeys)
#include "test_hotkeys.moc"
