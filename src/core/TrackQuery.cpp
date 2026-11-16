#include "core/TrackQuery.h"

#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

namespace {

QString fold(QString s)
{
    s = s.toCaseFolded();
    s.replace(QChar(0x0451), QChar(0x0435)); // ё -> е
    return s;
}

// fold(hay).contains(folded), without a folded copy of every field of every
// track on each keystroke: Qt's case-insensitive search folds as it goes.
// Only text with "ё" needs the copy (it is searched as "е").
bool containsFolded(QStringView hay, const QString &folded)
{
    if (hay.contains(QChar(0x0451)) || hay.contains(QChar(0x0401)))
        return fold(hay.toString()).contains(folded);
    return hay.contains(folded, Qt::CaseInsensitive);
}

// Track::fileName() without a QFileInfo for every track on every keystroke.
QStringView fileNameOf(const Track &t)
{
    if (t.isUrl())
        return t.path;
    return QStringView(t.path).mid(t.path.lastIndexOf(QLatin1Char('/')) + 1);
}

const QHash<QString, TrackQuery::Field> &fieldAliases()
{
    using F = TrackQuery::Field;
    static const QHash<QString, F> map = {
        {QStringLiteral("title"), F::Title},        {QStringLiteral("t"), F::Title},
        {QStringLiteral("название"), F::Title},
        {QStringLiteral("artist"), F::Artist},      {QStringLiteral("a"), F::Artist},
        {QStringLiteral("исполнитель"), F::Artist},
        {QStringLiteral("album"), F::Album},        {QStringLiteral("al"), F::Album},
        {QStringLiteral("альбом"), F::Album},
        {QStringLiteral("albumartist"), F::AlbumArtist}, {QStringLiteral("aa"), F::AlbumArtist},
        {QStringLiteral("genre"), F::Genre},        {QStringLiteral("g"), F::Genre},
        {QStringLiteral("жанр"), F::Genre},
        {QStringLiteral("year"), F::Year},          {QStringLiteral("y"), F::Year},
        {QStringLiteral("год"), F::Year},
        {QStringLiteral("path"), F::Path},          {QStringLiteral("file"), F::Path},
        {QStringLiteral("p"), F::Path},             {QStringLiteral("путь"), F::Path},
        {QStringLiteral("codec"), F::Codec},        {QStringLiteral("format"), F::Codec},
        {QStringLiteral("f"), F::Codec},            {QStringLiteral("формат"), F::Codec},
        {QStringLiteral("composer"), F::Composer},  {QStringLiteral("композитор"), F::Composer},
        {QStringLiteral("comment"), F::Comment},    {QStringLiteral("комментарий"), F::Comment},
    };
    return map;
}

// Splits on whitespace, keeping "quoted phrases" (also after "key:") whole.
QStringList tokenize(const QString &text)
{
    QStringList out;
    QString cur;
    bool inQuotes = false;
    for (QChar c : text) {
        if (c == QLatin1Char('"')) {
            inQuotes = !inQuotes;
        } else if (c.isSpace() && !inQuotes) {
            if (!cur.isEmpty())
                out.append(cur);
            cur.clear();
        } else {
            cur.append(c);
        }
    }
    if (!cur.isEmpty())
        out.append(cur);
    return out;
}

QString fieldValue(TrackQuery::Field f, const Track &t)
{
    using F = TrackQuery::Field;
    switch (f) {
    case F::Title: return t.title.isEmpty() ? t.fileName() : t.title;
    case F::Artist: return t.artist;
    case F::Album: return t.album;
    case F::AlbumArtist: return t.albumArtist;
    case F::Genre: return t.genre;
    case F::Year: return t.year;
    case F::Path: return t.path;
    case F::Codec: return t.codec;
    case F::Composer: return t.composer;
    case F::Comment: return t.comment;
    case F::Any: break;
    }
    return {};
}

} // namespace

TrackQuery TrackQuery::parse(const QString &text, Field defaultField)
{
    TrackQuery q;
    static const QRegularExpression yearRange(QStringLiteral("^(\\d{4})\\s*-\\s*(\\d{4})$"));

    for (QString tok : tokenize(text)) {
        Term term;
        term.field = defaultField;
        if (tok.size() > 1 && tok.startsWith(QLatin1Char('-'))) {
            term.negate = true;
            tok.remove(0, 1);
        }
        const int colon = tok.indexOf(QLatin1Char(':'));
        if (colon > 0) {
            const auto it = fieldAliases().constFind(tok.left(colon).toLower());
            if (it != fieldAliases().constEnd()) {
                term.field = it.value();
                tok = tok.mid(colon + 1);
            }
        }
        if (tok.isEmpty())
            continue;   // "artist:" typed but no value yet
        if (term.field == Field::Year) {
            const auto m = yearRange.match(tok);
            if (m.hasMatch()) {
                term.yearFrom = m.captured(1).toInt();
                term.yearTo = m.captured(2).toInt();
            }
        }
        term.text = fold(tok);
        q.m_terms.append(term);
    }
    return q;
}

bool TrackQuery::termMatches(const Term &term, const Track &t)
{
    if (term.yearFrom > 0) {
        const int y = t.year.left(4).toInt();
        return y >= term.yearFrom && y <= term.yearTo;
    }
    if (term.field != Field::Any)
        return containsFolded(fieldValue(term.field, t), term.text);

    for (Field f : {Field::Title, Field::Artist, Field::Album, Field::AlbumArtist, Field::Genre,
                    Field::Year, Field::Composer}) {
        if (containsFolded(fieldValue(f, t), term.text))
            return true;
    }
    return containsFolded(fileNameOf(t), term.text);
}

bool TrackQuery::matches(const Track &t) const
{
    for (const Term &term : m_terms) {
        if (termMatches(term, t) == term.negate)
            return false;
    }
    return true;
}

QList<QPair<QString, TrackQuery::Field>> TrackQuery::fieldChoices()
{
    return {
        {QStringLiteral("Везде"), Field::Any},
        {QStringLiteral("Название"), Field::Title},
        {QStringLiteral("Исполнитель"), Field::Artist},
        {QStringLiteral("Альбом"), Field::Album},
        {QStringLiteral("Жанр"), Field::Genre},
        {QStringLiteral("Год"), Field::Year},
        {QStringLiteral("Путь"), Field::Path},
        {QStringLiteral("Формат"), Field::Codec},
    };
}
