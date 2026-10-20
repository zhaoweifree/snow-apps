#include "../../presentation/services/globalshortcutbackend_p.h"

#include "snow_shot/compat/qt_compat.h"
#include "snow_linux.h"

#include <QGuiApplication>
#include <QHash>
#include <QKeySequence>
#include <QMetaObject>
#include <QObject>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace snow_shot::presentation {
namespace {

// Qt's key codes and the modifier flags the Rust side expects. Kept next to each
// other because they are one encoding in two places: the values mirror
// SNOW_LINUX_MODIFIER_* in snow_linux.h.
constexpr quint32 kModifierControl = SNOW_LINUX_MODIFIER_CONTROL;
constexpr quint32 kModifierAlt = SNOW_LINUX_MODIFIER_ALT;
constexpr quint32 kModifierShift = SNOW_LINUX_MODIFIER_SHIFT;
constexpr quint32 kModifierMeta = SNOW_LINUX_MODIFIER_META;

bool modifierKey(Qt::Key key) {
    return key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Shift ||
           key == Qt::Key_Meta || key == Qt::Key_AltGr || key == Qt::Key_Super_L ||
           key == Qt::Key_Super_R;
}

struct ParsedShortcut {
    Qt::Key key = Qt::Key_unknown;
    quint32 modifiers = 0;
};

// Turns a binding into the two values the platform layer takes. Returns
// std::nullopt for anything that cannot be a grab: a multi-step sequence, a
// modifier on its own (grabbing it would take that modifier away from the whole
// session), or text QKeySequence does not understand.
std::optional<ParsedShortcut> parseShortcut(const shortcuts::ShortcutBinding& binding) {
    const QKeySequence sequence =
        QKeySequence::fromString(binding.portableText, QKeySequence::PortableText);
    if (sequence.count() != 1) {
        return std::nullopt;
    }
    const QKeyCombination combination = snowKeyCombination(sequence);
    if (combination.key() == Qt::Key_unknown || modifierKey(combination.key())) {
        return std::nullopt;
    }

    quint32 modifiers = 0;
    const Qt::KeyboardModifiers qtModifiers = combination.keyboardModifiers();
    if (qtModifiers.testFlag(Qt::ControlModifier)) {
        modifiers |= kModifierControl;
    }
    if (qtModifiers.testFlag(Qt::AltModifier)) {
        modifiers |= kModifierAlt;
    }
    if (qtModifiers.testFlag(Qt::ShiftModifier)) {
        modifiers |= kModifierShift;
    }
    if (qtModifiers.testFlag(Qt::MetaModifier)) {
        modifiers |= kModifierMeta;
    }
    return ParsedShortcut{combination.key(), modifiers};
}

GlobalShortcutFailureReason failureReasonFor(int32_t status) {
    switch (status) {
    case SNOW_LINUX_HOTKEY_OK:
        return GlobalShortcutFailureReason::None;
    case SNOW_LINUX_HOTKEY_ALREADY_IN_USE:
        return GlobalShortcutFailureReason::AlreadyInUse;
    case SNOW_LINUX_HOTKEY_SERVER_ERROR:
        return GlobalShortcutFailureReason::SystemError;
    case SNOW_LINUX_HOTKEY_KEY_NOT_ON_KEYBOARD:
    case SNOW_LINUX_HOTKEY_INVALID_BINDING:
        return GlobalShortcutFailureReason::InvalidShortcut;
    default:
        return GlobalShortcutFailureReason::SystemError;
    }
}

// A callback from the Rust worker arrives on that worker's thread, so it cannot
// touch the manager directly. This object exists to be the context of a queued
// invocation: Qt drops pending metacalls whose context object has been destroyed,
// which is what keeps a callback that was already queued from running after the
// backend is gone.
class ActivationDispatcher final : public QObject {
  public:
    std::function<void(int)> deliver;
};

class LinuxGlobalShortcutBackend final : public GlobalShortcutBackend {
  public:
    LinuxGlobalShortcutBackend()
        : m_dispatcher(std::make_unique<ActivationDispatcher>()) {
        // X11 only. Under a Wayland session Qt uses the `wayland` platform even
        // when XWayland runs, and there XGrabKey would either fail or grab a key
        // only while an X11 window has focus -- a shortcut that stops working
        // exactly when it is needed.
        if (!qGuiApp || QGuiApplication::platformName() != QStringLiteral("xcb")) {
            return;
        }
        SnowLinuxHotkeys* hotkeys = snow_linux_hotkeys_create(&LinuxGlobalShortcutBackend::onActivated, this);
        if (hotkeys == nullptr) {
            return;
        }
        m_hotkeys.reset(hotkeys);
        m_dispatcher->deliver = [this](int registrationId) {
            if (m_handler) {
                m_handler(registrationId);
            }
        };
    }

    ~LinuxGlobalShortcutBackend() override {
        // Releases every grab and joins the worker before the dispatcher dies, so
        // no callback can still be in flight when the members below go away.
        m_hotkeys.reset();
    }

    void setActivationHandler(ActivationHandler handler) override {
        m_handler = std::move(handler);
    }

    GlobalShortcutValidationResult
    validateShortcut(const shortcuts::ShortcutBinding& binding) const override {
        GlobalShortcutValidationResult result;
        result.binding = binding;
        result.shortcut = binding.portableText;
        const auto parsed = parseShortcut(binding);
        const bool supported =
            m_hotkeys != nullptr && parsed.has_value() &&
            snow_linux_hotkeys_validate_binding(static_cast<int32_t>(parsed->key),
                                                parsed->modifiers) == SNOW_LINUX_HOTKEY_OK;
        result.supported = supported;
        result.failureReason =
            supported ? GlobalShortcutFailureReason::None
                      : (m_hotkeys == nullptr ? GlobalShortcutFailureReason::UnsupportedPlatform
                                              : GlobalShortcutFailureReason::InvalidShortcut);
        return result;
    }

    GlobalShortcutBackendResult
    registerShortcut(int registrationId, const shortcuts::ShortcutBinding& binding) override {
        if (m_hotkeys == nullptr) {
            return {false, GlobalShortcutFailureReason::UnsupportedPlatform, 0};
        }
        const auto parsed = parseShortcut(binding);
        if (!parsed.has_value()) {
            return {false, GlobalShortcutFailureReason::InvalidShortcut, 0};
        }
        const int32_t status =
            snow_linux_hotkeys_register(m_hotkeys.get(), registrationId,
                                        static_cast<int32_t>(parsed->key), parsed->modifiers);
        if (status == SNOW_LINUX_HOTKEY_OK) {
            m_bindings.insert(registrationId, *parsed);
            return {true, GlobalShortcutFailureReason::None, 0};
        }
        m_bindings.remove(registrationId);
        // The native error code stays zero: the status is the whole answer, and
        // inventing an X error code here would misreport "already in use" as a
        // protocol failure in the logs.
        return {false, failureReasonFor(status), 0};
    }

    void unregisterShortcut(int registrationId) override {
        m_bindings.remove(registrationId);
        if (m_hotkeys != nullptr) {
            snow_linux_hotkeys_unregister(m_hotkeys.get(), registrationId);
        }
    }

    std::optional<GlobalShortcutInputState> inputState(int id) const override {
        if (m_hotkeys == nullptr || (id != 0 && !m_bindings.contains(id))) {
            return std::nullopt;
        }
        const auto down = [this](int key) -> std::optional<bool> {
            const int32_t state = snow_linux_hotkeys_key_down(m_hotkeys.get(), key);
            if (state < 0) {
                return std::nullopt;
            }
            return state != 0;
        };

        const std::optional<bool> escape = down(Qt::Key_Escape);
        if (!escape.has_value()) {
            // Without a readable key state the whole answer is unavailable; a
            // picker that treats it as "nothing is down" would accept a
            // half-finished combination.
            return std::nullopt;
        }

        GlobalShortcutInputState state;
        state.escapeDown = *escape;
        if (id == 0) {
            return state;
        }

        const ParsedShortcut parsed = m_bindings.value(id);
        state.escapeIsShortcutKey = parsed.key == Qt::Key_Escape;
        bool anyDown = false;
        const auto check = [&](int key) {
            const std::optional<bool> value = down(key);
            if (value.has_value() && *value) {
                anyDown = true;
            }
        };
        check(static_cast<int>(parsed.key));
        if ((parsed.modifiers & kModifierControl) != 0) {
            check(Qt::Key_Control);
        }
        if ((parsed.modifiers & kModifierShift) != 0) {
            check(Qt::Key_Shift);
        }
        if ((parsed.modifiers & kModifierAlt) != 0) {
            check(Qt::Key_Alt);
        }
        if ((parsed.modifiers & kModifierMeta) != 0) {
            check(Qt::Key_Meta);
        }
        state.anyShortcutKeyDown = anyDown;
        return state;
    }

  private:
    static void onActivated(int32_t registrationId, void* userData) {
        static_cast<LinuxGlobalShortcutBackend*>(userData)->postActivation(registrationId);
    }

    void postActivation(int registrationId) {
        ActivationDispatcher* dispatcher = m_dispatcher.get();
        QMetaObject::invokeMethod(
            dispatcher,
            [dispatcher, registrationId] {
                if (dispatcher->deliver) {
                    dispatcher->deliver(registrationId);
                }
            },
            Qt::QueuedConnection);
    }

    struct HotkeyHandleDeleter {
        void operator()(SnowLinuxHotkeys* handle) const {
            snow_linux_hotkeys_destroy(handle);
        }
    };

    std::unique_ptr<ActivationDispatcher> m_dispatcher;
    std::unique_ptr<SnowLinuxHotkeys, HotkeyHandleDeleter> m_hotkeys;
    QHash<int, ParsedShortcut> m_bindings;
    ActivationHandler m_handler;
};

} // namespace

std::unique_ptr<GlobalShortcutBackend> createLinuxGlobalShortcutBackend() {
    return std::make_unique<LinuxGlobalShortcutBackend>();
}

} // namespace snow_shot::presentation
