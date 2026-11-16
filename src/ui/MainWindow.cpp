#include "ui/MainWindow.h"

#include "core/FileTrash.h"
#include "core/LocalFiles.h"
#include "core/MediaCollector.h"
#include "core/MetadataReader.h"
#include "core/PathResolver.h"
#include "core/PlaylistIO.h"
#include "core/SessionFiles.h"
#include "core/Relocator.h"
#include "platform/Mpris.h"
#include "ui/DeleteFilesDialog.h"
#include "ui/Icons.h"
#include "ui/LocalFilesDialog.h"
#include "ui/MigrationDialog.h"
#include "ui/NowPlayingPanel.h"
#include "ui/PlaybackController.h"
#include "ui/PlaylistFilterProxy.h"
#include "ui/PlaylistModel.h"
#include "ui/PlaylistPage.h"
#include "ui/PlaylistPanes.h"
#include "ui/Theme.h"
#include "ui/TransportBar.h"
#include "ui/TrackOsd.h"
#include "ui/TrackInfoDialog.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QActionGroup>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

// QMenu::addAction(text, shortcut, receiver, slot) only exists since Qt 6.3.
template <typename Receiver, typename Slot>
QAction *addItem(QMenu *menu, const QString &text, const QKeySequence &key, Receiver *receiver, Slot slot)
{
    QAction *a = menu->addAction(text);
    a->setShortcut(key);
    QObject::connect(a, &QAction::triggered, receiver, slot);
    return a;
}

QString playlistDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/playlists");
}

QString audioFileFilter()
{
    QStringList patterns;
    for (const QString &ext : MediaCollector::audioExtensions() + PlaylistIO::readableFormats())
        patterns << QStringLiteral("*.") + ext;
    return QStringLiteral("Аудио и плейлисты (%1);;Все файлы (*)").arg(patterns.join(QLatin1Char(' ')));
}

// Where AIMP running under Wine keeps its playlists, if it is installed.
QString aimpPlaylistDir()
{
    const QString prefix = qEnvironmentVariable("WINEPREFIX", QDir::homePath() + QStringLiteral("/.wine"));
    const QString user = qEnvironmentVariable("USER");
    for (const QString &sub : {QStringLiteral("AppData/Roaming/AIMP/PLS"), QStringLiteral("Application Data/AIMP/PLS")}) {
        const QString dir = QStringLiteral("%1/drive_c/users/%2/%3").arg(prefix, user, sub);
        if (QFileInfo(dir).isDir())
            return dir;
    }
    return {};
}

QList<PathResolver::Remap> loadRemaps()
{
    QList<PathResolver::Remap> out;
    const QStringList flat = QSettings().value(QStringLiteral("paths/remaps")).toStringList();
    for (int i = 0; i + 1 < flat.size(); i += 2)
        out.append({flat[i], flat[i + 1]});
    return out;
}

void saveRemaps(const QList<PathResolver::Remap> &remaps)
{
    QStringList flat;
    for (const auto &r : remaps)
        flat << r.first << r.second;
    QSettings().setValue(QStringLiteral("paths/remaps"), flat);
}

} // namespace

MainWindow::MainWindow(const MainWindowOptions &options, QWidget *parent)
    : QMainWindow(parent)
    , m_options(options)
    , m_engine(new AudioEngine(this))
    , m_playback(new PlaybackController(m_engine, this))
    , m_mpris(new MprisService(m_engine, this))
    , m_localFiles(new LocalFiles(this))
    , m_osd(new TrackOsd)
{
    setWindowTitle(QStringLiteral("Tembr"));
    PathResolver::setRemaps(loadRemaps());

    buildActions();
    buildUi();

    connect(m_engine, &AudioEngine::errorOccurred, this,
            [this](const QString &msg) { statusBar()->showMessage(msg, 5000); });

    // What plays is decided by m_playback; the window shows it.
    connect(m_playback, &PlaybackController::currentChanged, this, &MainWindow::updateNowPlaying);
    connect(m_playback, &PlaybackController::navigationChanged, this, &MainWindow::updateNavigation);
    connect(m_playback, &PlaybackController::message, this,
            [this](const QString &text) { statusBar()->showMessage(text, 5000); });
    connect(m_playback, &PlaybackController::trackStarted, this, [this](const Track &t) {
        // The title on screen when it cannot be seen in the window (AIMP's OSD).
        if (TrackOsd::wanted(m_settings.osd.when, isMinimized(), isVisible(), isActiveWindow()))
            m_osd->showTrack(t);
    });
    connect(m_playback, &PlaybackController::trackOpened, this,
            [this](const Track &t) { m_osd->refresh(t, m_engine->durationMs()); });   // the length, if the tags had none
    connect(m_playback, &PlaybackController::currentTagsChanged, this, [this] {
        updateNowPlaying();
        if (const Track *t = m_playback->currentTrack())
            m_osd->refresh(*t, m_engine->durationMs());   // tags just read
    });
    connect(m_playback, &PlaybackController::followRow, this, [this](PlaylistModel *model, int row) {
        if (PlaylistPage *page = pageOf(model))
            page->scrollToSourceRow(row);
    });
    connect(m_playback, &PlaybackController::volumeChanged, this, [this](int v) { m_mpris->setVolume(v / 100.0); });

    // MPRIS: GNOME notification list, media keys, `tembr --next`, second instances.
    connect(m_mpris, &MprisService::commandRequested, this, &MainWindow::handleCommand);
    connect(m_mpris, &MprisService::raiseRequested, this, &MainWindow::showAndRaise);
    connect(m_mpris, &MprisService::quitRequested, this, &MainWindow::quit);
    connect(m_mpris, &MprisService::seekRequested, m_engine, &AudioEngine::seek);
    connect(m_mpris, &MprisService::volumeRequested, this, [this](double v) { m_playback->setVolume(qRound(v * 100)); });
    connect(m_mpris, &MprisService::openRequested, this, [this](const QStringList &paths) {
        showAndRaise();
        openPaths(paths);
    });
    if (m_options.desktopIntegration && !m_mpris->registerOnBus())
        qWarning("MPRIS: could not register on the session bus");

    connect(m_localFiles, &LocalFiles::scanned, this, &MainWindow::applyLocalScan);
    connect(m_localFiles, &LocalFiles::scanningChanged, this, &MainWindow::updateStats);
    connect(m_localFiles, &LocalFiles::changedOnDisk, this, &MainWindow::rescanLocal);

    m_settings = AppSettings::load();
    applySettings(m_settings);
    restoreSession();
    registerHotkeys();
    // Also on logout and other ways out that skip closeEvent.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::releaseHotkeys);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (!m_finalSaveDone)
            saveSession();
    });
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this] { saveSession(); });   // desktop logout

    m_autosave = new QTimer(this);
    m_autosave->setSingleShot(true);
    m_autosave->setInterval(3000);
    connect(m_autosave, &QTimer::timeout, this, [this] { saveSession(SaveScope::Playlists); });

    if (!m_settings.droppedHotkeys.isEmpty()) {
        m_settings.save();   // forget them for good
        QTimer::singleShot(0, this, [this, dropped = m_settings.droppedHotkeys] {
            QMessageBox::information(
                this, windowTitle(),
                QStringLiteral("Сброшены глобальные сочетания: %1.\n\n"
                               "Клавиши без Ctrl, Alt или Super перехватывались во всех программах, "
                               "и их нельзя было набрать. Назначьте сочетания заново в «Файл → Настройки»: "
                               "теперь клавиши цифрового блока отличаются от обычных цифр.")
                    .arg(dropped.join(QStringLiteral(", "))));
        });
    }

    if (!m_engine->isDeviceOk()) {
        QTimer::singleShot(0, this, [this] {
            QMessageBox::warning(this, windowTitle(),
                                 QStringLiteral("Звуковое устройство недоступно: %1").arg(m_engine->lastError()));
        });
    }
}

// --- UI construction ---------------------------------------------------------

void MainWindow::buildActions()
{
    m_actPrev = new QAction(QStringLiteral("Предыдущий"), this);
    m_actPrev->setShortcuts({Qt::Key_Z, Qt::Key_MediaPrevious});
    m_actPlay = new QAction(QStringLiteral("Воспроизвести"), this);
    m_actPlay->setShortcuts({Qt::Key_Space, Qt::Key_X, Qt::Key_MediaTogglePlayPause, Qt::Key_MediaPlay});
    m_actStop = new QAction(QStringLiteral("Стоп"), this);
    m_actStop->setShortcuts({Qt::Key_V, Qt::Key_MediaStop});
    m_actNext = new QAction(QStringLiteral("Следующий"), this);
    m_actNext->setShortcuts({Qt::Key_B, Qt::Key_MediaNext});
    m_actShow = new QAction(QStringLiteral("Показать / скрыть окно"), this);
    m_actQuit = new QAction(QStringLiteral("Выход"), this);
    m_actQuit->setShortcut(QKeySequence::Quit);

    connect(m_actPrev, &QAction::triggered, this, [this] { handleCommand(QStringLiteral("previous")); });
    connect(m_actPlay, &QAction::triggered, this, [this] { handleCommand(QStringLiteral("play-pause")); });
    connect(m_actStop, &QAction::triggered, this, [this] { handleCommand(QStringLiteral("stop")); });
    connect(m_actNext, &QAction::triggered, this, [this] { handleCommand(QStringLiteral("next")); });
    connect(m_actShow, &QAction::triggered, this, &MainWindow::toggleVisible);
    connect(m_actQuit, &QAction::triggered, this, &MainWindow::quit);

    // --- File
    QMenu *file = menuBar()->addMenu(QStringLiteral("&Файл"));
    addItem(file, QStringLiteral("Добавить файлы…"), QKeySequence::Open, this, &MainWindow::addFiles);
    addItem(file, QStringLiteral("Добавить папку…"), QKeySequence(QStringLiteral("Ctrl+Shift+O")), this,
            &MainWindow::addFolder);
    file->addSeparator();
    m_actNewPlaylist = addItem(file, QStringLiteral("Новый плейлист"), QKeySequence::New, this,
                               [this] { bringUp(addPage(newPlaylistName())); });
    m_actOpenPlaylists = addItem(file, QStringLiteral("Открыть плейлисты…"), QKeySequence(QStringLiteral("Ctrl+L")), this,
                                 &MainWindow::openPlaylists);
    addItem(file, QStringLiteral("Импорт из AIMP (компьютер, Android)…"), QKeySequence(QStringLiteral("Ctrl+Shift+I")),
            this, &MainWindow::importFromAimp);
    addItem(file, QStringLiteral("Сохранить плейлист как…"), QKeySequence::Save, this, &MainWindow::savePlaylist);
    // The playlist in the right pane, also when Local files are active: it
    // is the one named in its header.
    m_actRename = addItem(file, QStringLiteral("Переименовать плейлист…"), QKeySequence(Qt::Key_F2), this,
                          [this] { renamePage(m_panes->shownPlaylist()); });
    m_actClose = addItem(file, QStringLiteral("Закрыть плейлист"), QKeySequence::Close, this,
                         [this] { closePage(m_panes->shownPlaylist()); });
    file->addSeparator();
    addItem(file, QStringLiteral("Настройки…"), QKeySequence(QStringLiteral("Ctrl+,")), this, &MainWindow::showSettings);
    file->addSeparator();
    file->addAction(m_actQuit);

    // --- Playlist
    QMenu *pl = menuBar()->addMenu(QStringLiteral("&Плейлист"));
    addItem(pl, QStringLiteral("Информация о треке…"), QKeySequence(QStringLiteral("Ctrl+I")), this,
            [this] { showTrackInfo(); });
    addItem(pl, QStringLiteral("Найти…"), QKeySequence::Find, this, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
    pl->addSeparator();
    pl->addAction(QStringLiteral("Найти перемещённые файлы…"), this, &MainWindow::findMovedFiles);
    pl->addAction(QStringLiteral("Удалить выбранные"), this, [this] {
        if (auto *p = activePage(); p && !refuseReadOnly(p))
            p->removeSelected();
    });
    pl->addAction(QStringLiteral("Удалить с диска…"), this, [this] { deleteSelectedFiles(); });
    pl->addAction(QStringLiteral("Удалить отсутствующие файлы"), this, [this] {
        if (auto *p = activePage(); p && !refuseReadOnly(p))
            p->model()->removeMissing();
    });
    pl->addAction(QStringLiteral("Очистить плейлист"), this, [this] {
        if (auto *p = activePage(); p && !refuseReadOnly(p))
            p->model()->clear();
    });
    pl->addSeparator();
    addItem(pl, QStringLiteral("Обновить локальные файлы"), QKeySequence(Qt::Key_F5), this, &MainWindow::rescanLocal);
    pl->addAction(QStringLiteral("Настройки локальных файлов…"), this, &MainWindow::showLocalFilesSettings);

    // Work on playlists: off until the session is loaded (see restoreSession()).
    for (QMenu *menu : {file, pl}) {
        for (QAction *a : menu->actions()) {
            if (!a->isSeparator() && a != m_actQuit && a->text() != QStringLiteral("Настройки…"))
                m_needSession << a;
        }
    }

    // --- Playback
    QMenu *pb = menuBar()->addMenu(QStringLiteral("&Воспроизведение"));
    pb->addAction(m_actPlay);
    pb->addAction(m_actStop);
    pb->addAction(m_actPrev);
    pb->addAction(m_actNext);
    pb->addSeparator();
    addItem(pb, QStringLiteral("Вперёд на 5 с"), QKeySequence(QStringLiteral("Ctrl+Right")), this,
            [this] { m_engine->seek(m_engine->positionMs() + 5000); });
    addItem(pb, QStringLiteral("Назад на 5 с"), QKeySequence(QStringLiteral("Ctrl+Left")), this,
            [this] { m_engine->seek(m_engine->positionMs() - 5000); });
    addItem(pb, QStringLiteral("Громче"), QKeySequence(QStringLiteral("Ctrl+Up")), this, [this] { m_playback->changeVolume(5); });
    addItem(pb, QStringLiteral("Тише"), QKeySequence(QStringLiteral("Ctrl+Down")), this, [this] { m_playback->changeVolume(-5); });
}

void MainWindow::buildUi()
{
    // --- Left: big cover + track info
    m_nowPlaying = new NowPlayingPanel;
    m_nowPlaying->setMinimumWidth(200);

    // --- Right: search above Local files and one of the user's playlists
    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(4, 10, 10, 0);
    rightLayout->setSpacing(6);

    auto *searchRow = new QHBoxLayout;
    m_searchField = new QComboBox;
    for (const auto &choice : TrackQuery::fieldChoices())
        m_searchField->addItem(choice.first, int(choice.second));
    m_search = new QLineEdit;
    m_search->setClearButtonEnabled(true);
    m_search->setPlaceholderText(QStringLiteral("Поиск…  например: artist:queen year:1975-1985 -live"));
    m_search->setToolTip(QStringLiteral(
        "Без префикса — поиск по выбранному полю.\n"
        "Поля: title: artist: album: albumartist: genre: year: path: format: composer: comment:\n"
        "(или по-русски: название: исполнитель: альбом: жанр: год: путь: формат:)\n"
        "Фразы в кавычках: album:\"a night at the opera\"\n"
        "Диапазон лет: year:1970-1979   Исключить: -live"));
    searchRow->addWidget(m_searchField);
    searchRow->addWidget(m_search, 1);
    rightLayout->addLayout(searchRow);

    auto *searchTimer = new QTimer(this);
    searchTimer->setSingleShot(true);
    searchTimer->setInterval(150);
    connect(m_search, &QLineEdit::textChanged, searchTimer, qOverload<>(&QTimer::start));
    connect(searchTimer, &QTimer::timeout, this, &MainWindow::applySearch);
    connect(m_searchField, &QComboBox::currentIndexChanged, this, &MainWindow::applySearch);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        // Enter in the search box plays the first match of the active list,
        // else of the other one (if the search filters it too).
        QList<PlaylistPage *> pages = {activePage()};
        if (m_settings.searchScope == AppSettings::SearchScope::BothLists)
            pages << m_panes->otherPage();
        for (PlaylistPage *p : std::as_const(pages)) {
            if (p && p->proxy()->rowCount() > 0) {
                m_playback->playRow(p->model(), p->proxy()->mapToSource(p->proxy()->index(0, 0)).row());
                return;
            }
        }
    });

    m_panes = new PlaylistPanes;
    m_panes->setPlaylistActions(m_actNewPlaylist, m_actOpenPlaylists, m_actRename, m_actClose);
    // Another playlist shown or another pane active: the search, the totals
    // and "next" (when nothing plays) follow.
    const auto follow = [this] {
        applySearch();
        updateStats();
        updateNavigation();
    };
    connect(m_panes, &PlaylistPanes::shownChanged, this, follow);
    connect(m_panes, &PlaylistPanes::activeChanged, this, follow);
    connect(m_panes, &PlaylistPanes::orderChanged, this, &MainWindow::scheduleSave);
    rightLayout->addWidget(m_panes, 1);

    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->addWidget(m_nowPlaying);
    m_splitter->addWidget(right);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(1, false);
    m_splitter->setSizes({300, 900});

    // --- Bottom: transport bar
    m_transport = new TransportBar(m_playback, {m_actPrev, m_actPlay, m_actNext, m_actStop});

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(m_splitter, 1);
    root->addWidget(m_transport);
    setCentralWidget(central);

    m_stats = new QLabel;
    statusBar()->addWidget(m_stats);
    auto *device = new QLabel(m_engine->isDeviceOk() ? m_engine->deviceName() : QStringLiteral("нет звука"));
    device->setForegroundRole(QPalette::PlaceholderText);
    statusBar()->addPermanentWidget(device);

    resize(1200, 720);
}

void MainWindow::refreshIcons()
{
    m_transport->refreshIcons();
    setWindowIcon(Icons::app());
}

// --- Settings, tray ------------------------------------------------------------

MainWindow::~MainWindow()
{
    delete m_osd;
}

void MainWindow::applySettings(const AppSettings &s)
{
    m_osd->setSettings(s.osd);
    if (Theme::current() != s.theme || !qApp->property("tembrThemeApplied").toBool()) {
        Theme::apply(s.theme);
        qApp->setProperty("tembrThemeApplied", true);
    }
    refreshIcons();
    m_panes->refreshActiveMark();
    setupTray(s.trayIcon);
    applySearch();   // the search may now filter one list or both
}

void MainWindow::setupTray(bool enabled)
{
    if (!enabled || !m_options.desktopIntegration || !QSystemTrayIcon::isSystemTrayAvailable()) {
        delete m_tray;
        m_tray = nullptr;
        return;
    }
    if (!m_tray) {
        m_trayMenu = new QMenu(this);
        m_trayTitle = m_trayMenu->addAction(QStringLiteral("Tembr"));
        m_trayTitle->setEnabled(false);
        m_trayMenu->addSeparator();
        m_trayMenu->addAction(m_actPlay);
        m_trayMenu->addAction(m_actPrev);
        m_trayMenu->addAction(m_actNext);
        m_trayMenu->addAction(m_actStop);
        m_trayMenu->addSeparator();
        m_trayMenu->addAction(m_actShow);
        m_trayMenu->addAction(m_actQuit);

        m_tray = new QSystemTrayIcon(Icons::app(), this);
        m_tray->setContextMenu(m_trayMenu);
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger)
                toggleVisible();
            else if (r == QSystemTrayIcon::MiddleClick)
                handleCommand(QStringLiteral("play-pause"));
        });
    }
    m_tray->show();
    updateNowPlaying();
}

void MainWindow::registerHotkeys()
{
    if (!m_options.desktopIntegration)
        return;
    // gsettings takes a while (also to find out whether it can be used):
    // don't hold up the window.
    m_hotkeyJob = QtConcurrent::run([keys = m_settings.hotkeys] {
        return GlobalHotkeys::desktopSupported() ? GlobalHotkeys::applyToDesktop(keys) : QString();
    });
}

void MainWindow::releaseHotkeys()
{
    if (m_hotkeysReleased || !m_options.desktopIntegration)
        return;
    m_hotkeysReleased = true;
    m_hotkeyJob.waitForFinished();
    if (m_settings.releaseHotkeysOnExit && !m_settings.hotkeys.isEmpty())
        GlobalHotkeys::releaseFromDesktop();
}

void MainWindow::showSettings()
{
    SettingsDialog dlg(m_settings, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const AppSettings s = dlg.values();
    const bool keysChanged = s.hotkeys != m_settings.hotkeys;
    m_settings = s;
    m_settings.save();
    applySettings(m_settings);
    if (keysChanged && m_options.desktopIntegration) {
        // Some 20 gsettings calls: in the background, after the one before.
        auto *watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
            watcher->deleteLater();
            const QString err = watcher->result();
            if (!err.isEmpty())
                QMessageBox::warning(this, windowTitle(), QStringLiteral("Не все клавиши удалось назначить:\n") + err);
        });
        m_hotkeyJob = QtConcurrent::run([previous = m_hotkeyJob, keys = m_settings.hotkeys]() mutable {
            previous.waitForFinished();
            return GlobalHotkeys::applyToDesktop(keys);
        });
        watcher->setFuture(m_hotkeyJob);
    }
}

void MainWindow::showAndRaise()
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
}

void MainWindow::toggleVisible()
{
    if (isVisible() && !isMinimized())
        hide();
    else
        showAndRaise();
}

void MainWindow::quit()
{
    m_quitting = true;
    // An open modal dialog (message box, settings) blocks closing the window,
    // so Quit from the tray or MPRIS would do nothing. Dismiss it first.
    while (QWidget *modal = QApplication::activeModalWidget()) {
        if (auto *dlg = qobject_cast<QDialog *>(modal))
            dlg->reject();
        else
            modal->close();
        if (QApplication::activeModalWidget() == modal)
            break;
    }
    QTimer::singleShot(0, this, &QWidget::close);
}

void MainWindow::handleCommand(const QString &command)
{
    // Before the playlists are in there is nothing to play: done once they are.
    static const QStringList needSession = {QStringLiteral("play-pause"), QStringLiteral("play"),
                                            QStringLiteral("next"), QStringLiteral("previous")};
    if (!m_sessionLoaded && needSession.contains(command)) {
        m_pendingCommands << command;
        return;
    }
    PlaylistModel *fallback = activePage() ? activePage()->model() : nullptr;   // when nothing plays
    if (command == QLatin1String("play-pause"))
        playOrResume();
    else if (command == QLatin1String("play")) {
        if (m_engine->state() != AudioEngine::State::Playing)
            playOrResume();
    } else if (command == QLatin1String("pause"))
        m_playback->pause();
    else if (command == QLatin1String("stop"))
        m_playback->stop();
    else if (command == QLatin1String("next"))
        m_playback->next(fallback);
    else if (command == QLatin1String("previous"))
        m_playback->previous(fallback);
    else if (command == QLatin1String("volume-up"))
        m_playback->changeVolume(5);
    else if (command == QLatin1String("volume-down"))
        m_playback->changeVolume(-5);
    else if (command == QLatin1String("show"))
        showAndRaise();
}

// --- Pages -------------------------------------------------------------------

PlaylistPage *MainWindow::activePage() const
{
    return m_panes->activePage();
}

PlaylistPage *MainWindow::createPage(const QString &name)
{
    // A new list takes the column layout of the one worked in.
    if (auto *from = activePage()) {
        m_headerState = from->view()->horizontalHeader()->saveState();
        m_headerColumns = PlaylistModel::ColumnCount;
    }

    auto *page = new PlaylistPage(name);
    if (!m_headerState.isEmpty() && page->view()->horizontalHeader()->restoreState(m_headerState)) {
        for (int col = m_headerColumns; col < PlaylistModel::ColumnCount; ++col)
            page->view()->horizontalHeader()->setSectionHidden(col, true);
    }
    page->view()->horizontalHeader()->setSortIndicatorShown(false);

    connect(page, &PlaylistPage::trackActivated, this, [this, page](int row) { m_playback->playRow(page->model(), row); });
    connect(page, &PlaylistPage::tracksAdded, this, [this, page](int row, bool playFirst) {
        if (playFirst)
            m_playback->playRow(page->model(), row);
    });
    // Ordinary playlists are saved soon after they change (also when sorted).
    const auto changed = [this, page] {
        if (page != m_localPage)
            scheduleSave();
    };
    connect(page->model(), &PlaylistModel::contentsChanged, this, changed);
    connect(page->model(), &QAbstractItemModel::layoutChanged, this, changed);
    connect(page, &PlaylistPage::statsChanged, this, [this, page] {
        if (page == activePage())
            updateStats();
        if (page->model() == m_playback->playingModel())
            updateNavigation();
    });
    // Del in Local files: say why nothing happens (the page itself ignores it).
    connect(page->view(), &PlaylistView::deletePressed, this, [this, page] { refuseReadOnly(page); });
    connect(page, &PlaylistPage::contextMenuRequested, this,
            [this, page](const QPoint &p) { showContextMenu(page, p); });
    connect(page->view()->horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this, page](const QPoint &p) { showHeaderMenu(page, page->view()->horizontalHeader()->mapToGlobal(p)); });
    return page;
}

PlaylistPage *MainWindow::addPage(const QString &name)
{
    PlaylistPage *page = createPage(name);
    m_panes->addPlaylist(page);
    return page;
}

void MainWindow::bringUp(PlaylistPage *page)
{
    if (!page)
        return;
    m_panes->showPlaylist(page);
    m_panes->activatePage(page);
}

QString MainWindow::newPlaylistName() const
{
    QSet<QString> taken;
    for (const PlaylistPage *p : m_panes->playlists())
        taken.insert(p->model()->name());
    for (int n = int(m_panes->playlists().size()) + 1;; ++n) {
        const QString name = QStringLiteral("Плейлист %1").arg(n);
        if (!taken.contains(name))
            return name;
    }
}

void MainWindow::closePage(PlaylistPage *page)
{
    if (!page || refuseReadOnly(page))
        return;
    if (page->model()->rowCount() > 0) {
        QMessageBox box(QMessageBox::Question, windowTitle(),
                        QStringLiteral("Закрыть плейлист «%1»?").arg(page->model()->name()), QMessageBox::NoButton, this);
        QPushButton *yes = box.addButton(QStringLiteral("Закрыть"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("Отмена"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() != yes)
            return;
    }

    m_playback->release(page->model());
    m_pageQueries.remove(page);
    m_panes->removePlaylist(page);
    scheduleSave();
    page->deleteLater();
}

void MainWindow::renamePage(PlaylistPage *page)
{
    if (!page || refuseReadOnly(page))
        return;
    QInputDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Переименовать плейлист"));
    dlg.setLabelText(QStringLiteral("Название:"));
    dlg.setTextValue(page->model()->name());
    dlg.setOkButtonText(QStringLiteral("Переименовать"));
    dlg.setCancelButtonText(QStringLiteral("Отмена"));
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.textValue().trimmed();
    if (name.isEmpty())
        return;
    page->model()->setName(name);
    m_panes->refreshNames();
    scheduleSave();
}

void MainWindow::openPaths(const QStringList &paths)
{
    if (!m_sessionLoaded) {
        m_pendingOpen << paths;   // into the playlist restored, not a new empty one
        return;
    }
    PlaylistPage *page = editablePage();
    bringUp(page);
    page->addPaths(paths, -1, true);
}

PlaylistPage *MainWindow::editablePage()
{
    if (auto *active = activePage(); active && !active->isReadOnly())
        return active;
    if (auto *shown = m_panes->shownPlaylist())
        return shown;
    // No playlists (the right pane is free): one is made for the files.
    return addPage(newPlaylistName());
}

bool MainWindow::refuseReadOnly(PlaylistPage *page)
{
    if (!page || !page->isReadOnly())
        return false;
    statusBar()->showMessage(QStringLiteral("«Локальные файлы» составляются из папок с музыкой автоматически — "
                                            "папки задаются в «Плейлист → Настройки локальных файлов»."),
                             6000);
    return true;
}

void MainWindow::createLocalPage()
{
    m_localPage = createPage(QStringLiteral("Локальные файлы"));
    m_localPage->setReadOnly(true);
    m_panes->setLocalPage(m_localPage);
    // Sorting by a column header is remembered too, as in AIMP.
    connect(m_localPage->model(), &PlaylistModel::sorted, this, [this](int column, Qt::SortOrder order) {
        m_localSortColumn = column;
        m_localSortOrder = order;
        syncLocalSort();
        QSettings s;
        s.setValue(QStringLiteral("localFiles/sortColumn"), column);
        s.setValue(QStringLiteral("localFiles/sortOrder"), int(order));
    });
    // New files are sorted before their tags are read; a sort by tags is
    // applied again once they are in.
    connect(m_localPage->model(), &PlaylistModel::contentsChanged, this, [this] {
        PlaylistModel *model = m_localPage->model();
        if (!m_localResort || model->isScanning())
            return;
        m_localResort = false;
        if (m_localSortColumn >= 0)
            model->sort(m_localSortColumn, m_localSortOrder);
    });
}

void MainWindow::setLocalSort(int column, Qt::SortOrder order)
{
    if (column >= 0) {
        m_localPage->model()->sort(column, order);   // stored via sorted()
        return;
    }
    m_localSortColumn = -1;
    syncLocalSort();
    QSettings().setValue(QStringLiteral("localFiles/sortColumn"), -1);
    rescanLocal();   // comes back in folder order
}

void MainWindow::syncLocalSort()
{
    TrackSort::Key key = TrackSort::Key::Title;
    const bool sorted = PlaylistModel::sortKey(m_localSortColumn, &key);
    m_localFiles->setSort(sorted, key, m_localSortOrder);
}

void MainWindow::applyLocalScan(const TrackList &found, int sortGeneration)
{
    PlaylistModel *model = m_localPage->model();
    TrackList tracks = found;   // sorted by the scan, if its sort is still the one wanted
    TrackSort::Key key;
    bool byTags = false;
    if (PlaylistModel::sortKey(m_localSortColumn, &key)) {
        if (sortGeneration != m_localFiles->sortGeneration())
            TrackSort::sort(tracks, key, m_localSortOrder);   // changed while it ran
        byTags = key != TrackSort::Key::Added && key != TrackSort::Key::Modified && key != TrackSort::Key::Path;
    }
    // Nothing changed (same entries, files and dates, in the same order):
    // keep the view as it is.
    if (LocalLibrary::sameList(model->tracks(), tracks))
        return;

    // Keep the list where the user was looking.
    PlaylistView *view = m_localPage->view();
    const QModelIndex top = m_localPage->proxy()->mapToSource(view->indexAt(QPoint(4, 4)));
    const quint64 topUid = top.isValid() ? model->track(top.row()).uid : 0;
    model->setTracks(tracks);
    m_localResort = byTags && std::any_of(tracks.cbegin(), tracks.cend(), [](const Track &t) { return !t.scanned; });
    if (topUid) {
        const QModelIndex idx = m_localPage->proxy()->mapFromSource(model->index(model->rowOfUid(topUid), 0));
        if (idx.isValid())
            view->scrollTo(idx, QAbstractItemView::PositionAtTop);
    }
    if (m_localPage->model() == m_playback->playingModel())
        updateNowPlaying();
    updateStats();
}

void MainWindow::showLocalFilesSettings()
{
    LocalFilesDialog dlg(m_localFiles->settings(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const LocalFilesSettings s = dlg.values();
    if (s == m_localFiles->settings())
        return;
    m_localFiles->setSettings(s);
    rescanLocal();
}

// --- Playback ----------------------------------------------------------------

void MainWindow::playOrResume()
{
    if (m_playback->pauseOrResume())
        return;
    // Stopped: what to start is what the user works in.
    PlaylistPage *page = activePage();
    if (!page)
        return;
    PlaylistModel *model = page->model();
    // From the tray/media keys the selection is not what the user means.
    int row = isActiveWindow() ? page->focusedSourceRow() : -1;
    if (PlaylistModel *playing = m_playback->playingModel(); row < 0 && playing && playing->currentRow() >= 0) {
        model = playing;
        row = playing->currentRow();
    }
    if (row < 0 && page->proxy()->rowCount() > 0)
        row = page->proxy()->mapToSource(page->proxy()->index(0, 0)).row();
    m_playback->playRow(model, row);
}

PlaylistPage *MainWindow::pageOf(PlaylistModel *model) const
{
    return model ? qobject_cast<PlaylistPage *>(model->parent()) : nullptr;
}

void MainWindow::updateNowPlaying()
{
    // Also a track whose row is gone: it plays on, so it is still shown.
    const Track *current = m_playback->currentTrack();
    if (!current) {
        m_nowPlaying->clear();
        m_coverPath.clear();
        ++m_coverJob;   // a cover still being read is for nothing now
        m_coverImage = QImage();
        setWindowTitle(QStringLiteral("Tembr"));
        m_mpris->setTrack(nullptr, {});
        if (m_tray) {
            m_tray->setToolTip(QStringLiteral("Tembr"));
            m_trayTitle->setText(QStringLiteral("Tembr"));
        }
        updateNavigation();
        return;
    }
    const Track t = *current;
    if (t.path != m_coverPath) {
        m_coverPath = t.path;
        if (t.isUrl())
            setCover(++m_coverJob, {}, {});
        else
            loadCover(t);   // the previous cover stays until this one is in (the same, within an album)
    }
    m_nowPlaying->showTrack(t, m_coverImage);
    setWindowTitle(t.displayTitle() + QStringLiteral(" — Tembr"));
    m_mpris->setTrack(&t, m_artFile);
    if (m_tray) {
        m_tray->setToolTip(t.displayTitle());
        m_trayTitle->setText(t.displayTitle());
    }
    updateNavigation();
}

// Reading the cover (and writing it as a file for MPRIS clients) touches the
// disk, sometimes a slow one: done in the background, shown when it is in.
void MainWindow::loadCover(const Track &t)
{
    const int job = ++m_coverJob;
    const QString path = t.path;
    const QString file = QStringLiteral("%1/cover-%2.jpg")
                             .arg(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                             .arg(t.uid);
    auto *watcher = new QFutureWatcher<QPair<QImage, QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<QImage, QString>>::finished, this, [this, watcher, job] {
        const auto result = watcher->result();
        watcher->deleteLater();
        setCover(job, result.first, result.second);
    });
    watcher->setFuture(QtConcurrent::run([path, file] {
        const QImage image = MetadataReader::readCover(path);
        QString art;
        if (!image.isNull()) {
            QDir().mkpath(QFileInfo(file).absolutePath());
            if (image.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(file, "JPG", 90))
                art = file;
        }
        return qMakePair(image, art);
    }));
}

void MainWindow::setCover(int job, const QImage &image, const QString &artFile)
{
    if (job != m_coverJob) {   // the track changed meanwhile
        if (!artFile.isEmpty() && artFile != m_artFile)
            QFile::remove(artFile);
        return;
    }
    if (!m_artFile.isEmpty() && m_artFile != artFile)
        QFile::remove(m_artFile);
    m_artFile = artFile;
    m_coverImage = image;
    if (const Track *current = m_playback->currentTrack()) {
        m_nowPlaying->showTrack(*current, m_coverImage);
        m_mpris->setTrack(current, m_artFile);
    }
}

void MainWindow::updateNavigation()
{
    PlaylistModel *fallback = activePage() ? activePage()->model() : nullptr;
    m_mpris->setNavigation(m_playback->hasNext(fallback), m_playback->hasPrevious());
}

// --- Playlist helpers ----------------------------------------------------------

void MainWindow::updateStats()
{
    auto *page = activePage();
    if (!page) {
        m_stats->clear();
        return;
    }
    const int total = page->model()->rowCount();
    QString text = QStringLiteral("%1 треков · %2").arg(total).arg(formatDuration(page->model()->totalDurationMs()));
    if (const int missing = page->model()->missingCount())
        text += QStringLiteral(" · не найдено %1").arg(missing);
    if (page->proxy()->isFiltering())
        text += QStringLiteral(" · найдено по запросу %1").arg(page->proxy()->rowCount());
    if (page == m_localPage && m_localFiles->isScanning())
        text += QStringLiteral(" · обзор папок…");
    if (page->model()->isScanning())
        text += QStringLiteral(" · чтение тегов…");
    if (page == m_localPage && total == 0 && !m_localFiles->isScanning())
        text = QStringLiteral("Папки с музыкой не заданы или пусты — «Плейлист → Настройки локальных файлов»");
    m_stats->setText(text);
}

// Both lists on screen, or only the active one (the other is shown whole).
// Playlists not shown keep their filter until they are shown again.
void MainWindow::applySearch()
{
    const QString text = m_search->text();
    const int field = m_searchField->currentData().toInt();
    const bool both = m_settings.searchScope == AppSettings::SearchScope::BothLists;
    PlaylistPage *active = activePage();
    for (PlaylistPage *page : {m_localPage, m_panes->shownPlaylist()}) {
        if (page)
            setPageQuery(page, both || page == active ? text : QString(), field);
    }
}

void MainWindow::setPageQuery(PlaylistPage *page, const QString &text, int field)
{
    const QString key = text.trimmed().isEmpty() ? QString() : QString::number(field) + QLatin1Char(':') + text;
    const auto it = m_pageQueries.constFind(page);
    if (it == m_pageQueries.cend() ? key.isEmpty() : *it == key)
        return;   // filtered so already
    m_pageQueries.insert(page, key);
    page->setQuery(text, TrackQuery::Field(field));
}

void MainWindow::showTrackInfo(PlaylistPage *page)
{
    if (!page)
        page = activePage();
    if (!page)
        return;
    int row = page->focusedSourceRow();
    if (row < 0 && page->model() == m_playback->playingModel())
        row = page->model()->currentRow();
    if (row < 0)
        return;
    TrackInfoDialog dlg(page->model()->track(row), this);
    dlg.exec();
}

void MainWindow::showContextMenu(PlaylistPage *page, const QPoint &globalPos)
{
    if (!page)
        return;
    // A right click works in that list: its pane becomes the active one.
    m_panes->activatePage(page, false);
    const int row = page->focusedSourceRow();
    QMenu menu(this);
    if (row >= 0) {
        menu.addAction(Icons::make(Icons::Kind::Play, Theme::iconColor()), QStringLiteral("Воспроизвести"), this,
                       [this, page, row] { m_playback->playRow(page->model(), row); });
        menu.addAction(QStringLiteral("Информация о треке…"), this, [this, page] { showTrackInfo(page); });
        const Track &t = page->model()->track(row);
        if (!t.isUrl()) {
            menu.addAction(QStringLiteral("Открыть папку с файлом"), this, [path = t.path] {
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
            });
        }
        // Into another playlist (from Local files above all): copies, the
        // tracks stay here too.
        QMenu *send = menu.addMenu(QStringLiteral("Отправить в плейлист"));
        for (PlaylistPage *to : m_panes->playlists()) {
            if (to != page)
                send->addAction(to->model()->name(), this, [this, page, to] { sendSelected(page, to); });
        }
        if (!send->isEmpty())
            send->addSeparator();
        send->addAction(QStringLiteral("Новый плейлист…"), this, [this, page] { sendSelected(page, nullptr); });
        menu.addSeparator();
        if (!page->isReadOnly())
            menu.addAction(QStringLiteral("Удалить из плейлиста"), page, &PlaylistPage::removeSelected);
        if (!t.isUrl())
            menu.addAction(QStringLiteral("Удалить с диска…"), this, [this, page] { deleteSelectedFiles(page); });
        menu.addSeparator();
    }
    if (page->isReadOnly()) {
        QMenu *sortMenu = menu.addMenu(QStringLiteral("Сортировка"));
        auto *group = new QActionGroup(sortMenu);
        auto sortItem = [&](const QString &text, int column, Qt::SortOrder order) {
            QAction *a = sortMenu->addAction(text, this, [this, column, order] { setLocalSort(column, order); });
            a->setCheckable(true);
            a->setChecked(m_localSortColumn == column && (column < 0 || m_localSortOrder == order));
            group->addAction(a);
        };
        sortItem(QStringLiteral("По папкам"), -1, Qt::AscendingOrder);
        sortMenu->addSeparator();
        sortItem(QStringLiteral("Сначала недавно добавленные"), PlaylistModel::ColAdded, Qt::DescendingOrder);
        sortItem(QStringLiteral("Сначала давно добавленные"), PlaylistModel::ColAdded, Qt::AscendingOrder);
        sortMenu->addSeparator();
        sortItem(QStringLiteral("Сначала недавно изменённые"), PlaylistModel::ColModified, Qt::DescendingOrder);
        sortItem(QStringLiteral("Сначала давно изменённые"), PlaylistModel::ColModified, Qt::AscendingOrder);
        menu.addAction(QStringLiteral("Обновить"), this, &MainWindow::rescanLocal);
        menu.addAction(QStringLiteral("Настройки локальных файлов…"), this, &MainWindow::showLocalFilesSettings);
        menu.exec(globalPos);
        return;
    }
    menu.addAction(QStringLiteral("Добавить файлы…"), this, &MainWindow::addFiles);
    menu.addAction(QStringLiteral("Добавить папку…"), this, &MainWindow::addFolder);
    if (page->model()->missingCount() > 0)
        menu.addAction(QStringLiteral("Найти перемещённые файлы…"), this, &MainWindow::findMovedFiles);
    menu.exec(globalPos);
}

void MainWindow::sendSelected(PlaylistPage *from, PlaylistPage *to)
{
    const TrackList tracks = from->model()->copiesOf(from->selectedSourceRows());
    if (tracks.isEmpty())
        return;
    if (!to) {
        QInputDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("Новый плейлист"));
        dlg.setLabelText(QStringLiteral("Название:"));
        dlg.setTextValue(newPlaylistName());
        dlg.setOkButtonText(QStringLiteral("Создать"));
        dlg.setCancelButtonText(QStringLiteral("Отмена"));
        if (dlg.exec() != QDialog::Accepted || dlg.textValue().trimmed().isEmpty())
            return;
        to = addPage(dlg.textValue().trimmed());
    }
    to->model()->insertTracks(-1, tracks);   // saved by the autosave, as any change
    statusBar()->showMessage(QStringLiteral("Добавлено треков в «%1»: %2").arg(to->model()->name()).arg(tracks.size()), 5000);
}

// The columns are the same in every list: shown or hidden in all at once;
// each list fits the widths to its own pane.
void MainWindow::showHeaderMenu(PlaylistPage *page, const QPoint &globalPos)
{
    if (!page)
        return;
    QHeaderView *header = page->view()->horizontalHeader();
    QMenu menu(this);
    for (int col = 0; col < PlaylistModel::ColumnCount; ++col) {
        if (col == PlaylistModel::ColTitle)
            continue;
        QAction *a = menu.addAction(page->model()->headerData(col, Qt::Horizontal, Qt::DisplayRole).toString());
        a->setCheckable(true);
        a->setChecked(!header->isSectionHidden(col));
        connect(a, &QAction::toggled, this, [this, col](bool on) {
            m_localPage->view()->horizontalHeader()->setSectionHidden(col, !on);
            for (PlaylistPage *p : m_panes->playlists())
                p->view()->horizontalHeader()->setSectionHidden(col, !on);
        });
    }
    menu.addSeparator();
    menu.addAction(QStringLiteral("Ширина колонок по содержимому"), this, [this] {
        PlaylistPage::resetColumnWidths();
        // The lists on screen now; the others when they are shown.
        for (PlaylistPage *p : {m_localPage, m_panes->shownPlaylist()}) {
            if (p)
                p->fitColumns();
        }
    });
    menu.exec(globalPos);
}

// --- Files -------------------------------------------------------------------

void MainWindow::addFiles()
{
    QSettings s;
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Добавить файлы"),
                                                            s.value(QStringLiteral("lastDir"), QDir::homePath()).toString(),
                                                            audioFileFilter());
    if (files.isEmpty())
        return;
    s.setValue(QStringLiteral("lastDir"), QFileInfo(files.first()).absolutePath());
    PlaylistPage *page = editablePage();
    bringUp(page);
    page->addPaths(files);
}

void MainWindow::addFolder()
{
    QSettings s;
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Добавить папку"),
                                                          s.value(QStringLiteral("lastDir"), QDir::homePath()).toString());
    if (dir.isEmpty())
        return;
    s.setValue(QStringLiteral("lastDir"), dir);
    PlaylistPage *page = editablePage();
    bringUp(page);
    page->addPaths({dir});
}

void MainWindow::openPlaylists()
{
    QSettings s;
    QStringList patterns;
    for (const QString &ext : PlaylistIO::readableFormats())
        patterns << QStringLiteral("*.") + ext;
    QString startDir = s.value(QStringLiteral("lastPlaylistDir")).toString();
    if (startDir.isEmpty())
        startDir = aimpPlaylistDir();
    if (startDir.isEmpty())
        startDir = QDir::homePath();
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Открыть плейлисты (можно выбрать несколько)"), startDir,
        QStringLiteral("Плейлисты (%1)").arg(patterns.join(QLatin1Char(' '))));
    if (files.isEmpty())
        return;
    s.setValue(QStringLiteral("lastPlaylistDir"), QFileInfo(files.first()).absolutePath());

    QStringList errors;
    int total = 0, missing = 0;
    PlaylistPage *first = nullptr;
    for (const QString &file : files) {
        PlaylistData data;
        QString error;
        if (!PlaylistIO::load(file, data, &error)) {
            errors << QStringLiteral("%1: %2").arg(QFileInfo(file).fileName(), error);
            if (data.tracks.isEmpty())
                continue;   // a damaged file: open what could be read
        }
        // Reuse the playlist shown if it is empty, as AIMP does with a fresh playlist.
        PlaylistPage *page = m_panes->shownPlaylist();
        if (first || !page || page->model()->rowCount() > 0)
            page = addPage(data.name);
        page->model()->setName(data.name);
        page->model()->insertTracks(-1, data.tracks);
        if (!first)
            first = page;
        total += int(data.tracks.size());
        missing += page->model()->missingCount();
    }
    m_panes->refreshNames();
    bringUp(first);   // each of them can be picked in the header
    if (!errors.isEmpty())
        QMessageBox::warning(this, windowTitle(), errors.join(QLatin1Char('\n')));
    statusBar()->showMessage(QStringLiteral("Открыто плейлистов: %1, треков: %2").arg(files.size() - errors.size()).arg(total), 5000);
    if (missing > 0)
        offerToFindMoved(missing, total);
}

void MainWindow::offerToFindMoved(int missing, int total)
{
    QMessageBox box(QMessageBox::Question, windowTitle(),
                    QStringLiteral("Не найдено %1 из %2 файлов.").arg(missing).arg(total), QMessageBox::NoButton, this);
    box.setInformativeText(QStringLiteral(
        "Если музыка была перемещена, укажите папку, где она сейчас лежит: плеер найдёт файлы по имени "
        "и запомнит новое место для следующих плейлистов."));
    QPushButton *find = box.addButton(QStringLiteral("Указать папку…"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Позже"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == find)
        findMovedFiles();
}

void MainWindow::findMovedFiles()
{
    QSettings s;
    QString start = s.value(QStringLiteral("paths/lastSearchDir")).toString();
    if (start.isEmpty())
        start = QFileInfo::exists(QStringLiteral("/media")) ? QStringLiteral("/media") : QDir::homePath();
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Где теперь лежит музыка?"), start);
    if (folder.isEmpty())
        return;
    s.setValue(QStringLiteral("paths/lastSearchDir"), folder);
    statusBar()->showMessage(QStringLiteral("Поиск музыки в %1…").arg(folder));

    auto *watcher = new QFutureWatcher<Relocator::Index>(this);
    connect(watcher, &QFutureWatcher<Relocator::Index>::finished, this, [this, watcher, folder] {
        const Relocator::Index index = watcher->result();
        watcher->deleteLater();

        int missing = 0;
        QList<QPair<QString, QString>> moves;
        // Local files follow the folders by themselves.
        for (PlaylistPage *page : m_panes->playlists()) {
            QList<QPair<int, QString>> pageMoves;
            const TrackList &tracks = page->model()->tracks();
            for (int row = 0; row < tracks.size(); ++row) {
                if (!tracks[row].missing || tracks[row].isUrl())
                    continue;
                ++missing;
                const QString found = Relocator::find(tracks[row], index);
                if (!found.isEmpty()) {
                    pageMoves.append({row, found});
                    moves.append({tracks[row].path, found});
                }
            }
            page->model()->relocate(pageMoves);
        }

        rememberMoves(moves);

        statusBar()->clearMessage();
        updateStats();
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("В «%1» (%2 аудиофайлов) найдено %3 из %4 отсутствующих треков.")
                                     .arg(folder)
                                     .arg(index.files)
                                     .arg(moves.size())
                                     .arg(missing));
    });
    watcher->setFuture(QtConcurrent::run([folder] { return Relocator::build(folder); }));
}

// Remember folder moves so that playlists opened later find their files.
void MainWindow::rememberMoves(const QList<QPair<QString, QString>> &moves)
{
    if (moves.isEmpty())
        return;
    QList<PathResolver::Remap> remaps = Relocator::deriveRemaps(moves);
    for (const auto &old : PathResolver::remaps()) {
        const bool known = std::any_of(remaps.cbegin(), remaps.cend(),
                                       [&](const PathResolver::Remap &r) { return r.first == old.first; });
        if (!known)
            remaps.append(old);
    }
    PathResolver::setRemaps(remaps);
    saveRemaps(remaps);
}

void MainWindow::importFromAimp()
{
    MigrationDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const MigrationResult r = dlg.result();

    PlaylistPage *first = nullptr;
    PlaylistPage *active = nullptr;
    for (int i = 0; i < r.playlists.size(); ++i) {
        const PlaylistData &data = r.playlists[i];
        PlaylistPage *page = nullptr;
        if (r.replaceSameName) {
            for (PlaylistPage *p : m_panes->playlists()) {
                if (p->model()->name() == data.name) {
                    page = p;
                    break;
                }
            }
        }
        if (PlaylistPage *shown = m_panes->shownPlaylist(); !page && i == 0 && shown && shown->model()->rowCount() == 0)
            page = shown;   // a fresh empty playlist
        if (page && data.tracks.isEmpty() && page->model()->rowCount() > 0)
            continue;   // nothing read from it: never empty a full playlist
        if (!page)
            page = addPage(data.name);
        page->model()->clear();
        page->model()->setName(data.name);
        page->model()->insertTracks(-1, data.tracks);

        if (!first)
            first = page;
        if (i == r.activeIndex)
            active = page;
        // Continue where AIMP was, without starting playback.
        if (i == r.playingIndex && r.playingRow >= 0 && r.playingRow < page->model()->rowCount()
            && m_engine->state() == AudioEngine::State::Stopped) {
            m_playback->setCurrent(page->model(), r.playingRow);
            page->scrollToSourceRow(r.playingRow);
        }
    }
    m_panes->refreshNames();
    bringUp(active ? active : first);
    rememberMoves(r.moves);
    updateNowPlaying();   // a replaced playlist may have held the current track
    updateStats();
    QMessageBox::information(this, windowTitle(), QStringLiteral("Импорт завершён.\n\n") + r.summary);
}

void MainWindow::deleteSelectedFiles(PlaylistPage *page)
{
    if (!page)
        page = activePage();
    if (!page)
        return;
    QStringList paths;
    int cueTracks = 0;
    int absent = 0;   // no file there (a drive not mounted?): nothing to delete, keep the rows
    QSet<QString> seen;
    for (int row : page->selectedSourceRows()) {
        const Track &t = page->model()->track(row);
        if (t.isUrl())
            continue;
        if (t.isCueTrack()) {
            ++cueTracks;   // its file is the whole album
            continue;
        }
        if (seen.contains(t.path))
            continue;
        seen.insert(t.path);
        if (!QFileInfo::exists(t.path)) {
            ++absent;
            continue;
        }
        paths << t.path;
    }
    if (paths.isEmpty()) {
        statusBar()->showMessage(
            absent ? QStringLiteral("Файлов выбранных треков нет на диске (диск отключён или они уже удалены): ничего не удалено.")
            : cueTracks ? QStringLiteral("Треки из CUE не удаляются с диска: их файл содержит весь альбом.")
                        : QStringLiteral("Выберите треки, файлы которых нужно удалить."),
            6000);
        return;
    }

    DeleteFilesDialog confirm(DeleteFilesDialog::Kind::ToTrash, paths, cueTracks, this);
    if (confirm.exec() != QDialog::Accepted)
        return;
    const bool fromAll = confirm.fromAllPlaylists();

    FileTrash::Result r = FileTrash::moveToTrash(paths);
    if (!r.failed.isEmpty()) {
        DeleteFilesDialog ask(DeleteFilesDialog::Kind::Forever, r.failed, 0, this);
        if (ask.exec() == QDialog::Accepted) {
            const FileTrash::Result gone = FileTrash::removeForever(r.failed);
            r.done += gone.done;
            r.failed = gone.failed;
            if (!r.failed.isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("Удалить с диска"),
                                     QStringLiteral("Не удалось удалить:\n\n") + r.failed.join(QLatin1Char('\n')));
            }
        }
    }
    forgetDeletedFiles(page, r.done, fromAll);
    if (absent && !r.done.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Удалено с диска файлов: %1. Не найдено на диске и пропущено: %2.")
                                     .arg(r.done.size()).arg(absent),
                                 6000);
    }
}

// The files are gone from this playlist (and from all, if asked; elsewhere
// they are marked missing). If the playing track was one of them, the player
// moves on at once to the next track that was not deleted.
void MainWindow::forgetDeletedFiles(PlaylistPage *from, const QStringList &paths, bool fromAllPlaylists)
{
    if (paths.isEmpty())
        return;
    const QSet<QString> gone(paths.cbegin(), paths.cend());
    const Track *playing = m_playback->currentTrack();
    const bool playingGone = playing && gone.contains(playing->path);

    for (PlaylistPage *page : m_panes->playlists()) {   // Local files: rescanned below
        QList<int> rows;
        const TrackList &tracks = page->model()->tracks();
        for (int row = 0; row < tracks.size(); ++row) {
            if (gone.contains(tracks[row].path))
                rows << row;
        }
        if (page == from || fromAllPlaylists) {
            page->model()->removeRowList(rows);
        } else {
            for (int row : std::as_const(rows))
                page->model()->setMissing(row, true);
        }
    }

    if (playingGone)
        m_playback->skipDeleted(gone);

    rescanLocal();   // at once, not after the watcher's delay
    updateStats();
    statusBar()->showMessage(QStringLiteral("Удалено с диска файлов: %1").arg(paths.size()), 5000);
}

void MainWindow::savePlaylist()
{
    auto *page = activePage();
    if (!page)
        return;
    QSettings s;
    const QString filters = QStringLiteral("M3U8 (*.m3u8);;M3U (*.m3u);;PLS (*.pls);;XSPF (*.xspf);;AIMP (*.aimppl4)");
    QString selected;
    const QString base = s.value(QStringLiteral("lastDir"), QDir::homePath()).toString() + QLatin1Char('/')
                         + page->model()->name() + QStringLiteral(".m3u8");
    QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Сохранить плейлист"), base, filters, &selected);
    if (file.isEmpty())
        return;
    if (!PlaylistIO::writableFormats().contains(QFileInfo(file).suffix().toLower())) {
        const QString ext = selected.section(QLatin1String("*."), 1).section(QLatin1Char(')'), 0, 0);
        file += QLatin1Char('.') + (ext.isEmpty() ? QStringLiteral("m3u8") : ext);
    }
    QString error;
    if (!PlaylistIO::save(file, {page->model()->name(), page->model()->tracks()}, &error))
        QMessageBox::warning(this, windowTitle(), error);
    else
        statusBar()->showMessage(QStringLiteral("Сохранено: %1").arg(file), 4000);
}

// --- Session -----------------------------------------------------------------

void MainWindow::scheduleSave()
{
    if (m_autosave)
        m_autosave->start();
}

void MainWindow::saveSession(SaveScope scope)
{
    if (m_autosave)
        m_autosave->stop();
    // Not loaded yet: an empty set written now would remove every playlist
    // file (SessionFiles::save() drops files it did not write), and the
    // Local files cache would be emptied.
    if (!m_sessionLoaded)
        return;
    // Files are written for the user's playlists only (Local files are
    // rebuilt from the folders, with their own cache).
    QList<PlaylistData> playlists;
    for (const PlaylistPage *page : m_panes->playlists())
        playlists.append({page->model()->name(), page->model()->tracks()});
    PlaylistModel *playingModel = m_playback->playingModel();
    const bool playingLocal = playingModel && playingModel == m_localPage->model();
    const int playingPlaylist = m_panes->indexOf(pageOf(playingModel));
    const int files = int(playlists.size());
    const auto reportFailed = [this](const QStringList &failed) {
        if (!failed.isEmpty())
            statusBar()->showMessage(QStringLiteral("Не удалось сохранить плейлисты: ") + failed.join(QStringLiteral("; ")), 10000);
    };
    if (scope == SaveScope::Playlists) {
        // Autosave: written in the background (100k tracks of XSPF take a
        // second or two); the lists are shared copies, so nothing is
        // copied here. One save at a time, in order.
        auto *watcher = new QFutureWatcher<QStringList>(this);
        connect(watcher, &QFutureWatcher<QStringList>::finished, this, [watcher, reportFailed] {
            watcher->deleteLater();
            reportFailed(watcher->result());
        });
        m_saveJob = QtConcurrent::run([previous = m_saveJob, dir = playlistDir(), playlists]() mutable {
            previous.waitForFinished();
            return SessionFiles::save(dir, playlists);
        });
        watcher->setFuture(m_saveJob);
    } else {
        // Quitting or logging out: done before going on.
        m_saveJob.waitForFinished();
        reportFailed(SessionFiles::save(playlistDir(), playlists));
        m_localFiles->saveCache(m_localPage->model()->tracks());
    }

    // Version 3: places are named by list (Local files, or user playlist N
    // in the order of the files), not by tab position as in versions 1-2.
    QSettings s;
    s.setValue(QStringLiteral("session/version"), 3);
    s.setValue(QStringLiteral("session/count"), files);
    s.setValue(QStringLiteral("session/shown"), m_panes->indexOf(m_panes->shownPlaylist()));
    s.setValue(QStringLiteral("session/active"), m_panes->activePane() == PlaylistPanes::Pane::Local
                                                     ? QStringLiteral("local")
                                                     : QStringLiteral("playlist"));
    s.setValue(QStringLiteral("session/playingList"), playingLocal           ? QStringLiteral("local")
                                                      : playingPlaylist >= 0 ? QStringLiteral("playlist")
                                                                             : QString());
    s.setValue(QStringLiteral("session/playingPlaylist"), playingPlaylist);
    s.setValue(QStringLiteral("session/playingRow"), playingModel ? playingModel->currentRow() : -1);
    for (const char *old : {"session/localTab", "session/current", "session/playingTab"})
        s.remove(QLatin1String(old));   // tab positions of version 2
    // Rows of Local files change with the folders: remember the file.
    const Track *playing = m_playback->currentTrack();
    s.setValue(QStringLiteral("session/playingPath"), playing ? playing->path : QString());
    s.setValue(QStringLiteral("volume"), m_playback->volume());
    s.setValue(QStringLiteral("searchField"), m_searchField->currentIndex());
    s.setValue(QStringLiteral("geometry"), saveGeometry());
    s.setValue(QStringLiteral("splitter"), m_splitter->saveState());
    s.setValue(QStringLiteral("panes"), m_panes->splitter()->saveState());
    if (auto *page = activePage())
        s.setValue(QStringLiteral("header"), page->view()->horizontalHeader()->saveState());
    s.setValue(QStringLiteral("headerColumns"), int(PlaylistModel::ColumnCount));
}

void MainWindow::restoreSession()
{
    QSettings s;
    restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
    m_splitter->restoreState(s.value(QStringLiteral("splitter")).toByteArray());
    m_panes->splitter()->restoreState(s.value(QStringLiteral("panes")).toByteArray());
    m_playback->setVolume(s.value(QStringLiteral("volume"), 70).toInt());
    m_mpris->setVolume(m_playback->volume() / 100.0);
    m_searchField->setCurrentIndex(s.value(QStringLiteral("searchField"), 0).toInt());
    m_headerState = s.value(QStringLiteral("header")).toByteArray();
    m_headerColumns = s.value(QStringLiteral("headerColumns"), 10).toInt();   // 10: before the date columns
    m_localSortColumn = s.value(QStringLiteral("localFiles/sortColumn"), -1).toInt();
    m_localSortOrder = s.value(QStringLiteral("localFiles/sortOrder"), 0).toInt() == 1 ? Qt::DescendingOrder
                                                                                       : Qt::AscendingOrder;

    createLocalPage();
    syncLocalSort();

    // The playlists and the Local files cache are read on a worker thread:
    // big lists take a second or more, a drive that does not answer any time
    // at all, and the window should be there at once. Meanwhile the
    // playlists cannot be used; files and commands that come are kept.
    for (const QPointer<QAction> &a : std::as_const(m_needSession))
        a->setEnabled(false);
    for (QWidget *w : {static_cast<QWidget *>(m_panes), static_cast<QWidget *>(m_search),
                       static_cast<QWidget *>(m_searchField)})
        w->setEnabled(false);
    statusBar()->showMessage(QStringLiteral("Загрузка плейлистов…"));

    std::function<SessionSnapshot()> load = m_options.loadSession;
    if (!load) {
        load = [dir = playlistDir(), cache = LocalFiles::cacheFile()] {
            SessionSnapshot out;
            out.localFiles = LocalFiles::loadCache(cache);
            SessionFiles::Loaded loaded = SessionFiles::load(dir);
            out.playlists = std::move(loaded.playlists);
            out.keptAside = std::move(loaded.keptAside);
            return out;
        };
    }
    // Owned by the window: closed before the files are in, the result is
    // dropped with it (the job has its own copies; see main() for the end).
    auto *watcher = new QFutureWatcher<SessionSnapshot>(this);
    connect(watcher, &QFutureWatcher<SessionSnapshot>::finished, this, [this, watcher] {
        watcher->deleteLater();
        applySession(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run(std::move(load)));
}

void MainWindow::applySession(const SessionSnapshot &session)
{
    QSettings s;
    m_localPage->model()->setTracks(session.localFiles);   // saved in its order
    for (int i = 0; i < session.playlists.size(); ++i) {
        const PlaylistData &data = session.playlists[i];
        PlaylistPage *page = addPage(data.name.isEmpty() ? QStringLiteral("Плейлист %1").arg(i + 1) : data.name);
        page->model()->insertTracks(-1, data.tracks);
    }
    if (const QStringList unreadable = session.keptAside; !unreadable.isEmpty()) {
        QTimer::singleShot(0, this, [this, unreadable] {
            QMessageBox::warning(this, windowTitle(),
                                 QStringLiteral("Не удалось прочитать плейлисты прошлого сеанса полностью. Копии файлов сохранены:\n\n")
                                     + unreadable.join(QLatin1Char('\n')));
        });
    }
    // No playlists is allowed: the right pane then stays free until one is
    // made (adding files makes one).

    // Where the user was: the playlist shown, the active list, the track
    // played last. Versions 1 and 2 saved tab positions: version 2 counts
    // the Local files tab (at session/localTab) among them; version 1 predates
    // it, so its indexes are those of the playlists.
    const int version = s.value(QStringLiteral("session/version"), 1).toInt();
    int shown = 0;
    bool localActive = false;
    bool playingLocal = false;
    int playingPlaylist = -1;
    int playingRow = s.value(QStringLiteral("session/playingRow"), -1).toInt();
    if (version >= 3) {
        shown = s.value(QStringLiteral("session/shown"), 0).toInt();
        localActive = s.value(QStringLiteral("session/active")).toString() == QLatin1String("local");
        const QString list = s.value(QStringLiteral("session/playingList")).toString();
        playingLocal = list == QLatin1String("local");
        if (list == QLatin1String("playlist"))
            playingPlaylist = s.value(QStringLiteral("session/playingPlaylist"), -1).toInt();
    } else {
        const int localTab = version == 2 ? s.value(QStringLiteral("session/localTab"), 0).toInt() : -1;
        // A tab position to a playlist index; -1 with *local for Local files.
        const auto fromTab = [localTab](int tab, bool *local) {
            *local = localTab >= 0 && tab == localTab;
            if (*local || tab < 0)
                return -1;
            return localTab >= 0 && tab > localTab ? tab - 1 : tab;
        };
        const int current = fromTab(s.value(QStringLiteral("session/current"), 0).toInt(), &localActive);
        playingPlaylist = fromTab(s.value(QStringLiteral("session/playingTab"), -1).toInt(), &playingLocal);
        // Local files were in front: the playlist that played goes beside them.
        shown = localActive ? qMax(0, playingPlaylist) : current;
    }

    const QList<PlaylistPage *> &pages = m_panes->playlists();
    if (!pages.isEmpty())
        m_panes->showPlaylist(pages.at(qBound(0, shown, int(pages.size()) - 1)));

    // Mark the last played track so that Play continues from it.
    PlaylistPage *playing = playingLocal ? m_localPage : pages.value(playingPlaylist, nullptr);
    if (playing && playing == m_localPage) {
        const QString path = s.value(QStringLiteral("session/playingPath")).toString();
        const TrackList &tracks = playing->model()->tracks();
        playingRow = -1;
        for (int i = 0; i < tracks.size() && !path.isEmpty(); ++i) {
            if (tracks[i].path == path) {
                playingRow = i;
                break;
            }
        }
    }
    if (auto *page = playing; page && playingRow >= 0 && playingRow < page->model()->rowCount()) {
        m_playback->setCurrent(page->model(), playingRow);
        page->scrollToSourceRow(playingRow);
    }

    m_sessionLoaded = true;
    for (const QPointer<QAction> &a : std::as_const(m_needSession)) {
        if (a)
            a->setEnabled(true);
    }
    for (QWidget *w : {static_cast<QWidget *>(m_panes), static_cast<QWidget *>(m_search),
                       static_cast<QWidget *>(m_searchField)})
        w->setEnabled(true);
    statusBar()->clearMessage();
    m_panes->activate(localActive ? PlaylistPanes::Pane::Local : PlaylistPanes::Pane::Playlist);
    updateNowPlaying();
    updateStats();
    // Bring Local files up to date with the folders (the cache was shown
    // meanwhile); only now, with the cache in: from an empty list every
    // track would get a new uid and its tags read again.
    rescanLocal();

    const QList<QStringList> open = std::exchange(m_pendingOpen, {});
    for (const QStringList &paths : open)
        openPaths(paths);
    const QStringList commands = std::exchange(m_pendingCommands, {});
    for (const QString &command : commands)
        handleCommand(command);
    emit sessionLoaded();
}

void MainWindow::rescanLocal()
{
    if (m_sessionLoaded)   // before: applySession() starts it, from the cache
        m_localFiles->rescan(m_localPage->model()->tracks());
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (m_tray && m_settings.closeToTray && !m_quitting) {
        hide();
        e->ignore();
        return;
    }
    saveSession();
    m_finalSaveDone = true;
    m_mpris->unregisterFromBus();   // saved: a new start may take over now
    m_engine->unload();
    releaseHotkeys();
    if (!m_artFile.isEmpty())
        QFile::remove(m_artFile);
    e->accept();
    QApplication::quit();
}
