#include "snow_shot/platform/linux/systemnotificationservice.h"

#include "snow_shot/app/edition.h"
#include "snow_shot/platform/linux/notificationpayload.h"

#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QStringList>
#include <utility>

#ifdef linux
#undef linux
#endif

namespace snow_shot::platform::linux {
namespace {

using Status = SystemNotificationResult::Status;

SystemNotificationResult unavailable(const QString& diagnostic) {
    return {Status::Unavailable, diagnostic};
}

/// D-Bus error to result status. `AccessDenied` is the daemon or its policy
/// refusing the bubble (do-not-disturb, per-application suppression), which is a
/// decision rather than a fault, so it is reported as `Denied` and not retried.
SystemNotificationResult failureFrom(const QDBusError& error) {
    Status status = Status::Failed;
    if (error.type() == QDBusError::AccessDenied)
        status = Status::Denied;
    else if (error.type() == QDBusError::ServiceUnknown || error.type() == QDBusError::Disconnected)
        status = Status::Unavailable;
    return {status, QStringLiteral("%1: %2").arg(error.name(), error.message())};
}

QDBusMessage notificationCall(const QString& member) {
    return QDBusMessage::createMethodCall(QLatin1String(notification::serviceName),
                                          QLatin1String(notification::objectPath),
                                          QLatin1String(notification::interfaceName), member);
}

} // namespace

SystemNotificationService::SystemNotificationService(QObject* parent)
    : QObject(parent), m_bus(QDBusConnection::sessionBus()) {
    if (!m_bus.isConnected()) {
        // Nothing to subscribe to. `show` reports this per request so the caller
        // can fall back to the tray instead of a switch that silently does nothing.
        return;
    }

    auto* capabilities = new QDBusPendingCallWatcher(
        m_bus.asyncCall(notificationCall(QStringLiteral("GetCapabilities"))), this);
    QObject::connect(capabilities, &QDBusPendingCallWatcher::finished, this,
                     [this](QDBusPendingCallWatcher* call) {
                         call->deleteLater();
                         const QDBusPendingReply<QStringList> reply = *call;
                         // A failed probe keeps the optimistic default: every daemon
                         // in the field implements markup, and the only cost of being
                         // wrong is that a `&` in a body is shown as `&amp;` once.
                         if (!reply.isError()) {
                             m_bodyMarkupSupported =
                                 reply.value().contains(QStringLiteral("body-markup"));
                         }
                     });

    (void)m_bus.connect(QLatin1String(notification::serviceName),
                        QLatin1String(notification::objectPath),
                        QLatin1String(notification::interfaceName), QStringLiteral("ActionInvoked"),
                        this, SLOT(onActionInvoked(uint, QString)));
    (void)m_bus.connect(
        QLatin1String(notification::serviceName), QLatin1String(notification::objectPath),
        QLatin1String(notification::interfaceName), QStringLiteral("NotificationClosed"), this,
        SLOT(onNotificationClosed(uint, uint)));
}

SystemNotificationService::~SystemNotificationService() {
    if (!m_bus.isConnected())
        return;
    (void)m_bus.disconnect(
        QLatin1String(notification::serviceName), QLatin1String(notification::objectPath),
        QLatin1String(notification::interfaceName), QStringLiteral("ActionInvoked"), this,
        SLOT(onActionInvoked(uint, QString)));
    (void)m_bus.disconnect(
        QLatin1String(notification::serviceName), QLatin1String(notification::objectPath),
        QLatin1String(notification::interfaceName), QStringLiteral("NotificationClosed"), this,
        SLOT(onNotificationClosed(uint, uint)));
}

void SystemNotificationService::show(const SystemNotificationRequest& request,
                                     SystemNotificationCompletion completed) {
    const auto finish = [completed](SystemNotificationResult result) {
        if (completed)
            completed(std::move(result));
    };

    if (!m_bus.isConnected()) {
        finish(unavailable(QStringLiteral(
            "The D-Bus session bus is unreachable, so no notification daemon can be asked.")));
        return;
    }
    if (!daemonAvailable()) {
        finish(unavailable(
            QStringLiteral("org.freedesktop.Notifications is not registered on the session bus.")));
        return;
    }

    const notification::Payload payload =
        notification::buildPayload(request, app::edition::productId(), m_bodyMarkupSupported);
    const uint replaces = m_replacesId.value(static_cast<int>(request.action), 0u);
    QDBusMessage message = notificationCall(QStringLiteral("Notify"));
    message.setArguments(
        notification::notifyArguments(payload, app::edition::productName(), replaces));

    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message), this);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, this,
                     [this, request, finish](QDBusPendingCallWatcher* call) {
                         call->deleteLater();
                         const QDBusPendingReply<uint> reply = *call;
                         if (reply.isError()) {
                             finish(failureFrom(reply.error()));
                             return;
                         }
                         const uint id = reply.value();
                         m_daemonSeen = true;
                         m_delivered.insert(id, Delivered{request.action, request.filePath});
                         if (request.action != SystemNotificationAction::None)
                             m_replacesId.insert(static_cast<int>(request.action), id);
                         finish({});
                     });
}

bool SystemNotificationService::daemonAvailable() {
    if (m_daemonSeen)
        return true;
    QDBusConnectionInterface* busInterface = m_bus.interface();
    if (busInterface == nullptr)
        return false;
    // Not cached when false: a session that starts its notification daemon after
    // this application must still get working notifications.
    m_daemonSeen = busInterface->isServiceRegistered(QLatin1String(notification::serviceName));
    return m_daemonSeen;
}

void SystemNotificationService::onActionInvoked(uint id, const QString& key) {
    const auto it = m_delivered.constFind(id);
    if (it == m_delivered.constEnd())
        return;
    const Delivered delivered = it.value();
    m_delivered.erase(it);
    // Only the reserved key is offered (see notification::actionList); anything
    // else means a daemon invention and has no meaning here.
    if (key != QLatin1String("default"))
        return;
    switch (delivered.action) {
    case SystemNotificationAction::OpenAbout:
        emit activated(Action::OpenAbout, QString());
        break;
    case SystemNotificationAction::OpenRecording:
        emit activated(Action::OpenRecording, delivered.filePath);
        break;
    case SystemNotificationAction::None:
        break;
    }
}

void SystemNotificationService::onNotificationClosed(uint id, uint reason) {
    Q_UNUSED(reason)
    if (m_delivered.remove(id) == 0)
        return;
    // The bubble is gone, so its id can no longer be replaced. Forgetting it here
    // keeps `replaces_id` from pointing at a recycled identifier later.
    for (auto it = m_replacesId.begin(); it != m_replacesId.end();) {
        if (it.value() == id)
            it = m_replacesId.erase(it);
        else
            ++it;
    }
}

} // namespace snow_shot::platform::linux
