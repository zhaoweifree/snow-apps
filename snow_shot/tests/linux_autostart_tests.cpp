#include "snow_shot/platform/linux/desktopentry.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdlib>
#include <iostream>

using namespace snow_shot::platform::linux::desktopentry;

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void requireEqual(const QString& actual, const QString& expected, const char* message) {
    if (actual != expected) {
        std::cerr << message << "\n  actual:   " << actual.toStdString()
                  << "\n  expected: " << expected.toStdString() << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void requireEqual(const QStringList& actual, const QStringList& expected, const char* message) {
    if (actual != expected) {
        std::cerr << message << "\n  actual:   " << actual.join(u'|').toStdString()
                  << "\n  expected: " << expected.join(u'|').toStdString() << '\n';
        std::exit(EXIT_FAILURE);
    }
}

/// The parts of the Exec rules a naive implementation gets wrong. Each case is
/// written as the exact byte sequence the specification asks for.
void execEscaping() {
    // Nothing reserved: no quotes, so a plain install path stays readable.
    requireEqual(escapeExecArgument(QStringLiteral("/usr/bin/snow_shot")),
                 QStringLiteral("/usr/bin/snow_shot"), "plain path must not be quoted");

    // A space forces quotes.
    requireEqual(escapeExecArgument(QStringLiteral("/opt/snow shot/bin/snow_shot")),
                 QStringLiteral("\"/opt/snow shot/bin/snow_shot\""),
                 "space must quote the argument");

    // A literal dollar sign inside quotes needs two backslashes: the string
    // layer halves them, then the quoting layer consumes the one it sees.
    requireEqual(escapeExecArgument(QStringLiteral("/opt/$x/snow_shot")),
                 QStringLiteral("\"/opt/\\\\$x/snow_shot\""),
                 "dollar sign must survive both escape layers");

    // A double quote is escaped once by the quoting layer and untouched by the
    // string layer, which does not define it.
    requireEqual(escapeExecArgument(QStringLiteral("/opt/a\"b/snow_shot")),
                 QStringLiteral("\"/opt/a\\\"b/snow_shot\""), "double quote must be escaped");

    // A backtick is escaped once, for the same reason.
    requireEqual(escapeExecArgument(QStringLiteral("/opt/a`b/snow_shot")),
                 QStringLiteral("\"/opt/a\\`b/snow_shot\""), "backtick must be escaped");

    // A literal backslash needs four: doubled by the quoting layer, then each
    // pair doubled again by the string layer.
    requireEqual(escapeExecArgument(QStringLiteral("/opt/a\\b/snow_shot")),
                 QStringLiteral("\"/opt/a\\\\\\\\b/snow_shot\""),
                 "backslash must be escaped four times");

    // A percent sign starts a field code, so unquoted it is doubled.
    requireEqual(escapeExecArgument(QStringLiteral("/opt/100%/snow_shot")),
                 QStringLiteral("/opt/100%%/snow_shot"),
                 "unquoted percent must be doubled and stay unquoted");

    // Doubled in quotes as well: `%%` is the escape for a literal percent, not a
    // field code, so it applies wherever a percent appears.
    requireEqual(escapeExecArgument(QStringLiteral("/opt/100% x/snow_shot")),
                 QStringLiteral("\"/opt/100%% x/snow_shot\""),
                 "quoted percent must be doubled too");
}

void execJoining() {
    requireEqual(execValue({QStringLiteral("/usr/bin/snow_shot"), QStringLiteral("--autostart")}),
                 QStringLiteral("/usr/bin/snow_shot --autostart"),
                 "arguments must be separated by single spaces");
    requireEqual(execValue({QStringLiteral("/a b/snow_shot"), QStringLiteral("--autostart")}),
                 QStringLiteral("\"/a b/snow_shot\" --autostart"),
                 "only the argument that needs quotes gets them");
}

/// The loop that matters: what this module writes, read back by this module,
/// must produce the command line it started from. A mismatch here means
/// `matchesExpectedEntry` rewrites the entry on every start, or worse, believes
/// a wrong entry is right.
void execRoundTrip() {
    const QStringList cases{
        QStringLiteral("/usr/bin/snow_shot"),   QStringLiteral("/opt/snow shot/bin/snow_shot"),
        QStringLiteral("/opt/$HOME/snow_shot"), QStringLiteral("/opt/a\"b/snow_shot"),
        QStringLiteral("/opt/a`b/snow_shot"),   QStringLiteral("/opt/a\\b/snow_shot"),
        QStringLiteral("/opt/100%/snow_shot"),  QStringLiteral("/opt/100% x/snow_shot"),
        QStringLiteral("/opt/a%%b/snow_shot"),  QStringLiteral("/opt/a'b/snow_shot"),
        QStringLiteral("/opt/a&b;snow_shot"),   QStringLiteral("/opt/(x)/snow_shot"),
    };
    for (const QString& path : cases) {
        const QStringList command{path, QStringLiteral("--autostart")};
        const QByteArray entry = autoStartEntry(QStringLiteral("Snow Shot"), execValue(command));
        const QString exec =
            readValue(entry, QString::fromLatin1(desktopEntryGroup), QStringLiteral("Exec"));
        require(!exec.isNull(), "Exec must be readable back");
        requireEqual(parseExec(exec), command, "round trip must reproduce the command line");
    }
}

void entryBody() {
    const QByteArray entry = autoStartEntry(QStringLiteral("Snow Shot"),
                                            QStringLiteral("/usr/bin/snow_shot --autostart"));
    requireEqual(readValue(entry, QString::fromLatin1(desktopEntryGroup), QStringLiteral("Type")),
                 QStringLiteral("Application"), "entry must declare its type");
    requireEqual(readValue(entry, QString::fromLatin1(desktopEntryGroup), QStringLiteral("Name")),
                 QStringLiteral("Snow Shot"), "entry must carry the display name");
    require(requestsAutoStart(entry), "a written entry must read as enabled");
    require(isApplicationEntry(entry), "a written entry must be an application");

    // A name with a backslash exercises the string layer on a value other than
    // Exec.
    const QByteArray escaped =
        autoStartEntry(QStringLiteral("Snow\\Shot"), QStringLiteral("/usr/bin/snow_shot"));
    requireEqual(readValue(escaped, QString::fromLatin1(desktopEntryGroup), QStringLiteral("Name")),
                 QStringLiteral("Snow\\Shot"), "name escapes must round trip");

    // A comment is optional and must not leave an empty key behind.
    require(!entry.contains("Comment="), "an empty comment must be omitted");
    require(autoStartEntry(QStringLiteral("Snow Shot"), QStringLiteral("x"),
                           QStringLiteral("Screenshot tool"))
                .contains("Comment=Screenshot tool"),
            "a comment must be written when given");
}

void valueReading() {
    const QByteArray entry = QByteArrayLiteral("# a comment\n"
                                               "[Desktop Entry]\n"
                                               "Name=Snow Shot\n"
                                               "Name=Duplicate\n"
                                               "X-Other=first\n"
                                               "\n"
                                               "[Desktop Action new]\n"
                                               "Name=From another group\n"
                                               "Exec=/bin/other\n");

    const QString group = QString::fromLatin1(desktopEntryGroup);
    requireEqual(readValue(entry, group, QStringLiteral("Name")), QStringLiteral("Snow Shot"),
                 "the first duplicate key must win");
    require(readValue(entry, group, QStringLiteral("Exec")).isNull(),
            "a key from a later group must not leak into the first");
    requireEqual(readValue(entry, QStringLiteral("Desktop Action new"), QStringLiteral("Exec")),
                 QStringLiteral("/bin/other"), "a later group must be readable on its own");
    requireEqual(readValue(entry, group, QStringLiteral("X-Other")), QStringLiteral("first"),
                 "extension keys must read like any other");

    // CRLF line endings must not become part of the value.
    requireEqual(readValue(QByteArrayLiteral("[Desktop Entry]\r\nName=Snow Shot\r\n"), group,
                           QStringLiteral("Name")),
                 QStringLiteral("Snow Shot"), "carriage returns must be stripped");

    // An absent group is not the same as the first group.
    require(readValue(entry, QStringLiteral("Missing"), QStringLiteral("Name")).isNull(),
            "an absent group must return nothing");
    // A null array means the file does not exist.
    require(readValue(QByteArray(), group, QStringLiteral("Name")).isNull(),
            "a missing file must return nothing");
}

void stringEscapes() {
    const QString group = QString::fromLatin1(desktopEntryGroup);
    // The two defined escapes that change meaning, plus the one that must be
    // left alone so the quoting layer still sees it.
    requireEqual(readValue(QByteArrayLiteral("[Desktop Entry]\nName=a\\sb\n"), group,
                           QStringLiteral("Name")),
                 QStringLiteral("a b"), "\\s must become a space");
    requireEqual(readValue(QByteArrayLiteral("[Desktop Entry]\nName=a\\tb\n"), group,
                           QStringLiteral("Name")),
                 QStringLiteral("a\tb"), "\\t must become a tab");
    requireEqual(readValue(QByteArrayLiteral("[Desktop Entry]\nName=a\\\\b\n"), group,
                           QStringLiteral("Name")),
                 QStringLiteral("a\\b"), "\\\\ must become one backslash");
    requireEqual(readValue(QByteArrayLiteral("[Desktop Entry]\nName=a\\\"b\n"), group,
                           QStringLiteral("Name")),
                 QStringLiteral("a\\\"b"),
                 "an undefined escape must be left for the quoting layer");
}

void enablement() {
    const QByteArray enabled = autoStartEntry(QStringLiteral("Snow Shot"), QStringLiteral("x"));
    require(requestsAutoStart(enabled), "an entry without Hidden is enabled");

    require(!requestsAutoStart(QByteArray()), "a missing file is disabled");

    // Hidden=true is the specified way to disable, and it wins over everything.
    const QByteArray hidden = QByteArrayLiteral(
        "[Desktop Entry]\nType=Application\nName=Snow Shot\nExec=x\nHidden=true\n");
    require(!requestsAutoStart(hidden), "Hidden=true must disable");
    require(!requestsAutoStart(hiddenEntry(QStringLiteral("Snow Shot"))),
            "the masking stub must read as disabled");
    require(isApplicationEntry(hiddenEntry(QStringLiteral("Snow Shot"))),
            "the masking stub must still be a valid application entry");

    // GNOME's startup panel disables by this key and leaves Hidden alone.
    const QByteArray gnome = QByteArrayLiteral("[Desktop Entry]\nType=Application\nName=Snow "
                                               "Shot\nExec=x\nX-GNOME-Autostart-enabled=false\n");
    require(!requestsAutoStart(gnome), "X-GNOME-Autostart-enabled=false must disable");

    // A stray file of another type is never started at login.
    require(!isApplicationEntry(QByteArrayLiteral("[Desktop Entry]\nType=Link\nName=x\n")),
            "a link is not an application");
    require(!requestsAutoStart(QByteArrayLiteral("[Desktop Entry]\nType=Link\nName=x\nExec=x\n")),
            "a non-application entry must not read as enabled");

    // OnlyShowIn does not express the user's intent, so it must not be read as
    // disabled: the switch would then disagree with the entry we write next.
    require(requestsAutoStart(QByteArrayLiteral("[Desktop Entry]\nType=Application\nName=x\nExec="
                                                "x\nOnlyShowIn=GNOME;\n")),
            "OnlyShowIn must not change the reported state");
}

void parsingEdgeCases() {
    // Adjacent quoted and unquoted text is one argument.
    requireEqual(parseExec(QStringLiteral("\"/a b\"/c")), {QStringLiteral("/a b/c")},
                 "quote boundaries must not split an argument");
    requireEqual(parseExec(QStringLiteral("a  b")), {QStringLiteral("a"), QStringLiteral("b")},
                 "repeated separators must not produce empty arguments");
    requireEqual(parseExec(QString()), {}, "an empty value has no arguments");
    requireEqual(parseExec(QStringLiteral("\"\"")), {QString()},
                 "an explicitly empty argument must survive");
    // A field code is not expanded here, so it stays as text and can never be
    // mistaken for a plain command line.
    requireEqual(parseExec(QStringLiteral("/bin/x %F")),
                 {QStringLiteral("/bin/x"), QStringLiteral("%F")},
                 "field codes must be left in place");
}
} // namespace

int main() {
    execEscaping();
    execJoining();
    execRoundTrip();
    entryBody();
    valueReading();
    stringEscapes();
    enablement();
    parsingEdgeCases();
    std::cout << "Linux autostart tests passed\n";
}
