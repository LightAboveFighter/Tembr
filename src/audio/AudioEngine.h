#pragma once

#include <QObject>
#include <QString>

#include <memory>

// Playback engine: FFmpeg opens and decodes on a worker thread into a ring
// buffer, miniaudio pulls from it on the audio thread (ALSA / PulseAudio /
// PipeWire / JACK, picked at runtime). Everything is resampled to stereo
// float at the device's native rate. All public methods are for the GUI
// thread and never wait for a file or a stream.
//
// TEMBR_AUDIO_BACKEND=null in the environment plays into miniaudio's null
// device (it keeps real time, nothing is heard): for tests.
class AudioEngine : public QObject
{
    Q_OBJECT
public:
    // What the user asked for. Playing also while the file is still being
    // opened: the sound starts as soon as it is (see isLoading()).
    enum class State { Stopped, Playing, Paused };
    Q_ENUM(State)

    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine() override;

    bool isDeviceOk() const;
    QString deviceName() const;

    // Starts opening a file (or URL) in the background and returns at once;
    // loaded() or loadFailed() follows, unless another load() or unload()
    // comes first. [startMs, endMs) restricts playback to a range, used for
    // CUE tracks; endMs == 0 plays to the end. Does not start playing, but
    // play(), pause(), stop() and seek() may be called right away.
    void load(const QString &path, qint64 startMs = 0, qint64 endMs = 0);
    void unload();

    void play();
    void pause();
    void togglePause();
    void stop();
    void seek(qint64 ms);        // relative to the track start

    void setVolume(float v);     // 0..1, perceptual (cubic) curve
    float volume() const;

    State state() const { return m_state; }
    bool isLoaded() const;       // load() was called and has not failed
    bool isLoading() const;      // ... and the file is still being opened
    qint64 positionMs() const;   // relative to the track start
    qint64 durationMs() const;   // 0 while loading
    bool isSeekable() const;     // false while loading
    QString lastError() const { return m_lastError; }

signals:
    void loaded();                            // the file is open: duration and seekability are known
    // The engine is unloaded. `wasPlaying`: the user still wanted it to play
    // (not stopped or paused while it was opening).
    void loadFailed(const QString &message, bool wasPlaying);
    void stateChanged(AudioEngine::State state);
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void seeked(qint64 ms);      // position jumped (user seek), relative to the track start
    void finished();             // reached the end of the track
    void errorOccurred(const QString &message);   // playback trouble other than opening

private:
    void setState(State s);
    void poll();
    void checkOpened();
    bool startDevice();
    void haltDevice();

    struct Impl;
    std::unique_ptr<Impl> d;
    State m_state = State::Stopped;
    QString m_lastError;
};
