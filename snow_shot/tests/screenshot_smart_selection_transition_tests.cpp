#include "snow_shot/presentation/screenshotsmartselectiontransition.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using Animation = snow_shot::SelectionTransitionAnimation;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool nearRect(const QRectF& actual, const QRectF& expected) {
    constexpr qreal tolerance = 0.000001;
    return std::abs(actual.x() - expected.x()) < tolerance &&
           std::abs(actual.y() - expected.y()) < tolerance &&
           std::abs(actual.width() - expected.width()) < tolerance &&
           std::abs(actual.height() - expected.height()) < tolerance;
}

QRectF interpolated(const QRectF& start, const QRectF& target, qreal progress) {
    const qreal eased = 1.0 - (1.0 - progress) * (1.0 - progress);
    return QRectF(start.x() + (target.x() - start.x()) * eased,
                  start.y() + (target.y() - start.y()) * eased,
                  start.width() + (target.width() - start.width()) * eased,
                  start.height() + (target.height() - start.height()) * eased);
}

void firstSmartSelectionIsPresentedDirectly() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);

    require(transition.animation() == Animation::Soft, "selection animation must default to Soft");
    require(transition.update(first, true, 1000), "first smart selection must change presentation");
    require(updateCount == 1, "first smart selection must update immediately");
    require(presented == first, "first smart selection must not be interpolated");
    require(!transition.isRunning(), "first smart selection must not animate");
    require(!transition.advance(2000), "direct selection must not produce later frames");
}

void framesUseAbsoluteTimeAndQuadraticEasing() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    static_cast<void>(transition.update(first, true, 1000));
    require(transition.update(second, true, 1100), "new smart target must start a transition");
    require(transition.isRunning(), "subsequent smart selection must animate");
    require(presented == first, "transition must begin at the displayed rectangle");
    require(!transition.advance(1100), "transition start must not repeat the previous frame");

    require(transition.advance(1108), "an eight-millisecond frame must advance presentation");
    require(nearRect(presented, interpolated(first, second, 8.0 / 101.0)),
            "Soft must preserve the previous 101 ms quadratic geometry calculation");
    require(transition.advance(1116), "high-refresh frames must not wait for a second timer");
    require(nearRect(presented, interpolated(first, second, 16.0 / 101.0)),
            "high-refresh frames must preserve the configured duration");
    require(presented.left() < first.left() + (second.left() - first.left()) * 0.5,
            "the first sixteen milliseconds must leave most of the motion for users to track");
    const int updatesAfterFrame = updateCount;
    require(!transition.advance(1116), "repeated frame timestamps must not emit an update");
    require(!transition.advance(1110), "older frame timestamps must not reverse the transition");
    require(updateCount == updatesAfterFrame, "repeated or older frames must leave callbacks idle");

    require(transition.advance(1148), "a delayed frame must use all elapsed time");
    require(nearRect(presented, interpolated(first, second, 48.0 / 101.0)),
            "a delayed frame must catch up without assuming a fixed tick interval");
    require(transition.isRunning(), "the transition must continue until its configured duration");
    require(transition.advance(1200),
            "the transition must continue through the first hundred milliseconds");
    require(nearRect(presented, interpolated(first, second, 100.0 / 101.0)) &&
                transition.isRunning(),
            "the previous size calculation must remain active until the full 101 ms duration");
    require(transition.advance(1201), "the final frame must reach the target exactly");
    require(!transition.isRunning(), "the transition must stop at its configured duration");
    require(presented == second, "smart selection transition must end at its exact target");
    require(!transition.advance(100000), "completed transition must not emit extra frames");
}

void frameCadenceDoesNotChangeTheTrajectory() {
    ScreenshotSmartSelectionTransition regular({});
    ScreenshotSmartSelectionTransition irregular({});
    ScreenshotSmartSelectionTransition delayed({});
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF target(110.0, 70.0, 500.0, 400.0);
    for (auto* transition : {&regular, &irregular, &delayed}) {
        static_cast<void>(transition->update(first, true, 1000));
        static_cast<void>(transition->update(target, true, 1100));
    }
    for (qint64 elapsedMs = 8; elapsedMs <= 64; elapsedMs += 8)
        static_cast<void>(regular.advance(1100 + elapsedMs));
    for (const qint64 elapsedMs : {5, 17, 41, 64})
        static_cast<void>(irregular.advance(1100 + elapsedMs));
    static_cast<void>(delayed.advance(1164));

    const QRectF expected = interpolated(first, target, 64.0 / 101.0);
    require(nearRect(regular.displayedSelection(), expected) &&
                nearRect(irregular.displayedSelection(), expected) &&
                nearRect(delayed.displayedSelection(), expected),
            "regular, irregular and delayed frames must reach the same geometry at the same time");
}

void retargetingSamplesTheCurrentTrajectory() {
    QRectF presented;
    ScreenshotSmartSelectionTransition transition(
        [&](const QRectF& selection) { presented = selection; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);
    const QRectF third(30.0, 200.0, 100.0, 80.0);

    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(second, true, 1100));
    static_cast<void>(transition.update(third, true, 1150));
    const QRectF retargetStart = interpolated(first, second, 50.0 / 101.0);
    require(nearRect(presented, retargetStart),
            "retargeting must sample the preceding trajectory at the supplied timestamp");

    static_cast<void>(transition.advance(1200));
    require(nearRect(presented, interpolated(retargetStart, third, 50.0 / 101.0)),
            "retargeted animation must use the current rectangle as its new start");
    static_cast<void>(transition.advance(100000));
    require(presented == third, "a delayed frame must finish at the latest target");
    require(!transition.isRunning(), "a delayed final frame must stop the transition");
}

void retargetingDoesNotRewindTheClock() {
    ScreenshotSmartSelectionTransition transition({});
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);
    const QRectF third(30.0, 200.0, 100.0, 80.0);
    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(second, true, 1100));
    static_cast<void>(transition.advance(1116));
    const QRectF current = transition.displayedSelection();
    static_cast<void>(transition.update(third, true, 1108));
    require(transition.displayedSelection() == current,
            "retargeting with an old timestamp must not reverse the displayed trajectory");
    static_cast<void>(transition.advance(1124));
    require(nearRect(transition.displayedSelection(), interpolated(current, third, 8.0 / 101.0)),
            "retargeting with an old timestamp must not count already consumed time again");
}

void subpixelTargetChangesSnapImmediately() {
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF&) { ++updateCount; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF target = first.translated(0.25, -0.25);
    static_cast<void>(transition.update(first, true, 1000));
    require(transition.update(target, true, 1100), "a nearby target must change presentation");
    require(transition.displayedSelection() == target && !transition.isRunning(),
            "a target within 0.4 pixels on every edge must snap without scheduling animation");
    require(!transition.advance(1116) && updateCount == 2,
            "snapped target changes must notify exactly once and leave no animation frames");
}

void immediateSnappingChecksEveryEdge() {
    const QRectF first(0.0, 0.0, 100.0, 100.0);
    for (const QRectF& target : {QRectF(0.25, 0.0, 100.25, 100.0), QRectF(0.0, 0.25, 100.0, 100.25),
                                 QRectF(0.5, 0.0, 99.75, 100.0), QRectF(0.0, 0.5, 100.0, 99.75)}) {
        ScreenshotSmartSelectionTransition transition({});
        static_cast<void>(transition.update(first, true, 1000));
        static_cast<void>(transition.update(target, true, 1100));
        require(transition.isRunning(), "any edge outside 0.4 pixels must start a transition");
        static_cast<void>(transition.advance(1101));
        require(transition.isRunning() && nearRect(transition.displayedSelection(),
                                                   interpolated(first, target, 1.0 / 101.0)),
                "snapping must check all four edges rather than position and size separately");
        static_cast<void>(transition.advance(1201));
        require(!transition.isRunning() && transition.displayedSelection() == target,
                "small animated changes must still finish at the exact target");
    }
}

void disabledTransitionPresentsSmartSelectionsDirectly() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    transition.setAnimation(Animation::Disabled, 1000);
    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(second, true, 1008));

    require(transition.animation() == Animation::Disabled,
            "disabled smart selection transition must remain disabled");
    require(!transition.isRunning(), "disabled smart selection transition must not animate");
    require(presented == second, "disabled transition must present the latest target directly");
    require(!transition.update(second, true, 1016),
            "unchanged disabled selection must not report a presentation change");
    require(updateCount == 2, "disabled transition must emit each changed selection once");
}

void disablingRunningTransitionPresentsItsTargetDirectly() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(second, true, 1100));
    static_cast<void>(transition.advance(1116));
    transition.setAnimation(Animation::Disabled, 1116);
    const int updateCountAfterDisable = updateCount;

    require(transition.animation() == Animation::Disabled,
            "running smart selection transition must become disabled");
    require(!transition.isRunning(), "disabling a running transition must stop its animation");
    require(presented == second,
            "disabling a running transition must immediately reach its target");
    require(!transition.advance(1201), "disabled transition must not produce stale frames");
    require(updateCount == updateCountAfterDisable,
            "disabled transition must not emit stale animation callbacks");
}

void leavingSmartFramingAndClearingResetTheFirstResultRule() {
    QRectF presented;
    ScreenshotSmartSelectionTransition transition(
        [&](const QRectF& selection) { presented = selection; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF target(110.0, 70.0, 500.0, 400.0);
    const QRectF manual(30.0, 40.0, 100.0, 80.0);
    const QRectF nextFirst(400.0, 300.0, 250.0, 180.0);

    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(target, true, 1100));
    static_cast<void>(transition.update(manual, false, 1116));
    require(presented == manual, "leaving smart framing must present manual geometry immediately");
    require(!transition.advance(1201), "leaving smart framing must cancel all animation frames");
    static_cast<void>(transition.update(nextFirst, true, 1300));
    require(!transition.isRunning(), "first result after re-entry must not animate");
    require(presented == nextFirst, "first result after re-entry must be direct");

    static_cast<void>(transition.update(target, true, 1400));
    static_cast<void>(transition.update({}, true, 1416));
    require(presented.isEmpty(), "clearing must present an empty selection immediately");
    require(!transition.advance(1501), "clearing must cancel all animation frames");
    static_cast<void>(transition.update(first, true, 1600));
    require(!transition.isRunning(), "first result after clearing must not animate");
    require(presented == first, "first result after clearing must be direct");
}

void unchangedTargetsAndDirectGeometryDoNotEmitUpdates() {
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF&) { ++updateCount; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    require(transition.update(first, true, 1000), "first smart selection must report a change");
    require(!transition.update(first, true, 1050), "unchanged smart target must not restart");
    require(updateCount == 1, "unchanged smart target must not emit an update");
    static_cast<void>(transition.update(second, true, 1100));
    require(!transition.update(second, true, 1150), "unchanged running target must not restart");
    require(updateCount == 1,
            "target delivery must not advance frames independently of presentation");
    require(transition.advance(1150), "the presentation frame must still advance a running target");
    require(!transition.update(transition.displayedSelection(), false, 1150),
            "unchanged manual geometry must not emit a redundant update");
    require(!transition.isRunning(), "manual geometry must stop a running transition");
    require(!transition.update(transition.displayedSelection(), false, 1200),
            "repeated manual geometry must remain unchanged");
    require(updateCount == 2, "direct geometry repeats must leave presentation callbacks idle");
}

void fastAndSoftUseFixedDurationsRegardlessOfDistance() {
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    for (const auto animation : {Animation::Fast, Animation::Soft}) {
        const int durationMs = animation == Animation::Fast ? 67 : 101;
        for (const QRectF& target :
             {first.translated(1.0, -1.0), QRectF(-10000.0, -5000.0, 20000.0, 12000.0)}) {
            ScreenshotSmartSelectionTransition transition({});
            transition.setAnimation(animation, 1000);
            static_cast<void>(transition.update(first, true, 1000));
            static_cast<void>(transition.update(target, true, 1100));
            static_cast<void>(transition.advance(1116));
            require(nearRect(transition.displayedSelection(),
                             interpolated(first, target, 16.0 / durationMs)),
                    "Fast and Soft must use quadratic geometry with 67 and 101 ms durations");
            static_cast<void>(transition.advance(1100 + durationMs - 1));
            require(transition.isRunning() &&
                        nearRect(transition.displayedSelection(),
                                 interpolated(first, target, (durationMs - 1.0) / durationMs)),
                    "selection distance must not change the animation duration");
            require(transition.advance(1100 + durationMs) && !transition.isRunning() &&
                        transition.displayedSelection() == target,
                    "every selection size must reach the exact target at the selected duration");
            require(!transition.advance(100000), "completed animations must leave callbacks idle");
        }
    }
}

void changingAnimationPreservesProgressBeforeSwitchingDuration() {
    ScreenshotSmartSelectionTransition transition({});
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF target(110.0, 70.0, 500.0, 400.0);
    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(target, true, 1100));
    transition.setAnimation(Animation::Fast, 1124);
    const QRectF current = interpolated(first, target, 24.0 / 101.0);
    require(nearRect(transition.displayedSelection(), current),
            "changing speed must consume elapsed time at the previous duration");
    static_cast<void>(transition.advance(1132));
    require(nearRect(transition.displayedSelection(),
                     interpolated(first, target, 24.0 / 101.0 + 8.0 / 67.0)),
            "speed changes must preserve quadratic progress and apply the new duration afterward");
    transition.setAnimation(Animation::Fast, 1140);
    static_cast<void>(transition.advance(1148));
    require(nearRect(transition.displayedSelection(),
                     interpolated(first, target, 24.0 / 101.0 + 24.0 / 67.0)),
            "reapplying the same speed must not reset elapsed time");

    transition.setAnimation(Animation::Disabled, 1148);
    transition.setAnimation(Animation::Soft, 1200);
    static_cast<void>(transition.update(first, true, 1200));
    static_cast<void>(transition.advance(1216));
    require(nearRect(transition.displayedSelection(), interpolated(target, first, 16.0 / 101.0)),
            "reenabling animation must start from the last directly presented target");
    transition.setAnimation(Animation::Fast, 1216);
    transition.setAnimation(static_cast<Animation>(-1), 1216);
    require(transition.animation() == Animation::Soft,
            "invalid animation modes must fall back to Soft");
}

void speedChangesKeepTheClockMonotonicAndRetargetingResetsProgress() {
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF target(110.0, 70.0, 500.0, 400.0);
    const QRectF nextTarget(-100.0, -200.0, 80.0, 60.0);
    for (const auto animation : {Animation::Fast, Animation::Soft}) {
        ScreenshotSmartSelectionTransition transition({});
        transition.setAnimation(animation, 1000);
        static_cast<void>(transition.update(first, true, 1000));
        static_cast<void>(transition.update(target, true, 1100));
        static_cast<void>(transition.advance(1116));
        const QRectF beforeSwitch = transition.displayedSelection();
        const auto nextAnimation = animation == Animation::Fast ? Animation::Soft : Animation::Fast;
        const int durationMs = animation == Animation::Fast ? 67 : 101;
        const int nextDurationMs = nextAnimation == Animation::Fast ? 67 : 101;
        transition.setAnimation(nextAnimation, 1108);
        require(transition.displayedSelection() == beforeSwitch,
                "an older speed-change timestamp must not rewind the curve");
        static_cast<void>(transition.advance(1124));
        const QRectF current =
            interpolated(first, target, 16.0 / durationMs + 8.0 / nextDurationMs);
        require(nearRect(transition.displayedSelection(), current),
                "both speed-change directions must preserve progress without recounting time");
        static_cast<void>(transition.update(nextTarget, true, 1124));
        static_cast<void>(transition.advance(1132));
        require(nearRect(transition.displayedSelection(),
                         interpolated(current, nextTarget, 8.0 / nextDurationMs)),
                "retargeting after a speed change must start a new curve with zero progress");
        static_cast<void>(transition.advance(1124 + nextDurationMs));
        require(transition.displayedSelection() == nextTarget && !transition.isRunning(),
                "retargeting must finish at the selected duration after a speed change");
    }
}
} // namespace

int main() {
    firstSmartSelectionIsPresentedDirectly();
    framesUseAbsoluteTimeAndQuadraticEasing();
    frameCadenceDoesNotChangeTheTrajectory();
    retargetingSamplesTheCurrentTrajectory();
    retargetingDoesNotRewindTheClock();
    subpixelTargetChangesSnapImmediately();
    immediateSnappingChecksEveryEdge();
    disabledTransitionPresentsSmartSelectionsDirectly();
    disablingRunningTransitionPresentsItsTargetDirectly();
    leavingSmartFramingAndClearingResetTheFirstResultRule();
    unchangedTargetsAndDirectGeometryDoNotEmitUpdates();
    fastAndSoftUseFixedDurationsRegardlessOfDistance();
    changingAnimationPreservesProgressBeforeSwitchingDuration();
    speedChangesKeepTheClockMonotonicAndRetargetingResetsProgress();
    return 0;
}
