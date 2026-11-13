#include "platform/GlobalHotkeys.h"

#include <QCoreApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

namespace GlobalHotkeys {

namespace {

const QString kSchema = QStringLiteral("org.gnome.settings-daemon.plugins.media-keys");
const QString kRelocatable = QStringLiteral("org.gnome.settings-daemon.plugins.media-keys.custom-keybinding");
const QString kBasePath = QStringLiteral("/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/");

QString bindingPath(Action a)
{
    return kBasePath + QStringLiteral("tembr-") + idOf(a) + QLatin1Char('/');
}

bool gsettings(const QStringList &args, QString *out = nullptr, QString *err = nullptr)
{
    QProcess p;
    p.start(QStringLiteral("gsettings"), args);
    if (!p.waitForFinished(5000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (err)
            *err = QString::fromUtf8(p.readAllStandardError()).trimmed();
        return false;
    }
    if (out)
        *out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    return true;
}

// GVariant text for a string: 'it\'s'.
QString gvString(QString s)
{
    s.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    s.replace(QLatin1Char('\''), QLatin1String("\\'"));
    return QLatin1Char('\'') + s + QLatin1Char('\'');
}

// X keysym names (what GNOME accelerators use) for numpad keys. With NumLock
// off the numpad sends navigation keys (KP_Left for 4, KP_Begin for 5, ...).
QString keypadName(int key)
{
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return QStringLiteral("KP_%1").arg(key - Qt::Key_0);
    static const QHash<int, QString> names = {
        {Qt::Key_Plus, QStringLiteral("KP_Add")},           {Qt::Key_Minus, QStringLiteral("KP_Subtract")},
        {Qt::Key_Asterisk, QStringLiteral("KP_Multiply")},  {Qt::Key_Slash, QStringLiteral("KP_Divide")},
        {Qt::Key_Enter, QStringLiteral("KP_Enter")},        {Qt::Key_Return, QStringLiteral("KP_Enter")},
        {Qt::Key_Period, QStringLiteral("KP_Decimal")},     {Qt::Key_Comma, QStringLiteral("KP_Decimal")},
        {Qt::Key_Equal, QStringLiteral("KP_Equal")},        {Qt::Key_Left, QStringLiteral("KP_Left")},
        {Qt::Key_Right, QStringLiteral("KP_Right")},        {Qt::Key_Up, QStringLiteral("KP_Up")},
        {Qt::Key_Down, QStringLiteral("KP_Down")},          {Qt::Key_Home, QStringLiteral("KP_Home")},
        {Qt::Key_End, QStringLiteral("KP_End")},            {Qt::Key_PageUp, QStringLiteral("KP_Page_Up")},
        {Qt::Key_PageDown, QStringLiteral("KP_Page_Down")}, {Qt::Key_Insert, QStringLiteral("KP_Insert")},
        {Qt::Key_Delete, QStringLiteral("KP_Delete")},      {Qt::Key_Clear, QStringLiteral("KP_Begin")},
    };
    return names.value(key);
}

bool isMediaKey(int key)
{
    switch (key) {
    case Qt::Key_MediaPlay: case Qt::Key_MediaTogglePlayPause: case Qt::Key_MediaPause:
    case Qt::Key_MediaStop: case Qt::Key_MediaNext: case Qt::Key_MediaPrevious:
    case Qt::Key_VolumeUp: case Qt::Key_VolumeDown: case Qt::Key_VolumeMute:
        return true;
    default:
        return false;
    }
}

QString keyName(int key, bool keypad)
{
    if (keypad)
        return keypadName(key);
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return QString(QChar('a' + (key - Qt::Key_A)));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return QString(QChar('0' + (key - Qt::Key_0)));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);

    static const QHash<int, QString> names = {
        {Qt::Key_Space, QStringLiteral("space")},       {Qt::Key_Return, QStringLiteral("Return")},
        {Qt::Key_Enter, QStringLiteral("KP_Enter")},    {Qt::Key_Tab, QStringLiteral("Tab")},
        {Qt::Key_Backspace, QStringLiteral("BackSpace")}, {Qt::Key_Escape, QStringLiteral("Escape")},
        {Qt::Key_Insert, QStringLiteral("Insert")},     {Qt::Key_Delete, QStringLiteral("Delete")},
        {Qt::Key_Home, QStringLiteral("Home")},         {Qt::Key_End, QStringLiteral("End")},
        {Qt::Key_PageUp, QStringLiteral("Page_Up")},    {Qt::Key_PageDown, QStringLiteral("Page_Down")},
        {Qt::Key_Left, QStringLiteral("Left")},         {Qt::Key_Right, QStringLiteral("Right")},
        {Qt::Key_Up, QStringLiteral("Up")},             {Qt::Key_Down, QStringLiteral("Down")},
        {Qt::Key_Pause, QStringLiteral("Pause")},       {Qt::Key_Print, QStringLiteral("Print")},
        {Qt::Key_ScrollLock, QStringLiteral("Scroll_Lock")},
        {Qt::Key_Comma, QStringLiteral("comma")},       {Qt::Key_Period, QStringLiteral("period")},
        {Qt::Key_Slash, QStringLiteral("slash")},       {Qt::Key_Backslash, QStringLiteral("backslash")},
        {Qt::Key_Semicolon, QStringLiteral("semicolon")}, {Qt::Key_Apostrophe, QStringLiteral("apostrophe")},
        {Qt::Key_BracketLeft, QStringLiteral("bracketleft")}, {Qt::Key_BracketRight, QStringLiteral("bracketright")},
        {Qt::Key_Minus, QStringLiteral("minus")},       {Qt::Key_Equal, QStringLiteral("equal")},
        {Qt::Key_Plus, QStringLiteral("plus")},         {Qt::Key_Asterisk, QStringLiteral("asterisk")},
        {Qt::Key_QuoteLeft, QStringLiteral("grave")},
        {Qt::Key_MediaPlay, QStringLiteral("XF86AudioPlay")},
        {Qt::Key_MediaTogglePlayPause, QStringLiteral("XF86AudioPlay")},
        {Qt::Key_MediaPause, QStringLiteral("XF86AudioPause")},
        {Qt::Key_MediaStop, QStringLiteral("XF86AudioStop")},
        {Qt::Key_MediaNext, QStringLiteral("XF86AudioNext")},
        {Qt::Key_MediaPrevious, QStringLiteral("XF86AudioPrev")},
        {Qt::Key_VolumeUp, QStringLiteral("XF86AudioRaiseVolume")},
        {Qt::Key_VolumeDown, QStringLiteral("XF86AudioLowerVolume")},
        {Qt::Key_VolumeMute, QStringLiteral("XF86AudioMute")},
    };
    return names.value(key);
}

} // namespace

QList<ActionInfo> actions()
{
    return {
        {Action::PlayPause, QStringLiteral("play-pause"), QStringLiteral("Воспроизведение / пауза")},
        {Action::Stop, QStringLiteral("stop"), QStringLiteral("Стоп")},
        {Action::Next, QStringLiteral("next"), QStringLiteral("Следующий трек")},
        {Action::Previous, QStringLiteral("previous"), QStringLiteral("Предыдущий трек")},
        {Action::VolumeUp, QStringLiteral("volume-up"), QStringLiteral("Громче")},
        {Action::VolumeDown, QStringLiteral("volume-down"), QStringLiteral("Тише")},
        {Action::Show, QStringLiteral("show"), QStringLiteral("Показать окно")},
    };
}

QString idOf(Action a)
{
    for (const ActionInfo &i : actions()) {
        if (i.action == a)
            return i.id;
    }
    return {};
}

Bindings load(QStringList *dropped)
{
    QSettings s;
    Bindings b;
    for (const ActionInfo &i : actions()) {
        const QString v = s.value(QStringLiteral("hotkeys/") + i.id).toString();
        if (v.isEmpty())
            continue;
        const QKeySequence seq = QKeySequence::fromString(v, QKeySequence::PortableText);
        if (!unsafeReason(seq).isEmpty()) {
            if (dropped)
                *dropped << QStringLiteral("%1 (%2)").arg(displayText(seq), i.title);
            continue;
        }
        b.insert(i.action, seq);
    }
    return b;
}

QString unsafeReason(const QKeySequence &seq)
{
    if (seq.isEmpty())
        return {};
    const QKeyCombination kc = seq[0];
    const int key = kc.key();
    const Qt::KeyboardModifiers mods = kc.keyboardModifiers();
    if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return {};
    if (mods.testFlag(Qt::KeypadModifier) || (key >= Qt::Key_F1 && key <= Qt::Key_F35) || isMediaKey(key)
        || key == Qt::Key_Pause || key == Qt::Key_Print || key == Qt::Key_ScrollLock)
        return {};
    return QStringLiteral("«%1» без Ctrl, Alt или Super перехватит эту клавишу во всех программах — "
                          "её нельзя будет набрать. Добавьте модификатор или используйте клавишу "
                          "цифрового блока, F1–F12 или медиаклавишу.")
        .arg(displayText(seq));
}

QString displayText(const QKeySequence &seq)
{
    if (seq.isEmpty())
        return {};
    const QKeyCombination kc = seq[0];
    const Qt::KeyboardModifiers mods = kc.keyboardModifiers();
    QString out;
    if (mods.testFlag(Qt::ControlModifier))
        out += QStringLiteral("Ctrl+");
    if (mods.testFlag(Qt::AltModifier))
        out += QStringLiteral("Alt+");
    if (mods.testFlag(Qt::ShiftModifier))
        out += QStringLiteral("Shift+");
    if (mods.testFlag(Qt::MetaModifier))
        out += QStringLiteral("Super+");
    if (mods.testFlag(Qt::KeypadModifier))
        out += QStringLiteral("Numpad ");
    return out + QKeySequence(kc.key()).toString(QKeySequence::NativeText);
}

void save(const Bindings &b)
{
    QSettings s;
    for (const ActionInfo &i : actions())
        s.setValue(QStringLiteral("hotkeys/") + i.id, b.value(i.action).toString(QKeySequence::PortableText));
}

bool desktopSupported()
{
    static const bool supported = [] {
        const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
        if (!desktop.contains(QLatin1String("GNOME"), Qt::CaseInsensitive)
            && !desktop.contains(QLatin1String("Unity"), Qt::CaseInsensitive))
            return false;
        if (QStandardPaths::findExecutable(QStringLiteral("gsettings")).isEmpty())
            return false;
        return gsettings({QStringLiteral("get"), kSchema, QStringLiteral("custom-keybindings")});
    }();
    return supported;
}

QString desktopName()
{
    const QString d = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    return d.isEmpty() ? QStringLiteral("неизвестное окружение") : d;
}

QString command(Action a)
{
    QString exe = QCoreApplication::applicationFilePath();
    if (exe.contains(QLatin1Char(' ')))
        exe = QLatin1Char('"') + exe + QLatin1Char('"');
    return exe + QStringLiteral(" --") + idOf(a);
}

QString gtkAccelerator(const QKeySequence &seq)
{
    if (seq.isEmpty())
        return {};
    const QKeyCombination kc = seq[0];
    const Qt::KeyboardModifiers mods = kc.keyboardModifiers();
    const QString key = keyName(kc.key(), mods.testFlag(Qt::KeypadModifier));
    if (key.isEmpty())
        return {};
    QString out;
    if (mods.testFlag(Qt::ControlModifier))
        out += QStringLiteral("<Control>");
    if (mods.testFlag(Qt::AltModifier))
        out += QStringLiteral("<Alt>");
    if (mods.testFlag(Qt::ShiftModifier))
        out += QStringLiteral("<Shift>");
    if (mods.testFlag(Qt::MetaModifier))
        out += QStringLiteral("<Super>");
    return out + key;
}

QString releaseFromDesktop()
{
    return desktopSupported() ? applyToDesktop({}) : QString();
}

QString applyToDesktop(const Bindings &b)
{
    if (!desktopSupported())
        return QStringLiteral("Автоматическая регистрация клавиш недоступна в %1.").arg(desktopName());

    QString listText, err;
    if (!gsettings({QStringLiteral("get"), kSchema, QStringLiteral("custom-keybindings")}, &listText, &err))
        return err;
    QStringList list;
    static const QRegularExpression item(QStringLiteral("'([^']*)'"));
    for (auto it = item.globalMatch(listText); it.hasNext();)
        list << it.next().captured(1);

    QStringList problems;
    for (const ActionInfo &i : actions()) {
        const QString path = bindingPath(i.action);
        const QString schema = kRelocatable + QLatin1Char(':') + path;
        const QKeySequence seq = b.value(i.action);
        const QString accel = gtkAccelerator(seq);
        if (accel.isEmpty()) {
            if (!seq.isEmpty())
                problems << QStringLiteral("%1: сочетание %2 не поддерживается (для букв включите английскую раскладку)")
                                .arg(i.title, displayText(seq));
            if (list.removeAll(path) > 0)
                gsettings({QStringLiteral("reset-recursively"), schema});
            continue;
        }
        const bool ok = gsettings({QStringLiteral("set"), schema, QStringLiteral("name"), gvString(QStringLiteral("Tembr: ") + i.title)}, nullptr, &err)
                        && gsettings({QStringLiteral("set"), schema, QStringLiteral("command"), gvString(command(i.action))}, nullptr, &err)
                        && gsettings({QStringLiteral("set"), schema, QStringLiteral("binding"), gvString(accel)}, nullptr, &err);
        if (!ok) {
            problems << QStringLiteral("%1: %2").arg(i.title, err);
            continue;
        }
        if (!list.contains(path))
            list << path;
    }

    QStringList quoted;
    for (const QString &p : std::as_const(list))
        quoted << gvString(p);
    const QString value = quoted.isEmpty() ? QStringLiteral("@as []") : QLatin1Char('[') + quoted.join(QStringLiteral(", ")) + QLatin1Char(']');
    if (!gsettings({QStringLiteral("set"), kSchema, QStringLiteral("custom-keybindings"), value}, nullptr, &err))
        problems << err;
    return problems.join(QLatin1Char('\n'));
}

} // namespace GlobalHotkeys
