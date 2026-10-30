#pragma once

#include "snow_shot/presentation/screenshottoolpalette.h"

#include <QObject>
#include <QPointer>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

namespace palette_style_edit_test_support {
class Observers final : public QObject {
  public:
    explicit Observers(ScreenshotToolPalette& palette) : QObject(&palette) {}
    std::vector<std::function<void(const SnowCanvasStyleEdit&)>> callbacks;
};

// Observe commands at the application host's canonical style-edit boundary.
// Standalone palettes must still remember accepted creation preferences.
template <typename Edit, typename Callback>
void observe(ScreenshotToolPalette& palette, Callback callback) {
    Observers* observers = nullptr;
    for (QObject* child : palette.children()) {
        if ((observers = dynamic_cast<Observers*>(child)) != nullptr)
            break;
    }
    if (observers == nullptr)
        observers = new Observers(palette);
    observers->callbacks.emplace_back(
        [callback = std::move(callback)](const SnowCanvasStyleEdit& command) mutable {
            const auto* edit = std::get_if<Edit>(&command);
            if (edit == nullptr)
                return;
            if constexpr (std::is_same_v<Edit, SnowCanvasShapeEdit> &&
                          std::is_invocable_v<Callback, const SnowCanvasShapeStyle&, quint32,
                                              SnowCanvasShapeKind>) {
                callback(edit->style, edit->properties, edit->kind);
            } else if constexpr (std::is_invocable_v<Callback, decltype(edit->style), quint32>) {
                callback(edit->style, edit->properties);
            } else if constexpr (std::is_invocable_v<Callback, decltype(edit->style)>) {
                callback(edit->style);
            } else {
                callback();
            }
        });
    palette.setStyleEditHandler(
        [&palette, observers = QPointer<Observers>(observers)](const SnowCanvasStyleEdit& edit) {
            palette.rememberStyleEdit(edit);
            if (observers) {
                const auto callbacks = observers->callbacks;
                for (const auto& callback : callbacks)
                    callback(edit);
            }
            return true;
        });
}
} // namespace palette_style_edit_test_support

template <typename Edit, typename Callback>
void observePaletteStyleEdit(ScreenshotToolPalette& palette, Callback callback) {
    palette_style_edit_test_support::observe<Edit>(palette, std::move(callback));
}
