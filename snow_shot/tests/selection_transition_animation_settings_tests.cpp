#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsformfield.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "widgets/select.h"

#include <QApplication>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;
using Animation = snow_shot::SelectionTransitionAnimation;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void animationSelectDefaultsPersistsAndResets(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto& registry = settings::builtInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    constexpr auto binding = settings::SettingsSelectBinding::SelectionTransitionAnimation;
    const QString key = QStringLiteral("screenshot_ui/selection_transition_animation");
    const auto* field = registry.fieldForSelect(binding);
    require(
        registry.validationErrors().isEmpty() && field != nullptr &&
            field->id == QStringLiteral("interface.screenshot.selection-transition-animation") &&
            field->kind == settings::SettingsFieldKind::Select && field->configurationKey == key &&
            field->defaultValue == QJsonValue(32) &&
            field->reset == settings::SettingsSectionReset::ScreenshotInterfaceSettings,
        "selection animation must be a select with a Soft default and screenshot interface reset");
    const auto& select = std::get<settings::SettingsSelectDefinition>(field->definition->payload);
    require(select.options.size() == 3 && select.options[0].value == 72 &&
                select.options[0].label.translated() == QStringLiteral("Fast") &&
                select.options[1].value == 32 &&
                select.options[1].label.translated() == QStringLiteral("Soft") &&
                select.options[2].value == 0 &&
                select.options[2].label.translated() == QStringLiteral("Off"),
            "the select must offer Fast, Soft and Off in order with values 72, 32 and 0");

    const storage::ScreenshotUiSettings stored;
    auto* form = settings::SettingsFormField::create(*field, session);
    require(form != nullptr, "selection animation must create a standard settings field");
    const std::unique_ptr<QWidget> view(form->viewWidget());
    auto* control = qobject_cast<adqt::widgets::AdSelect*>(form->editor());
    require(control != nullptr && control->currentValue() == 32,
            "selection animation must render an actual select with Soft selected");
    require(session.selectValue(binding) == 32 && backend.selectValue(binding) == 32 &&
                stored.selectionTransitionAnimation() == Animation::Soft,
            "new configurations and the settings session must default to Soft");
    for (const int value : {72, 0, 32}) {
        require(session.applySelectValue(binding, value) && backend.selectValue(binding) == value &&
                    static_cast<int>(stored.selectionTransitionAnimation()) == value &&
                    storage::ApplicationStorage::instance().configuration().flushNow().success,
                "each animation choice must commit through the session and typed settings adapter");
        form->sync();
        require(control->currentValue() == value,
                "the select must display each persisted mode, including zero-valued Off");
        storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
        require(reloaded.value(key) == QJsonValue(value),
                "each animation choice must survive a configuration reload");
    }
    require(!session.applySelectValue(binding, 42) &&
                !backend.applySelectValue(binding, QStringLiteral("invalid")) &&
                !backend.applySelectValue(binding, 32.5) &&
                !stored.setSelectionTransitionAnimation(static_cast<Animation>(42)) &&
                session.state(field->id).acceptedValue == 32 &&
                session.state(field->id).phase == settings::SettingsWritePhase::Rejected &&
                backend.selectValue(binding) == 32,
            "unsupported animation choices must be rejected without changing accepted or stored "
            "values");
    require(session.applySelectValue(binding, 0) &&
                session.reset(settings::SettingsSectionReset::ScreenshotInterfaceSettings) &&
                session.selectValue(binding) == 32 &&
                stored.selectionTransitionAnimation() == Animation::Soft,
            "resetting screenshot interface settings must restore Soft from Off");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "animation settings require isolated storage");
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage
                .initialize({temporary.filePath(QStringLiteral("bin")),
                             temporary.filePath(QStringLiteral("data")), 60000})
                .success,
            "initialize animation settings storage");
    animationSelectDefaultsPersistsAndResets(
        temporary.filePath(QStringLiteral("data/config.json")));
    appStorage.shutdown();
    return 0;
}
