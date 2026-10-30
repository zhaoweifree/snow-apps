#ifndef SNOW_SHOT_PRESENTATION_CURSORMOVEMENTSHORTCUTS_H
#define SNOW_SHOT_PRESENTATION_CURSORMOVEMENTSHORTCUTS_H

#include "snow_shot/presentation/windowshortcutmanager.h"

#include <QPoint>
#include <QSet>

#include <memory>
#include <utility>

namespace snow_shot::presentation {

// One shared held-direction state per shortcut surface. The shortcut manager
// balances aliases, physical releases and input interruption for each binding.
class CursorMovementShortcuts final {
  public:
    void configureBinding(WindowShortcutManager::Binding& binding, const QPoint& direction,
                          std::function<bool(const QPoint&)> move) const {
        binding.autoRepeat = true;
        binding.activate = [held = m_held, direction, move = std::move(move)](const auto&) {
            const bool alreadyHeld = held->contains(direction);
            held->insert(direction);
            QPoint combined;
            for (const auto& offset : *held)
                combined += offset;
            // Opposing keys still own their input sequence while cancelling movement.
            const bool handled = combined.isNull() || (move && move(combined));
            if (!handled && !alreadyHeld)
                held->remove(direction);
            return handled;
        };
        binding.release = [held = m_held, direction](const auto&) {
            held->remove(direction);
            return true;
        };
        binding.cancel = [held = m_held, direction] { held->remove(direction); };
    }

  private:
    const std::shared_ptr<QSet<QPoint>> m_held = std::make_shared<QSet<QPoint>>();
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_CURSORMOVEMENTSHORTCUTS_H
