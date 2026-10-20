#include "snow_shot/presentation/globalmousemanager.h"

namespace snow_shot::presentation {

#ifdef Q_OS_WIN
std::unique_ptr<GlobalMouseBackend> createWindowsGlobalMouseBackend();
#endif
#ifdef Q_OS_MACOS
std::unique_ptr<GlobalMouseBackend> createMacOSGlobalMouseBackend();
#endif
#ifdef Q_OS_LINUX
std::unique_ptr<GlobalMouseBackend> createLinuxGlobalMouseBackend();
#endif

namespace {
class UnsupportedGlobalMouseBackend final : public GlobalMouseBackend {
  public:
    void start(Handler, FailureHandler failure) override {
        if (failure) {
            failure(0);
        }
    }
    void stop() override {}
    void configure(const GlobalMouseConfiguration&) override {}
    void cancel(quint64) override {}
    void beginButtonDrag(settings::SettingsGlobalMouseAction) override {}
    GlobalMousePermissionState permissionState() const override {
        return {GlobalMousePermissionState::Status::Unavailable};
    }
};
} // namespace

std::unique_ptr<GlobalMouseBackend> createGlobalMouseBackend() {
#ifdef Q_OS_WIN
    return createWindowsGlobalMouseBackend();
#elif defined(Q_OS_MACOS)
    return createMacOSGlobalMouseBackend();
#elif defined(Q_OS_LINUX)
    return createLinuxGlobalMouseBackend();
#else
    return std::make_unique<UnsupportedGlobalMouseBackend>();
#endif
}

} // namespace snow_shot::presentation
