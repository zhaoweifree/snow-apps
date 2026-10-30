#ifndef SNOW_SHOT_SHORTCUTS_MENUACCESSKEY_H
#define SNOW_SHOT_SHORTCUTS_MENUACCESSKEY_H

#include <QString>

namespace snow_shot::shortcuts {

// Menu mnemonics identify a logical character, independent of the physical keyboard.
[[nodiscard]] inline QString canonicalMenuAccessKey(const QString& text) {
    if (text.size() != 1)
        return {};
    const auto character = text.front().unicode();
    if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9'))
        return text.toUpper();
    return {};
}

} // namespace snow_shot::shortcuts

#endif // SNOW_SHOT_SHORTCUTS_MENUACCESSKEY_H
