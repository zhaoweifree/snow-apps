#include "presentation/capture/windowcaptureexclusion.h"
#include "presentation/capture/windowinputtransparency.h"

#include <QApplication>
#ifdef Q_OS_WIN
#include "platform/windows/captureexclusioncomposition.h"
#endif

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

using snow_shot::presentation::WindowCaptureExclusion;
using snow_shot::presentation::WindowInputTransparency;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void scrollingExclusionAttemptsAreIndependent(bool overlaySucceeds, bool toolbarSucceeds) {
    QWidget overlay;
    QWidget toolbar;
    overlay.show();
    toolbar.show();
    std::vector<std::pair<QWidget*, bool>> calls;
    WindowCaptureExclusion exclusion([&](QWidget* window, bool excluded) {
        calls.emplace_back(window, excluded);
        return window == &overlay ? overlaySucceeds : toolbarSucceeds;
    });

    exclusion.exclude(&overlay);
    exclusion.exclude(&toolbar);
    require(calls == std::vector<std::pair<QWidget*, bool>>{{&overlay, true}, {&toolbar, true}},
            "scrolling must attempt both exclusions without rolling back a partial success");
    require(overlay.isVisible() && toolbar.isVisible(),
            "scrolling windows must stay visible regardless of exclusion results");

    calls.clear();
    exclusion.restore();
    std::vector<std::pair<QWidget*, bool>> expected;
    if (toolbarSucceeds) {
        expected.emplace_back(&toolbar, false);
    }
    if (overlaySucceeds) {
        expected.emplace_back(&overlay, false);
    }
    require(calls == expected, "cleanup must restore each successful exclusion");
    exclusion.restore();
    require(calls == expected, "repeated cleanup must not restore exclusions again");
}

void recordingToolbarRemainsVisible(bool succeeds) {
    QWidget toolbar;
    toolbar.show();
    int attempts = 0;
    int restores = 0;
    WindowCaptureExclusion exclusion([&](QWidget* window, bool excluded) {
        require(window == &toolbar, "recording must target its toolbar");
        excluded ? ++attempts : ++restores;
        return succeeds;
    });

    exclusion.exclude(&toolbar);
    require(attempts == 1 && restores == 0, "recording must attempt exclusion once");
    require(toolbar.isVisible(), "failed exclusion must not hide the recording toolbar");
    exclusion.restore();
    require(restores == (succeeds ? 1 : 0), "recording must clean up successful exclusion only");
    require(toolbar.isVisible(), "cleanup must preserve recording toolbar visibility");
}

void cleanupToleratesDestroyedWindowsAndRestoreFailures() {
    auto destroyedWindow = std::make_unique<QWidget>();
    QWidget toolbar;
    QWidget overlay;
    std::vector<QWidget*> restored;
    WindowCaptureExclusion exclusion([&](QWidget* window, bool excluded) {
        if (!excluded) {
            restored.push_back(window);
        }
        return excluded;
    });
    exclusion.exclude(destroyedWindow.get());
    exclusion.exclude(&overlay);
    exclusion.exclude(&toolbar);
    exclusion.exclude(nullptr);
    destroyedWindow.reset();
    exclusion.restore();
    require(restored == std::vector<QWidget*>{&toolbar, &overlay},
            "cleanup must skip destroyed windows and continue after restore failure");
    exclusion.restore();
    require(restored.size() == 2, "cleanup must clear tracking even after restore failure");
}

void unavailableExclusionPreservesVisibility() {
    QWidget toolbar;
    toolbar.show();
    WindowCaptureExclusion exclusion;
    exclusion.exclude(&toolbar);
    exclusion.restore();
    require(toolbar.isVisible(), "unavailable exclusion must not hide the toolbar");
}

void successfulIdsAndScopeCleanup() {
    QWidget first, second, failed;
    int attempts = 0;
    std::vector<QWidget*> restored;
    {
        WindowCaptureExclusion exclusion([&](QWidget* window, bool exclude) {
            if (exclude)
                ++attempts;
            else
                restored.push_back(window);
            return window != &failed;
        });
        require(exclusion.exclude(&first), "first exclusion succeeds");
        require(exclusion.exclude(&first), "repeated exclusion succeeds without another lease");
        require(exclusion.exclude(&second), "second exclusion succeeds");
        require(!exclusion.exclude(&failed), "failed exclusion reports failure");
        require(attempts == 3, "duplicate exclusion is idempotent");
        const auto resolve = [&](QWidget* window) -> std::optional<std::uint32_t> {
            return window == &first ? 17 : 23;
        };
        auto ids = exclusion.windowIds(resolve);
        require(ids == QVector<std::uint32_t>{17, 23},
                "only successful exclusions reach capture configs");
        require(exclusion.windowIds(resolve) == ids,
                "mode changes and pause snapshots retain exclusions");
    }
    require(restored == std::vector<QWidget*>{&second, &first},
            "scope exit restores in reverse order");
}

void overlappingCapturesKeepExclusionUntilTheFinalOwner() {
    QWidget overlay, toolbar;
    std::vector<std::pair<QWidget*, bool>> firstCalls, secondCalls;
    WindowCaptureExclusion first([&](QWidget* window, bool excluded) {
        firstCalls.emplace_back(window, excluded);
        return true;
    });
    WindowCaptureExclusion second([&](QWidget* window, bool excluded) {
        secondCalls.emplace_back(window, excluded);
        return true;
    });
    require(first.exclude(&overlay) && first.exclude(&toolbar) && second.exclude(&overlay),
            "overlapping captures must acquire the shared overlay independently");
    firstCalls.clear();
    secondCalls.clear();
    first.restore();
    require(firstCalls == std::vector<std::pair<QWidget*, bool>>{{&toolbar, false}} &&
                secondCalls.empty(),
            "retiring a capture restores only windows with no remaining capture owner");
    require(second.windowIds([](QWidget*) { return std::optional<std::uint32_t>(17); }) ==
                QVector<std::uint32_t>{17},
            "a replacement capture retains its successful native exclusion");
    second.restore();
    require(secondCalls == std::vector<std::pair<QWidget*, bool>>{{&overlay, false}},
            "the final owner restores through its own setter");
}

void childrenShareTheirTopLevelWindowLease() {
    QWidget overlay;
    QWidget child(&overlay);
    int exclusions = 0, restores = 0;
    const auto setter = [&](QWidget* window, bool excluded) {
        require(window == &overlay, "native exclusion leases belong to top-level windows");
        excluded ? ++exclusions : ++restores;
        return true;
    };
    WindowCaptureExclusion first(setter), second(setter);
    require(first.exclude(&child) && first.exclude(&overlay) && second.exclude(&overlay),
            "child and top-level requests must share one native window");
    require(exclusions == 2, "each owner validates exclusion once for the native window");
    first.restore();
    require(restores == 0, "a child owner cannot restore another owner's top-level window");
    second.restore();
    require(restores == 1, "shared top-level exclusion restores exactly once");
}

void newOwnerRevalidatesTheReplacementNativeSurface() {
    class NativeWindow final : public QWidget {
      public:
        void replaceSurface() {
            destroy();
            static_cast<void>(winId());
        }
    };
    NativeWindow overlay;
    static_cast<void>(overlay.winId());
    int exclusions = 0, restores = 0;
    const auto setter = [&](QWidget*, bool excluded) {
        excluded ? ++exclusions : ++restores;
        return true;
    };
    WindowCaptureExclusion retired(setter), restarted(setter);
    require(retired.exclude(&overlay), "first native surface exclusion must succeed");
    overlay.replaceSurface();
    require(restarted.exclude(&overlay) && exclusions == 2,
            "a restart must reapply exclusion after native surface replacement");
    retired.restore();
    require(restores == 0, "old surface cleanup must not restore the restarted surface");
    restarted.restore();
    require(restores == 1, "the replacement surface restores only after the final owner");
}

void failedOverlappingOwnerDoesNotAcquireALease() {
    QWidget overlay;
    int restores = 0;
    WindowCaptureExclusion first([&](QWidget*, bool excluded) {
        if (!excluded)
            ++restores;
        return true;
    });
    WindowCaptureExclusion failed([](QWidget*, bool) { return false; });
    require(first.exclude(&overlay) && !failed.exclude(&overlay),
            "each capture reports its own native exclusion failure");
    failed.restore();
    require(restores == 0, "failed owners must never restore another capture's exclusion");
    first.restore();
    require(restores == 1, "failed owners must not delay successful exclusion restoration");
}

void recaptureCanRollbackPartialExclusionsBeforeFallback() {
    QWidget overlay;
    QWidget toolbar;
    std::vector<std::pair<QWidget*, bool>> calls;
    WindowCaptureExclusion exclusion([&](QWidget* window, bool excluded) {
        calls.emplace_back(window, excluded);
        return window == &overlay;
    });

    const bool overlayExcluded = exclusion.exclude(&overlay);
    const bool toolbarExcluded = exclusion.exclude(&toolbar);
    require(overlayExcluded && !toolbarExcluded,
            "recapture must be able to detect a partial exclusion failure");
    exclusion.restore();
    require(calls == std::vector<std::pair<QWidget*, bool>>{{&overlay, true},
                                                            {&toolbar, true},
                                                            {&overlay, false}},
            "partial recapture exclusion must restore successes before hidden fallback");
}
void inputTransparencyRestoresOriginalStateAndRollsBackPartialFailure() {
    QWidget overlay;
    QWidget toolbar;
    QWidget unsupported;
    overlay.show();
    toolbar.show();
    bool overlayTransparent = false;
    bool toolbarTransparent = true;
    std::vector<QWidget*> restored;
    {
        WindowInputTransparency guard(
            [&](QWidget* window, bool transparent) -> std::optional<bool> {
                if (window == &unsupported) {
                    return std::nullopt;
                }
                bool& state = window == &overlay ? overlayTransparent : toolbarTransparent;
                const bool previous = state;
                state = transparent;
                restored.push_back(window);
                return previous;
            });
        require(!guard.enable(nullptr), "null windows must reject input transparency");
        require(guard.enable(&overlay) && guard.enable(&toolbar),
                "recapture must enable input transparency for overlays and toolbar");
        require(guard.enable(&overlay) && restored.size() == 2,
                "duplicate enable must not overwrite the original input state");
        require(overlayTransparent && toolbarTransparent && overlay.isVisible() &&
                    toolbar.isVisible(),
                "input transparency must preserve visibility while allowing input through");
        require(!guard.enable(&unsupported), "unsupported windows must trigger capture fallback");
        restored.clear();
        guard.restore();
        require(!overlayTransparent && toolbarTransparent &&
                    restored == std::vector<QWidget*>{&toolbar, &overlay},
                "fallback must restore original states in reverse order");
        guard.restore();
        require(restored.size() == 2, "restoration must be idempotent");
        require(guard.enable(&overlay), "guard must support another capture after cleanup");
    }
    require(!overlayTransparent, "scope exit must restore input after capture or cancellation");
}

void inputTransparencySkipsDestroyedWindowsAndContinuesAfterRestoreFailure() {
    auto destroyed = std::make_unique<QWidget>();
    QWidget overlay;
    QWidget toolbar;
    std::vector<QWidget*> restored;
    WindowInputTransparency guard([&](QWidget* window, bool transparent) -> std::optional<bool> {
        if (!transparent) {
            restored.push_back(window);
            return std::nullopt;
        }
        return false;
    });
    require(guard.enable(&overlay) && guard.enable(destroyed.get()) && guard.enable(&toolbar),
            "input guard fixtures must enable transparency");
    destroyed.reset();
    guard.restore();
    require(restored == std::vector<QWidget*>{&toolbar, &overlay},
            "input cleanup must skip deleted windows and continue after native restore failures");
}

void inputTransparencyDoesNotRestoreAnObsoleteNativeWindow() {
    class NativeWindow final : public QWidget {
      public:
        void destroyNativeWindow() {
            destroy();
        }
    };
    NativeWindow window;
    static_cast<void>(window.winId());
    int calls = 0;
    WindowInputTransparency guard([&](QWidget*, bool) -> std::optional<bool> {
        ++calls;
        return false;
    });
    require(guard.enable(&window), "native input guard fixture must enable transparency");
    window.destroyNativeWindow();
    guard.restore();
    require(calls == 1, "cleanup must not recreate or modify an obsolete native window");
}

#ifdef Q_OS_WIN
void compositionFallbackOnlyRetriesTheAffectedFailure() {
    using namespace snow_shot::platform::windows::detail;
    for (const int build :
         {18363, 19041, 19044, 19045, 22000, 22621, 22631, 26099, 26100, 26200, 26300}) {
        for (const bool excluded : {false, true}) {
            for (const DWORD error :
                 {ERROR_SUCCESS, ERROR_NOT_ENOUGH_MEMORY, ERROR_ACCESS_DENIED}) {
                for (const bool fallbackAvailable : {false, true}) {
                    for (const bool retrySucceeds : {false, true}) {
                        int attempts = 0;
                        int fallbacks = 0;
                        bool compositionApplied = false;
                        const bool actual = setCaptureExclusionWithCompositionFallback(
                            excluded, 10, build,
                            [&]() -> CaptureExclusionResult {
                                ++attempts;
                                if (attempts == 1)
                                    return {error == ERROR_SUCCESS, error};
                                require(compositionApplied,
                                        "affinity retry must follow the rendering upgrade");
                                return {retrySucceeds, ERROR_NOT_ENOUGH_MEMORY};
                            },
                            [&]() {
                                ++fallbacks;
                                compositionApplied = fallbackAvailable;
                                return fallbackAvailable;
                            });
                        const bool affected = build >= 19041 && build < 26100 && excluded &&
                                              error == ERROR_NOT_ENOUGH_MEMORY;
                        const bool retried = affected && fallbackAvailable;
                        require(fallbacks == (affected ? 1 : 0),
                                "only error 8 in the compatibility range may request fallback");
                        require(attempts == (retried ? 2 : 1),
                                "affinity must retry once only after an available fallback");
                        require(actual == (error == ERROR_SUCCESS || (retried && retrySucceeds)),
                                "result must reflect native success or the single retry result");
                    }
                }
            }
        }
    }
    require(!supportsCaptureExclusionCompositionFallback(6, 19041) &&
                !supportsCaptureExclusionCompositionFallback(11, 19041),
            "unrelated OS major versions must not enter the compatibility path");

    QWidget offscreenWindow(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    offscreenWindow.setAttribute(Qt::WA_TranslucentBackground);
    enableCaptureExclusionCompositionFallback(&offscreenWindow);
    require(!applyCaptureExclusionCompositionFallback(&offscreenWindow),
            "offscreen Qt platforms cannot apply native composition");
    require(offscreenWindow.internalWinId() == 0 && !offscreenWindow.isVisible() &&
                offscreenWindow.children().isEmpty(),
            "fallback registration must not create native or GPU surfaces");
}
#endif
} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    compositionFallbackOnlyRetriesTheAffectedFailure();
#endif
    for (bool overlaySucceeds : {false, true}) {
        for (bool toolbarSucceeds : {false, true}) {
            scrollingExclusionAttemptsAreIndependent(overlaySucceeds, toolbarSucceeds);
        }
        recordingToolbarRemainsVisible(overlaySucceeds);
    }
    cleanupToleratesDestroyedWindowsAndRestoreFailures();
    unavailableExclusionPreservesVisibility();
    successfulIdsAndScopeCleanup();
    overlappingCapturesKeepExclusionUntilTheFinalOwner();
    childrenShareTheirTopLevelWindowLease();
    newOwnerRevalidatesTheReplacementNativeSurface();
    failedOverlappingOwnerDoesNotAcquireALease();
    recaptureCanRollbackPartialExclusionsBeforeFallback();
    inputTransparencyRestoresOriginalStateAndRollsBackPartialFailure();
    inputTransparencySkipsDestroyedWindowsAndContinuesAfterRestoreFailure();
    inputTransparencyDoesNotRestoreAnObsoleteNativeWindow();
    return 0;
}
