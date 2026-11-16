#pragma once

// Short audio files for tests that play something on miniaudio's null device
// (it keeps real time: long files make slow tests).

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace TestAudio {

// `ms` of 16-bit stereo silence at 44.1 kHz, written to `path`.
inline QString writeWav(const QString &path, int ms)
{
    QByteArray data(44100 * ms / 1000 * 4, '\0');
    QByteArray h;
    auto u32 = [&](quint32 v) { for (int i = 0; i < 4; ++i) h.append(char(v >> (8 * i))); };
    auto u16 = [&](quint16 v) { h.append(char(v)); h.append(char(v >> 8)); };
    h.append("RIFF"); u32(36 + data.size()); h.append("WAVEfmt ");
    u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
    h.append("data"); u32(data.size());
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(h + data);
    return path;
}

} // namespace TestAudio
