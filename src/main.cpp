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
#include <QHashFunctions>
#include <QLibraryInfo>
#include <QLocale>
#include <QThreadPool>
#include <QTranslator>
#include <QUrl>

#include <cstdio>
#include <cstdlib>
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
    // Qt 6.2's hardware string hash with a random seed ignores the end of
    // strings of 32+ characters: 100k paths below one long music folder fell
    // into a handful of buckets, and QSet/QHash of paths went quadratic (a
    // Local files rescan took over a minute). The fixed seed hashes them all;
    // only local paths and tags are hashed, so seed flooding is no concern.
    QHashSeed::setDeterministicGlobalSeed();
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
        if (arg.startsWith(QLatin1Char('-')))
            continue;
        if (const QUrl url(arg); arg.startsWith(QLatin1String("file:"), Qt::CaseInsensitive) && url.isLocalFile())
            paths << url.toLocalFile();   // Exec=tembr %U
        else
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

    // FFmpeg reports damaged tags and frames in files that play fine ("Incorrect BOM
    // value", "invalid new backstep"); real failures reach the UI anyway. TEMBR_DEBUG=1
    // brings the messages back.
    av_log_set_level(qEnvironmentVariableIsSet("TEMBR_DEBUG") ? AV_LOG_WARNING : AV_LOG_FATAL);

    Theme::apply(AppSettings::load().theme);
    app.setProperty("tembrThemeApplied", true);

    MainWindow w;
    w.show();
    if (!paths.isEmpty())
        w.openPaths(paths);

    const int code = app.exec();
    // Everything is saved by now. Background work still running (a tag read
    // or a folder scan stuck on a network drive) would hold the process in
    // the thread pool's destructor: give it a moment, then leave without it.
    if (!QThreadPool::globalInstance()->waitForDone(3000)) {
        std::fflush(nullptr);
        std::_Exit(code);
    }
    return code;
}
