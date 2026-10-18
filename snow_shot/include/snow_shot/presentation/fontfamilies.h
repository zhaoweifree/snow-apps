#ifndef SNOW_SHOT_PRESENTATION_FONTFAMILIES_H
#define SNOW_SHOT_PRESENTATION_FONTFAMILIES_H

#include <QFontDatabase>
#include <QStringList>
#include <QtGlobal> // QT_VERSION must be defined before any QT_VERSION_CHECK guard below

namespace snow_shot::presentation {
// Enumerate once: font selectors are rebuilt when pages and toolbar editors change.
inline const QStringList& applicationFontFamilies() {
    static const QStringList families = [] {
        QStringList result;
        // Qt6 turned these QFontDatabase accessors static; Qt 5.15 still requires
        // an instance ("call of non-static member function requires an object",
        // C2352). QFontDatabase is a lightweight handle, so constructing one per
        // enumeration is free -- and the result is cached below anyway.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QStringList available = QFontDatabase::families();
#else
        const QStringList available = QFontDatabase().families();
#endif
        for (const QString& family : available) {
            const QString trimmed = family.trimmed();
            if (!trimmed.isEmpty()) {
                result.append(trimmed);
            }
        }
        result.removeDuplicates();
        result.sort(Qt::CaseInsensitive);
        return result;
    }();
    return families;
}
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_FONTFAMILIES_H
