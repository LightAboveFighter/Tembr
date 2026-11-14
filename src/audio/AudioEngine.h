#pragma once

#include <QObject>
#include <QString>

#include <memory>

// Playback engine: FFmpeg decodes on a worker thread into a ring buffer,
// miniaudio pulls from it on the audio thread (ALSA / PulseAudio / PipeWire /
// JACK, picked at runtime). Everything is resampled to stereo float at the
// device's native rate. All public methods are for the GUI thread.
class AudioEngine : public QObject
{
    Q_OBJECT
public:
    enum class State { Stopped, Playing, Paused };
    Q_ENUM(State)

    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine() override;

    bool isDeviceOk() const;
    QString deviceName() const;

    // Opens a file (or URL). [startMs, endMs) restricts playback to a range,
    // used for CUE tracks; endMs == 0 plays to the end. Does not start playing.
    bool load(const QString &path, qint64 startMs = 0, qint64 endMs = 0);
    void unload();

    void play();
    void pause();
    void togglePause();
    void stop();
    void seek(qint64 ms);        // relative to the track start

    void setVolume(float v);     // 0..1, perceptual (cubic) curve
    float volume() const;

    State state() const { return m_state; }
    qint64 positionMs() const;   // relative to the track start
    qint64 durationMs() const;
    bool isSeekable() const;
    QString lastError() const { return m_lastError; }

signals:
    void stateChanged(AudioEngine::State state);
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void seeked(qint64 ms);      // position jumped (user seek), relative to the track start
    void finished();             // reached the end of the track
    void errorOccurred(const QString &message);

private:
    void setState(State s);
    void poll();
    void haltDevice();

    struct Impl;
    std::unique_ptr<Impl> d;
    State m_state = State::Stopped;
    QString m_lastError;
};
