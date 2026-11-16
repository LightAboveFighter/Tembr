#include "ui/TransportBar.h"

#include "audio/AudioEngine.h"
#include "core/Track.h"
#include "ui/Icons.h"
#include "ui/PlaybackController.h"
#include "ui/Theme.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>

TransportBar::TransportBar(PlaybackController *playback, const Actions &actions, QWidget *parent)
    : QWidget(parent)
    , m_playback(playback)
    , m_actions(actions)
{
    setObjectName(QStringLiteral("TransportBar"));   // styled by the theme
    setAttribute(Qt::WA_StyledBackground);
    auto *controls = new QHBoxLayout(this);
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
    makeButton(m_actions.previous, 22);
    m_playButton = makeButton(m_actions.play, 42);
    makeButton(m_actions.next, 22);
    makeButton(m_actions.stop, 20);
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

    AudioEngine *engine = m_playback->engine();
    connect(engine, &AudioEngine::stateChanged, this, [this](AudioEngine::State s) {
        m_actions.play->setText(s == AudioEngine::State::Playing ? QStringLiteral("Пауза")
                                                                 : QStringLiteral("Воспроизвести"));
        refreshIcons();
    });
    connect(engine, &AudioEngine::positionChanged, this, [this](qint64 ms) {
        if (!m_seek->isSliderDown()) {   // the user is choosing a place
            m_seek->setValue(int(ms));
            m_position->setText(formatDuration(ms + 1));
        }
    });
    connect(engine, &AudioEngine::durationChanged, this, [this, engine](qint64 ms) {
        m_seek->setRange(0, int(ms));
        m_seek->setEnabled(engine->isSeekable());
        m_duration->setText(formatDuration(ms + 1));
    });
    connect(m_seek, &QSlider::sliderReleased, this, [this, engine] { engine->seek(m_seek->value()); });
    connect(m_seek, &QSlider::sliderMoved, this, [this](int v) { m_position->setText(formatDuration(v + 1)); });

    showVolume(m_playback->volume());
    connect(m_volume, &QSlider::valueChanged, m_playback, &PlaybackController::setVolume);
    connect(m_playback, &PlaybackController::volumeChanged, this, &TransportBar::showVolume);
    refreshIcons();
}

void TransportBar::showVolume(int volume)
{
    m_volume->setValue(volume);
    m_volume->setToolTip(QStringLiteral("Громкость: %1%").arg(volume));
}

void TransportBar::refreshIcons()
{
    const QColor c = Theme::iconColor();
    const bool playing = m_playback->engine()->state() == AudioEngine::State::Playing;
    m_actions.previous->setIcon(Icons::make(Icons::Kind::Previous, c));
    m_actions.next->setIcon(Icons::make(Icons::Kind::Next, c));
    m_actions.stop->setIcon(Icons::make(Icons::Kind::Stop, c));
    m_actions.play->setIcon(Icons::accentButton(playing ? Icons::Kind::Pause : Icons::Kind::Play));
    m_volumeIcon->setPixmap(Icons::make(Icons::Kind::Volume, c).pixmap(18, 18));
}
