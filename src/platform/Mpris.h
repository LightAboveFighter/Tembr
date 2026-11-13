#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class AudioEngine;
struct Track;

// MPRIS 2 on the session bus (org.mpris.MediaPlayer2.tembr). This is what
// puts the player into GNOME's notification list / KDE's media applet with
// play-pause / next / previous and cover art, and what makes the keyboard's
// media keys work while the window is minimised.
//
// Also exports org.tembr.Tembr (Command, Open) used by "tembr --next" etc.
// and by a second instance to hand files over to the running one.
class MprisService : public QObject
{
    Q_OBJECT
public:
    static QString serviceName();
    static QString objectPath();
    static QString appInterface();

    explicit MprisService(AudioEngine *engine, QObject *parent = nullptr);

    bool registerOnBus();
    bool isRegistered() const { return m_registered; }

    // nullptr = nothing loaded. artPath: an image file for the cover, or empty.
    void setTrack(const Track *track, const QString &artPath);
    void setNavigation(bool canGoNext, bool canGoPrevious);
    void setVolume(double v);   // reflect UI changes to MPRIS clients

    // Used by the adaptors.
    QString playbackStatus() const;
    QVariantMap metadata() const { return m_metadata; }
    double volume() const { return m_volume; }
    qlonglong positionUs() const;
    bool canGoNext() const { return m_canGoNext; }
    bool canGoPrevious() const { return m_canGoPrevious; }
    bool canSeek() const;
    bool hasTrack() const { return m_hasTrack; }

signals:
    void raiseRequested();
    void quitRequested();
    void commandRequested(const QString &command);   // "play-pause", "next", ...
    void seekRequested(qint64 ms);                    // absolute, in the track
    void volumeRequested(double v);
    void openRequested(const QStringList &paths);

private:
    void notify(const QString &interface, const QVariantMap &changed);

    AudioEngine *m_engine;
    QObject *m_playerAdaptor = nullptr;
    QVariantMap m_metadata;
    double m_volume = 1.0;
    bool m_canGoNext = false;
    bool m_canGoPrevious = false;
    bool m_hasTrack = false;
    bool m_registered = false;
};
