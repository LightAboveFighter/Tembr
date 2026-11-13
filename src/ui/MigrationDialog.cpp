#include "ui/MigrationDialog.h"

#include "core/Relocator.h"
#include "migration/PhoneImport.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

QWidget *withBrowse(QLineEdit *edit, QWidget *parent, const QString &title)
{
    auto *w = new QWidget;
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    auto *browse = new QPushButton(QStringLiteral("Обзор…"));
    QObject::connect(browse, &QPushButton::clicked, parent, [edit, parent, title] {
        const QString dir = QFileDialog::getExistingDirectory(parent, title, edit->text());
        if (!dir.isEmpty())
            edit->setText(dir);
    });
    l->addWidget(edit, 1);
    l->addWidget(browse);
    return w;
}

QLabel *noteLabel()
{
    auto *l = new QLabel;
    l->setWordWrap(true);
    l->setForegroundRole(QPalette::PlaceholderText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

} // namespace

MigrationDialog::MigrationDialog(QWidget *parent)
    : QDialog(parent)
    , m_cancel(std::make_shared<std::atomic<bool>>(false))
{
    setWindowTitle(QStringLiteral("Импорт плейлистов из AIMP"));
    resize(760, 640);

    // --- Source
    auto *sourceBox = new QGroupBox(QStringLiteral("Откуда"));
    auto *grid = new QGridLayout(sourceBox);
    m_fromAimp = new QRadioButton(QStringLiteral("AIMP на этом компьютере"));
    m_fromPhone = new QRadioButton(QStringLiteral("Телефон Android по USB"));
    m_aimpProfiles = new QComboBox;
    m_phones = new QComboBox;
    m_refresh = new QPushButton(QStringLiteral("Обновить"));
    m_pickFolder = new QPushButton(QStringLiteral("Папка…"));
    m_pickFolder->setToolTip(QStringLiteral("Телефон, подключённый иначе, SD-карта или копия памяти телефона"));
    auto *phoneRow = new QHBoxLayout;
    phoneRow->addWidget(m_phones, 1);
    phoneRow->addWidget(m_refresh);
    phoneRow->addWidget(m_pickFolder);
    grid->addWidget(m_fromAimp, 0, 0);
    grid->addWidget(m_aimpProfiles, 0, 1);
    grid->addWidget(m_fromPhone, 1, 0);
    grid->addLayout(phoneRow, 1, 1);
    m_notes = noteLabel();
    grid->addWidget(m_notes, 2, 0, 1, 2);
    m_includeOld = new QCheckBox(QStringLiteral("Показывать и старые копии плейлистов, которые AIMP оставил в папке"));
    grid->addWidget(m_includeOld, 3, 0, 1, 2);
    grid->setColumnStretch(1, 1);

    // --- Music
    QSettings settings;
    auto *musicBox = new QGroupBox(QStringLiteral("Музыка"));
    auto *musicGrid = new QGridLayout(musicBox);
    QString library = settings.value(QStringLiteral("migration/library"),
                                     settings.value(QStringLiteral("paths/lastSearchDir"))).toString();
    if (library.isEmpty())
        library = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    m_library = new QLineEdit(library);
    m_library->setPlaceholderText(QStringLiteral("не искать"));
    m_library->setToolTip(QStringLiteral("Треки, которых нет на прежнем месте, ищутся здесь по имени файла"));
    m_copy = new QCheckBox(QStringLiteral("Копировать с телефона треки, которых нет на компьютере, в:"));
    m_copy->setChecked(settings.value(QStringLiteral("migration/copy"), false).toBool());
    m_copyTo = new QLineEdit(settings.value(QStringLiteral("migration/copyTo"), library).toString());
    musicGrid->addWidget(new QLabel(QStringLiteral("Музыка на этом компьютере:")), 0, 0);
    musicGrid->addWidget(withBrowse(m_library, this, QStringLiteral("Где лежит музыка")), 0, 1);
    musicGrid->addWidget(m_copy, 1, 0);
    auto *copyWidget = withBrowse(m_copyTo, this, QStringLiteral("Куда копировать треки с телефона"));
    musicGrid->addWidget(copyWidget, 1, 1);
    auto *copyNote = noteLabel();
    copyNote->setText(QStringLiteral("Папки телефона сохраняются (кроме верхней Music). "
                                     "Уже существующие файлы не перезаписываются."));
    musicGrid->addWidget(copyNote, 2, 0, 1, 2);
    musicGrid->setColumnStretch(1, 1);

    // --- Playlists
    m_find = new QPushButton(QStringLiteral("Найти плейлисты"));
    m_list = new QTreeWidget;
    m_list->setRootIsDecorated(false);
    m_list->setHeaderLabels({QStringLiteral("Плейлист"), QStringLiteral("Треков"), QStringLiteral("Где")});
    m_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_list->header()->setStretchLastSection(true);
    auto *checkAll = new QPushButton(QStringLiteral("Отметить все"));
    auto *uncheckAll = new QPushButton(QStringLiteral("Снять все"));
    m_replace = new QCheckBox(QStringLiteral("Заменять вкладки с такими же названиями"));
    m_replace->setChecked(settings.value(QStringLiteral("migration/replace"), true).toBool());
    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(m_find);
    listButtons->addWidget(checkAll);
    listButtons->addWidget(uncheckAll);
    listButtons->addStretch();
    listButtons->addWidget(m_replace);

    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);
    m_progress->hide();
    m_status = noteLabel();

    m_import = new QPushButton(QStringLiteral("Импортировать"));
    m_import->setDefault(true);
    m_close = new QPushButton(QStringLiteral("Закрыть"));
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_status, 1);
    buttons->addWidget(m_import);
    buttons->addWidget(m_close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(sourceBox);
    layout->addWidget(musicBox);
    layout->addLayout(listButtons);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_progress);
    layout->addLayout(buttons);

    connect(m_fromAimp, &QRadioButton::toggled, this, [this, copyWidget, copyNote] {
        const bool phone = phoneMode();
        m_copy->setVisible(phone);
        copyWidget->setVisible(phone);
        copyNote->setVisible(phone);
        m_includeOld->setVisible(!phone);
        findPlaylists();
    });
    connect(m_aimpProfiles, &QComboBox::currentIndexChanged, this, [this] {
        if (!phoneMode())
            findPlaylists();
    });
    connect(m_phones, &QComboBox::currentIndexChanged, this, [this] {
        if (phoneMode())
            findPlaylists();
    });
    connect(m_includeOld, &QCheckBox::toggled, this, &MigrationDialog::findPlaylists);
    connect(m_refresh, &QPushButton::clicked, this, &MigrationDialog::refreshSources);
    connect(m_pickFolder, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Папка телефона"));
        if (dir.isEmpty())
            return;
        m_phoneSources.append(Migration::folderSource(dir));
        m_phones->addItem(m_phoneSources.last()->label());
        m_fromPhone->setEnabled(true);
        m_fromPhone->setChecked(true);
        m_phones->setCurrentIndex(m_phones->count() - 1);
    });
    connect(m_find, &QPushButton::clicked, this, &MigrationDialog::findPlaylists);
    connect(checkAll, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < m_list->topLevelItemCount(); ++i)
            m_list->topLevelItem(i)->setCheckState(0, Qt::Checked);
    });
    connect(uncheckAll, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < m_list->topLevelItemCount(); ++i)
            m_list->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
    });
    connect(m_list, &QTreeWidget::itemChanged, this, &MigrationDialog::updateButtons);
    connect(m_copy, &QCheckBox::toggled, m_copyTo, &QWidget::setEnabled);
    connect(m_import, &QPushButton::clicked, this, &MigrationDialog::startImport);
    connect(m_close, &QPushButton::clicked, this, [this] {
        reject();
    });

    m_copyTo->setEnabled(m_copy->isChecked());
    refreshSources();
}

MigrationDialog::~MigrationDialog()
{
    m_cancel->store(true);
}

void MigrationDialog::reject()
{
    if (m_busy) {
        m_cancel->store(true);
        return;
    }
    QDialog::reject();
}

bool MigrationDialog::phoneMode() const
{
    return m_fromPhone->isChecked();
}

void MigrationDialog::refreshSources()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_profiles = Migration::findAimpProfiles();
    const Migration::PhoneDiscovery phones = Migration::discoverPhones();
    QApplication::restoreOverrideCursor();

    {
        // Refill without each change triggering a search; one search follows below.
        const QSignalBlocker b1(m_aimpProfiles), b2(m_phones), b3(m_fromAimp);
        m_aimpProfiles->clear();
        for (const auto &p : std::as_const(m_profiles))
            m_aimpProfiles->addItem(p.label, p.dir);
        m_aimpProfiles->setToolTip(m_profiles.isEmpty() ? QString() : m_profiles.first().dir);
        // Keep folders picked by hand.
        QList<Migration::PhoneSourcePtr> folders;
        for (const auto &s : std::as_const(m_phoneSources)) {
            if (s->kind() == Migration::PhoneSource::Kind::Folder)
                folders.append(s);
        }
        m_phoneSources = phones.sources + folders;
        m_phones->clear();
        for (const auto &s : std::as_const(m_phoneSources))
            m_phones->addItem(s->label());

        QStringList notes = phones.notes;
        if (m_phoneSources.isEmpty())
            notes << QStringLiteral("Телефон не найден: подключите его кабелем и выберите режим USB «Передача файлов».");
        notes << QStringLiteral("AIMP для Android хранит плейлисты внутри приложения. Чтобы перенести их, "
                                "сохраните плейлисты в файлы: меню плейлиста → «Экспорт» (например, в папку Music).");
        m_notes->setText(notes.join(QLatin1Char('\n')));

        m_fromAimp->setEnabled(!m_profiles.isEmpty());
        m_fromPhone->setEnabled(!m_phoneSources.isEmpty());
        if (m_profiles.isEmpty() && !m_phoneSources.isEmpty())
            m_fromPhone->setChecked(true);
        else if (!m_fromPhone->isChecked() || m_phoneSources.isEmpty())
            m_fromAimp->setChecked(true);
        if (m_profiles.isEmpty())
            m_aimpProfiles->addItem(QStringLiteral("AIMP не найден"));
    }

    emit m_fromAimp->toggled(m_fromAimp->isChecked());   // update visibility and the list
}

void MigrationDialog::findPlaylists()
{
    if (m_busy)
        return;
    m_list->clear();
    m_items.clear();
    updateButtons();

    if (!phoneMode()) {
        const int idx = m_aimpProfiles->currentIndex();
        if (idx < 0 || idx >= m_profiles.size())
            return;
        m_session = Migration::readAimpSession(m_profiles[idx]);
        QList<Item> items;
        for (int i = 0; i < m_session.playlists.size(); ++i) {
            const auto &p = m_session.playlists[i];
            if (!p.open && !m_includeOld->isChecked())
                continue;
            Item it;
            it.name = p.name;
            it.tracks = p.trackCount;
            it.where = QFileInfo(p.file).fileName();
            if (!p.open)
                it.where += QStringLiteral("  (старая копия от %1)").arg(QLocale().toString(p.modified, QLocale::ShortFormat));
            if (i == m_session.activeIndex)
                it.where += QStringLiteral("  — активный");
            it.checked = p.open;
            it.aimpIndex = i;
            items.append(it);
        }
        showItems(items);
        return;
    }

    const int idx = m_phones->currentIndex();
    if (idx < 0 || idx >= m_phoneSources.size())
        return;
    const Migration::PhoneSourcePtr source = m_phoneSources[idx];
    setBusy(true, QStringLiteral("Поиск плейлистов на телефоне…"));
    auto *watcher = new QFutureWatcher<QList<Item>>(this);
    connect(watcher, &QFutureWatcher<QList<Item>>::finished, this, [this, watcher] {
        const QList<Item> items = watcher->result();
        watcher->deleteLater();
        setBusy(false, items.isEmpty() ? QStringLiteral("Плейлистов на телефоне не найдено.")
                                       : QStringLiteral("Найдено плейлистов: %1").arg(items.size()));
        showItems(items);
    });
    watcher->setFuture(QtConcurrent::run([source] {
        QList<Item> items;
        for (const auto &f : source->findPlaylists()) {
            Item it;
            it.name = QFileInfo(f.path).completeBaseName();
            it.where = source->phonePath(f.storage, f.path);
            it.phoneFile = f;
            items.append(it);
        }
        std::sort(items.begin(), items.end(),
                  [](const Item &a, const Item &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
        return items;
    }));
}

void MigrationDialog::showItems(const QList<Item> &items)
{
    m_items = items;
    const QSignalBlocker block(m_list);
    m_list->clear();
    for (const Item &it : items) {
        auto *w = new QTreeWidgetItem({it.name, it.tracks >= 0 ? QString::number(it.tracks) : QStringLiteral("—"), it.where});
        w->setCheckState(0, it.checked ? Qt::Checked : Qt::Unchecked);
        w->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        w->setToolTip(2, it.where);
        m_list->addTopLevelItem(w);
    }
    updateButtons();
}

void MigrationDialog::updateButtons()
{
    int checked = 0;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        checked += m_list->topLevelItem(i)->checkState(0) == Qt::Checked;
    m_import->setEnabled(!m_busy && checked > 0);
    m_import->setText(checked > 0 ? QStringLiteral("Импортировать (%1)").arg(checked) : QStringLiteral("Импортировать"));
}

void MigrationDialog::setBusy(bool busy, const QString &text)
{
    m_busy = busy;
    m_progress->setVisible(busy);
    if (busy)
        m_progress->setRange(0, 0);
    m_status->setText(text);
    m_close->setText(busy ? QStringLiteral("Отмена") : QStringLiteral("Закрыть"));
    for (QWidget *w : {static_cast<QWidget *>(m_find), static_cast<QWidget *>(m_refresh),
                       static_cast<QWidget *>(m_pickFolder), static_cast<QWidget *>(m_fromAimp),
                       static_cast<QWidget *>(m_fromPhone), static_cast<QWidget *>(m_list)})
        w->setEnabled(!busy);
    updateButtons();
}

void MigrationDialog::startImport()
{
    QList<Item> chosen;
    for (int i = 0; i < m_list->topLevelItemCount() && i < m_items.size(); ++i) {
        if (m_list->topLevelItem(i)->checkState(0) == Qt::Checked)
            chosen.append(m_items[i]);
    }
    if (chosen.isEmpty())
        return;

    const bool phone = phoneMode();
    const QString library = m_library->text().trimmed();
    const QString copyTo = phone && m_copy->isChecked() ? m_copyTo->text().trimmed() : QString();
    QSettings s;
    s.setValue(QStringLiteral("migration/library"), library);
    s.setValue(QStringLiteral("migration/copy"), m_copy->isChecked());
    s.setValue(QStringLiteral("migration/copyTo"), m_copyTo->text().trimmed());
    s.setValue(QStringLiteral("migration/replace"), m_replace->isChecked());

    const Migration::PhoneSourcePtr source = phone ? m_phoneSources.value(m_phones->currentIndex()) : nullptr;
    const Migration::AimpSession session = m_session;
    const bool replace = m_replace->isChecked();
    auto cancel = m_cancel;
    cancel->store(false);
    QPointer<MigrationDialog> self(this);

    // Progress from the worker thread, delivered on the GUI thread.
    auto report = [self](int done, int total, const QString &text) {
        QMetaObject::invokeMethod(qApp, [self, done, total, text] {
            if (!self)
                return;
            self->m_progress->setRange(0, total);
            self->m_progress->setValue(done);
            self->m_status->setText(text);
        }, Qt::QueuedConnection);
    };

    setBusy(true, QStringLiteral("Импорт…"));
    auto *watcher = new QFutureWatcher<MigrationResult>(this);
    connect(watcher, &QFutureWatcher<MigrationResult>::finished, this, [this, watcher, cancel] {
        const MigrationResult r = watcher->result();
        watcher->deleteLater();
        if (cancel->load()) {
            setBusy(false, QStringLiteral("Импорт отменён."));
            return;
        }
        m_result = r;
        setBusy(false, r.summary);
        accept();
    });
    watcher->setFuture(QtConcurrent::run([=]() {
        MigrationResult r;
        r.replaceSameName = replace;
        Relocator::Index index;
        if (!library.isEmpty() && QFileInfo(library).isDir()) {
            report(0, 0, QStringLiteral("Индексирую музыку в %1…").arg(library));
            index = Relocator::build(library);
        }
        int total = 0, found = 0;
        Migration::PhoneImportStats phoneStats;
        QStringList errors;
        for (int i = 0; i < chosen.size() && !cancel->load(); ++i) {
            const Item &it = chosen[i];
            report(i, int(chosen.size()), QStringLiteral("%1 из %2: %3").arg(i + 1).arg(chosen.size()).arg(it.name));
            PlaylistData data;
            QString error;
            if (it.aimpIndex >= 0) {
                const auto &info = session.playlists[it.aimpIndex];
                if (!Migration::loadAimpPlaylist(info, data, &error)) {
                    errors << error;
                    continue;
                }
                if (index.files > 0)
                    Relocator::relocateMissing(data.tracks, index, &r.moves);
                if (it.aimpIndex == session.activeIndex)
                    r.activeIndex = int(r.playlists.size());
                if (it.aimpIndex == session.playingIndex) {
                    r.playingIndex = int(r.playlists.size());
                    r.playingRow = info.playbackCursor;
                }
            } else {
                Migration::PhoneImportOptions opt;
                opt.library = index.files > 0 ? &index : nullptr;
                opt.copyMissingTo = copyTo;
                const QString label = QStringLiteral("%1 из %2: %3").arg(i + 1).arg(chosen.size()).arg(it.name);
                const auto progress = [&](int done, int count, const QString &current) {
                    if (done % 10 == 0 || done == count)
                        report(i, int(chosen.size()), QStringLiteral("%1 — трек %2 из %3 %4").arg(label).arg(done).arg(count).arg(current));
                    return !cancel->load();
                };
                if (!Migration::importPhonePlaylist(*source, it.phoneFile, opt, data, phoneStats, progress, &error)) {
                    errors << error;
                    continue;
                }
            }
            total += int(data.tracks.size());
            found += int(std::count_if(data.tracks.cbegin(), data.tracks.cend(), [](const Track &t) { return !t.missing; }));
            r.playlists.append(data);
        }
        r.summary = QStringLiteral("Плейлистов: %1, треков: %2, найдено на компьютере: %3")
                        .arg(r.playlists.size()).arg(total).arg(found);
        if (phoneStats.copied > 0)
            r.summary += QStringLiteral(", скопировано с телефона: %1").arg(phoneStats.copied);
        errors += phoneStats.errors;
        if (!errors.isEmpty())
            r.summary += QStringLiteral("\nОшибки (%1): %2").arg(errors.size()).arg(errors.mid(0, 5).join(QStringLiteral("; ")));
        return r;
    }));
}
