#include "core/TextDecode.h"

#include <QStringDecoder>

namespace {

// Windows-1251, bytes 0x80..0xBF (0xC0..0xFF map linearly to U+0410..U+044F).
const char16_t kCp1251High[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
};

} // namespace

QString decodeWindows1251(const QByteArray &data)
{
    QString out;
    out.reserve(data.size());
    for (char c : data) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x80)
            out.append(QChar(b));
        else if (b < 0xC0)
            out.append(QChar(kCp1251High[b - 0x80]));
        else
            out.append(QChar(char16_t(0x0410 + (b - 0xC0))));
    }
    return out;
}

QString decodeText(const QByteArray &data)
{
    if (data.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(data.mid(3));
    if (data.startsWith("\xFF\xFE")) {
        QStringDecoder dec(QStringDecoder::Utf16LE);
        return dec.decode(data.mid(2));
    }
    if (data.startsWith("\xFE\xFF")) {
        QStringDecoder dec(QStringDecoder::Utf16BE);
        return dec.decode(data.mid(2));
    }

    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8.decode(data);
    if (!utf8.hasError())
        return text;
    return decodeWindows1251(data);
}
