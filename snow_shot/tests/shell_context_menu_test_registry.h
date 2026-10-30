#ifndef SNOW_SHOT_TESTS_SHELL_CONTEXT_MENU_TEST_REGISTRY_H
#define SNOW_SHOT_TESTS_SHELL_CONTEXT_MENU_TEST_REGISTRY_H

#include "snow_shot/app/edition.h"
#include "snow_shot/platform/windows/shellcontextmenuregistration.h"

#include <algorithm>
#include <string>

namespace shell_context_menu_tests {
namespace windows = snow_shot::platform::windows;

inline windows::ShellContextMenuRegistryValue stringValue(const QString& text) {
    const auto native = text.toStdU16String();
    return {1, QByteArray(reinterpret_cast<const char*>(native.c_str()),
                          static_cast<qsizetype>((native.size() + 1) * sizeof(char16_t)))};
}

inline QString stringText(const windows::ShellContextMenuRegistryValue& value) {
    std::u16string native(static_cast<size_t>(value.data.size() / 2), u'\0');
    std::copy(value.data.cbegin(), value.data.cend(), reinterpret_cast<char*>(native.data()));
    return QString::fromStdU16String(native.substr(0, native.find(u'\0')));
}

class Registry final {
  public:
    QMap<QString, windows::ShellContextMenuRegistryKey> keys;
    bool failReads = false;
    int writesBeforeFailure = -1;
    int writes = 0;
    int notifications = 0;

    [[nodiscard]] windows::ShellContextMenuRegistryAccess access() {
        return {
            [this](const QString& path, QStringList* children, QString* error) {
                if (failReads) {
                    *error = QStringLiteral("injected read failure");
                    return false;
                }
                const auto prefix = path.toLower() + QStringLiteral("\\");
                for (auto key = keys.cbegin(); key != keys.cend(); ++key) {
                    if (!key.key().startsWith(prefix))
                        continue;
                    const auto child =
                        key.key().mid(prefix.size()).section(QLatin1Char('\\'), 0, 0);
                    if (!children->contains(child))
                        children->append(child);
                }
                return true;
            },
            [this](const QString& path, windows::ShellContextMenuRegistryKey* key, QString* error) {
                if (failReads) {
                    *error = QStringLiteral("injected read failure");
                    return false;
                }
                *key = keys.value(path.toLower());
                return true;
            },
            [this](const QString& path, const windows::ShellContextMenuRegistryKey& key,
                   QString* error) {
                ++writes;
                const auto normalized = path.toLower();
                if (key.exists)
                    keys.insert(normalized, key);
                else
                    keys.remove(normalized);
                if (writesBeforeFailure == 0) {
                    writesBeforeFailure = -1;
                    // Model an operation that failed after partially changing native state.
                    keys[normalized].exists = true;
                    keys[normalized].values.insert(QStringLiteral("PartialWrite"),
                                                   stringValue(path));
                    *error = QStringLiteral("injected write failure");
                    return false;
                }
                if (writesBeforeFailure > 0)
                    --writesBeforeFailure;
                return true;
            },
            [this] { ++notifications; },
        };
    }

    [[nodiscard]] windows::ShellContextMenuRegistration::Options options() {
        windows::ShellContextMenuRegistration::Options options;
        options.classesRoot = QStringLiteral("Software\\SnowShotTests");
        options.executablePath =
            QStringLiteral("C:/Snow Shot/雪/") + snow_shot::app::edition::executableName();
        options.fileExtensions = {QStringLiteral("png"), QStringLiteral("jpg")};
        options.registry = access();
        return options;
    }

    [[nodiscard]] static QString path(const QString& extension, const QString& verb = {}) {
        return (QStringLiteral("Software\\SnowShotTests\\SystemFileAssociations\\.") + extension +
                QStringLiteral("\\shell\\") +
                (verb.isEmpty() ? snow_shot::app::edition::registryName() + QStringLiteral(".Edit")
                                : verb))
            .toLower();
    }
};

} // namespace shell_context_menu_tests

#endif // SNOW_SHOT_TESTS_SHELL_CONTEXT_MENU_TEST_REGISTRY_H
