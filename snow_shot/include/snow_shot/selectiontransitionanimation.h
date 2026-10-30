#ifndef SNOW_SHOT_SELECTIONTRANSITIONANIMATION_H
#define SNOW_SHOT_SELECTIONTRANSITIONANIMATION_H

namespace snow_shot {

enum class SelectionTransitionAnimation {
    Disabled = 0,
    Soft = 32,
    Fast = 72,
};

[[nodiscard]] constexpr SelectionTransitionAnimation
normalizedSelectionTransitionAnimation(SelectionTransitionAnimation animation) {
    switch (animation) {
    case SelectionTransitionAnimation::Disabled:
    case SelectionTransitionAnimation::Soft:
    case SelectionTransitionAnimation::Fast:
        return animation;
    }
    return SelectionTransitionAnimation::Soft;
}

} // namespace snow_shot

#endif // SNOW_SHOT_SELECTIONTRANSITIONANIMATION_H
