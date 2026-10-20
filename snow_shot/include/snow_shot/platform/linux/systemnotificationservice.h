#ifndef SNOW_SHOT_PLATFORM_LINUX_SYSTEMNOTIFICATIONSERVICE_H
#define SNOW_SHOT_PLATFORM_LINUX_SYSTEMNOTIFICATIONSERVICE_H

#include "snow_shot/platform/systemnotification.h"

#include <QDBusConnection>
#include <QHash>
#include <QObject>
#include <QString>

#ifdef linux
#undef linux
#endif

namespace snow_shot::platform::linux {

/// Notifications through the notification daemon on the session bus.
///
/// The alternative - `QSystemTrayIcon::showMessage` - is not equivalent on
/// Linux. It travels through the StatusNotifier item, so it silently produces
/// nothing when the desktop has no SNI host, and it reports success either way.
/// A desktop-installed Deepin or UOS session runs `dde-daemon`'s notification
/// service regardless of the tray, and a session without any daemon answers with
/// an error this class can report as `Unavailable`.
///
/// Works the same under Wayland: the daemon is a session service, not an X11
/// client, so nothing here depends on the display protocol.
class SystemNotificationService final : public QObject {
    Q_OBJECT

  public:
    /// Mirrors the macOS service so the tray controller's activation handler
    /// reads identically on both platforms.
    enum class Action { OpenAbout, OpenRecording };

    explicit SystemNotificationService(QObject* parent = nullptr);
    ~SystemNotificationService() override;

    /// Submits the notification. Completion runs on the thread that owns this
    /// object; the caller already re-queues it.
    void show(const SystemNotificationRequest& request, SystemNotificationCompletion completed);

  signals:
    /// Raised when the user clicks a delivered bubble. `filePath` is the path the
    /// notification carried, empty for anything but `OpenRecording`.
    void activated(snow_shot::platform::linux::SystemNotificationService::Action action,
                   const QString& filePath);

  private slots:
    void onActionInvoked(uint id, const QString& key);
    void onNotificationClosed(uint id, uint reason);

  private:
    /// True once the daemon has been seen on the bus. Only the positive answer is
    /// cached: a session that starts the daemon later must still work.
    [[nodiscard]] bool daemonAvailable();

    struct Delivered {
        SystemNotificationAction action = SystemNotificationAction::None;
        QString filePath;
    };

    QDBusConnection m_bus;
    bool m_bodyMarkupSupported = true;
    bool m_daemonSeen = false;
    /// Bubbles still on screen, keyed by the id the daemon returned. Unknown ids
    /// in the signals below belong to other applications and are ignored.
    QHash<uint, Delivered> m_delivered;
    /// The last id per action, so a repeated notification replaces its
    /// predecessor instead of stacking a second bubble.
    QHash<int, uint> m_replacesId;
};

} // namespace snow_shot::platform::linux

#endif // SNOW_SHOT_PLATFORM_LINUX_SYSTEMNOTIFICATIONSERVICE_H
