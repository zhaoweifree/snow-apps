#include "snow_shot/compat/qt_compat.h"
#include "snow_shot/presentation/globalmousemanager.h"
#include "snow_linux.h"

#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QPoint>
#include <QPointF>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace snow_shot::presentation {
namespace {

/// The status the manager reports when nothing can be observed.
constexpr quint32 kUnavailableCode = 1;

// GlobalMouseModifier -> the bits snow_linux.h defines. Two encodings, one
// meaning: the flag values in globalmousetypes.h are this layer's own, and
// keeping the translation explicit is what stops a renumbering on either side
// from silently grabbing the wrong chord.
std::optional<quint32> modifierBit(GlobalMouseModifier modifier) {
    switch (modifier) {
    case GlobalMouseModifier::Control:
        return SNOW_LINUX_MODIFIER_CONTROL;
    case GlobalMouseModifier::Alt:
        return SNOW_LINUX_MODIFIER_ALT;
    case GlobalMouseModifier::Shift:
        return SNOW_LINUX_MODIFIER_SHIFT;
    case GlobalMouseModifier::Super:
        return SNOW_LINUX_MODIFIER_META;
    case GlobalMouseModifier::Command:
        // macOS's Command key. X11 has no equivalent chord that a user would
        // expect to work here, and mapping it onto Super would make a binding
        // recorded on macOS fire on a key the user never mentioned.
        return std::nullopt;
    }
    return std::nullopt;
}

bool x11Session() {
    // X11 only. Under a Wayland session Qt reports `wayland` even while XWayland
    // runs, and there a grab would only fire for windows that XWayland owns --
    // a gesture that stops working exactly when the user is in a native Wayland
    // application. Wayland needs the portal, which is a later stage of the port.
    return qGuiApp != nullptr && QGuiApplication::platformName() == QStringLiteral("xcb");
}

// A callback from the Rust worker arrives on that worker's thread, so it cannot
// touch the manager directly. This object is the context of the queued
// invocation: Qt drops pending metacalls whose context object has been
// destroyed, which is what keeps a callback that was already queued from running
// after the backend is gone.
class DragDispatcher final : public QObject {
  public:
    std::function<void(GlobalMouseDragEvent)> deliver;
};

class LinuxGlobalMouseBackend final : public GlobalMouseBackend {
  public:
    LinuxGlobalMouseBackend() : m_dispatcher(std::make_unique<DragDispatcher>()) {
        // Created up front, not in start(): the manager asks for the permission
        // state before it starts the backend, and "not created yet" would read
        // as "unsupported" on every launch.
        if (x11Session()) {
            static_cast<void>(ensureService());
        }
    }

    ~LinuxGlobalMouseBackend() override {
        // Releases every grab and joins the worker before the dispatcher dies, so
        // no callback can still be in flight when the members below go away.
        m_service.reset();
    }

    void start(Handler handler, FailureHandler failure) override {
        m_handler = std::move(handler);
        if (!x11Session()) {
            return;
        }
        if (m_service == nullptr && !ensureService()) {
            if (failure) {
                failure(kUnavailableCode);
            }
            return;
        }
        // A restart after stop() gets a fresh service, so it needs the bindings
        // again.
        applyConfiguration();
    }

    void stop() override {
        m_service.reset();
        m_handler = {};
    }

    void configure(const GlobalMouseConfiguration& configuration) override {
        m_configuration = configuration;
        applyConfiguration();
    }

    void cancel(quint64 id) override {
        if (m_service != nullptr) {
            snow_linux_global_mouse_cancel(m_service.get(), id);
        }
    }

    void beginButtonDrag(settings::SettingsGlobalMouseAction) override {
        // Deliberately unimplemented on X11. This entry point exists for the
        // home screen's "press on a card, then drag anywhere" flow: the press has
        // already been delivered to one of this program's own windows, so the Qt
        // connection holds the implicit grab. An XGrabPointer from here would be
        // refused (AlreadyGrabbed) while that grab lives, and the motion would
        // keep going to the Qt window rather than to this service -- so the drag
        // would report a press and then nothing. Doing it properly means
        // observing the drag through Qt and following the pointer with
        // QCursor::pos(), which is a different design from the chord path; until
        // then the card drag is a no-op on Linux rather than a gesture that
        // half-works.
    }

    GlobalMousePermissionState permissionState() const override {
        GlobalMousePermissionState state;
        if (m_service != nullptr) {
            state.status = GlobalMousePermissionState::Status::Ready;
            state.listenGranted = true;
            state.accessibilityGranted = true;
            state.tapAvailable = true;
        } else {
            state.status = GlobalMousePermissionState::Status::Unavailable;
        }
        return state;
    }

  private:
    bool ensureService() {
        SnowLinuxGlobalMouse* service =
            snow_linux_global_mouse_create(&LinuxGlobalMouseBackend::onDrag, this);
        if (service == nullptr) {
            return false;
        }
        m_service.reset(service);
        m_dispatcher->deliver = [this](const GlobalMouseDragEvent& event) {
            if (m_handler) {
                m_handler(event);
            }
        };
        return true;
    }

    void applyConfiguration() {
        if (m_service == nullptr) {
            return;
        }
        std::vector<SnowLinuxMouseBinding> entries;
        entries.reserve(static_cast<std::size_t>(m_configuration.bindings.size()));
        for (const GlobalMouseBinding& binding : m_configuration.bindings) {
            quint32 modifiers = 0;
            bool supported = true;
            for (const auto modifier : {GlobalMouseModifier::Control, GlobalMouseModifier::Alt,
                                        GlobalMouseModifier::Shift, GlobalMouseModifier::Super,
                                        GlobalMouseModifier::Command}) {
                if (!binding.modifiers.testFlag(modifier)) {
                    continue;
                }
                const std::optional<quint32> bit = modifierBit(modifier);
                if (!bit.has_value()) {
                    supported = false;
                    break;
                }
                modifiers |= *bit;
            }
            if (!supported) {
                continue;
            }
            entries.push_back(SnowLinuxMouseBinding{static_cast<std::int32_t>(binding.action),
                                                    static_cast<std::int32_t>(binding.button),
                                                    modifiers});
        }
        // Both calls are synchronous round-trips to the worker, which is what
        // makes the grabs match the configuration before the next press.
        const int32_t installed =
            snow_linux_global_mouse_set_bindings(m_service.get(), entries.data(), entries.size());
        if (!entries.empty() && installed == 0) {
            // Every chord refused. The usual cause is a window manager that
            // already holds them, and it is worth one line in the log: from the
            // user's side the feature simply never fires, with nothing to see.
            qWarning("Global mouse: none of the %llu configured chords could be grabbed",
                     static_cast<unsigned long long>(entries.size()));
        }
        // Off while a capture is already running: a press that cannot start
        // anything must not be swallowed.
        static_cast<void>(snow_linux_global_mouse_set_enabled(
            m_service.get(), m_configuration.captureAvailable ? 1 : 0));
    }

    static void onDrag(std::int32_t phase, std::int32_t action, std::uint64_t dragId,
                       std::int32_t x, std::int32_t y, void* userData) {
        static_cast<LinuxGlobalMouseBackend*>(userData)->postDrag(phase, action, dragId,
                                                                  QPoint(x, y));
    }

    void postDrag(std::int32_t phase, std::int32_t action, quint64 dragId, const QPoint& position) {
        DragDispatcher* dispatcher = m_dispatcher.get();
        QMetaObject::invokeMethod(
            dispatcher,
            [dispatcher, phase, action, dragId, position] {
                if (!dispatcher->deliver) {
                    return;
                }
                using Kind = GlobalMouseDragEvent::Kind;
                GlobalMouseDragEvent event;
                switch (phase) {
                case SNOW_LINUX_DRAG_BEGIN:
                    event.kind = Kind::Begin;
                    break;
                case SNOW_LINUX_DRAG_UPDATE:
                    event.kind = Kind::Update;
                    break;
                case SNOW_LINUX_DRAG_FINISH:
                    event.kind = Kind::Finish;
                    break;
                case SNOW_LINUX_DRAG_CANCEL:
                    event.kind = Kind::Cancel;
                    break;
                default:
                    return;
                }
                // An action id outside the enum is a build mismatch, not a
                // runtime condition: reporting it as "no action" would hand the
                // manager an event it cannot route.
                constexpr auto last =
                    static_cast<std::int32_t>(settings::SettingsGlobalMouseAction::ScreenRecording);
                if (action < 0 || action > last) {
                    return;
                }
                event.id = dragId;
                event.action = static_cast<settings::SettingsGlobalMouseAction>(action);
                event.position = QPointF(position);
                // The grab reports root-window pixels, which are device pixels.
                event.coordinateSpace = GlobalMouseCoordinateSpace::PhysicalPixels;
                dispatcher->deliver(event);
            },
            Qt::QueuedConnection);
    }

    struct ServiceDeleter {
        void operator()(SnowLinuxGlobalMouse* handle) const {
            snow_linux_global_mouse_destroy(handle);
        }
    };

    std::unique_ptr<DragDispatcher> m_dispatcher;
    std::unique_ptr<SnowLinuxGlobalMouse, ServiceDeleter> m_service;
    GlobalMouseConfiguration m_configuration;
    Handler m_handler;
};

} // namespace

std::unique_ptr<GlobalMouseBackend> createLinuxGlobalMouseBackend() {
    return std::make_unique<LinuxGlobalMouseBackend>();
}

} // namespace snow_shot::presentation
