#include "ui/NowPlayingPanel.h"

#include "core/Track.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

CoverView::CoverView(QWidget *parent)
    : QWidget(parent)
{
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    setMinimumSize(160, 160);
}

void CoverView::setImage(const QImage &image)
{
    m_image = image;
    m_scaled = QPixmap();
    update();
}

void CoverView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int side = qMin(width(), height());
    const QRectF box((width() - side) / 2.0, 0, side, side);
    QPainterPath clip;
    clip.addRoundedRect(box, 10, 10);
    p.setClipPath(clip);

    if (m_image.isNull()) {
        p.fillRect(box, palette().color(QPalette::AlternateBase));
        const int iconSide = side / 3;
        const QRect iconRect(int(box.center().x()) - iconSide / 2, int(box.center().y()) - iconSide / 2, iconSide,
                             iconSide);
        QColor c = Theme::dimText();
        c.setAlphaF(0.5);
        Icons::make(Icons::Kind::Note, c).paint(&p, iconRect);
        return;
    }
    if (m_scaled.isNull() || m_scaled.width() != side * devicePixelRatioF()) {
        // Fill the square (crop), as album art is nearly always square anyway.
        const QSize target = QSize(side, side) * devicePixelRatioF();
        QImage img = m_image.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        img = img.copy((img.width() - target.width()) / 2, (img.height() - target.height()) / 2, target.width(),
                       target.height());
        m_scaled = QPixmap::fromImage(img);
        m_scaled.setDevicePixelRatio(devicePixelRatioF());
    }
    p.drawPixmap(box.topLeft(), m_scaled);
}

NowPlayingPanel::NowPlayingPanel(QWidget *parent)
    : QWidget(parent)
    , m_cover(new CoverView(this))
    , m_title(new QLabel(this))
    , m_artist(new QLabel(this))
    , m_album(new QLabel(this))
    , m_details(new QLabel(this))
{
    m_title->setObjectName(QStringLiteral("NowPlayingTitle"));
    m_artist->setObjectName(QStringLiteral("NowPlayingArtist"));
    m_album->setObjectName(QStringLiteral("NowPlayingDetails"));
    m_details->setObjectName(QStringLiteral("NowPlayingDetails"));

    QFont f = font();
    QFont title = f;
    title.setPointSizeF(f.pointSizeF() * 1.6);
    title.setBold(true);
    m_title->setFont(title);
    QFont artist = f;
    artist.setPointSizeF(f.pointSizeF() * 1.25);
    m_artist->setFont(artist);
    QFont album = f;
    album.setPointSizeF(f.pointSizeF() * 1.05);
    m_album->setFont(album);

    for (QLabel *l : {m_title, m_artist, m_album, m_details}) {
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(4);
    layout->addWidget(m_cover);
    layout->addSpacing(10);
    layout->addWidget(m_title);
    layout->addWidget(m_artist);
    layout->addWidget(m_album);
    layout->addSpacing(4);
    layout->addWidget(m_details);
    layout->addStretch(1);
    clear();
}

void NowPlayingPanel::showTrack(const Track &t, const QImage &cover)
{
    m_cover->setImage(cover);
    m_title->setText(t.title.isEmpty() ? t.fileName() : t.title);
    m_artist->setText(t.artist);
    m_artist->setVisible(!t.artist.isEmpty());
    QString album = t.album;
    if (!t.year.isEmpty())
        album = album.isEmpty() ? t.year : QStringLiteral("%1 · %2").arg(album, t.year);
    m_album->setText(album);
    m_album->setVisible(!album.isEmpty());
    m_details->setText(t.formatSummary());
}

void NowPlayingPanel::clear()
{
    m_cover->setImage({});
    m_title->setText(QStringLiteral("Ничего не играет"));
    m_artist->clear();
    m_artist->hide();
    m_album->clear();
    m_album->hide();
    m_details->clear();
}
