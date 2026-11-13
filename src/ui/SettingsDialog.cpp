#include "ui/SettingsDialog.h"

#include "core/MediaCollector.h"
#include "ui/Icons.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

AppSettings AppSettings::load()
{
    QSettings s;
    AppSettings a;
    a.theme = Theme::fromString(s.value(QStringLiteral("ui/theme"), QStringLiteral("dark")).toString());
    a.trayIcon = s.value(QStringLiteral("ui/trayIcon"), true).toBool();
    a.closeToTray = s.value(QStringLiteral("ui/closeToTray"), false).toBool();
    a.hotkeys = GlobalHotkeys::load();
    return a;
}

void AppSettings::save() const
{
    QSettings s;
    s.setValue(QStringLiteral("ui/theme"), Theme::toString(theme));
    s.setValue(QStringLiteral("ui/trayIcon"), trayIcon);
    s.setValue(QStringLiteral("ui/closeToTray"), closeToTray);
    GlobalHotkeys::save(hotkeys);
}

SettingsDialog::SettingsDialog(const AppSettings &current, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Настройки"));
    setMinimumWidth(560);

    // --- Appearance
    auto *look = new QGroupBox(QStringLiteral("Внешний вид"));
    auto *lookForm = new QFormLayout(look);
    m_theme = new QComboBox;
    m_theme->addItem(QStringLiteral("Тёмная, оранжевое выделение"), Theme::toString(Theme::Kind::Dark));
    m_theme->addItem(QStringLiteral("Системная"), Theme::toString(Theme::Kind::System));
    m_theme->setCurrentIndex(current.theme == Theme::Kind::Dark ? 0 : 1);
    lookForm->addRow(QStringLiteral("Тема:"), m_theme);

    // --- Tray
    auto *tray = new QGroupBox(QStringLiteral("Значок в области уведомлений"));
    auto *trayLayout = new QVBoxLayout(tray);
    m_tray = new QCheckBox(QStringLiteral("Показывать значок с меню (пауза, следующий, предыдущий)"));
    m_tray->setChecked(current.trayIcon);
    m_closeToTray = new QCheckBox(QStringLiteral("При закрытии окна сворачивать в значок"));
    m_closeToTray->setChecked(current.closeToTray);
    m_closeToTray->setEnabled(current.trayIcon);
    connect(m_tray, &QCheckBox::toggled, m_closeToTray, &QWidget::setEnabled);
    auto *trayNote = new QLabel(QStringLiteral(
        "Кнопки управления и обложка также появляются в списке уведомлений GNOME (MPRIS) — "
        "это работает всегда, даже без значка."));
    trayNote->setWordWrap(true);
    trayNote->setForegroundRole(QPalette::PlaceholderText);
    trayLayout->addWidget(m_tray);
    trayLayout->addWidget(m_closeToTray);
    trayLayout->addWidget(trayNote);

    // --- Global hotkeys
    auto *keys = new QGroupBox(QStringLiteral("Глобальные горячие клавиши (работают при свёрнутом окне)"));
    auto *keysLayout = new QVBoxLayout(keys);
    auto *grid = new QGridLayout;
    const bool supported = GlobalHotkeys::desktopSupported();
    int row = 0;
    for (const auto &info : GlobalHotkeys::actions()) {
        auto *edit = new QKeySequenceEdit(current.hotkeys.value(info.action));
        edit->setEnabled(supported);
        if (auto *line = edit->findChild<QLineEdit *>())
            line->setPlaceholderText(QStringLiteral("Нажмите сочетание…"));
        auto *clear = new QToolButton;
        clear->setText(QStringLiteral("✕"));
        clear->setToolTip(QStringLiteral("Убрать сочетание"));
        clear->setEnabled(supported);
        connect(clear, &QToolButton::clicked, edit, &QKeySequenceEdit::clear);
        // Only the first chord is used.
        connect(edit, &QKeySequenceEdit::editingFinished, edit, [edit] {
            if (edit->keySequence().count() > 1)
                edit->setKeySequence(QKeySequence(edit->keySequence()[0]));
        });
        grid->addWidget(new QLabel(info.title + QLatin1Char(':')), row, 0);
        grid->addWidget(edit, row, 1);
        grid->addWidget(clear, row, 2);
        m_keys.insert(info.action, edit);
        ++row;
    }
    keysLayout->addLayout(grid);

    auto *keysNote = new QLabel;
    keysNote->setWordWrap(true);
    keysNote->setTextInteractionFlags(Qt::TextSelectableByMouse);
    keysNote->setForegroundRole(QPalette::PlaceholderText);
    if (supported) {
        keysNote->setText(QStringLiteral(
            "Сочетания регистрируются в GNOME (Настройки → Клавиатура → Комбинации клавиш → "
            "Дополнительные комбинации), поэтому срабатывают в любом окне. Выбирайте сочетания, "
            "не занятые системой, например Ctrl+Alt+→. Медиаклавиши клавиатуры работают и без этого."));
    } else {
        QStringList cmds;
        for (const auto &info : GlobalHotkeys::actions())
            cmds << QStringLiteral("%1:  %2").arg(info.title, GlobalHotkeys::command(info.action));
        keysNote->setText(QStringLiteral("Окружение %1 не позволяет программам регистрировать клавиши напрямую. "
                                         "Назначьте их в системных настройках на команды:\n\n%2")
                              .arg(GlobalHotkeys::desktopName(), cmds.join(QLatin1Char('\n'))));
    }
    keysLayout->addWidget(keysNote);

    // --- Desktop entry
    auto *install = new QPushButton(QStringLiteral("Добавить Tembr в меню приложений"));
    install->setToolTip(QStringLiteral("Ярлык в меню, «Открыть с помощью» и значок плеера в уведомлениях GNOME"));
    connect(install, &QPushButton::clicked, this, [this] {
        const QString err = installDesktopEntry();
        if (err.isEmpty())
            QMessageBox::information(this, windowTitle(), QStringLiteral("Ярлык добавлен в меню приложений."));
        else
            QMessageBox::warning(this, windowTitle(), err);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(look);
    layout->addWidget(tray);
    layout->addWidget(keys);
    layout->addWidget(install, 0, Qt::AlignLeft);
    layout->addStretch();
    layout->addWidget(buttons);
}

AppSettings SettingsDialog::values() const
{
    AppSettings a;
    a.theme = Theme::fromString(m_theme->currentData().toString());
    a.trayIcon = m_tray->isChecked();
    a.closeToTray = m_tray->isChecked() && m_closeToTray->isChecked();
    for (auto it = m_keys.cbegin(); it != m_keys.cend(); ++it) {
        if (!it.value()->keySequence().isEmpty())
            a.hotkeys.insert(it.key(), it.value()->keySequence());
    }
    return a;
}

QString installDesktopEntry()
{
    const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString iconDir = dataHome + QStringLiteral("/icons/hicolor/256x256/apps");
    const QString appDir = dataHome + QStringLiteral("/applications");
    if (!QDir().mkpath(iconDir) || !QDir().mkpath(appDir))
        return QStringLiteral("Не удалось создать папки в %1").arg(dataHome);
    if (!Icons::appPixmap(256).save(iconDir + QStringLiteral("/tembr.png")))
        return QStringLiteral("Не удалось сохранить значок");

    QStringList mimes = {QStringLiteral("audio/mpeg"), QStringLiteral("audio/flac"), QStringLiteral("audio/ogg"),
                         QStringLiteral("audio/x-vorbis+ogg"), QStringLiteral("audio/opus"), QStringLiteral("audio/mp4"),
                         QStringLiteral("audio/x-wav"), QStringLiteral("audio/x-ape"), QStringLiteral("audio/x-wavpack"),
                         QStringLiteral("audio/x-ms-wma"), QStringLiteral("audio/x-mpegurl"), QStringLiteral("audio/x-scpls"),
                         QStringLiteral("application/xspf+xml"), QStringLiteral("application/x-cue")};
    QString exe = QCoreApplication::applicationFilePath();
    if (exe.contains(QLatin1Char(' ')))
        exe = QLatin1Char('"') + exe + QLatin1Char('"');
    const QString entry = QStringLiteral(
                              "[Desktop Entry]\nType=Application\nName=Tembr\nGenericName=Аудиоплеер\n"
                              "Comment=Лёгкий аудиоплеер\nExec=%1 %U\nIcon=tembr\nTerminal=false\n"
                              "Categories=AudioVideo;Audio;Player;\nMimeType=%2;\nStartupWMClass=tembr\n"
                              "Actions=PlayPause;Next;Previous;\n\n"
                              "[Desktop Action PlayPause]\nName=Воспроизведение / пауза\nExec=%1 --play-pause\n\n"
                              "[Desktop Action Next]\nName=Следующий трек\nExec=%1 --next\n\n"
                              "[Desktop Action Previous]\nName=Предыдущий трек\nExec=%1 --previous\n")
                              .arg(exe, mimes.join(QLatin1Char(';')));
    QSaveFile f(appDir + QStringLiteral("/tembr.desktop"));
    if (!f.open(QIODevice::WriteOnly))
        return f.errorString();
    f.write(entry.toUtf8());
    if (!f.commit())
        return f.errorString();
    return {};
}
