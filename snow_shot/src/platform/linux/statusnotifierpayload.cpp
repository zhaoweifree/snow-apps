#include "snow_shot/platform/linux/statusnotifierpayload.h"

#include <QtGlobal>

#include <algorithm>

namespace snow_shot::platform::linux::tray {
namespace {

/// An `&...;` run longer than this is not an entity; the longest valid one is
/// `&#x10FFFF` plus the semicolon.
constexpr qsizetype maximumEntityLength = 10;

struct NamedEntity {
    const char* name;
    char value;
};

constexpr NamedEntity namedEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
};

/// Parses the body of a numeric entity (`#39`, `#x27`) into a code point.
bool parseCodePoint(QStringView body, uint& code) {
    if (!body.startsWith(u'#'))
        return false;
    qsizetype index = 1;
    int base = 10;
    if (index < body.size() && (body.at(index) == u'x' || body.at(index) == u'X')) {
        base = 16;
        ++index;
    }
    if (index >= body.size())
        return false;
    uint value = 0;
    for (; index < body.size(); ++index) {
        const QChar character = body.at(index);
        int digit = -1;
        if (character >= u'0' && character <= u'9')
            digit = character.unicode() - u'0';
        else if (base == 16 && character >= u'a' && character <= u'f')
            digit = character.unicode() - u'a' + 10;
        else if (base == 16 && character >= u'A' && character <= u'F')
            digit = character.unicode() - u'A' + 10;
        if (digit < 0 || digit >= base)
            return false;
        value = value * static_cast<uint>(base) + static_cast<uint>(digit);
        if (value > 0x10FFFFu)
            return false;
    }
    code = value;
    return true;
}

void appendCodePoint(QString& output, uint code) {
    if (code <= 0xFFFFu) {
        output += QChar(static_cast<ushort>(code));
        return;
    }
    const uint offset = code - 0x10000u;
    output += QChar(static_cast<ushort>(0xD800u + (offset >> 10)));
    output += QChar(static_cast<ushort>(0xDC00u + (offset & 0x3FFu)));
}

/// Whether a tag introduces a line or paragraph boundary, so that
/// `first<br>second` reads as two lines instead of `firstsecond`.
bool isLineBreakTag(QStringView tag) {
    QStringView name = tag.trimmed();
    if (name.startsWith(u'/'))
        name = name.mid(1);
    qsizetype length = 0;
    while (length < name.size()) {
        const QChar character = name.at(length);
        if (character.isSpace() || character == u'/')
            break;
        ++length;
    }
    name = name.left(length);
    return name.compare(QLatin1String("br"), Qt::CaseInsensitive) == 0 ||
           name.compare(QLatin1String("p"), Qt::CaseInsensitive) == 0;
}

} // namespace

QString itemStatusName(ItemStatus status) {
    switch (status) {
    case ItemStatus::Passive:
        return QStringLiteral("Passive");
    case ItemStatus::Active:
        return QStringLiteral("Active");
    case ItemStatus::NeedsAttention:
        return QStringLiteral("NeedsAttention");
    }
    return QStringLiteral("Passive");
}

IconPixmap encodeIconPixmap(const QImage& image) {
    if (image.isNull())
        return {};
    // `Format_ARGB32` is the straight-alpha 0xAARRGGBB layout the specification
    // describes. Converting also normalises externally backed images, whose
    // scanlines may be padded, to the tight layout written below.
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);
    if (source.isNull())
        return {};

    IconPixmap result;
    result.width = source.width();
    result.height = source.height();
    result.argb32.resize(source.width() * source.height() * 4);
    char* output = result.argb32.data();
    for (int y = 0; y < source.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(source.constScanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const QRgb pixel = line[x];
            // Network byte order: alpha first, then red, green and blue.
            output[0] = static_cast<char>(qAlpha(pixel));
            output[1] = static_cast<char>(qRed(pixel));
            output[2] = static_cast<char>(qGreen(pixel));
            output[3] = static_cast<char>(qBlue(pixel));
            output += 4;
        }
    }
    return result;
}

QVector<IconPixmap> encodeIconPixmaps(const QImage& image, const QVector<int>& sizes) {
    QVector<IconPixmap> result;
    if (image.isNull())
        return result;
    result.reserve(sizes.size());
    for (int size : sizes) {
        if (size <= 0)
            continue;
        const QImage scaled =
            image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        result.push_back(encodeIconPixmap(scaled));
    }
    return result;
}

QString plainText(QStringView richText) {
    QString output;
    output.reserve(richText.size());
    for (qsizetype index = 0; index < richText.size(); ++index) {
        const QChar character = richText.at(index);
        if (character == u'<') {
            const qsizetype closing = richText.indexOf(u'>', index + 1);
            if (closing < 0) {
                // A bare `<` is text, not the start of markup.
                output += character;
                continue;
            }
            if (isLineBreakTag(richText.mid(index + 1, closing - index - 1)))
                output += QLatin1Char('\n');
            index = closing;
            continue;
        }
        if (character == u'&') {
            const qsizetype semicolon = richText.indexOf(u';', index + 1);
            if (semicolon > index && semicolon - index - 1 <= maximumEntityLength - 2) {
                const QStringView body = richText.mid(index + 1, semicolon - index - 1);
                bool decoded = false;
                for (const NamedEntity& entity : namedEntities) {
                    if (body.compare(QLatin1String(entity.name), Qt::CaseInsensitive) == 0) {
                        output += QLatin1Char(entity.value);
                        decoded = true;
                        break;
                    }
                }
                uint code = 0;
                if (!decoded && parseCodePoint(body, code) && code != 0) {
                    appendCodePoint(output, code);
                    decoded = true;
                }
                if (decoded) {
                    index = semicolon;
                    continue;
                }
            }
            output += character;
            continue;
        }
        output += character;
    }
    return output.trimmed();
}

ToolTip makeToolTip(const QString& title, const QString& subtitle,
                    const QVector<IconPixmap>& iconPixmaps) {
    ToolTip toolTip;
    toolTip.iconPixmaps = iconPixmaps;
    toolTip.title = plainText(title);
    toolTip.subtitle = plainText(subtitle);
    return toolTip;
}

bool isWaylandSession(const QByteArray& waylandDisplay, const QByteArray& sessionType) {
    if (sessionType.compare("wayland", Qt::CaseInsensitive) == 0)
        return true;
    if (sessionType.compare("x11", Qt::CaseInsensitive) == 0)
        return false;
    return !waylandDisplay.isEmpty();
}

bool needsStatusNotifierItem(bool platformTrayAvailable, bool waylandSession) {
    return !platformTrayAvailable || waylandSession;
}

} // namespace snow_shot::platform::linux::tray
