#include "ui/TrackInfoDialog.h"

#include "core/MetadataReader.h"
#include "ui/DialogFrame.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

TrackInfoDialog::TrackInfoDialog(const Track &track, QWidget *parent)
    : QDialog(parent)
{
    resizesLikeAWindow(this);
    const Track t = track.isUrl() ? track : MetadataReader::read(track);
    setWindowTitle(t.displayTitle());
    resize(640, 480);

    auto *tabs = new QTabWidget(this);

    // --- General -------------------------------------------------------------
    auto *general = new QWidget;
    auto *generalLayout = new QHBoxLayout(general);

    auto *cover = new QLabel;
    cover->setFixedSize(200, 200);
    cover->setAlignment(Qt::AlignCenter);
    cover->setFrameShape(QFrame::StyledPanel);
    const QImage img = t.isUrl() ? QImage() : MetadataReader::readCover(t.path);
    if (img.isNull())
        cover->setText(QStringLiteral("Нет обложки"));
    else
        cover->setPixmap(QPixmap::fromImage(img.scaled(cover->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    generalLayout->addWidget(cover, 0, Qt::AlignTop);

    auto *form = new QFormLayout;
    auto add = [&](const QString &label, const QString &value) {
        if (value.isEmpty() || value == QLatin1String("0"))
            return;
        auto *v = new QLabel(value);
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->setWordWrap(true);
        form->addRow(label + QLatin1Char(':'), v);
    };
    const QLocale loc;
    add(QStringLiteral("Название"), t.title);
    add(QStringLiteral("Исполнитель"), t.artist);
    add(QStringLiteral("Альбом"), t.album);
    add(QStringLiteral("Исполнитель альбома"), t.albumArtist);
    add(QStringLiteral("Год"), t.year);
    add(QStringLiteral("Жанр"), t.genre);
    add(QStringLiteral("Номер трека"), QString::number(t.trackNo));
    add(QStringLiteral("Диск"), QString::number(t.discNo));
    add(QStringLiteral("Композитор"), t.composer);
    add(QStringLiteral("Комментарий"), t.comment);
    add(QStringLiteral("Длительность"), formatDuration(t.durationMs));
    add(QStringLiteral("Кодек"), t.codec.toUpper());
    add(QStringLiteral("Битрейт"), t.bitrateKbps > 0 ? QStringLiteral("%1 kbps").arg(t.bitrateKbps) : QString());
    add(QStringLiteral("Частота"), t.sampleRate > 0 ? loc.toString(t.sampleRate) + QStringLiteral(" Гц") : QString());
    add(QStringLiteral("Каналы"), QString::number(t.channels));
    add(QStringLiteral("Размер файла"), t.fileSize > 0 ? loc.formattedDataSize(t.fileSize) : QString());
    if (t.isCueTrack()) {
        add(QStringLiteral("CUE"), t.cueSheet);
        add(QStringLiteral("Фрагмент"), formatDuration(t.startMs + 1) + QStringLiteral(" – ")
                                             + (t.endMs > 0 ? formatDuration(t.endMs) : QStringLiteral("конец")));
    }
    add(QStringLiteral("Путь"), t.path);
    if (t.missing)
        add(QStringLiteral("Состояние"), QStringLiteral("Файл не найден или не читается"));
    generalLayout->addLayout(form, 1);
    tabs->addTab(general, QStringLiteral("Общее"));

    // --- All tags ------------------------------------------------------------
    if (!t.isUrl()) {
        const auto tags = MetadataReader::readAllTags(t.path);
        auto *table = new QTableWidget(int(tags.size()), 2);
        table->setHorizontalHeaderLabels({QStringLiteral("Тег"), QStringLiteral("Значение")});
        table->verticalHeader()->hide();
        table->horizontalHeader()->setStretchLastSection(true);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        for (int i = 0; i < tags.size(); ++i) {
            table->setItem(i, 0, new QTableWidgetItem(tags[i].first));
            table->setItem(i, 1, new QTableWidgetItem(tags[i].second));
        }
        table->resizeColumnToContents(0);
        tabs->addTab(table, QStringLiteral("Все теги"));
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
    if (!t.isUrl()) {
        auto *openDir = buttons->addButton(QStringLiteral("Открыть папку"), QDialogButtonBox::ActionRole);
        connect(openDir, &QPushButton::clicked, this, [path = t.path] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        });
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}
