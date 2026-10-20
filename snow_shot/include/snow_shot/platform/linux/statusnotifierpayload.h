#ifndef SNOW_SHOT_PLATFORM_LINUX_STATUSNOTIFIERPAYLOAD_H
#define SNOW_SHOT_PLATFORM_LINUX_STATUSNOTIFIERPAYLOAD_H

#include <QByteArray>
#include <QImage>
#include <QMetaType>
#include <QString>
#include <QStringView>
#include <QVector>

// GCC used to define `linux` as a macro. Current toolchains no longer do, but a
// translation unit that pulls in an old system header still can, and that would
// turn the namespace below into `platform::1`. Clearing it here costs nothing.
#ifdef linux
#undef linux
#endif

namespace snow_shot::platform::linux::tray {

/// Names from the StatusNotifierItem specification.
///
/// The specification is a KDE one that freedesktop desktops adopted; UOS's
/// `dde-dock`, Plasma's panel and the GNOME AppIndicator extension all watch for
/// this exact service name. Nothing else in the session is consulted.
inline constexpr char watcherService[] = "org.kde.StatusNotifierWatcher";
inline constexpr char watcherPath[] = "/StatusNotifierWatcher";
inline constexpr char watcherInterface[] = "org.kde.StatusNotifierWatcher";
inline constexpr char itemInterface[] = "org.kde.StatusNotifierItem";
inline constexpr char itemPath[] = "/StatusNotifierItem";

/// The `Menu` property when the item has no D-Bus menu. Hosts that read this
/// value call `ContextMenu` on the item instead of rendering a menu of their own.
inline constexpr char noMenuPath[] = "/NO_DBUSMENU";

/// One `(iiay)` entry of `IconPixmap`: `argb32` is the image in network byte
/// order - four bytes per pixel, alpha first - row-major, with no row padding.
struct IconPixmap {
    int width = 0;
    int height = 0;
    QByteArray argb32;

    bool operator==(const IconPixmap& other) const {
        return width == other.width && height == other.height && argb32 == other.argb32;
    }
};

/// The `ToolTip` property, whose D-Bus signature is `(sa(iiay)ss)`: a theme icon
/// name, fallback pixmaps, a title and a subtitle. Hosts render the title bold
/// and the subtitle below it.
struct ToolTip {
    QString iconName;
    QVector<IconPixmap> iconPixmaps;
    QString title;
    QString subtitle;

    bool operator==(const ToolTip& other) const {
        return iconName == other.iconName && iconPixmaps == other.iconPixmaps &&
               title == other.title && subtitle == other.subtitle;
    }
};

/// The `Status` property. `Passive` means "not shown by the host", which is what
/// hiding the tray icon has to mean: the item stays registered either way, so
/// showing it again is a status change rather than a re-registration.
enum class ItemStatus { Passive, Active, NeedsAttention };

/// Which of the item's three activation methods fired.
enum class Activation { Primary, Secondary, Context };

[[nodiscard]] QString itemStatusName(ItemStatus status);

/// Encodes an image for the `IconPixmap` property.
///
/// The specification asks for ARGB32 in network byte order, which is *not* what
/// a `QImage` holds in memory on any platform this application ships on: Qt's
/// `Format_ARGB32` is non-premultiplied 0xAARRGGBB in host byte order. So each
/// pixel is written byte by byte, alpha first, and non-contiguous scanlines
/// (sub-images, externally backed images) are repacked to a tight buffer.
///
/// Premultiplication is deliberately *not* applied: hosts composite the icon
/// themselves, and Qt's own tray implementation ships straight alpha here.
[[nodiscard]] IconPixmap encodeIconPixmap(const QImage& image);

/// Encodes one image per requested size, for hosts that pick a size instead of
/// scaling. A size that is not positive is skipped; aspect ratio is preserved,
/// so a non-square source yields non-square entries.
[[nodiscard]] QVector<IconPixmap> encodeIconPixmaps(const QImage& image, const QVector<int>& sizes);

/// Converts the small HTML subset this project uses in labels into the plain
/// text a tooltip needs.
///
/// `<br>` and paragraph boundaries become newlines, every other tag is dropped,
/// and the five predefined entities plus numeric ones are decoded. A `<` with no
/// closing `>` is kept literally rather than swallowing the rest of the string -
/// tooltips carry file paths, and dropping a path is worse than showing markup.
[[nodiscard]] QString plainText(QStringView richText);

/// Builds the `ToolTip` property. The title and subtitle are converted to plain
/// text; `iconName` is left empty so hosts fall back to `iconPixmaps`.
[[nodiscard]] ToolTip makeToolTip(const QString& title, const QString& subtitle,
                                  const QVector<IconPixmap>& iconPixmaps);

/// Whether this is a Wayland session, judged from the two variables the session
/// sets. `XDG_SESSION_TYPE` wins when present because `WAYLAND_DISPLAY` also
/// survives inside `XWayland` clients that were started from a Wayland session.
[[nodiscard]] bool isWaylandSession(const QByteArray& waylandDisplay,
                                    const QByteArray& sessionType);

/// Whether to export our own item at all.
///
/// The platform plugin already exports one wherever an X11 tray exists, and a
/// second icon for the same process is worse than none - so it is only skipped
/// when that plugin cannot be seen by the desktop: no tray at all, or a Wayland
/// session, where the `xcb` plugin's item would live in `XWayland` and never
/// reach the compositor's panel.
[[nodiscard]] bool needsStatusNotifierItem(bool platformTrayAvailable, bool waylandSession);

} // namespace snow_shot::platform::linux::tray

Q_DECLARE_METATYPE(snow_shot::platform::linux::tray::IconPixmap)
Q_DECLARE_METATYPE(QVector<snow_shot::platform::linux::tray::IconPixmap>)
Q_DECLARE_METATYPE(snow_shot::platform::linux::tray::ToolTip)

#endif // SNOW_SHOT_PLATFORM_LINUX_STATUSNOTIFIERPAYLOAD_H
