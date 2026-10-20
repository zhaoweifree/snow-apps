#ifndef SNOW_SHOT_PLATFORM_LINUX_STATUSNOTIFIERSERVICE_H
#define SNOW_SHOT_PLATFORM_LINUX_STATUSNOTIFIERSERVICE_H

#include "snow_shot/platform/linux/statusnotifierpayload.h"

#include <QDBusObjectPath>
#include <QIcon>
#include <QObject>
#include <QString>

#include <functional>

class QDBusArgument;

namespace snow_shot::platform::linux {

/// Exports this process as a `org.kde.StatusNotifierItem` on the session bus.
///
/// Qt 5 gets its tray from the platform plugin: `QSystemTrayIcon` is backed by
/// the `xcb` plugin's own item, so on a Wayland session there is no tray at all,
/// and inside `XWayland` the item exists but the compositor's panel never sees
/// it. The item itself is a session-bus object, which has nothing to do with the
/// display protocol - the same reasoning that made the notification backend in
/// this directory work on both sessions.
///
/// There is no D-Bus menu: the popup is a skinned `AdContextMenu`, not a `QMenu`,
/// so the item advertises `/NO_DBUSMENU` and the host calls `ContextMenu`, which
/// the owner turns into a popup at the pointer. A host that renders nothing in
/// that case would show no menu at all - that trade-off keeps the menu looking
/// like the rest of the application instead of like the panel's.
class StatusNotifierService final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierItem")
    Q_PROPERTY(QString Category READ category CONSTANT)
    Q_PROPERTY(QString Id READ id CONSTANT)
    Q_PROPERTY(QString Title READ title)
    Q_PROPERTY(QString Status READ status)
    Q_PROPERTY(QString IconName READ iconName)
    Q_PROPERTY(QString IconThemePath READ iconThemePath CONSTANT)
    Q_PROPERTY(QVector<snow_shot::platform::linux::tray::IconPixmap> IconPixmap READ iconPixmap)
    Q_PROPERTY(snow_shot::platform::linux::tray::ToolTip ToolTip READ toolTip)
    Q_PROPERTY(bool ItemIsMenu READ itemIsMenu CONSTANT)
    Q_PROPERTY(QDBusObjectPath Menu READ menu CONSTANT)

  public:
    explicit StatusNotifierService(QObject* parent = nullptr);
    ~StatusNotifierService() override;

    /// Registration with the watcher is synchronous, so this answers for the
    /// state the desktop is in right now. A watcher that appears later is picked
    /// up from `NameOwnerChanged` - panels legitimately start after applications.
    [[nodiscard]] bool isRegistered() const;

    void setTitle(const QString& title);
    void setIcon(const QIcon& icon);
    void setToolTip(const QString& text);
    void setStatus(tray::ItemStatus status);

    /// Called from the three activation methods. A callback rather than a signal
    /// keeps a C++-only type out of the exported signal list.
    void setActivationHandler(std::function<void(tray::Activation)> handler);

    /// Called whenever registration with the watcher succeeds or is lost. The
    /// owner uses it to retire the platform tray icon when a panel shows up
    /// after the application started - the item is then the one to show.
    void setRegistrationHandler(std::function<void(bool)> handler);

    [[nodiscard]] QString category() const;
    [[nodiscard]] QString id() const;
    [[nodiscard]] QString title() const;
    [[nodiscard]] QString status() const;
    [[nodiscard]] QString iconName() const;
    [[nodiscard]] QString iconThemePath() const;
    [[nodiscard]] QVector<tray::IconPixmap> iconPixmap() const;
    [[nodiscard]] tray::ToolTip toolTip() const;
    [[nodiscard]] bool itemIsMenu() const;
    [[nodiscard]] QDBusObjectPath menu() const;

  signals:
    void NewIcon();
    void NewStatus(const QString& status);
    void NewToolTip();
    void NewTitle();

  public slots:
    void Activate(int x, int y);
    void SecondaryActivate(int x, int y);
    void ContextMenu(int x, int y);

  private slots:
    void onNameOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner);

  private:
    void registerWithWatcher();

    QVector<tray::IconPixmap> iconPixmaps_;
    tray::ToolTip toolTip_;
    QString title_;
    tray::ItemStatus status_ = tray::ItemStatus::Active;
    std::function<void(tray::Activation)> activationHandler_;
    std::function<void(bool)> registrationHandler_;
    bool exported_ = false;
    bool registered_ = false;
};

} // namespace snow_shot::platform::linux

#endif // SNOW_SHOT_PLATFORM_LINUX_STATUSNOTIFIERSERVICE_H
