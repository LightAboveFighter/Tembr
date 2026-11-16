#include "ui/PlaybackController.h"

#include "audio/AudioEngine.h"
#include "ui/PlaylistModel.h"

#include <QFileInfo>
#include <QTimer>

PlaybackController::PlaybackController(AudioEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
    m_engine->setVolume(m_volume / 100.0f);
    connect(m_engine, &AudioEngine::stateChanged, this, [this](AudioEngine::State s) {
        if (m_model)
            m_model->setPaused(s == AudioEngine::State::Paused);
    });
    connect(m_engine, &AudioEngine::finished, this, [this] { next(); });
    connect(m_engine, &AudioEngine::loaded, this, &PlaybackController::opened);
    connect(m_engine, &AudioEngine::loadFailed, this, &PlaybackController::openFailed);
}

const Track *PlaybackController::currentTrack() const
{
    return m_model ? m_model->currentTrack() : nullptr;
}

void PlaybackController::watch(PlaylistModel *model)
{
    if (model == m_model)
        return;
    for (const auto &c : std::as_const(m_modelConnections))
        disconnect(c);
    m_modelConnections.clear();
    m_model = model;
    if (!model)
        return;
    m_modelConnections << connect(model, &PlaylistModel::currentDetached, this, [this, model] {
        if (m_engine->state() == AudioEngine::State::Stopped) {
            model->setCurrent(0);   // nothing playing: nothing to go on from
            emit currentChanged();
        } else {
            emit navigationChanged();   // plays on, shown as before; "next" is decided by the model
        }
    });
    m_modelConnections << connect(model, &QAbstractItemModel::dataChanged, this,
                                  [this, model](const QModelIndex &tl, const QModelIndex &br) {
                                      const int cur = model->currentRow();
                                      if (cur >= tl.row() && cur <= br.row())
                                          emit currentTagsChanged();
                                  });
}

void PlaybackController::playRow(PlaylistModel *model, int row, bool start)
{
    if (!model || row < 0 || row >= model->rowCount())
        return;
    const Track t = model->track(row);
    if (m_model && m_model != model)
        m_model->setCurrent(0);
    watch(model);
    model->setCurrent(t.uid);

    // Opens in the background: opened() or openFailed() follows.
    m_engine->load(t.path, t.startMs, t.endMs);
    if (start)
        m_engine->play();   // starts as soon as the file is open
    emit currentChanged();
    emit trackStarted(t);
}

void PlaybackController::setCurrent(PlaylistModel *model, int row)
{
    if (m_model && m_model != model)
        m_model->setCurrent(0);
    watch(model);
    if (model)
        model->setCurrent(row >= 0 && row < model->rowCount() ? model->track(row).uid : 0);
    emit currentChanged();
}

void PlaybackController::release(PlaylistModel *model)
{
    if (!model || model != m_model)
        return;
    m_engine->unload();
    watch(nullptr);
    emit currentChanged();
}

bool PlaybackController::pauseOrResume()
{
    switch (m_engine->state()) {
    case AudioEngine::State::Playing:
        m_engine->pause();
        return true;
    case AudioEngine::State::Paused:
        m_engine->play();
        return true;
    case AudioEngine::State::Stopped:
        break;
    }
    return false;
}

void PlaybackController::pause()
{
    m_engine->pause();
}

void PlaybackController::stop()
{
    m_engine->stop();
}

void PlaybackController::next(PlaylistModel *fallback)
{
    PlaylistModel *model = m_model ? m_model.data() : fallback;
    if (!model)
        return;
    // Also from a track whose row has gone (deleted): see PlaylistModel::nextRow().
    const int row = model->currentUid() ? model->nextRow() : (model->rowCount() > 0 ? 0 : -1);
    if (row < 0) {
        m_engine->stop();   // end of playlist
        return;
    }
    playRow(model, row);
    emit followRow(model, row);
}

void PlaybackController::previous(PlaylistModel *fallback)
{
    if (m_engine->positionMs() > 3000) {
        m_engine->seek(0);
        return;
    }
    PlaylistModel *model = m_model ? m_model.data() : fallback;
    if (!model)
        return;
    const int row = model->currentUid() ? model->previousRow() : -1;
    if (row >= 0) {
        playRow(model, row);
        emit followRow(model, row);
    }
}

void PlaybackController::opened()
{
    m_failStreak = 0;
    if (!m_model)
        return;
    const int row = m_model->currentRow();
    if (row < 0)
        return;
    m_model->setMissing(row, false);
    emit trackOpened(m_model->track(row));
}

void PlaybackController::openFailed(const QString &error, bool wasPlaying)
{
    PlaylistModel *model = m_model;
    const int row = model ? model->currentRow() : -1;
    if (row >= 0) {
        model->setMissing(row, true);
        emit message(QStringLiteral("%1: %2").arg(model->track(row).fileName(), error));
    } else {
        emit message(error);
    }
    // Skip to the next track, but don't spin through a playlist of dead files,
    // and not when the user stopped or paused while it was opening.
    if (model && wasPlaying && ++m_failStreak < model->rowCount())
        QTimer::singleShot(0, this, [this] { next(); });
    else
        m_failStreak = 0;
    emit currentChanged();
}

void PlaybackController::skipDeleted(const QSet<QString> &gone)
{
    PlaylistModel *model = m_model;
    if (!model)
        return;
    int row = -1;
    if (model->isCurrentDetached()) {
        row = model->nextRow();   // its row was removed: the first survivor after it
    } else {
        for (int r = model->currentRow() + 1; r < model->rowCount() && row < 0; ++r) {
            if (!gone.contains(model->track(r).path))
                row = r;   // still listed (Local files, marked missing): skip deleted ones
        }
    }
    const AudioEngine::State state = m_engine->state();
    if (row >= 0 && state != AudioEngine::State::Stopped) {
        playRow(model, row, state == AudioEngine::State::Playing);   // paused: stays stopped there
        emit followRow(model, row);
    } else if (row < 0) {
        m_engine->stop();   // nothing after it
        model->setCurrent(0);
        emit currentChanged();
    }
}

bool PlaybackController::hasNext(PlaylistModel *fallback) const
{
    if (m_model && m_model->currentUid())
        return m_model->nextRow() >= 0;
    return fallback && fallback->rowCount() > 0;
}

bool PlaybackController::hasPrevious() const
{
    return m_model && m_model->currentUid() && m_model->previousRow() >= 0;
}

void PlaybackController::setVolume(int volume)
{
    volume = qBound(0, volume, 100);
    if (volume == m_volume)
        return;
    m_volume = volume;
    m_engine->setVolume(volume / 100.0f);
    emit volumeChanged(volume);
}

void PlaybackController::changeVolume(int delta)
{
    setVolume(m_volume + delta);
}
