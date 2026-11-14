#include "ui/LocalFilesDialog.h"

#include "ui/DialogFrame.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// A list of folders with "Add…" / "Remove" buttons.
QGroupBox *folderList(const QString &title, QListWidget *list, QWidget *parent)
{
    auto *box = new QGroupBox(title);
    auto *layout = new QHBoxLayout(box);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto *add = new QPushButton(QStringLiteral("Добавить…"));
    auto *remove = new QPushButton(QStringLiteral("Убрать"));
    QObject::connect(add, &QPushButton::clicked, parent, [list, parent, title] {
        const QString dir = QFileDialog::getExistingDirectory(parent, title, QDir::homePath());
        if (!dir.isEmpty() && list->findItems(dir, Qt::MatchExactly).isEmpty())
            list->addItem(QDir::cleanPath(dir));
    });
    QObject::connect(remove, &QPushButton::clicked, parent, [list] { qDeleteAll(list->selectedItems()); });
    auto *buttons = new QVBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    layout->addWidget(list, 1);
    layout->addLayout(buttons);
    return box;
}

QStringList items(const QListWidget *list)
{
    QStringList out;
    for (int i = 0; i < list->count(); ++i)
        out << list->item(i)->text();
    return out;
}

} // namespace

LocalFilesDialog::LocalFilesDialog(const LocalFilesSettings &current, QWidget *parent)
    : QDialog(parent)
    , m_folders(new QListWidget)
    , m_excluded(new QListWidget)
    , m_watch(new QCheckBox(QStringLiteral("Следить за изменениями в папках")))
    , m_aimpNote(new QLabel)
{
    resizesLikeAWindow(this);
    setWindowTitle(QStringLiteral("Локальные файлы"));
    resize(600, 480);

    auto *intro = new QLabel(QStringLiteral(
        "«Локальные файлы» показывают всю музыку из этих папок (с подпапками), сгруппированную по папкам, "
        "как одноимённый раздел фонотеки AIMP. Сам список изменить нельзя — он следует за папками."));
    intro->setWordWrap(true);
    m_watch->setToolTip(QStringLiteral("Новые и удалённые файлы появляются в списке сами"));

    auto *fromAimp = new QPushButton(QStringLiteral("Взять настройки из AIMP"));
    m_aimpNote->setWordWrap(true);
    m_aimpNote->setForegroundRole(QPalette::PlaceholderText);
    LocalFilesSettings aimp;
    QString source;
    const bool haveAimp = LocalFilesSettings::fromAimp(aimp, &source);
    fromAimp->setEnabled(haveAimp);
    m_aimpNote->setText(haveAimp ? QStringLiteral("Фонотека AIMP: %1").arg(aimp.folders.join(QStringLiteral(", ")))
                                 : QStringLiteral("AIMP на этом компьютере не найден."));
    connect(fromAimp, &QPushButton::clicked, this, [this, aimp] { fill(aimp); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *aimpRow = new QHBoxLayout;
    aimpRow->addWidget(fromAimp);
    aimpRow->addWidget(m_aimpNote, 1);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(folderList(QStringLiteral("Папки с музыкой"), m_folders, this), 2);
    layout->addWidget(folderList(QStringLiteral("Исключить папки"), m_excluded, this), 1);
    layout->addWidget(m_watch);
    layout->addLayout(aimpRow);
    layout->addWidget(buttons);

    fill(current);
}

void LocalFilesDialog::fill(const LocalFilesSettings &s)
{
    m_folders->clear();
    m_folders->addItems(s.folders);
    m_excluded->clear();
    m_excluded->addItems(s.excluded);
    m_watch->setChecked(s.watch);
}

LocalFilesSettings LocalFilesDialog::values() const
{
    LocalFilesSettings s;
    s.folders = items(m_folders);
    s.excluded = items(m_excluded);
    s.watch = m_watch->isChecked();
    return s;
}
