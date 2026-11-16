#include "core/MediaOpen.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

namespace {

// One second of 16-bit stereo silence.
QByteArray wav()
{
    QByteArray data(44100 * 4, '\0');
    QByteArray h;
    auto u32 = [&](quint32 v) { for (int i = 0; i < 4; ++i) h.append(char(v >> (8 * i))); };
    auto u16 = [&](quint16 v) { h.append(char(v)); h.append(char(v >> 8)); };
    h.append("RIFF"); u32(36 + data.size()); h.append("WAVEfmt ");
    u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
    h.append("data"); u32(data.size());
    return h + data;
}

QString writeWav(const QTemporaryDir &dir)
{
    const QString path = dir.filePath(QStringLiteral("silence.wav"));
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(wav());
    return path;
}

// A server that accepts connections and never answers.
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
        ::listen(fd, 4);   // the kernel completes the handshake; nothing is ever sent
        socklen_t len = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
        port = ntohs(a.sin_port);
    }
    ~SilentServer() { ::close(fd); }
};

} // namespace

class TestMediaOpen : public QObject
{
    Q_OBJECT

private slots:
    void onlyFilesAndHttp()
    {
        QVERIFY(MediaOpen::isAllowed(QStringLiteral("/m/a.mp3")));
        QVERIFY(MediaOpen::isAllowed(QStringLiteral("file:///m/a.mp3")));
        QVERIFY(MediaOpen::isAllowed(QStringLiteral("https://radio.example/live")));
        QVERIFY(MediaOpen::isAllowed(QStringLiteral("HTTP://radio.example/live")));
        QVERIFY(!MediaOpen::isAllowed(QStringLiteral("ftp://host/a.mp3")));
        QVERIFY(!MediaOpen::isAllowed(QStringLiteral("rtmp://host/live")));
        QVERIFY(!MediaOpen::isAllowed(QStringLiteral("smb://host/share/a.mp3")));

        MediaOpen::Interrupt in;
        AVFormatContext *ctx = nullptr;
        QVERIFY(MediaOpen::open(&ctx, QStringLiteral("ftp://127.0.0.1/a.mp3"), &in) < 0);
        QVERIFY(!ctx);
        // Not a URL, so only the file protocol: FFmpeg's own prefixes are
        // refused, even for a file that opens fine on its own.
        QTemporaryDir dir;
        const QString path = writeWav(dir);
        QCOMPARE(MediaOpen::open(&ctx, path, &in), 0);
        avformat_close_input(&ctx);
        QVERIFY(MediaOpen::open(&ctx, QStringLiteral("concat:%1|%1").arg(path), &in) < 0);
        QVERIFY(MediaOpen::open(&ctx, QStringLiteral("subfile:,start,0,end,0,:%1").arg(path), &in) < 0);
        QVERIFY(!ctx);
    }

    void opensLocalFiles()
    {
        QTemporaryDir dir;
        const QString path = writeWav(dir);
        MediaOpen::Interrupt in;
        AVFormatContext *ctx = nullptr;
        QCOMPARE(MediaOpen::open(&ctx, path, &in), 0);
        QVERIFY(ctx);
        QCOMPARE(int(ctx->nb_streams), 1);
        QCOMPARE(in.deadlineMs.load(), 0);   // no deadline left for later reads
        avformat_close_input(&ctx);
    }

    // A server that never answers: opening gives up after the timeout
    // instead of blocking the caller (the GUI thread for the player).
    void silentServerTimesOut()
    {
        SilentServer server;
        MediaOpen::Interrupt in;
        AVFormatContext *ctx = nullptr;
        QElapsedTimer t;
        t.start();
        QVERIFY(MediaOpen::open(&ctx, QStringLiteral("http://127.0.0.1:%1/live").arg(server.port), &in, true, 700) < 0);
        QVERIFY2(t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
        QVERIFY(!ctx);
    }

    // The stop flag interrupts a blocking open at once.
    void stopFlagInterrupts()
    {
        SilentServer server;
        std::atomic<bool> quit{false};
        MediaOpen::Interrupt in;
        in.quit = &quit;
        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            quit = true;
        });
        AVFormatContext *ctx = nullptr;
        QElapsedTimer t;
        t.start();
        const int r = MediaOpen::open(&ctx, QStringLiteral("http://127.0.0.1:%1/live").arg(server.port), &in, true, 60000);
        stopper.join();
        QVERIFY(r < 0);
        QVERIFY2(t.elapsed() < 5000, qPrintable(QString::number(t.elapsed())));
    }
};

QTEST_GUILESS_MAIN(TestMediaOpen)
#include "test_mediaopen.moc"
