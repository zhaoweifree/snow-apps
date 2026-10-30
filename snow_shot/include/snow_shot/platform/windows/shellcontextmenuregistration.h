#ifndef SNOW_SHOT_PLATFORM_WINDOWS_SHELLCONTEXTMENUREGISTRATION_H
#define SNOW_SHOT_PLATFORM_WINDOWS_SHELLCONTEXTMENUREGISTRATION_H

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
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
#include <functional>

namespace snow_shot::platform::windows {

struct ShellContextMenuRegistryValue {
    quint32 type = 0;
    QByteArray data;

    friend bool operator==(const ShellContextMenuRegistryValue&,
                           const ShellContextMenuRegistryValue&) = default;
};

struct ShellContextMenuRegistryKey {
    bool exists = false;
    QMap<QString, ShellContextMenuRegistryValue> values;
    QMap<QString, ShellContextMenuRegistryKey> children;

    friend bool operator==(const ShellContextMenuRegistryKey&,
                           const ShellContextMenuRegistryKey&) = default;
};

// All paths are relative to HKCU. Injection keeps tests away from the user's registrations.
struct ShellContextMenuRegistryAccess {
    std::function<bool(const QString&, QStringList*, QString*)> childKeys;
    std::function<bool(const QString&, ShellContextMenuRegistryKey*, QString*)> read;
    std::function<bool(const QString&, const ShellContextMenuRegistryKey&, QString*)> replace;
    std::function<void()> notifyChanged;
};

struct ShellContextMenuRegistrationSnapshot {
    bool valid = false;
    QMap<QString, ShellContextMenuRegistryKey> keys;
    QString error;
};

struct ShellContextMenuResult {
    bool success = false;
    QString error;
};

class ShellContextMenuRegistration final {
  public:
    struct Options {
        QString classesRoot;
        QString executablePath;
        QString productId;
        QString verbName;
        QStringList fileExtensions;
        ShellContextMenuRegistryAccess registry;
    };

    ShellContextMenuRegistration();
    explicit ShellContextMenuRegistration(Options options);

    [[nodiscard]] static bool isSupported();
    [[nodiscard]] QString expectedCommand() const;
    [[nodiscard]] ShellContextMenuRegistrationSnapshot snapshot() const;
    [[nodiscard]] bool restore(const ShellContextMenuRegistrationSnapshot& previous,
                               QString* error = nullptr) const;
    [[nodiscard]] bool setEnabled(bool enabled, QString* error = nullptr) const;
    [[nodiscard]] bool setEnabled(bool enabled, const QString& accessKey,
                                  QString* error = nullptr) const;
    [[nodiscard]] bool refreshLabels(QString* error = nullptr) const;
    [[nodiscard]] bool refreshLabels(const QString& accessKey, QString* error = nullptr) const;
    // Native changes and the preference commit form one transaction.
    [[nodiscard]] ShellContextMenuResult
    changeEnabled(bool enabled, const std::function<bool()>& commitPreference) const;
    [[nodiscard]] ShellContextMenuResult
    changeEnabled(bool enabled, const QString& accessKey,
                  const std::function<bool()>& commitPreference) const;

  private:
    [[nodiscard]] bool ownedByProduct(const ShellContextMenuRegistryKey& key) const;
    [[nodiscard]] ShellContextMenuRegistryKey enabledKey(ShellContextMenuRegistryKey previous,
                                                         const QString& accessKey) const;
    [[nodiscard]] bool apply(bool enabled, const QString& accessKey,
                             const ShellContextMenuRegistrationSnapshot& previous,
                             QString* error) const;
    QString m_associationsRoot;
    QString m_executable;
    QString m_productId;
    QString m_verbName;
    QStringList m_extensions;
    ShellContextMenuRegistryAccess m_registry;
};

} // namespace snow_shot::platform::windows

#endif // SNOW_SHOT_PLATFORM_WINDOWS_SHELLCONTEXTMENUREGISTRATION_H
