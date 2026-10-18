#ifndef SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTTYPES_H
#define SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTTYPES_H

#include "snow_shot/shortcuts/shortcutbinding.h"

#include <QHash>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace snow_shot::presentation {
enum class GlobalShortcutAction {
    Screenshot,
    ScreenshotDelay,
    ScreenshotFixed,
    ScreenshotOcr,
    ScreenshotTranslation,
    ScreenshotCopy,
    ScreenshotFullScreen,
    ScreenshotFocusedWindow,
    ScreenRecord,
    ScreenRecordCopy,
    OpenScreenRecordingFolder,
    OpenCaptureHistory,
    OpenSettings,
    PinClipboardContent,
    TranslateSelectedText,
    PinSelectedFiles,
    RestoreLastClosedWindows,
    ToggleGlobalHotkeys,
    ToggleDisableOnFocusedFullscreenWindow,
    OpenPinToScreenManagement,
    GlobalCanvas,
    SwitchWindowGroup,
    ScreenshotSave,
    ScreenshotQuickSave,
    HideShowAllWindows,
};

// Qt6 provides a qHash overload for *every* enum in qhashfunctions.h; Qt5 does
// not. settingscatalog stores a QSet<GlobalShortcutAction>, and the missing
// overload fails inside the Qt headers themselves
// (error C2665: no overload of "qHash" can convert all argument types).
// Same namespace plus same parameter type means ADL finds this one, while the
// exact match still wins under Qt6, so the two do not conflict.
// Keep this comment ASCII: some TUs that include this header are not compiled
// with /utf-8.
inline uint qHash(GlobalShortcutAction action, uint seed = 0) noexcept
{
    return ::qHash(static_cast<int>(action), seed);
}

[[nodiscard]] constexpr bool controlsGlobalHotkeyGates(GlobalShortcutAction action) {
    return action == GlobalShortcutAction::ToggleGlobalHotkeys ||
           action == GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow;
}

enum class GlobalShortcutStatus {
    Unset,
    Registered,
    PartiallyRegistered,
    Failed,
    Disabled,
};

enum class GlobalShortcutFailureReason {
    None,
    InvalidShortcut,
    AlreadyInUse,
    UnsupportedPlatform,
    SystemError,
};

struct GlobalShortcutBindingResult {
    QString shortcut;
    bool registered = false;
    GlobalShortcutFailureReason failureReason = GlobalShortcutFailureReason::None;
    qint64 nativeErrorCode = 0;
    snow_shot::shortcuts::ShortcutBinding binding;
};

struct GlobalShortcutRegistrationState {
    GlobalShortcutAction action = GlobalShortcutAction::Screenshot;
    snow_shot::shortcuts::ShortcutBindingList shortcuts;
    GlobalShortcutStatus status = GlobalShortcutStatus::Unset;
    QVector<GlobalShortcutBindingResult> bindings;
};

// A missing value means the native input state cannot be observed safely.
struct GlobalShortcutInputState {
    bool anyShortcutKeyDown = false;
    bool escapeDown = false;
    bool escapeIsShortcutKey = false;
};

struct GlobalShortcutBackendResult {
    bool registered = false;
    GlobalShortcutFailureReason failureReason = GlobalShortcutFailureReason::None;
    qint64 nativeErrorCode = 0;
};

struct GlobalShortcutValidationResult {
    QString shortcut;
    bool supported = false;
    GlobalShortcutFailureReason failureReason = GlobalShortcutFailureReason::InvalidShortcut;
    snow_shot::shortcuts::ShortcutBinding binding;
};
} // namespace snow_shot::presentation

Q_DECLARE_METATYPE(snow_shot::presentation::GlobalShortcutAction)
Q_DECLARE_METATYPE(snow_shot::presentation::GlobalShortcutRegistrationState)

#endif // SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTTYPES_H
