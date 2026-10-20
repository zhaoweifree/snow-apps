#include "snow_shot/platform/linux/statusnotifierservice.h"

#include "snow_shot/app/edition.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDebug>
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QVector>

#include <utility>

#ifdef linux
#undef linux
#endif

namespace snow_shot::platform::linux {

namespace {

/// How long registering with the watcher may block. The call happens once, on
/// the GUI thread, so a panel that is hung must not take the application with it;
/// 500 ms is far longer than a healthy round trip to a session-bus peer.
constexpr int registrationTimeoutMs = 500;

/// Sizes handed to the host. 22 is the classic tray height and 24 what dde-dock
/// uses; the rest cover hidpi panels that ask for a larger pixmap instead of
/// scaling the one they got.
QVector<int> iconSizes() {
    return {16, 22, 24, 32, 48, 64};
}

/// The largest pixmap asked of the `QIcon` before it is scaled down. Icons in
/// this application ship up to 256 px, but the tray never needs more than this
/// and decoding a 256 px pixmap for a 22 px slot wastes startup time.
constexpr int sourceIconSize = 64;

} // namespace

} // namespace snow_shot::platform::linux

namespace snow_shot::platform::linux::tray {

// `(iiay)` and `(sa(iiay)ss)` respectively. Declared in the type's own namespace
// so that `qDBusRegisterMetaType` below finds them by argument-dependent lookup
// and can derive the signatures from them.
QDBusArgument& operator<<(QDBusArgument& argument, const IconPixmap& pixmap) {
    argument.beginStructure();
    argument << pixmap.width << pixmap.height << pixmap.argb32;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, IconPixmap& pixmap) {
    argument.beginStructure();
    argument >> pixmap.width >> pixmap.height >> pixmap.argb32;
    argument.endStructure();
    return argument;
}

QDBusArgument& operator<<(QDBusArgument& argument, const ToolTip& toolTip) {
    argument.beginStructure();
    argument << toolTip.iconName << toolTip.iconPixmaps << toolTip.title << toolTip.subtitle;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, ToolTip& toolTip) {
    argument.beginStructure();
    argument >> toolTip.iconName >> toolTip.iconPixmaps >> toolTip.title >> toolTip.subtitle;
    argument.endStructure();
    return argument;
}

} // namespace snow_shot::platform::linux::tray

namespace snow_shot::platform::linux {

namespace {

void registerMetaTypes() {
    static const bool registered = []() {
        qDBusRegisterMetaType<tray::IconPixmap>();
        qDBusRegisterMetaType<QVector<tray::IconPixmap>>();
        qDBusRegisterMetaType<tray::ToolTip>();
        return true;
    }();
    Q_UNUSED(registered)
}

} // namespace

StatusNotifierService::StatusNotifierService(QObject* parent) : QObject(parent) {
    registerMetaTypes();
    title_ = app::edition::productName();
    toolTip_ = tray::makeToolTip(title_, {}, {});

    QDBusConnection connection = QDBusConnection::sessionBus();
    if (!connection.isConnected()) {
        qWarning("StatusNotifierItem: no session bus, the tray has no backend.");
        return;
    }
    // A panel that starts after the application is normal on a desktop session
    // that restores its panels late, so registration is retried when the watcher
    // shows up rather than being attempted once at construction time.
    connection.connect(QLatin1String("org.freedesktop.DBus"),
                       QLatin1String("/org/freedesktop/DBus"),
                       QLatin1String("org.freedesktop.DBus"), QLatin1String("NameOwnerChanged"),
                       this, SLOT(onNameOwnerChanged(QString, QString, QString)));
    exported_ = connection.registerObject(QLatin1String(tray::itemPath), this,
                                          QDBusConnection::ExportAllProperties |
                                              QDBusConnection::ExportAllSignals |
                                              QDBusConnection::ExportAllSlots);
    if (!exported_) {
        qWarning("StatusNotifierItem: could not export %s on the session bus.", tray::itemPath);
        return;
    }
    registerWithWatcher();
}

StatusNotifierService::~StatusNotifierService() {
    if (!exported_)
        return;
    QDBusConnection connection = QDBusConnection::sessionBus();
    connection.unregisterObject(QLatin1String(tray::itemPath));
}

bool StatusNotifierService::isRegistered() const {
    return registered_;
}

void StatusNotifierService::registerWithWatcher() {
    if (!exported_ || registered_)
        return;
    QDBusConnection connection = QDBusConnection::sessionBus();
    QDBusConnectionInterface* bus = connection.interface();
    if (bus == nullptr)
        return;
    const QDBusReply<bool> present = bus->isServiceRegistered(QLatin1String(tray::watcherService));
    if (!present.isValid() || !present.value())
        return;

    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(tray::watcherService), QLatin1String(tray::watcherPath),
        QLatin1String(tray::watcherInterface), QStringLiteral("RegisterStatusNotifierItem"));
    // A bus name rather than an object path: that is what Qt's and GTK's own
    // items send, so watchers have exercised this form the most. The item is
    // then assumed to live at `itemPath`, which is where it was exported.
    call << connection.baseService();
    const QDBusMessage reply = connection.call(call, QDBus::Block, registrationTimeoutMs);
    registered_ = reply.type() != QDBusMessage::ErrorMessage;
    if (!registered_) {
        qWarning("StatusNotifierItem: the watcher refused the item: %s %s",
                 qPrintable(reply.errorName()), qPrintable(reply.errorMessage()));
    }
    if (registrationHandler_)
        registrationHandler_(registered_);
}

void StatusNotifierService::onNameOwnerChanged(const QString& name, const QString& oldOwner,
                                               const QString& newOwner) {
    if (name != QLatin1String(tray::watcherService))
        return;
    // Losing the watcher means the item is no longer displayed; the new one has
    // an empty list of items and has to be told about this one.
    if (newOwner.isEmpty()) {
        if (registered_ && registrationHandler_)
            registrationHandler_(false);
        registered_ = false;
        return;
    }
    Q_UNUSED(oldOwner)
    registerWithWatcher();
}

void StatusNotifierService::setTitle(const QString& title) {
    if (title_ == title)
        return;
    title_ = title;
    toolTip_.title = tray::plainText(title_);
    emit NewTitle();
}

void StatusNotifierService::setIcon(const QIcon& icon) {
    QVector<tray::IconPixmap> pixmaps;
    if (!icon.isNull()) {
        const QImage source = icon.pixmap(QSize(sourceIconSize, sourceIconSize)).toImage();
        pixmaps = tray::encodeIconPixmaps(source, iconSizes());
    }
    if (pixmaps == iconPixmaps_)
        return;
    iconPixmaps_ = pixmaps;
    toolTip_.iconPixmaps = iconPixmaps_;
    emit NewIcon();
}

void StatusNotifierService::setToolTip(const QString& text) {
    const tray::ToolTip toolTip = tray::makeToolTip(title_, text, iconPixmaps_);
    if (toolTip == toolTip_)
        return;
    toolTip_ = toolTip;
    emit NewToolTip();
}

void StatusNotifierService::setStatus(tray::ItemStatus status) {
    if (status_ == status)
        return;
    status_ = status;
    emit NewStatus(tray::itemStatusName(status_));
}

void StatusNotifierService::setActivationHandler(std::function<void(tray::Activation)> handler) {
    activationHandler_ = std::move(handler);
}

void StatusNotifierService::setRegistrationHandler(std::function<void(bool)> handler) {
    registrationHandler_ = std::move(handler);
}

QString StatusNotifierService::category() const {
    // "ApplicationStatus" is what a panel shows permanently; the other category
    // value ("SystemServices") is for items that belong to the session itself.
    return QStringLiteral("ApplicationStatus");
}

QString StatusNotifierService::id() const {
    return app::edition::productId();
}

QString StatusNotifierService::title() const {
    return title_;
}

QString StatusNotifierService::status() const {
    return tray::itemStatusName(status_);
}

QString StatusNotifierService::iconName() const {
    // Empty on purpose: the pixmaps are authoritative, and a name the host cannot
    // find would make it render its own fallback instead.
    return {};
}

QString StatusNotifierService::iconThemePath() const {
    return {};
}

QVector<tray::IconPixmap> StatusNotifierService::iconPixmap() const {
    return iconPixmaps_;
}

tray::ToolTip StatusNotifierService::toolTip() const {
    return toolTip_;
}

bool StatusNotifierService::itemIsMenu() const {
    // false: a primary activation is a click, not "open the menu". The menu is
    // requested separately through `ContextMenu`.
    return false;
}

QDBusObjectPath StatusNotifierService::menu() const {
    return QDBusObjectPath(QString::fromLatin1(tray::noMenuPath));
}

void StatusNotifierService::Activate(int x, int y) {
    Q_UNUSED(x)
    Q_UNUSED(y)
    if (activationHandler_)
        activationHandler_(tray::Activation::Primary);
}

void StatusNotifierService::SecondaryActivate(int x, int y) {
    Q_UNUSED(x)
    Q_UNUSED(y)
    if (activationHandler_)
        activationHandler_(tray::Activation::Secondary);
}

void StatusNotifierService::ContextMenu(int x, int y) {
    Q_UNUSED(x)
    Q_UNUSED(y)
    if (activationHandler_)
        activationHandler_(tray::Activation::Context);
}

} // namespace snow_shot::platform::linux
