// AudioEngine on miniaudio's null device (real time, nothing is heard).

#include "audio/AudioEngine.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// `ms` of 16-bit stereo silence at 44.1 kHz.
QString writeWav(const QTemporaryDir &dir, const QString &name, int ms)
{
    QByteArray data(44100 * ms / 1000 * 4, '\0');
    QByteArray h;
    auto u32 = [&](quint32 v) { for (int i = 0; i < 4; ++i) h.append(char(v >> (8 * i))); };
    auto u16 = [&](quint16 v) { h.append(char(v)); h.append(char(v >> 8)); };
    h.append("RIFF"); u32(36 + data.size()); h.append("WAVEfmt ");
    u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
    h.append("data"); u32(data.size());
    const QString path = dir.filePath(name);
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(h + data);
    return path;
}

// Accepts connections and never answers: an open of it blocks until interrupted.
struct SilentServer
{
    int fd = -1;
    int port = 0;
    SilentServer()
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a);
        ::listen(fd, 16);
        socklen_t len = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
        port = ntohs(a.sin_port);
    }
    ~SilentServer() { ::close(fd); }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1/live").arg(port); }
};

} // namespace

class TestAudioEngine : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_short, m_long;

private slots:
    void initTestCase()
    {
        qputenv("TEMBR_AUDIO_BACKEND", "null");
        m_short = writeWav(m_dir, QStringLiteral("short.wav"), 600);
        m_long = writeWav(m_dir, QStringLiteral("long.wav"), 3000);
    }

    void opensInTheBackground()
    {
        AudioEngine e;
        QVERIFY(e.isDeviceOk());
        QSignalSpy loaded(&e, &AudioEngine::loaded);
        e.load(m_long);
        QVERIFY(e.isLoading());
        QCOMPARE(e.durationMs(), 0);
        QVERIFY(!e.isSeekable());
        QVERIFY(loaded.wait(2000));
        QVERIFY(!e.isLoading());
        QVERIFY(qAbs(e.durationMs() - 3000) < 50);
        QVERIFY(e.isSeekable());
        QCOMPARE(e.state(), AudioEngine::State::Stopped);   // load() does not start
    }

    // A stream that never answers blocks neither load() nor unload().
    void stalledStreamNeverBlocks()
    {
        SilentServer server;
        AudioEngine e;
        QSignalSpy loaded(&e, &AudioEngine::loaded);
        QSignalSpy failed(&e, &AudioEngine::loadFailed);
        QElapsedTimer t;
        t.start();
        e.load(server.url());
        e.play();
        QVERIFY2(t.elapsed() < 100, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(e.state(), AudioEngine::State::Playing);   // what the user asked for
        QTest::qWait(200);
        t.restart();
        e.unload();
        QVERIFY2(t.elapsed() < 100, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(e.state(), AudioEngine::State::Stopped);
        QTest::qWait(200);
        QCOMPARE(loaded.count(), 0);
        QCOMPARE(failed.count(), 0);   // a load given up on reports nothing
    }

    void destroyedWhileOpening()
    {
        SilentServer server;
        QElapsedTimer t;
        t.start();
        {
            AudioEngine e;
            e.load(server.url());
            QTest::qWait(50);
        }
        QVERIFY2(t.elapsed() < 1000, qPrintable(QString::number(t.elapsed())));
    }

    void playBeforeOpenStartsAfterIt()
    {
        AudioEngine e;
        QSignalSpy finished(&e, &AudioEngine::finished);
        QElapsedTimer t;
        t.start();
        e.load(m_short);
        e.play();
        QVERIFY(finished.wait(3000));
        QVERIFY2(t.elapsed() > 450, qPrintable(QString::number(t.elapsed())));   // it really played
        QCOMPARE(e.state(), AudioEngine::State::Stopped);
    }

    void seekBeforeOpenIsKept()
    {
        AudioEngine e;
        QSignalSpy loaded(&e, &AudioEngine::loaded);
        QSignalSpy seeked(&e, &AudioEngine::seeked);
        QSignalSpy finished(&e, &AudioEngine::finished);
        e.load(m_long);
        e.seek(2500);
        QCOMPARE(e.positionMs(), 2500);
        e.play();
        QVERIFY(loaded.wait(2000));
        QCOMPARE(seeked.count(), 1);
        QVERIFY(e.positionMs() >= 2500);
        QElapsedTimer t;
        t.start();
        QVERIFY(finished.wait(3000));
        QVERIFY2(t.elapsed() < 1200, qPrintable(QString::number(t.elapsed())));   // ~0.5 s left, not 3 s
    }

    void missingFileFails()
    {
        AudioEngine e;
        QSignalSpy failed(&e, &AudioEngine::loadFailed);
        e.load(m_dir.filePath(QStringLiteral("nope.wav")));
        e.play();
        QVERIFY(failed.wait(2000));
        QVERIFY(!failed.first().first().toString().isEmpty());
        QVERIFY(!e.isLoaded());
        QCOMPARE(e.state(), AudioEngine::State::Stopped);
    }

    void disallowedSchemeFails()
    {
        AudioEngine e;
        QSignalSpy failed(&e, &AudioEngine::loadFailed);
        e.load(QStringLiteral("ftp://127.0.0.1/a.mp3"));
        QVERIFY(failed.wait(2000));
    }

    // Switching tracks fast: only the last load reports, once.
    void onlyTheLastLoadReports()
    {
        SilentServer server;
        AudioEngine e;
        QSignalSpy loaded(&e, &AudioEngine::loaded);
        QSignalSpy failed(&e, &AudioEngine::loadFailed);
        for (int i = 0; i < 20; ++i)
            e.load(i % 2 ? server.url() : m_long);
        e.load(m_short);
        QVERIFY(loaded.wait(2000));
        QTest::qWait(300);
        QCOMPARE(loaded.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(qAbs(e.durationMs() - 600) < 50);
    }

    void cueRange()
    {
        AudioEngine e;
        QSignalSpy loaded(&e, &AudioEngine::loaded);
        QSignalSpy finished(&e, &AudioEngine::finished);
        e.load(m_long, 1000, 1500);
        QVERIFY(loaded.wait(2000));
        QCOMPARE(e.durationMs(), 500);
        QElapsedTimer t;
        t.start();
        e.play();
        QVERIFY(finished.wait(3000));
        QVERIFY2(t.elapsed() > 400 && t.elapsed() < 1400, qPrintable(QString::number(t.elapsed())));
    }

    void stopAndPlayAgainFromTheStart()
    {
        AudioEngine e;
        QSignalSpy loaded(&e, &AudioEngine::loaded);
        e.load(m_long);
        e.play();
        QVERIFY(loaded.wait(2000));
        QTest::qWait(400);
        QVERIFY(e.positionMs() > 200);
        e.stop();
        QCOMPARE(e.positionMs(), 0);
        QCOMPARE(e.state(), AudioEngine::State::Stopped);
        e.play();
        QTest::qWait(100);
        QVERIFY2(e.positionMs() < 300, qPrintable(QString::number(e.positionMs())));
    }
};

QTEST_GUILESS_MAIN(TestAudioEngine)
#include "test_audioengine.moc"
