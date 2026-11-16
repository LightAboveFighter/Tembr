#include "ui/MainWindow.h"

#include "core/FileTrash.h"
#include "core/LocalFiles.h"
#include "core/MediaCollector.h"
#include "core/MetadataReader.h"
#include "core/PathResolver.h"
#include "core/PlaylistIO.h"
#include "core/Relocator.h"
#include "platform/Mpris.h"
#include "ui/DeleteFilesDialog.h"
#include "ui/Icons.h"
#include "ui/LocalFilesDialog.h"
#include "ui/MigrationDialog.h"
#include "ui/NowPlayingPanel.h"
#include "ui/PlaylistFilterProxy.h"
#include "ui/PlaylistModel.h"
#include "ui/PlaylistPage.h"
#include "ui/Theme.h"
#include "ui/TrackOsd.h"
#include "ui/TrackInfoDialog.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDateTime>
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
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_engine(new AudioEngine(this))
    , m_mpris(new MprisService(m_engine, this))
    , m_localFiles(new LocalFiles(this))
    , m_osd(new TrackOsd)
{
    setWindowTitle(QStringLiteral("Tembr"));
    PathResolver::setRemaps(loadRemaps());

    buildActions();
    buildUi();

    connect(m_engine, &AudioEngine::stateChanged, this, [this](AudioEngine::State s) {
        refreshIcons();
        m_actPlay->setText(s == AudioEngine::State::Playing ? QStringLiteral("Пауза") : QStringLiteral("Воспроизвести"));
        if (m_playingPage)
            m_playingPage->model()->setPaused(s == AudioEngine::State::Paused);
    });
    connect(m_engine, &AudioEngine::positionChanged, this, [this](qint64 ms) {
        if (!m_seek->isSliderDown()) {
            m_seek->setValue(int(ms));
            m_position->setText(formatDuration(ms + 1));
        }
    });
    connect(m_engine, &AudioEngine::durationChanged, this, [this](qint64 ms) {
        m_seek->setRange(0, int(ms));
        m_seek->setEnabled(m_engine->isSeekable());
        m_duration->setText(formatDuration(ms + 1));
    });
    connect(m_engine, &AudioEngine::finished, this, &MainWindow::playNext);
    connect(m_engine, &AudioEngine::loaded, this, &MainWindow::trackLoaded);
    connect(m_engine, &AudioEngine::loadFailed, this, &MainWindow::trackLoadFailed);
    connect(m_engine, &AudioEngine::errorOccurred, this,
            [this](const QString &msg) { statusBar()->showMessage(msg, 5000); });

    // MPRIS: GNOME notification list, media keys, `tembr --next`, second instances.
    connect(m_mpris, &MprisService::commandRequested, this, &MainWindow::handleCommand);
    connect(m_mpris, &MprisService::raiseRequested, this, &MainWindow::showAndRaise);
    connect(m_mpris, &MprisService::quitRequested, this, &MainWindow::quit);
    connect(m_mpris, &MprisService::seekRequested, m_engine, &AudioEngine::seek);
    connect(m_mpris, &MprisService::volumeRequested, this, [this](double v) { m_volume->setValue(qRound(v * 100)); });
    connect(m_mpris, &MprisService::openRequested, this, [this](const QStringList &paths) {
        showAndRaise();
        openPaths(paths);
    });
    if (!m_mpris->registerOnBus())
        qWarning("MPRIS: could not register on the session bus");

    connect(m_localFiles, &LocalFiles::scanned, this, &MainWindow::applyLocalScan);
    connect(m_localFiles, &LocalFiles::scanningChanged, this, &MainWindow::updateStats);
    connect(m_localFiles, &LocalFiles::changedOnDisk, this,
            [this] { m_localFiles->rescan(m_localPage->model()->tracks()); });

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

    connect(m_actPrev, &QAction::triggered, this, &MainWindow::playPrevious);
    connect(m_actPlay, &QAction::triggered, this, &MainWindow::playOrResume);
    connect(m_actStop, &QAction::triggered, m_engine, &AudioEngine::stop);
    connect(m_actNext, &QAction::triggered, this, &MainWindow::playNext);
    connect(m_actShow, &QAction::triggered, this, &MainWindow::toggleVisible);
    connect(m_actQuit, &QAction::triggered, this, &MainWindow::quit);

    // --- File
    QMenu *file = menuBar()->addMenu(QStringLiteral("&Файл"));
    addItem(file, QStringLiteral("Добавить файлы…"), QKeySequence::Open, this, &MainWindow::addFiles);
    addItem(file, QStringLiteral("Добавить папку…"), QKeySequence(QStringLiteral("Ctrl+Shift+O")), this,
            &MainWindow::addFolder);
    file->addSeparator();
    addItem(file, QStringLiteral("Новый плейлист"), QKeySequence::New, this,
            [this] { m_tabs->setCurrentWidget(addPage(QStringLiteral("Плейлист %1").arg(m_tabs->count() + 1))); });
    addItem(file, QStringLiteral("Открыть плейлисты…"), QKeySequence(QStringLiteral("Ctrl+L")), this,
            &MainWindow::openPlaylists);
    addItem(file, QStringLiteral("Импорт из AIMP (компьютер, Android)…"), QKeySequence(QStringLiteral("Ctrl+Shift+I")),
            this, &MainWindow::importFromAimp);
    addItem(file, QStringLiteral("Сохранить плейлист как…"), QKeySequence::Save, this, &MainWindow::savePlaylist);
    addItem(file, QStringLiteral("Переименовать плейлист…"), QKeySequence(Qt::Key_F2), this,
            [this] { renamePage(m_tabs->currentIndex()); });
    addItem(file, QStringLiteral("Закрыть плейлист"), QKeySequence::Close, this,
            [this] { closePage(m_tabs->currentIndex()); });
    file->addSeparator();
    addItem(file, QStringLiteral("Настройки…"), QKeySequence(QStringLiteral("Ctrl+,")), this, &MainWindow::showSettings);
    file->addSeparator();
    file->addAction(m_actQuit);

    // --- Playlist
    QMenu *pl = menuBar()->addMenu(QStringLiteral("&Плейлист"));
    addItem(pl, QStringLiteral("Информация о треке…"), QKeySequence(QStringLiteral("Ctrl+I")), this,
            &MainWindow::showTrackInfo);
    addItem(pl, QStringLiteral("Найти…"), QKeySequence::Find, this, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
    pl->addSeparator();
    pl->addAction(QStringLiteral("Найти перемещённые файлы…"), this, &MainWindow::findMovedFiles);
    pl->addAction(QStringLiteral("Удалить выбранные"), this, [this] {
        if (auto *p = currentPage(); p && !refuseReadOnly(p))
            p->removeSelected();
    });
    pl->addAction(QStringLiteral("Удалить с диска…"), this, &MainWindow::deleteSelectedFiles);
    pl->addAction(QStringLiteral("Удалить отсутствующие файлы"), this, [this] {
        if (auto *p = currentPage(); p && !refuseReadOnly(p))
            p->model()->removeMissing();
    });
    pl->addAction(QStringLiteral("Очистить плейлист"), this, [this] {
        if (auto *p = currentPage(); p && !refuseReadOnly(p))
            p->model()->clear();
    });
    pl->addSeparator();
    addItem(pl, QStringLiteral("Обновить локальные файлы"), QKeySequence(Qt::Key_F5), this,
            [this] { m_localFiles->rescan(m_localPage->model()->tracks()); });
    pl->addAction(QStringLiteral("Настройки локальных файлов…"), this, &MainWindow::showLocalFilesSettings);

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
    addItem(pb, QStringLiteral("Громче"), QKeySequence(QStringLiteral("Ctrl+Up")), this, [this] { changeVolume(5); });
    addItem(pb, QStringLiteral("Тише"), QKeySequence(QStringLiteral("Ctrl+Down")), this, [this] { changeVolume(-5); });
}

void MainWindow::buildUi()
{
    // --- Left: big cover + track info
    m_nowPlaying = new NowPlayingPanel;
    m_nowPlaying->setMinimumWidth(200);

    // --- Right: search + playlist tabs
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
        // Enter in the search box plays the first match.
        if (auto *p = currentPage(); p && p->proxy()->rowCount() > 0)
            playRow(p, p->proxy()->mapToSource(p->proxy()->index(0, 0)).row());
    });

    m_tabs = new QTabWidget;
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setExpanding(false);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closePage);
    connect(m_tabs->tabBar(), &QTabBar::tabMoved, this, &MainWindow::scheduleSave);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        applySearch();
        updateStats();
    });
    connect(m_tabs->tabBar(), &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index < 0)
            m_tabs->setCurrentWidget(addPage(QStringLiteral("Плейлист %1").arg(m_tabs->count() + 1)));
        else
            renamePage(index);
    });
    rightLayout->addWidget(m_tabs, 1);

    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->addWidget(m_nowPlaying);
    m_splitter->addWidget(right);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(1, false);
    m_splitter->setSizes({300, 900});

    // --- Bottom: transport bar
    auto *bar = new QWidget;
    bar->setObjectName(QStringLiteral("TransportBar"));
    bar->setAttribute(Qt::WA_StyledBackground);
    auto *controls = new QHBoxLayout(bar);
    controls->setContentsMargins(12, 8, 14, 8);
    controls->setSpacing(6);
    auto makeButton = [&](QAction *a, int iconSize) {
        auto *b = new QToolButton;
        b->setDefaultAction(a);
        b->setAutoRaise(true);
        b->setIconSize(QSize(iconSize, iconSize));
        b->setFocusPolicy(Qt::NoFocus);
        controls->addWidget(b);
        return b;
    };
    makeButton(m_actPrev, 22);
    m_playButton = makeButton(m_actPlay, 42);
    makeButton(m_actNext, 22);
    makeButton(m_actStop, 20);
    controls->addSpacing(10);

    m_position = new QLabel(QStringLiteral("0:00"));
    m_duration = new QLabel(QStringLiteral("0:00"));
    for (QLabel *l : {m_position, m_duration}) {
        l->setObjectName(QStringLiteral("TimeLabel"));
        l->setMinimumWidth(l->fontMetrics().horizontalAdvance(QStringLiteral("00:00:00")));
    }
    m_position->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_seek = new QSlider(Qt::Horizontal);
    m_seek->setEnabled(false);
    m_seek->setFocusPolicy(Qt::NoFocus);
    controls->addWidget(m_position);
    controls->addWidget(m_seek, 1);
    controls->addWidget(m_duration);
    controls->addSpacing(10);

    m_volumeIcon = new QLabel;
    m_volume = new QSlider(Qt::Horizontal);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(120);
    m_volume->setFocusPolicy(Qt::NoFocus);
    controls->addWidget(m_volumeIcon);
    controls->addWidget(m_volume);

    connect(m_seek, &QSlider::sliderReleased, this, [this] { m_engine->seek(m_seek->value()); });
    connect(m_seek, &QSlider::sliderMoved, this, [this](int v) { m_position->setText(formatDuration(v + 1)); });
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        m_engine->setVolume(v / 100.0f);
        m_mpris->setVolume(v / 100.0);
        m_volume->setToolTip(QStringLiteral("Громкость: %1%").arg(v));
    });

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(m_splitter, 1);
    root->addWidget(bar);
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
    const QColor c = Theme::iconColor();
    const bool playing = m_engine->state() == AudioEngine::State::Playing;
    m_actPrev->setIcon(Icons::make(Icons::Kind::Previous, c));
    m_actNext->setIcon(Icons::make(Icons::Kind::Next, c));
    m_actStop->setIcon(Icons::make(Icons::Kind::Stop, c));
    m_actPlay->setIcon(Icons::accentButton(playing ? Icons::Kind::Pause : Icons::Kind::Play));
    m_volumeIcon->setPixmap(Icons::make(Icons::Kind::Volume, c).pixmap(18, 18));
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
    setupTray(s.trayIcon);
}

void MainWindow::setupTray(bool enabled)
{
    if (!enabled || !QSystemTrayIcon::isSystemTrayAvailable()) {
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
                playOrResume();
        });
    }
    m_tray->show();
    updateNowPlaying();
}

void MainWindow::registerHotkeys()
{
    // gsettings takes a while; don't hold up the window.
    if (GlobalHotkeys::desktopSupported())
        m_hotkeyJob = QtConcurrent::run(&GlobalHotkeys::applyToDesktop, m_settings.hotkeys);
}

void MainWindow::releaseHotkeys()
{
    if (m_hotkeysReleased)
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
    if (keysChanged) {
        m_hotkeyJob.waitForFinished();
        const QString err = GlobalHotkeys::applyToDesktop(m_settings.hotkeys);
        if (!err.isEmpty())
            QMessageBox::warning(this, windowTitle(), QStringLiteral("Не все клавиши удалось назначить:\n") + err);
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
    if (command == QLatin1String("play-pause"))
        playOrResume();
    else if (command == QLatin1String("play")) {
        if (m_engine->state() != AudioEngine::State::Playing)
            playOrResume();
    } else if (command == QLatin1String("pause"))
        m_engine->pause();
    else if (command == QLatin1String("stop"))
        m_engine->stop();
    else if (command == QLatin1String("next"))
        playNext();
    else if (command == QLatin1String("previous"))
        playPrevious();
    else if (command == QLatin1String("volume-up"))
        changeVolume(5);
    else if (command == QLatin1String("volume-down"))
        changeVolume(-5);
    else if (command == QLatin1String("show"))
        showAndRaise();
}

// --- Pages -------------------------------------------------------------------

PlaylistPage *MainWindow::currentPage() const
{
    return qobject_cast<PlaylistPage *>(m_tabs->currentWidget());
}

PlaylistPage *MainWindow::addPage(const QString &name)
{
    if (auto *cur = currentPage()) {
        m_headerState = cur->view()->horizontalHeader()->saveState();
        m_headerColumns = PlaylistModel::ColumnCount;
    }

    auto *page = new PlaylistPage(name, m_tabs);
    if (!m_headerState.isEmpty() && page->view()->horizontalHeader()->restoreState(m_headerState)) {
        for (int col = m_headerColumns; col < PlaylistModel::ColumnCount; ++col)
            page->view()->horizontalHeader()->setSectionHidden(col, true);
    }
    page->view()->horizontalHeader()->setSortIndicatorShown(false);
    m_tabs->addTab(page, name);

    connect(page, &PlaylistPage::trackActivated, this, [this, page](int row) { playRow(page, row); });
    connect(page, &PlaylistPage::tracksAdded, this, [this, page](int row, bool playFirst) {
        if (playFirst)
            playRow(page, row);
    });
    // Ordinary playlists are saved soon after they change (also when sorted).
    const auto changed = [this, page] {
        if (page != m_localPage)
            scheduleSave();
    };
    connect(page->model(), &PlaylistModel::contentsChanged, this, changed);
    connect(page->model(), &QAbstractItemModel::layoutChanged, this, changed);
    connect(page, &PlaylistPage::statsChanged, this, [this, page] {
        if (page == currentPage())
            updateStats();
        if (page == m_playingPage)
            updateNavigation();
    });
    connect(page, &PlaylistPage::contextMenuRequested, this, &MainWindow::showContextMenu);
    connect(page->view()->horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this, page](const QPoint &p) { showHeaderMenu(page->view()->horizontalHeader()->mapToGlobal(p)); });
    connect(page->model(), &PlaylistModel::currentDetached, this, [this, page] {
        if (page != m_playingPage)
            return;
        if (m_engine->state() == AudioEngine::State::Stopped) {
            page->model()->setCurrent(0);   // nothing playing: nothing to go on from
            updateNowPlaying();
        } else {
            updateNavigation();   // plays on, shown as before; "next" is decided by the model
        }
    });
    connect(page->model(), &QAbstractItemModel::dataChanged, this,
            [this, page](const QModelIndex &tl, const QModelIndex &br) {
                const int cur = page->model()->currentRow();
                if (page == m_playingPage && cur >= tl.row() && cur <= br.row()) {
                    updateNowPlaying();
                    m_osd->refresh(page->model()->track(cur), m_engine->durationMs());   // tags just read
                }
            });
    return page;
}

void MainWindow::closePage(int index)
{
    auto *page = qobject_cast<PlaylistPage *>(m_tabs->widget(index));
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

    if (page == m_playingPage) {
        m_engine->unload();
        m_playingPage = nullptr;
        updateNowPlaying();
    }
    m_tabs->removeTab(index);
    scheduleSave();
    page->deleteLater();
}

void MainWindow::renamePage(int index)
{
    auto *page = qobject_cast<PlaylistPage *>(m_tabs->widget(index));
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
    m_tabs->setTabText(index, name);
    scheduleSave();
}

void MainWindow::openPaths(const QStringList &paths)
{
    PlaylistPage *page = editablePage();
    m_tabs->setCurrentWidget(page);
    page->addPaths(paths, -1, true);
}

PlaylistPage *MainWindow::editablePage()
{
    if (auto *cur = currentPage(); cur && !cur->isReadOnly())
        return cur;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *p = qobject_cast<PlaylistPage *>(m_tabs->widget(i));
        if (p && !p->isReadOnly())
            return p;
    }
    return addPage(QStringLiteral("Плейлист 1"));
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
    m_localPage = addPage(QStringLiteral("Локальные файлы"));
    m_localPage->setReadOnly(true);
    const int index = m_tabs->indexOf(m_localPage);
    m_tabs->setTabIcon(index, Icons::make(Icons::Kind::Note, Theme::iconColor()));
    m_tabs->setTabToolTip(index, QStringLiteral("Вся музыка из папок фонотеки. Изменить список нельзя — "
                                                "только папки в настройках локальных файлов."));
    // Not closable: drop the close button (its side depends on the style).
    m_tabs->tabBar()->setTabButton(index, QTabBar::RightSide, nullptr);
    m_tabs->tabBar()->setTabButton(index, QTabBar::LeftSide, nullptr);
    m_localPage->model()->setTracks(m_localFiles->loadCache());   // saved in its order
    // Sorting by a column header is remembered too, as in AIMP.
    connect(m_localPage->model(), &PlaylistModel::sorted, this, [this](int column, Qt::SortOrder order) {
        m_localSortColumn = column;
        m_localSortOrder = order;
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
    QSettings().setValue(QStringLiteral("localFiles/sortColumn"), -1);
    m_localFiles->rescan(m_localPage->model()->tracks());   // comes back in folder order
}

void MainWindow::applyLocalScan(const TrackList &found)
{
    PlaylistModel *model = m_localPage->model();
    TrackList tracks = found;   // in folder order
    TrackSort::Key key;
    bool byTags = false;
    if (PlaylistModel::sortKey(m_localSortColumn, &key)) {
        TrackSort::sort(tracks, key, m_localSortOrder);
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
    if (m_localPage == m_playingPage)
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
    m_localFiles->rescan(m_localPage->model()->tracks());
}

// --- Playback ----------------------------------------------------------------

void MainWindow::playRow(PlaylistPage *page, int row, bool start)
{
    if (!page || row < 0 || row >= page->model()->rowCount())
        return;
    const Track t = page->model()->track(row);

    if (m_playingPage && m_playingPage != page)
        m_playingPage->model()->setCurrent(0);
    m_playingPage = page;
    page->model()->setCurrent(t.uid);

    // Opens in the background: trackLoaded() or trackLoadFailed() follows.
    m_engine->load(t.path, t.startMs, t.endMs);
    if (start)
        m_engine->play();   // starts as soon as the file is open
    updateNowPlaying();
    // The title on screen when it cannot be seen in the window (AIMP's OSD).
    if (TrackOsd::wanted(m_settings.osd.when, isMinimized(), isVisible(), isActiveWindow()))
        m_osd->showTrack(t);
}

void MainWindow::trackLoaded()
{
    m_failStreak = 0;
    if (!m_playingPage)
        return;
    PlaylistModel *model = m_playingPage->model();
    const int row = model->currentRow();
    if (row < 0)
        return;
    model->setMissing(row, false);
    m_osd->refresh(model->track(row), m_engine->durationMs());   // the length, if the tags had none
}

void MainWindow::trackLoadFailed(const QString &error, bool wasPlaying)
{
    PlaylistModel *model = m_playingPage ? m_playingPage->model() : nullptr;
    const int row = model ? model->currentRow() : -1;
    if (row >= 0) {
        model->setMissing(row, true);
        statusBar()->showMessage(QStringLiteral("%1: %2").arg(model->track(row).fileName(), error), 5000);
    } else {
        statusBar()->showMessage(error, 5000);
    }
    // Skip to the next track, but don't spin through a playlist of dead files,
    // and not when the user stopped or paused while it was opening.
    if (model && wasPlaying && ++m_failStreak < model->rowCount())
        QTimer::singleShot(0, this, &MainWindow::playNext);
    else
        m_failStreak = 0;
    updateNowPlaying();
}

void MainWindow::playOrResume()
{
    switch (m_engine->state()) {
    case AudioEngine::State::Playing:
        m_engine->pause();
        return;
    case AudioEngine::State::Paused:
        m_engine->play();
        return;
    case AudioEngine::State::Stopped:
        break;
    }

    PlaylistPage *page = currentPage();
    if (!page)
        return;
    // From the tray/media keys the selection is not what the user means.
    int row = isActiveWindow() ? page->focusedSourceRow() : -1;
    if (row < 0 && m_playingPage && m_playingPage->model()->currentRow() >= 0) {
        page = m_playingPage;
        row = page->model()->currentRow();
    }
    if (row < 0 && page->proxy()->rowCount() > 0)
        row = page->proxy()->mapToSource(page->proxy()->index(0, 0)).row();
    playRow(page, row);
}

void MainWindow::playNext()
{
    PlaylistPage *page = m_playingPage ? m_playingPage.data() : currentPage();
    if (!page)
        return;
    PlaylistModel *model = page->model();
    // Also from a track whose row has gone (deleted): see PlaylistModel::nextRow().
    const int next = model->currentUid() ? model->nextRow() : (model->rowCount() > 0 ? 0 : -1);
    if (next < 0) {
        m_engine->stop();   // end of playlist
        return;
    }
    playRow(page, next);
    page->scrollToSourceRow(next);
}

void MainWindow::playPrevious()
{
    if (m_engine->positionMs() > 3000) {
        m_engine->seek(0);
        return;
    }
    PlaylistPage *page = m_playingPage ? m_playingPage.data() : currentPage();
    if (!page)
        return;
    const int prev = page->model()->currentUid() ? page->model()->previousRow() : -1;
    if (prev >= 0) {
        playRow(page, prev);
        page->scrollToSourceRow(prev);
    }
}

void MainWindow::changeVolume(int delta)
{
    m_volume->setValue(qBound(0, m_volume->value() + delta, 100));
}

void MainWindow::updateNowPlaying()
{
    // Also a track whose row is gone: it plays on, so it is still shown.
    const Track *current = m_playingPage ? m_playingPage->model()->currentTrack() : nullptr;
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
    if (const Track *current = m_playingPage ? m_playingPage->model()->currentTrack() : nullptr) {
        m_nowPlaying->showTrack(*current, m_coverImage);
        m_mpris->setTrack(current, m_artFile);
    }
}

void MainWindow::updateNavigation()
{
    PlaylistModel *model = m_playingPage ? m_playingPage->model() : nullptr;
    if (model && model->currentUid()) {
        m_mpris->setNavigation(model->nextRow() >= 0, model->previousRow() >= 0);
        return;
    }
    m_mpris->setNavigation(currentPage() && currentPage()->model()->rowCount() > 0, false);
}

// --- Playlist helpers ----------------------------------------------------------

void MainWindow::updateStats()
{
    auto *page = currentPage();
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

void MainWindow::applySearch()
{
    if (auto *page = currentPage())
        page->setQuery(m_search->text(), TrackQuery::Field(m_searchField->currentData().toInt()));
}

void MainWindow::showTrackInfo()
{
    auto *page = currentPage();
    if (!page)
        return;
    int row = page->focusedSourceRow();
    if (row < 0 && page == m_playingPage)
        row = page->model()->currentRow();
    if (row < 0)
        return;
    TrackInfoDialog dlg(page->model()->track(row), this);
    dlg.exec();
}

void MainWindow::showContextMenu(const QPoint &globalPos)
{
    auto *page = currentPage();
    if (!page)
        return;
    const int row = page->focusedSourceRow();
    QMenu menu(this);
    if (row >= 0) {
        menu.addAction(Icons::make(Icons::Kind::Play, Theme::iconColor()), QStringLiteral("Воспроизвести"), this,
                       [this, page, row] { playRow(page, row); });
        menu.addAction(QStringLiteral("Информация о треке…"), this, &MainWindow::showTrackInfo);
        const Track &t = page->model()->track(row);
        if (!t.isUrl()) {
            menu.addAction(QStringLiteral("Открыть папку с файлом"), this, [path = t.path] {
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
            });
        }
        menu.addSeparator();
        if (!page->isReadOnly())
            menu.addAction(QStringLiteral("Удалить из плейлиста"), page, &PlaylistPage::removeSelected);
        if (!t.isUrl())
            menu.addAction(QStringLiteral("Удалить с диска…"), this, &MainWindow::deleteSelectedFiles);
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
        menu.addAction(QStringLiteral("Обновить"), this, [this] { m_localFiles->rescan(m_localPage->model()->tracks()); });
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

void MainWindow::showHeaderMenu(const QPoint &globalPos)
{
    auto *page = currentPage();
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
            for (int i = 0; i < m_tabs->count(); ++i) {
                if (auto *p = qobject_cast<PlaylistPage *>(m_tabs->widget(i)))
                    p->view()->horizontalHeader()->setSectionHidden(col, !on);
            }
        });
    }
    menu.addSeparator();
    menu.addAction(QStringLiteral("Ширина колонок по содержимому"), this, [this] {
        PlaylistPage::resetColumnWidths();
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto *p = qobject_cast<PlaylistPage *>(m_tabs->widget(i)))
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
    m_tabs->setCurrentWidget(page);
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
    m_tabs->setCurrentWidget(page);
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
        // Reuse the current tab if it is empty, as AIMP does with a fresh playlist.
        PlaylistPage *page = currentPage();
        if (first || !page || page->isReadOnly() || page->model()->rowCount() > 0)
            page = addPage(data.name);
        page->model()->setName(data.name);
        m_tabs->setTabText(m_tabs->indexOf(page), data.name);
        page->model()->insertTracks(-1, data.tracks);
        if (!first)
            first = page;
        total += int(data.tracks.size());
        missing += page->model()->missingCount();
    }
    if (first)
        m_tabs->setCurrentWidget(first);
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
        for (int i = 0; i < m_tabs->count(); ++i) {
            auto *page = qobject_cast<PlaylistPage *>(m_tabs->widget(i));
            if (page->isReadOnly())
                continue;   // Local files follow the folders by themselves
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
            for (int t = 0; t < m_tabs->count() && !page; ++t) {
                auto *p = qobject_cast<PlaylistPage *>(m_tabs->widget(t));
                if (p && !p->isReadOnly() && p->model()->name() == data.name)
                    page = p;
            }
        }
        if (!page && i == 0 && currentPage() && !currentPage()->isReadOnly()
            && currentPage()->model()->rowCount() == 0)
            page = currentPage();   // a fresh empty tab
        if (page && data.tracks.isEmpty() && page->model()->rowCount() > 0)
            continue;   // nothing read from it: never empty a full tab
        if (!page)
            page = addPage(data.name);
        page->model()->clear();
        page->model()->setName(data.name);
        m_tabs->setTabText(m_tabs->indexOf(page), data.name);
        page->model()->insertTracks(-1, data.tracks);

        if (!first)
            first = page;
        if (i == r.activeIndex)
            active = page;
        // Continue where AIMP was, without starting playback.
        if (i == r.playingIndex && r.playingRow >= 0 && r.playingRow < page->model()->rowCount()
            && m_engine->state() == AudioEngine::State::Stopped) {
            if (m_playingPage && m_playingPage != page)
                m_playingPage->model()->setCurrent(0);
            m_playingPage = page;
            page->model()->setCurrent(page->model()->track(r.playingRow).uid);
            page->scrollToSourceRow(r.playingRow);
            updateNowPlaying();
        }
    }
    if (PlaylistPage *show = active ? active : first)
        m_tabs->setCurrentWidget(show);
    rememberMoves(r.moves);
    updateNowPlaying();   // a replaced tab may have held the current track
    updateStats();
    QMessageBox::information(this, windowTitle(), QStringLiteral("Импорт завершён.\n\n") + r.summary);
}

void MainWindow::deleteSelectedFiles()
{
    PlaylistPage *page = currentPage();
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
    const Track *playing = m_playingPage ? m_playingPage->model()->currentTrack() : nullptr;
    const bool playingGone = playing && gone.contains(playing->path);

    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *page = qobject_cast<PlaylistPage *>(m_tabs->widget(i));
        if (!page || page->isReadOnly())
            continue;   // Local files: rescanned below
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

    if (playingGone && m_playingPage) {
        PlaylistModel *model = m_playingPage->model();
        int next = -1;
        if (model->isCurrentDetached()) {
            next = model->nextRow();   // its row was removed: the first survivor after it
        } else {
            for (int row = model->currentRow() + 1; row < model->rowCount() && next < 0; ++row) {
                if (!gone.contains(model->track(row).path))
                    next = row;   // still listed (Local files, marked missing): skip deleted ones
            }
        }
        const AudioEngine::State state = m_engine->state();
        if (next >= 0 && state != AudioEngine::State::Stopped) {
            playRow(m_playingPage, next, state == AudioEngine::State::Playing);   // paused: stays stopped there
            m_playingPage->scrollToSourceRow(next);
        } else if (next < 0) {
            m_engine->stop();   // nothing after it
            model->setCurrent(0);
            updateNowPlaying();
        }
    }

    if (m_localPage)
        m_localFiles->rescan(m_localPage->model()->tracks());   // at once, not after the watcher's delay
    updateStats();
    statusBar()->showMessage(QStringLiteral("Удалено с диска файлов: %1").arg(paths.size()), 5000);
}

void MainWindow::savePlaylist()
{
    auto *page = currentPage();
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
    const QString dir = playlistDir();
    QDir().mkpath(dir);

    // Tab indexes count every tab; files are written for ordinary playlists only
    // (Local files are rebuilt from the folders, with their own cache). Each
    // file is replaced atomically; old files go only after all are written.
    int playingTab = -1;
    int files = 0;
    QSet<QString> written;
    QStringList failed;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *page = qobject_cast<PlaylistPage *>(m_tabs->widget(i));
        if (page == m_playingPage)
            playingTab = i;
        if (page == m_localPage)
            continue;
        const QString name = QStringLiteral("%1.xspf").arg(files++, 3, 10, QLatin1Char('0'));
        QString error;
        if (PlaylistIO::save(dir + QLatin1Char('/') + name, {page->model()->name(), page->model()->tracks()}, &error))
            written.insert(name);
        else
            failed << QStringLiteral("%1: %2").arg(page->model()->name(), error);
    }
    if (failed.isEmpty()) {
        for (const QString &old : QDir(dir).entryList({QStringLiteral("*.xspf")}, QDir::Files)) {
            if (!written.contains(old))
                QFile::remove(dir + QLatin1Char('/') + old);
        }
    } else {
        statusBar()->showMessage(QStringLiteral("Не удалось сохранить плейлисты: ") + failed.join(QStringLiteral("; ")), 10000);
    }
    if (scope == SaveScope::Everything)
        m_localFiles->saveCache(m_localPage->model()->tracks());

    QSettings s;
    s.setValue(QStringLiteral("session/version"), 2);
    s.setValue(QStringLiteral("session/count"), files);
    s.setValue(QStringLiteral("session/localTab"), m_tabs->indexOf(m_localPage));
    s.setValue(QStringLiteral("session/current"), m_tabs->currentIndex());
    s.setValue(QStringLiteral("session/playingTab"), playingTab);
    s.setValue(QStringLiteral("session/playingRow"), m_playingPage ? m_playingPage->model()->currentRow() : -1);
    // Rows of Local files change with the folders: remember the file.
    const Track *playing = m_playingPage ? m_playingPage->model()->currentTrack() : nullptr;
    s.setValue(QStringLiteral("session/playingPath"), playing ? playing->path : QString());
    s.setValue(QStringLiteral("volume"), m_volume->value());
    s.setValue(QStringLiteral("searchField"), m_searchField->currentIndex());
    s.setValue(QStringLiteral("geometry"), saveGeometry());
    s.setValue(QStringLiteral("splitter"), m_splitter->saveState());
    if (auto *page = currentPage())
        s.setValue(QStringLiteral("header"), page->view()->horizontalHeader()->saveState());
    s.setValue(QStringLiteral("headerColumns"), int(PlaylistModel::ColumnCount));
}

void MainWindow::restoreSession()
{
    QSettings s;
    restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
    m_splitter->restoreState(s.value(QStringLiteral("splitter")).toByteArray());
    m_volume->setValue(s.value(QStringLiteral("volume"), 70).toInt());
    m_engine->setVolume(m_volume->value() / 100.0f);
    m_mpris->setVolume(m_volume->value() / 100.0);
    m_searchField->setCurrentIndex(s.value(QStringLiteral("searchField"), 0).toInt());
    m_headerState = s.value(QStringLiteral("header")).toByteArray();
    m_headerColumns = s.value(QStringLiteral("headerColumns"), 10).toInt();   // 10: before the date columns
    m_localSortColumn = s.value(QStringLiteral("localFiles/sortColumn"), -1).toInt();
    m_localSortOrder = s.value(QStringLiteral("localFiles/sortOrder"), 0).toInt() == 1 ? Qt::DescendingOrder
                                                                                       : Qt::AscendingOrder;

    createLocalPage();
    // Every file there, also past session/count: a crash between writing the
    // files and the count leaves more of them. The next save replaces (and
    // removes) these files, so one that cannot be read is first kept aside.
    const QString dir = playlistDir();
    QStringList unreadable;
    const QStringList files = QDir(dir).entryList({QStringLiteral("[0-9][0-9][0-9].xspf")}, QDir::Files, QDir::Name);
    for (int i = 0; i < files.size(); ++i) {
        PlaylistData data;
        const QString file = dir + QLatin1Char('/') + files[i];
        if (!PlaylistIO::load(file, data)) {
            const QString kept = file + QDateTime::currentDateTime().toString(QStringLiteral(".yyyyMMdd-HHmmss")) + QStringLiteral(".damaged");
            if (QFile::copy(file, kept))
                unreadable << kept;
            if (data.tracks.isEmpty())
                continue;   // what could be read is kept below
        }
        PlaylistPage *page = addPage(data.name.isEmpty() ? QStringLiteral("Плейлист %1").arg(i + 1) : data.name);
        page->model()->insertTracks(-1, data.tracks);
    }
    if (!unreadable.isEmpty()) {
        QTimer::singleShot(0, this, [this, unreadable] {
            QMessageBox::warning(this, windowTitle(),
                                 QStringLiteral("Не удалось прочитать плейлисты прошлого сеанса полностью. Копии файлов сохранены:\n\n")
                                     + unreadable.join(QLatin1Char('\n')));
        });
    }
    if (m_tabs->count() == 1)
        addPage(QStringLiteral("Плейлист 1"));

    // Sessions saved before Local files existed had no such tab: it goes first
    // and every saved index moves by one.
    const bool oldSession = s.value(QStringLiteral("session/version"), 1).toInt() < 2;
    const int shift = oldSession ? 1 : 0;
    const int localTab = oldSession ? 0 : s.value(QStringLiteral("session/localTab"), 0).toInt();
    m_tabs->tabBar()->moveTab(m_tabs->indexOf(m_localPage), qBound(0, localTab, m_tabs->count() - 1));
    m_tabs->setCurrentIndex(qBound(0, s.value(QStringLiteral("session/current"), 0).toInt() + shift, m_tabs->count() - 1));

    // Mark the last played track so that Play continues from it.
    int playingTab = s.value(QStringLiteral("session/playingTab"), -1).toInt();
    if (playingTab >= 0)
        playingTab += shift;
    int playingRow = s.value(QStringLiteral("session/playingRow"), -1).toInt();
    auto *playing = qobject_cast<PlaylistPage *>(m_tabs->widget(playingTab));
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
        m_playingPage = page;
        page->model()->setCurrent(page->model()->track(playingRow).uid);
        page->scrollToSourceRow(playingRow);
    }
    updateNowPlaying();
    updateStats();
    if (auto *page = currentPage())
        page->view()->setFocus();
    // Bring Local files up to date with the folders (the cache was shown meanwhile).
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
