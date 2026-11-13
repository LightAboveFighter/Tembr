#pragma once

#include <QByteArray>
#include <QString>

// Decodes text files written by Windows players: honours UTF-8/UTF-16 BOMs,
// otherwise tries strict UTF-8 and falls back to Windows-1251 (the usual
// encoding of old Russian .m3u/.cue files).
QString decodeText(const QByteArray &data);

QString decodeWindows1251(const QByteArray &data);
