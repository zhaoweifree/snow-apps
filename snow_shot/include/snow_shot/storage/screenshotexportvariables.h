// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QString>

namespace snow_shot::storage {
struct ScreenshotWindowNameFields {
    bool applicationName = false;
    bool windowName = false;
    [[nodiscard]] bool any() const {
        return applicationName || windowName;
    }
    friend bool operator==(const ScreenshotWindowNameFields&,
                           const ScreenshotWindowNameFields&) = default;
};

struct ScreenshotWindowNames {
    QString applicationName;
    QString windowName;
    friend bool operator==(const ScreenshotWindowNames&, const ScreenshotWindowNames&) = default;
};

[[nodiscard]] bool containsScreenshotWindowVariables(const QString& text);
[[nodiscard]] ScreenshotWindowNameFields screenshotWindowNameFields(const QString& directory,
                                                                    const QString& filename);
// Expand once and bound each path component. Filename consumers must also reserve their suffix
// (extension, collision index, etc.) with composeExportFileName().
[[nodiscard]] QString expandScreenshotExportVariables(const QString& text,
                                                      const QDateTime& timestamp,
                                                      const ScreenshotWindowNames& names = {});
} // namespace snow_shot::storage
