#include "migration/PhoneSources.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace Migration {

namespace {

const QStringList kPlaylistPatterns = {QStringLiteral("*.xspf"), QStringLiteral("*.m3u"), QStringLiteral("*.m3u8"),
                                       QStringLiteral("*.pls"), QStringLiteral("*.aimppl4")};
const QString kAimpAppDir = QStringLiteral("Android/data/com.aimp.player");

bool isPlaylistName(const QString &name)
{
    const QString lower = name.toLower();
    for (const QString &p : kPlaylistPatterns) {
        if (lower.endsWith(p.mid(1)))
            return true;
    }
    return false;
}

// --- ADB ---------------------------------------------------------------------------

QString adbBinary()
{
    const QString env = qEnvironmentVariable("TEMBR_ADB");
    return env.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("adb")) : env;
}

struct ProcResult
{
    QByteArray out;
    QString err;
    bool ok = false;
};

ProcResult runProcess(const QString &program, const QStringList &args, int timeoutMs = 30000)
{
    ProcResult r;
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(5000)) {
        r.err = p.errorString();
        return r;
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        r.err = QStringLiteral("timeout");
        return r;
    }
    r.out = p.readAllStandardOutput();
    r.err = QString::fromUtf8(p.readAllStandardError()).trimmed();
    r.ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    return r;
}

// Single-quoted for the phone's shell.
QString shq(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

class AdbSource : public PhoneSource
{
public:
    AdbSource(QString serial, QString model)
        : m_serial(std::move(serial))
        , m_model(std::move(model))
    {
        m_storages.append({QStringLiteral("primary"), QStringLiteral("Внутренняя память"), QStringLiteral("/sdcard"), true});
        const ProcResult r = shell(QStringLiteral("ls /storage"));
        static const QRegularExpression volume(QStringLiteral("^[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}$"));
        for (const QString &line : QString::fromUtf8(r.out).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
            if (volume.match(line).hasMatch())
                m_storages.append({line.toUpper(), QStringLiteral("SD-карта %1").arg(line), QStringLiteral("/storage/") + line, false});
        }
    }

    Kind kind() const override { return Kind::Adb; }
    QString label() const override { return QStringLiteral("%1 (USB-отладка, %2)").arg(m_model, m_serial); }
    QList<PhoneStorage> storages() const override { return m_storages; }

    QList<PhonePlaylistFile> findPlaylists(QString *error) override
    {
        QList<PhonePlaylistFile> out;
        for (int i = 0; i < m_storages.size(); ++i) {
            const QString root = m_storages[i].root;
            QStringList names;
            for (const QString &p : kPlaylistPatterns)
                names << QStringLiteral("-iname %1").arg(shq(p));
            // Two start points: the whole storage without Android/, and AIMP's app folder.
            const QString cmd = QStringLiteral("find %1 %2 -path %3 -prune -o -type f \\( %4 \\) -print 2>/dev/null")
                                    .arg(shq(root + QLatin1Char('/')), shq(root + QLatin1Char('/') + kAimpAppDir),
                                         shq(root + QStringLiteral("/Android")), names.join(QStringLiteral(" -o ")));
            const ProcResult r = shell(cmd, 120000);
            if (r.out.isEmpty() && !r.err.isEmpty() && error)
                *error = r.err;
            for (QString line : QString::fromUtf8(r.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                line = line.trimmed();
                QString rel = line.mid(root.size());
                while (rel.startsWith(QLatin1Char('/')))
                    rel.remove(0, 1);
                if (!rel.isEmpty() && line.startsWith(root))
                    out.append({i, rel});
            }
        }
        return out;
    }

    QByteArray readFile(int storage, const QString &rel, QString *error) override
    {
        const ProcResult r = adb({QStringLiteral("exec-out"), QStringLiteral("cat ") + shq(abs(storage, rel))});
        if (r.out.isEmpty() && error)
            *error = r.err.isEmpty() ? QStringLiteral("пустой файл или нет доступа") : r.err;
        return r.out;
    }

    bool exists(int storage, const QString &rel) override
    {
        return shell(QStringLiteral("[ -f %1 ] && echo y").arg(shq(abs(storage, rel)))).out.trimmed() == "y";
    }

    bool copyTo(int storage, const QString &rel, const QString &localPath, QString *error) override
    {
        QDir().mkpath(QFileInfo(localPath).absolutePath());
        const ProcResult r = adb({QStringLiteral("pull"), abs(storage, rel), localPath}, 600000);
        if (r.ok && QFileInfo::exists(localPath))
            return true;
        if (error)
            *error = r.err;
        return false;
    }

private:
    QString abs(int storage, const QString &rel) const { return m_storages.value(storage).root + QLatin1Char('/') + rel; }

    ProcResult adb(QStringList args, int timeoutMs = 30000) const
    {
        args.prepend(m_serial);
        args.prepend(QStringLiteral("-s"));
        return runProcess(adbBinary(), args, timeoutMs);
    }

    ProcResult shell(const QString &cmd, int timeoutMs = 30000) const
    {
        return adb({QStringLiteral("shell"), cmd}, timeoutMs);
    }

    QString m_serial, m_model;
    QList<PhoneStorage> m_storages;
};

void discoverAdb(PhoneDiscovery &d)
{
    const QString adbPath = adbBinary();
    if (adbPath.isEmpty())
        return;
    const ProcResult r = runProcess(adbPath, {QStringLiteral("devices"), QStringLiteral("-l")}, 15000);
    for (const QString &line : QString::fromUtf8(r.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList parts = line.simplified().split(QLatin1Char(' '));
        if (parts.size() < 2 || line.startsWith(QLatin1String("List of devices")) || line.startsWith(QLatin1Char('*')))
            continue;
        const QString serial = parts[0];
        const QString state = parts[1];
        QString model = serial;
        for (const QString &p : parts) {
            if (p.startsWith(QLatin1String("model:")))
                model = p.mid(6).replace(QLatin1Char('_'), QLatin1Char(' '));
        }
        if (state == QLatin1String("device"))
            d.sources.append(std::make_shared<AdbSource>(serial, model));
        else if (state == QLatin1String("unauthorized"))
            d.notes << QStringLiteral("%1: разрешите отладку по USB в запросе на экране телефона и обновите список.").arg(model);
        else
            d.notes << QStringLiteral("%1: состояние «%2», переподключите кабель.").arg(model, state);
    }
}

// --- Local folders (MTP mounts, plain folders) ---------------------------------

class FolderSource : public PhoneSource
{
public:
    FolderSource(Kind kind, QString label, QList<PhoneStorage> storages)
        : m_kind(kind)
        , m_label(std::move(label))
        , m_storages(std::move(storages))
    {
    }

    Kind kind() const override { return m_kind; }
    QString label() const override { return m_label; }
    QList<PhoneStorage> storages() const override { return m_storages; }

    QList<PhonePlaylistFile> findPlaylists(QString *) override
    {
        QList<PhonePlaylistFile> out;
        for (int i = 0; i < m_storages.size(); ++i) {
            const QString root = m_storages[i].root;
            walk(root, root, 0, i, out);
            const QString aimp = root + QLatin1Char('/') + kAimpAppDir;
            if (QFileInfo(aimp).isDir())
                walk(root, aimp, 0, i, out);
        }
        return out;
    }

    QByteArray readFile(int storage, const QString &rel, QString *error) override
    {
        QFile f(abs(storage, rel));
        if (!f.open(QIODevice::ReadOnly)) {
            if (error)
                *error = f.errorString();
            return {};
        }
        return f.readAll();
    }

    bool exists(int storage, const QString &rel) override { return QFileInfo(abs(storage, rel)).isFile(); }

    bool copyTo(int storage, const QString &rel, const QString &localPath, QString *error) override
    {
        QDir().mkpath(QFileInfo(localPath).absolutePath());
        QFile src(abs(storage, rel));
        if (src.copy(localPath))
            return true;
        if (error)
            *error = src.errorString();
        return false;
    }

private:
    QString abs(int storage, const QString &rel) const { return m_storages.value(storage).root + QLatin1Char('/') + rel; }

    // Over MTP every directory listing is a round trip to the phone, so skip
    // the big media folders that never hold playlists.
    void walk(const QString &root, const QString &dir, int depth, int storage, QList<PhonePlaylistFile> &out) const
    {
        static const QStringList skip = {QStringLiteral("Android"), QStringLiteral("DCIM"), QStringLiteral("Pictures"),
                                         QStringLiteral("Movies"), QStringLiteral("Telegram"), QStringLiteral("WhatsApp")};
        if (depth > 8)
            return;
        const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &fi : entries) {
            if (fi.isDir()) {
                if (fi.fileName().startsWith(QLatin1Char('.')) || (depth == 0 && skip.contains(fi.fileName(), Qt::CaseInsensitive)))
                    continue;
                walk(root, fi.absoluteFilePath(), depth + 1, storage, out);
            } else if (isPlaylistName(fi.fileName())) {
                out.append({storage, fi.absoluteFilePath().mid(root.size() + 1)});
            }
        }
    }

    Kind m_kind;
    QString m_label;
    QList<PhoneStorage> m_storages;
};

bool looksPrimary(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("internal|внутр|phone|память|shared|телефон"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re.match(name).hasMatch();
}

} // namespace

QString PhoneSource::phonePath(int storage, const QString &rel) const
{
    const QList<PhoneStorage> st = storages();
    if (storage < 0 || storage >= st.size())
        return QStringLiteral("/storage/?/") + rel;
    if (st[storage].primary)
        return QStringLiteral("/sdcard/") + rel;
    return kind() == Kind::Adb ? st[storage].root + QLatin1Char('/') + rel : QStringLiteral("/storage/") + st[storage].name + QLatin1Char('/') + rel;
}

QList<PhoneSourcePtr> mtpSources(const QString &gvfsDir, QStringList *notes)
{
    QList<PhoneSourcePtr> out;
    for (const QFileInfo &mount : QDir(gvfsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!mount.fileName().startsWith(QLatin1String("mtp:host=")))
            continue;
        QString device = QUrl::fromPercentEncoding(mount.fileName().mid(9).toUtf8());
        device.replace(QLatin1Char('_'), QLatin1Char(' '));
        QList<PhoneStorage> storages;
        for (const QFileInfo &st : QDir(mount.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            PhoneStorage s;
            s.key = st.fileName();
            s.name = st.fileName();
            s.root = st.absoluteFilePath();
            s.primary = looksPrimary(s.name);
            storages.append(s);
        }
        if (storages.isEmpty()) {
            if (notes)
                *notes << QStringLiteral("%1: телефон подключён, но файлы недоступны. Разблокируйте его и выберите "
                                         "режим USB «Передача файлов».").arg(device);
            continue;
        }
        if (std::none_of(storages.cbegin(), storages.cend(), [](const PhoneStorage &s) { return s.primary; }))
            storages.first().primary = true;
        out.append(std::make_shared<FolderSource>(PhoneSource::Kind::Mtp,
                                                  QStringLiteral("%1 (передача файлов)").arg(device), storages));
    }
    return out;
}

PhoneDiscovery discoverPhones()
{
    PhoneDiscovery d;
    discoverAdb(d);
#ifdef Q_OS_UNIX
    d.sources += mtpSources(QStringLiteral("/run/user/%1/gvfs").arg(getuid()), &d.notes);
#endif
    return d;
}

PhoneSourcePtr folderSource(const QString &path)
{
    const QFileInfo fi(path);
    PhoneStorage s{QStringLiteral("primary"), fi.fileName(), fi.absoluteFilePath(), true};
    return std::make_shared<FolderSource>(PhoneSource::Kind::Folder, QStringLiteral("Папка %1").arg(fi.absoluteFilePath()),
                                          QList<PhoneStorage>{s});
}

} // namespace Migration
