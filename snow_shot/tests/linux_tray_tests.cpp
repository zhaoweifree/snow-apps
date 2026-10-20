#include "snow_shot/compat/qt_compat.h"
#include "snow_shot/platform/linux/statusnotifierpayload.h"

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace snow_shot::platform::linux::tray;

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

void requireEqual(int actual, int expected, const char* message) {
    if (actual != expected) {
        std::cerr << message << "\n  actual:   " << actual << "\n  expected: " << expected << '\n';
        std::exit(EXIT_FAILURE);
    }
}

/// Byte-for-byte comparison, so a failure names the offset instead of printing
/// two opaque blobs.
void requireBytes(const QByteArray& actual, const QByteArray& expected, const char* message) {
    if (actual == expected)
        return;
    std::cerr << message << "\n  expected " << expected.size() << " bytes, got " << actual.size()
              << ':';
    const qsizetype common = std::min(actual.size(), expected.size());
    for (qsizetype index = 0; index < common; ++index) {
        if (actual.at(index) != expected.at(index)) {
            std::cerr << " first difference at byte " << index;
            break;
        }
    }
    std::cerr << '\n';
    std::exit(EXIT_FAILURE);
}

/// One pixel is four bytes in network byte order: alpha, red, green, blue.
void pixelOrderIsNetworkByteOrder() {
    QImage image(1, 1, QImage::Format_ARGB32);
    image.setPixelColor(0, 0, QColor(0x11, 0x22, 0x33, 0x44));

    const IconPixmap encoded = encodeIconPixmap(image);
    requireEqual(encoded.width, 1, "a one pixel image keeps its width");
    requireEqual(encoded.height, 1, "a one pixel image keeps its height");
    requireBytes(encoded.argb32, QByteArray::fromHex("44112233"),
                 "the pixel must be laid out as A, R, G, B");
}

/// Every scanline must be copied, in order, with no padding in between.
void rowsArePackedInOrder() {
    QImage image(3, 2, QImage::Format_ARGB32);
    // A freshly allocated QImage holds whatever the allocator left behind; the
    // expectation below spells out the untouched pixels, so they must be known.
    image.fill(Qt::transparent);
    image.setPixelColor(0, 0, QColor(1, 2, 3, 255));
    image.setPixelColor(2, 0, QColor(4, 5, 6, 255));
    image.setPixelColor(1, 1, QColor(7, 8, 9, 128));

    const IconPixmap encoded = encodeIconPixmap(image);
    requireEqual(encoded.argb32.size(), 24, "two rows of three pixels are 24 bytes");

    QByteArray expected;
    expected.append(QByteArray::fromHex("ff010203")); // (0,0)
    expected.append(QByteArray::fromHex("00000000")); // (1,0) untouched, fully transparent
    expected.append(QByteArray::fromHex("ff040506")); // (2,0)
    expected.append(QByteArray::fromHex("00000000")); // (0,1)
    expected.append(QByteArray::fromHex("80070809")); // (1,1)
    expected.append(QByteArray::fromHex("00000000")); // (2,1)
    requireBytes(encoded.argb32, expected, "row-major, tightly packed");
}

/// An image backed by a buffer with a wider stride than its width still has to
/// come out tight: hosts index the array as `width * height * 4`.
void paddedStrideIsDropped() {
    const int width = 2;
    const int height = 2;
    const int stride = width * 4 + 8;
    std::vector<uchar> buffer(static_cast<size_t>(stride) * height, 0);
    QImage borrowed(buffer.data(), width, height, stride, QImage::Format_ARGB32);
    require(!borrowed.isNull(), "the externally backed image must be usable");
    borrowed.setPixelColor(0, 0, QColor(10, 20, 30, 40));
    borrowed.setPixelColor(1, 1, QColor(50, 60, 70, 80));
    requireEqual(borrowed.bytesPerLine(), stride, "the test image really has a wider stride");

    const IconPixmap encoded = encodeIconPixmap(borrowed);
    requireEqual(encoded.argb32.size(), width * height * 4,
                 "the encoding must not inherit the padding");
    requireBytes(encoded.argb32.left(4), QByteArray::fromHex("280a141e"),
                 "the first pixel survives the repack");
    requireBytes(encoded.argb32.right(4), QByteArray::fromHex("50323c46"),
                 "the last pixel survives the repack");
}

/// One entry per requested size, aspect ratio preserved.
void everyRequestedSizeIsEncoded() {
    QImage source(40, 20, QImage::Format_ARGB32);
    source.fill(QColor(0, 0, 0, 255));

    const QVector<IconPixmap> encoded = encodeIconPixmaps(source, {0, -4, 16, 22});
    requireEqual(encoded.size(), 2, "non-positive sizes are skipped");
    requireEqual(encoded.at(0).width, 16, "the first entry is the first size");
    requireEqual(encoded.at(1).width, 22, "the second entry is the second size");
    require(encoded.at(0).height <= 16, "aspect ratio is preserved, never exceeding the box");
    requireEqual(encoded.at(1).argb32.size(), encoded.at(1).width * encoded.at(1).height * 4,
                 "each encoded entry is tightly packed");

    require(encodeIconPixmaps(QImage(), {16}).isEmpty(), "a null image yields no entries");
    require(encodeIconPixmap(QImage()).argb32.isEmpty(), "a null image yields no data");
}

void tagsAreDropped() {
    requireEqual(plainText(QStringLiteral("<b>Snow Shot</b>")), QStringLiteral("Snow Shot"),
                 "formatting tags disappear");
    requireEqual(plainText(QStringLiteral("<span style=\"a\">x</span>")), QStringLiteral("x"),
                 "attributes go with the tag");
}

void lineBreaksSurvive() {
    requireEqual(plainText(QStringLiteral("first<br>second")), QStringLiteral("first\nsecond"),
                 "br is a newline");
    requireEqual(plainText(QStringLiteral("first<br/>second")), QStringLiteral("first\nsecond"),
                 "self closing br is a newline");
    requireEqual(plainText(QStringLiteral("first<BR />second")), QStringLiteral("first\nsecond"),
                 "tag names are case insensitive and may be padded");
    // Closing and opening a paragraph are two boundaries, so the rendered gap is
    // the blank line an HTML renderer produces as well.
    requireEqual(plainText(QStringLiteral("one</p><p>two")), QStringLiteral("one\n\ntwo"),
                 "paragraph boundaries are newlines");
}

void entitiesAreDecoded() {
    requireEqual(plainText(QStringLiteral("a &amp; b")), QStringLiteral("a & b"), "&amp;");
    requireEqual(plainText(QStringLiteral("&lt;tag&gt;")), QStringLiteral("<tag>"),
                 "&lt; and &gt;");
    requireEqual(plainText(QStringLiteral("&quot;q&quot;")), QStringLiteral("\"q\""), "&quot;");
    requireEqual(plainText(QStringLiteral("it&#39;s")), QStringLiteral("it's"), "decimal entity");
    requireEqual(plainText(QStringLiteral("it&#x27;s")), QStringLiteral("it's"),
                 "hexadecimal entity");
    requireEqual(plainText(QStringLiteral("a&nbsp;b")), QStringLiteral("a b"), "no-break space");
    requireEqual(plainText(QStringLiteral("&#x1F600;")), QStringLiteral("\U0001F600"),
                 "characters outside the BMP keep their code point");
}

void malformedMarkupIsKept() {
    requireEqual(plainText(QStringLiteral("costs < 5")), QStringLiteral("costs < 5"),
                 "a < with no > is text, not markup");
    requireEqual(plainText(QStringLiteral("&unknown;")), QStringLiteral("&unknown;"),
                 "an unknown entity is left alone");
    requireEqual(plainText(QStringLiteral("100% & more")), QStringLiteral("100% & more"),
                 "a & with no ; is text");
    requireEqual(plainText(QStringLiteral("&#0;")), QStringLiteral("&#0;"),
                 "a NUL code point is not a character");
    requireEqual(plainText(QStringLiteral("  padded  ")), QStringLiteral("padded"),
                 "surrounding whitespace is trimmed");
}

void tooltipCarriesBothLines() {
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(QColor(255, 255, 255, 255));
    const QVector<IconPixmap> pixmaps = encodeIconPixmaps(image, {16});

    const ToolTip toolTip =
        makeToolTip(QStringLiteral("<b>Snow Shot</b>"), QStringLiteral("a &amp; b"), pixmaps);
    requireEqual(toolTip.title, QStringLiteral("Snow Shot"), "the title is plain text");
    requireEqual(toolTip.subtitle, QStringLiteral("a & b"), "the subtitle is plain text");
    require(toolTip.iconName.isEmpty(), "no theme icon name, the pixmaps are used");
    requireEqual(toolTip.iconPixmaps.size(), 1, "the pixmaps are carried through");
}

void statusNamesMatchTheSpecification() {
    requireEqual(itemStatusName(ItemStatus::Passive), QStringLiteral("Passive"), "Passive");
    requireEqual(itemStatusName(ItemStatus::Active), QStringLiteral("Active"), "Active");
    requireEqual(itemStatusName(ItemStatus::NeedsAttention), QStringLiteral("NeedsAttention"),
                 "NeedsAttention");
}

void waylandIsDetectedFromEitherVariable() {
    require(isWaylandSession("wayland-0", "wayland"), "both variables agree");
    require(isWaylandSession("wayland-0", ""), "wayland-display alone is enough");
    require(isWaylandSession("", "wayland"), "the session type alone is enough");
    require(!isWaylandSession("wayland-0", "x11"),
            "an x11 session type wins over a stale wayland-display");
    require(!isWaylandSession("", "x11"), "an x11 session");
    require(!isWaylandSession("", ""), "no hint at all is not wayland");
}

void theItemIsOnlyExportedWhereThePluginCannotBeSeen() {
    require(!needsStatusNotifierItem(true, false), "an X11 tray is left to the platform plugin");
    require(needsStatusNotifierItem(false, false), "no tray at all needs our own item");
    require(needsStatusNotifierItem(true, true),
            "a Wayland session needs our own item even when XWayland reports a tray");
    require(needsStatusNotifierItem(false, true), "Wayland without a plugin tray");
}

} // namespace

int main() {
    pixelOrderIsNetworkByteOrder();
    rowsArePackedInOrder();
    paddedStrideIsDropped();
    everyRequestedSizeIsEncoded();
    tagsAreDropped();
    lineBreaksSurvive();
    entitiesAreDecoded();
    malformedMarkupIsKept();
    tooltipCarriesBothLines();
    statusNamesMatchTheSpecification();
    waylandIsDetectedFromEitherVariable();
    theItemIsOnlyExportedWhereThePluginCannotBeSeen();
    std::cout << "linux tray tests passed\n";
    return EXIT_SUCCESS;
}
