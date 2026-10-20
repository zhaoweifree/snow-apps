#include "snow_shot/platform/linux/desktopentry.h"

#include <QList>

#ifdef linux
#undef linux
#endif

namespace snow_shot::platform::linux::desktopentry {
namespace {

/// The characters that force an `Exec` argument to be quoted. The list is the
/// specification's, verbatim: space, tab, newline, double quote, single quote,
/// backslash, the redirections, tilde, pipe, ampersand, semicolon, dollar sign,
/// asterisk, question mark, hash, the parentheses and backtick.
bool reservedInExec(QChar character) {
    switch (character.unicode()) {
    case u' ':
    case u'\t':
    case u'\n':
    case u'"':
    case u'\'':
    case u'\\':
    case u'>':
    case u'<':
    case u'~':
    case u'|':
    case u'&':
    case u';':
    case u'$':
    case u'*':
    case u'?':
    case u'#':
    case u'(':
    case u')':
    case u'`':
        return true;
    default:
        return false;
    }
}

/// String-type escapes, which apply to every value before anything else.
/// A backslash followed by an unlisted character is left alone, matching what
/// GLib does, so the quoting layer can still see its own backslashes.
QString escapeStringValue(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        switch (character.unicode()) {
        case u'\\':
            out += QStringLiteral("\\\\");
            break;
        case u'\n':
            out += QStringLiteral("\\n");
            break;
        case u'\t':
            out += QStringLiteral("\\t");
            break;
        case u'\r':
            out += QStringLiteral("\\r");
            break;
        case u' ':
            // Leading and trailing spaces would be trimmed on read; the
            // specification's escape for them exists for exactly this case.
            if (index == 0 || index == value.size() - 1)
                out += QStringLiteral("\\s");
            else
                out += character;
            break;
        default:
            out += character;
            break;
        }
    }
    return out;
}

QString unescapeStringValue(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (character != u'\\' || index + 1 >= value.size()) {
            out += character;
            continue;
        }
        const QChar next = value.at(index + 1);
        switch (next.unicode()) {
        case u's':
            out += u' ';
            ++index;
            break;
        case u'n':
            out += u'\n';
            ++index;
            break;
        case u't':
            out += u'\t';
            ++index;
            break;
        case u'r':
            out += u'\r';
            ++index;
            break;
        case u'\\':
            out += u'\\';
            ++index;
            break;
        default:
            // Not one of the defined escapes: the backslash is literal and the
            // next character is handled on its own turn. Doubling it here would
            // corrupt the `\"` and `\$` sequences the quoting layer needs.
            out += character;
            break;
        }
    }
    return out;
}

bool readBoolean(const QByteArray& contents, const QString& key, bool fallback) {
    const QString value =
        readValue(contents, QString::fromLatin1(desktopEntryGroup), key).trimmed();
    if (value.isEmpty())
        return fallback;
    return value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

} // namespace

QString escapeExecArgument(const QString& argument) {
    bool needsQuotes = false;
    for (const QChar character : argument) {
        if (reservedInExec(character)) {
            needsQuotes = true;
            break;
        }
    }

    QString out;
    out.reserve(argument.size() + 2);
    if (needsQuotes) {
        out += u'"';
        for (const QChar character : argument) {
            switch (character.unicode()) {
            case u'\\':
                // Two layers: the quoting layer doubles it, then the string
                // layer doubles each of those.
                out += QStringLiteral("\\\\\\\\");
                break;
            case u'$':
                out += QStringLiteral("\\\\$");
                break;
            case u'"':
                out += QStringLiteral("\\\"");
                break;
            case u'`':
                out += QStringLiteral("\\`");
                break;
            case u'%':
                // Doubled here too. The specification's `%%` is the escape for a
                // literal percent, not a field code, so it applies inside quotes
                // as well; leaving it single would read back as half of a field
                // code and break the round trip for any path containing one.
                out += QStringLiteral("%%");
                break;
            case u'\n':
                out += QStringLiteral("\\n");
                break;
            case u'\t':
                out += QStringLiteral("\\t");
                break;
            case u'\r':
                out += QStringLiteral("\\r");
                break;
            default:
                out += character;
                break;
            }
        }
        out += u'"';
        return out;
    }

    // Unquoted, a percent sign would be read as the start of a field code, and
    // the specification spells the literal as a doubled one.
    for (const QChar character : argument) {
        if (character == u'%')
            out += QStringLiteral("%%");
        else
            out += character;
    }
    return out;
}

QString execValue(const QStringList& command) {
    QStringList escaped;
    escaped.reserve(command.size());
    for (const QString& argument : command)
        escaped.push_back(escapeExecArgument(argument));
    return escaped.join(u' ');
}

QStringList parseExec(const QString& value) {
    QStringList arguments;
    QString current;
    bool quoted = false;
    bool started = false;
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (character == u'"') {
            // A quote only delimits; it never reaches the argument.
            quoted = !quoted;
            started = true;
            continue;
        }
        if (character == u'%' && index + 1 < value.size() && value.at(index + 1) == u'%') {
            // The literal-percent escape. A single percent followed by anything
            // else is a field code and is left for the caller to notice.
            current += u'%';
            started = true;
            ++index;
            continue;
        }
        if (character == u'\\' && index + 1 < value.size()) {
            const QChar next = value.at(index + 1);
            if (next == u'"' || next == u'`' || next == u'$' || next == u'\\') {
                current += next;
                started = true;
                ++index;
                continue;
            }
            // Not one of the quoting layer's escapes. The string layer has
            // already had its turn, so this backslash is literal.
            current += character;
            started = true;
            continue;
        }
        if (!quoted && (character == u' ' || character == u'\t')) {
            if (started) {
                arguments.push_back(current);
                current.clear();
                started = false;
            }
            continue;
        }
        current += character;
        started = true;
    }
    if (started)
        arguments.push_back(current);
    return arguments;
}

QByteArray autoStartEntry(const QString& name, const QString& exec, const QString& comment) {
    QString body;
    body += QStringLiteral("[%1]\n").arg(QString::fromLatin1(desktopEntryGroup));
    body += QStringLiteral("Type=Application\n");
    body += QStringLiteral("Name=%1\n").arg(escapeStringValue(name));
    if (!comment.isEmpty())
        body += QStringLiteral("Comment=%1\n").arg(escapeStringValue(comment));
    body += QStringLiteral("Exec=%1\n").arg(exec);
    // Autostart entries are not menu entries; desktops that walk this directory
    // and build a list from it should skip this one.
    body += QStringLiteral("NoDisplay=true\n");
    // GNOME's startup panel owns this key. Writing it keeps that panel and this
    // application's own switch showing the same state.
    body += QStringLiteral("X-GNOME-Autostart-enabled=true\n");
    return body.toUtf8();
}

QByteArray hiddenEntry(const QString& name) {
    // Deliberately minimal: no Exec, because a hidden entry must never be
    // started, and the specification's own recommendation for masking a
    // system-wide file is exactly this key set.
    QString body;
    body += QStringLiteral("[%1]\n").arg(QString::fromLatin1(desktopEntryGroup));
    body += QStringLiteral("Type=Application\n");
    body += QStringLiteral("Name=%1\n").arg(escapeStringValue(name));
    body += QStringLiteral("Hidden=true\n");
    body += QStringLiteral("X-GNOME-Autostart-enabled=false\n");
    return body.toUtf8();
}

QString readValue(const QByteArray& contents, const QString& group, const QString& key) {
    if (contents.isNull())
        return {};

    const QString text = QString::fromUtf8(contents);
    const QList<QStringView> lines = QStringView(text).split(u'\n');
    bool inGroup = false;
    for (QStringView line : lines) {
        if (line.endsWith(u'\r'))
            line.chop(1);
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;

        if (line.startsWith(u'[')) {
            const qsizetype closing = line.indexOf(u']');
            if (closing < 0) {
                // A malformed header cannot open a group, so anything after it
                // belongs to whichever group was open before.
                continue;
            }
            inGroup = line.mid(1, closing - 1) == group;
            continue;
        }

        if (!inGroup)
            continue;

        const qsizetype separator = line.indexOf(u'=');
        if (separator <= 0)
            continue;
        if (line.left(separator).trimmed() != key)
            continue;
        // Duplicate keys are invalid; the first one wins, which is also what
        // GLib does.
        return unescapeStringValue(line.mid(separator + 1).toString());
    }
    return {};
}

bool isApplicationEntry(const QByteArray& contents) {
    const QString type =
        readValue(contents, QString::fromLatin1(desktopEntryGroup), QStringLiteral("Type"));
    return type.trimmed() == QStringLiteral("Application");
}

bool requestsAutoStart(const QByteArray& contents) {
    if (contents.isNull() || !isApplicationEntry(contents))
        return false;
    if (readBoolean(contents, QStringLiteral("Hidden"), false))
        return false;
    return readBoolean(contents, QStringLiteral("X-GNOME-Autostart-enabled"), true);
}

} // namespace snow_shot::platform::linux::desktopentry
