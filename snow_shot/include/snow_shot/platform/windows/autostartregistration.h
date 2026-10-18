#ifndef SNOW_SHOT_PLATFORM_WINDOWS_AUTOSTARTREGISTRATION_H
#define SNOW_SHOT_PLATFORM_WINDOWS_AUTOSTARTREGISTRATION_H

#include <QByteArray>
#include <QString>
// Qt 6 才有聚合头 <QtTypes>（qsizetype 等基础别名的统一入口）。
// Qt 5.15 里同样的内容在 <QtCore/qtypes.h>，而且 qglobal.h 并不包含它，
// 所以不能用 <QtGlobal> 代替。前面必须先引 <QtGlobal>，否则 QT_VERSION /
// QT_VERSION_CHECK 还没定义（有的头把 <QtTypes> 放在第一个 include）。
#include <QtGlobal>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtTypes>
#else
// Qt 5.15 **根本没有** qtypes.h（那是 Qt6 才拆出来的文件），qsizetype 直接
// typedef 在 qglobal.h 里，所以这里引 <QtGlobal> 即可。
#include <QtGlobal>
#endif

namespace snow_shot::platform::windows {

struct AutoStartRegistrationSnapshot {
    bool valid = false;
    bool exists = false;
    quint32 nativeType = 0;
    QByteArray nativeData;
    QString error;
};

class AutoStartRegistration final {
  public:
    [[nodiscard]] static bool isSupported();
    [[nodiscard]] static QString expectedCommand();
    [[nodiscard]] static AutoStartRegistrationSnapshot snapshot();
    [[nodiscard]] static bool matchesExpectedCommand();
    [[nodiscard]] static bool setEnabled(bool enabled, QString* error = nullptr);
    [[nodiscard]] static bool restore(const AutoStartRegistrationSnapshot& snapshot,
                                      QString* error = nullptr);
};

} // namespace snow_shot::platform::windows

#endif // SNOW_SHOT_PLATFORM_WINDOWS_AUTOSTARTREGISTRATION_H
