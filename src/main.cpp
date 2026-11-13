#include "platform/GlobalHotkeys.h"
#include "platform/Mpris.h"
#include "ui/MainWindow.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/log.h>
}

namespace {

bool otherInstanceRunning()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    return bus.isConnected() && bus.interface()->isServiceRegistered(MprisService::serviceName());
}

void callRunningInstance(const QString &method, const QVariant &arg)
{
    QDBusInterface other(MprisService::serviceName(), MprisService::objectPath(), MprisService::appInterface(),
                         QDBusConnection::sessionBus());
    other.call(method, arg);
}

void printHelp()
{
    std::printf("Usage: tembr [files or folders...]\n"
                "       tembr --COMMAND\n\nCommands for the running player (bind them to keys):\n");
    for (const auto &a : GlobalHotkeys::actions())
        std::printf("  --%-12s %s\n", qPrintable(a.id), a.title.toUtf8().constData());
}

} // namespace

int main(int argc, char *argv[])
{
    // `tembr --next` & co.: forward to the running player and exit quickly,
    // without starting a GUI (this is what desktop keybindings run).
    QStringList commandIds;
    for (const auto &a : GlobalHotkeys::actions())
        commandIds << a.id;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printHelp();
            return 0;
        }
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith(QLatin1String("--")) && commandIds.contains(arg.mid(2)) && arg != QLatin1String("--show")) {
            QCoreApplication app(argc, argv);
            if (!otherInstanceRunning())
                return 1;   // nothing to control
            callRunningInstance(QStringLiteral("Command"), arg.mid(2));
            return 0;
        }
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("tembr"));
    QApplication::setApplicationName(QStringLiteral("tembr"));
    QApplication::setApplicationDisplayName(QStringLiteral("Tembr"));
    QApplication::setApplicationVersion(QStringLiteral(TEMBR_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("tembr"));

    // Standard dialogs/buttons in the user's language, if Qt's translations are installed.
    auto *qtTranslator = new QTranslator(&app);
    if (qtTranslator->load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                           QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(qtTranslator);

    QStringList paths;
    for (const QString &arg : app.arguments().mid(1)) {
        if (!arg.startsWith(QLatin1Char('-')))
            paths << (arg.contains(QLatin1String("://")) ? arg : QFileInfo(arg).absoluteFilePath());
    }

    // Single instance: hand the files to the running player, or just raise it.
    if (otherInstanceRunning()) {
        if (paths.isEmpty())
            callRunningInstance(QStringLiteral("Command"), QStringLiteral("show"));
        else
            callRunningInstance(QStringLiteral("Open"), paths);
        return 0;
    }

    av_log_set_level(AV_LOG_ERROR);   // FFmpeg is chatty about odd tags

    Theme::apply(AppSettings::load().theme);
    app.setProperty("tembrThemeApplied", true);

    MainWindow w;
    w.show();
    if (!paths.isEmpty())
        w.openPaths(paths);

    return app.exec();
}
