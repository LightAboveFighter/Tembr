#include "platform/Mpris.h"

#include "audio/AudioEngine.h"
#include "core/Track.h"

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QUrl>

namespace {

QString trackObjectPath(quint64 uid)
{
    return QStringLiteral("/org/tembr/track/%1").arg(uid);
}

} // namespace

// --- org.mpris.MediaPlayer2 ------------------------------------------------------

class MprisRootAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
    explicit MprisRootAdaptor(MprisService *s)
        : QDBusAbstractAdaptor(s)
        , m_s(s)
    {
    }

    bool canQuit() const { return true; }
    bool canRaise() const { return true; }
    bool hasTrackList() const { return false; }
    QString identity() const { return QStringLiteral("Tembr"); }
    QString desktopEntry() const { return QStringLiteral("tembr"); }
    QStringList supportedUriSchemes() const { return {QStringLiteral("file"), QStringLiteral("http"), QStringLiteral("https")}; }
    QStringList supportedMimeTypes() const
    {
        return {QStringLiteral("audio/mpeg"), QStringLiteral("audio/flac"), QStringLiteral("audio/ogg"),
                QStringLiteral("audio/x-vorbis+ogg"), QStringLiteral("audio/opus"), QStringLiteral("audio/mp4"),
                QStringLiteral("audio/x-wav"), QStringLiteral("audio/x-ape"), QStringLiteral("audio/x-wavpack"),
                QStringLiteral("audio/x-ms-wma"), QStringLiteral("audio/x-mpegurl"), QStringLiteral("audio/x-scpls")};
    }

public slots:
    void Raise() { emit m_s->raiseRequested(); }
    void Quit() { emit m_s->quitRequested(); }

private:
    MprisService *m_s;
};

// --- org.mpris.MediaPlayer2.Player -----------------------------------------------

class MprisPlayerAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ rate)
    Q_PROPERTY(double MaximumRate READ rate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPlay)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    explicit MprisPlayerAdaptor(MprisService *s)
        : QDBusAbstractAdaptor(s)
        , m_s(s)
    {
    }

    QString playbackStatus() const { return m_s->playbackStatus(); }
    double rate() const { return 1.0; }
    void setRate(double) {}
    QVariantMap metadata() const { return m_s->metadata(); }
    double volume() const { return m_s->volume(); }
    void setVolume(double v) { emit m_s->volumeRequested(qBound(0.0, v, 1.0)); }
    qlonglong position() const { return m_s->positionUs(); }
    bool canGoNext() const { return m_s->canGoNext(); }
    bool canGoPrevious() const { return m_s->canGoPrevious(); }
    bool canPlay() const { return m_s->hasTrack() || m_s->canGoNext(); }
    bool canSeek() const { return m_s->canSeek(); }
    bool canControl() const { return true; }

public slots:
    void Next() { emit m_s->commandRequested(QStringLiteral("next")); }
    void Previous() { emit m_s->commandRequested(QStringLiteral("previous")); }
    void Pause() { emit m_s->commandRequested(QStringLiteral("pause")); }
    void PlayPause() { emit m_s->commandRequested(QStringLiteral("play-pause")); }
    void Stop() { emit m_s->commandRequested(QStringLiteral("stop")); }
    void Play() { emit m_s->commandRequested(QStringLiteral("play")); }
    void Seek(qlonglong offsetUs) { emit m_s->seekRequested((m_s->positionUs() + offsetUs) / 1000); }
    void SetPosition(const QDBusObjectPath &trackId, qlonglong positionUs)
    {
        if (trackId.path() == m_s->metadata().value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>().path())
            emit m_s->seekRequested(positionUs / 1000);
    }
    void OpenUri(const QString &uri)
    {
        const QUrl url(uri);
        emit m_s->openRequested({url.isLocalFile() ? url.toLocalFile() : uri});
    }

signals:
    void Seeked(qlonglong position);

private:
    MprisService *m_s;
};

// --- org.tembr.Tembr: command line and single instance ---------------------------

class TembrAppAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tembr.Tembr")

public:
    explicit TembrAppAdaptor(MprisService *s)
        : QDBusAbstractAdaptor(s)
        , m_s(s)
    {
    }

public slots:
    void Command(const QString &name) { emit m_s->commandRequested(name); }
    void Open(const QStringList &paths) { emit m_s->openRequested(paths); }

private:
    MprisService *m_s;
};

// --- MprisService ------------------------------------------------------------------

QString MprisService::serviceName()
{
    return QStringLiteral("org.mpris.MediaPlayer2.tembr");
}

QString MprisService::objectPath()
{
    return QStringLiteral("/org/mpris/MediaPlayer2");
}

QString MprisService::appInterface()
{
    return QStringLiteral("org.tembr.Tembr");
}

MprisService::MprisService(AudioEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
    new MprisRootAdaptor(this);
    m_playerAdaptor = new MprisPlayerAdaptor(this);
    new TembrAppAdaptor(this);
    setTrack(nullptr, {});

    connect(engine, &AudioEngine::stateChanged, this, [this] {
        notify(QStringLiteral("org.mpris.MediaPlayer2.Player"),
               {{QStringLiteral("PlaybackStatus"), playbackStatus()}});
    });
    connect(engine, &AudioEngine::durationChanged, this, [this] {
        notify(QStringLiteral("org.mpris.MediaPlayer2.Player"), {{QStringLiteral("CanSeek"), canSeek()}});
    });
    connect(engine, &AudioEngine::seeked, this, [this](qint64 ms) {
        emit static_cast<MprisPlayerAdaptor *>(m_playerAdaptor)->Seeked(ms * 1000);
    });
}

bool MprisService::registerOnBus()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;
    if (!bus.registerObject(objectPath(), this))
        return false;
    m_registered = bus.registerService(serviceName());
    return m_registered;
}

void MprisService::unregisterFromBus()
{
    if (!m_registered)
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.unregisterService(serviceName());
    bus.unregisterObject(objectPath());
    m_registered = false;
}

void MprisService::setTrack(const Track *t, const QString &artPath)
{
    QVariantMap m;
    m_hasTrack = t != nullptr;
    if (!t) {
        m.insert(QStringLiteral("mpris:trackid"),
                 QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack"))));
    } else {
        m.insert(QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(trackObjectPath(t->uid))));
        if (t->durationMs > 0)
            m.insert(QStringLiteral("mpris:length"), qlonglong(t->durationMs) * 1000);
        m.insert(QStringLiteral("xesam:title"), t->title.isEmpty() ? t->fileName() : t->title);
        if (!t->artist.isEmpty())
            m.insert(QStringLiteral("xesam:artist"), QStringList{t->artist});
        if (!t->album.isEmpty())
            m.insert(QStringLiteral("xesam:album"), t->album);
        if (!t->albumArtist.isEmpty())
            m.insert(QStringLiteral("xesam:albumArtist"), QStringList{t->albumArtist});
        if (!t->genre.isEmpty())
            m.insert(QStringLiteral("xesam:genre"), QStringList{t->genre});
        if (t->trackNo > 0)
            m.insert(QStringLiteral("xesam:trackNumber"), t->trackNo);
        if (t->discNo > 0)
            m.insert(QStringLiteral("xesam:discNumber"), t->discNo);
        m.insert(QStringLiteral("xesam:url"),
                 t->isUrl() ? t->path : QUrl::fromLocalFile(t->path).toString(QUrl::FullyEncoded));
        if (!artPath.isEmpty())
            m.insert(QStringLiteral("mpris:artUrl"), QUrl::fromLocalFile(artPath).toString(QUrl::FullyEncoded));
    }
    if (m == m_metadata)
        return;   // refreshed with nothing new (tags of other tracks arrived)
    m_metadata = m;
    notify(QStringLiteral("org.mpris.MediaPlayer2.Player"),
           {{QStringLiteral("Metadata"), m_metadata},
            {QStringLiteral("PlaybackStatus"), playbackStatus()},
            {QStringLiteral("CanPlay"), m_hasTrack || m_canGoNext},
            {QStringLiteral("CanPause"), m_hasTrack || m_canGoNext}});
}

void MprisService::setNavigation(bool canGoNext, bool canGoPrevious)
{
    if (canGoNext == m_canGoNext && canGoPrevious == m_canGoPrevious)
        return;
    m_canGoNext = canGoNext;
    m_canGoPrevious = canGoPrevious;
    notify(QStringLiteral("org.mpris.MediaPlayer2.Player"),
           {{QStringLiteral("CanGoNext"), canGoNext}, {QStringLiteral("CanGoPrevious"), canGoPrevious}});
}

void MprisService::setVolume(double v)
{
    if (qFuzzyCompare(v + 1.0, m_volume + 1.0))
        return;
    m_volume = v;
    notify(QStringLiteral("org.mpris.MediaPlayer2.Player"), {{QStringLiteral("Volume"), v}});
}

QString MprisService::playbackStatus() const
{
    switch (m_engine->state()) {
    case AudioEngine::State::Playing: return QStringLiteral("Playing");
    case AudioEngine::State::Paused: return QStringLiteral("Paused");
    case AudioEngine::State::Stopped: break;
    }
    return QStringLiteral("Stopped");
}

qlonglong MprisService::positionUs() const
{
    return qlonglong(m_engine->positionMs()) * 1000;
}

bool MprisService::canSeek() const
{
    return m_engine->isSeekable();
}

void MprisService::notify(const QString &interface, const QVariantMap &changed)
{
    if (!m_registered)
        return;
    QDBusMessage msg = QDBusMessage::createSignal(objectPath(), QStringLiteral("org.freedesktop.DBus.Properties"),
                                                  QStringLiteral("PropertiesChanged"));
    msg << interface << changed << QStringList();
    QDBusConnection::sessionBus().send(msg);
}

#include "Mpris.moc"
