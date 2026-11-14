#include "ui/DeleteFilesDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
const QString kAllPlaylistsKey = QStringLiteral("delete/fromAllPlaylists");
}

DeleteFilesDialog::DeleteFilesDialog(Kind kind, const QStringList &paths, int skippedCueTracks, QWidget *parent)
    : QDialog(parent)
    , m_table(new QTreeWidget)
    , m_details(new QPushButton)
{
    setWindowTitle(QStringLiteral("Удалить с диска"));

    auto *icon = new QLabel;
    const int iconSize = style()->pixelMetric(QStyle::PM_MessageBoxIconSize, nullptr, this);
    icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, this).pixmap(iconSize, iconSize));
    icon->setAlignment(Qt::AlignTop);

    auto *text = new QLabel;
    text->setWordWrap(true);
    text->setTextFormat(Qt::PlainText);
    QFont bold = text->font();
    bold.setBold(true);
    text->setFont(bold);
    auto *info = new QLabel;
    info->setWordWrap(true);
    info->setTextFormat(Qt::PlainText);
    info->setForegroundRole(QPalette::PlaceholderText);

    const bool one = paths.size() == 1;
    QString infoText;
    if (kind == Kind::ToTrash) {
        text->setText(one ? QStringLiteral("Удалить файл «%1» с диска?").arg(QFileInfo(paths.first()).fileName())
                          : QStringLiteral("Удалить с диска выбранные файлы (%1)?").arg(paths.size()));
        infoText = QStringLiteral("Файлы будут перемещены в корзину.");
        if (one)
            infoText = QDir::toNativeSeparators(QFileInfo(paths.first()).path()) + QStringLiteral("\n\n") + infoText;
        if (skippedCueTracks)
            infoText += QStringLiteral("\n\nТреки из CUE (%1) пропущены: их файл содержит весь альбом.").arg(skippedCueTracks);
    } else {
        text->setText(QStringLiteral("Не удалось переместить в корзину файлы (%1).").arg(paths.size()));
        infoText = QStringLiteral("На этом диске нет корзины или нет прав на её создание. Удалить их безвозвратно?");
    }
    info->setText(infoText);

    // Name / Folder table, hidden until "Details".
    m_table->setColumnCount(2);
    m_table->setHeaderLabels({QStringLiteral("Имя"), QStringLiteral("Папка")});
    m_table->setRootIsDecorated(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setUniformRowHeights(true);
    m_table->setTextElideMode(Qt::ElideMiddle);
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        auto *item = new QTreeWidgetItem({fi.fileName(), QDir::toNativeSeparators(fi.path())});
        item->setToolTip(0, p);
        item->setToolTip(1, p);
        m_table->addTopLevelItem(item);
    }
    m_table->header()->setStretchLastSection(true);
    m_table->resizeColumnToContents(0);
    m_table->setColumnWidth(0, qMin(m_table->columnWidth(0) + 12, 360));
    const int rowHeight = m_table->sizeHintForRow(0) > 0 ? m_table->sizeHintForRow(0) : fontMetrics().height() + 6;
    m_table->setMinimumHeight(m_table->header()->sizeHint().height() + rowHeight * qMin(int(paths.size()), 8) + 4);
    int folderWidth = 0;
    for (const QString &p : paths)
        folderWidth = qMax(folderWidth, fontMetrics().horizontalAdvance(QDir::toNativeSeparators(QFileInfo(p).path())));
    m_table->setMinimumWidth(qMin(m_table->columnWidth(0) + folderWidth + 40, 760));   // grows the dialog when shown
    m_table->setVisible(false);

    m_details->setText(QStringLiteral("Подробнее…"));
    m_details->setVisible(!one);
    connect(m_details, &QPushButton::clicked, this, [this] {
        const bool show = !m_table->isVisible();
        m_table->setVisible(show);
        m_details->setText(show ? QStringLiteral("Скрыть список") : QStringLiteral("Подробнее…"));
        adjustSize();
    });

    if (kind == Kind::ToTrash) {
        m_allPlaylists = new QCheckBox(one ? QStringLiteral("Убрать трек и из всех плейлистов, где он есть")
                                           : QStringLiteral("Убрать треки и из всех плейлистов, где они есть"));
        m_allPlaylists->setToolTip(QStringLiteral("Иначе в других плейлистах они остаются и помечаются как отсутствующие"));
        m_allPlaylists->setChecked(QSettings().value(kAllPlaylistsKey, false).toBool());
    }

    auto *buttons = new QDialogButtonBox;
    QPushButton *remove = buttons->addButton(kind == Kind::ToTrash ? QStringLiteral("Удалить")
                                                                   : QStringLiteral("Удалить безвозвратно"),
                                             QDialogButtonBox::DestructiveRole);
    QPushButton *cancel = buttons->addButton(QStringLiteral("Отмена"), QDialogButtonBox::RejectRole);
    for (QPushButton *b : {remove, m_details})
        b->setAutoDefault(false);
    cancel->setDefault(true);
    connect(remove, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    auto *textColumn = new QVBoxLayout;
    textColumn->addWidget(text);
    textColumn->addWidget(info);
    auto *top = new QHBoxLayout;
    top->addWidget(icon);
    top->addLayout(textColumn, 1);
    top->setSpacing(16);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_table, 1);
    if (m_allPlaylists)
        layout->addWidget(m_allPlaylists);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_details);
    bottom->addWidget(buttons, 1);
    layout->addLayout(bottom);
    setMinimumWidth(460);
    cancel->setFocus();

    connect(this, &QDialog::accepted, this, [this] {
        if (m_allPlaylists)
            QSettings().setValue(kAllPlaylistsKey, m_allPlaylists->isChecked());
    });
}

bool DeleteFilesDialog::fromAllPlaylists() const
{
    return m_allPlaylists && m_allPlaylists->isChecked();
}
