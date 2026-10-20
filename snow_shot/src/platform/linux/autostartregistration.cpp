#include "snow_shot/platform/linux/autostartregistration.h"

#include "snow_shot/app/edition.h"
#include "snow_shot/platform/linux/desktopentry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

#ifdef linux
#undef linux
#endif

namespace snow_shot::platform::linux {
namespace {

QString translated(const char* source) {
    return QCoreApplication::translate("AutoStart", source);
}

QString entryFileName() {
    // The name doubles as the entry's identity: a system-wide file with this
    // name is the one the user's file overrides, and the one a `Hidden` stub has
    // to mask. It therefore follows the product id, not the display name, so
    // that a translated name never changes which file is masked.
    return app::edition::productId() + QStringLiteral(".desktop");
}

QString userAutostartDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
        .filePath(QStringLiteral("autostart"));
}

QStringList systemAutostartDirectories() {
    QStringList directories;
    const QStringList bases =
        QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation);
    directories.reserve(bases.size());
    for (const QString& base : bases)
        directories.push_back(QDir(base).filePath(QStringLiteral("autostart")));
    return directories;
}

/// Reads a file, returning a null array when it is absent or unreadable. Null
/// and empty are different answers here: an empty file exists and must not be
/// mistaken for a missing one.
QByteArray readEntry(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool entryExists(const QString& path) {
    const QFileInfo info(path);
    // A dangling symlink is still something the desktop will try to read, so it
    // counts as present.
    return info.exists() || info.isSymLink();
}

bool writeEntry(const QByteArray& contents, QString* error) {
    const QString directory = userAutostartDirectory();
    if (!QDir().mkpath(directory)) {
        if (error != nullptr) {
            *error = translated("The autostart directory could not be created: %1")
                         .arg(QDir::toNativeSeparators(directory));
        }
        return false;
    }

    const QString path = QDir(directory).filePath(entryFileName());
    QSaveFile file(path);
    // No direct-write fallback: a partially written entry on a desktop that
    // reads it during login is worse than a reported failure.
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error =
                translated("The autostart entry could not be written: %1").arg(file.errorString());
        }
        return false;
    }
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) {
            *error =
                translated("The autostart entry could not be written: %1").arg(file.errorString());
        }
        return false;
    }
    return true;
}

QString nameForEntry() {
    return app::edition::productName();
}

/// The command the entry has to launch. Compared as arguments rather than as
/// text, so an entry written by a packager with different but equivalent quoting
/// is recognised instead of being rewritten on every start.
QStringList expectedCommand() {
    return {QCoreApplication::applicationFilePath(), QStringLiteral("--autostart")};
}

bool systemEntryExists() {
    for (const QString& directory : systemAutostartDirectories()) {
        if (entryExists(QDir(directory).filePath(entryFileName())))
            return true;
    }
    return false;
}

} // namespace

bool AutoStartRegistration::isSupported() {
#if defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

QString AutoStartRegistration::entryPath() {
    return QDir(userAutostartDirectory()).filePath(entryFileName());
}

QString AutoStartRegistration::expectedExec() {
#if defined(Q_OS_LINUX)
    return desktopentry::execValue(expectedCommand());
#else
    return {};
#endif
}

bool AutoStartRegistration::isEnabled() {
#if defined(Q_OS_LINUX)
    const QByteArray userEntry = readEntry(entryPath());
    if (!userEntry.isNull())
        return desktopentry::requestsAutoStart(userEntry);
    for (const QString& directory : systemAutostartDirectories()) {
        const QByteArray entry = readEntry(QDir(directory).filePath(entryFileName()));
        if (!entry.isNull())
            return desktopentry::requestsAutoStart(entry);
    }
    return false;
#else
    return false;
#endif
}

bool AutoStartRegistration::matchesExpectedEntry() {
#if defined(Q_OS_LINUX)
    const QStringList expected = expectedCommand();
    const auto matches = [&expected](const QByteArray& entry) {
        if (!desktopentry::requestsAutoStart(entry))
            return false;
        const QString exec = desktopentry::readValue(
            entry, QString::fromLatin1(desktopentry::desktopEntryGroup), QStringLiteral("Exec"));
        return desktopentry::parseExec(exec) == expected;
    };

    const QByteArray userEntry = readEntry(entryPath());
    if (!userEntry.isNull())
        return matches(userEntry);
    for (const QString& directory : systemAutostartDirectories()) {
        const QByteArray entry = readEntry(QDir(directory).filePath(entryFileName()));
        if (!entry.isNull())
            return matches(entry);
    }
    return false;
#else
    return false;
#endif
}

AutoStartSnapshot AutoStartRegistration::snapshot() {
    AutoStartSnapshot result;
#if defined(Q_OS_LINUX)
    const QString path = entryPath();
    result.existed = entryExists(path);
    if (result.existed) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            result.error = translated("The current autostart entry could not be read: %1")
                               .arg(file.errorString());
            return result;
        }
        result.contents = file.readAll();
    }
    result.valid = true;
#else
    result.error = translated("Auto-start is only supported on Linux");
#endif
    return result;
}

bool AutoStartRegistration::setEnabled(bool enabled, QString* error) {
#if defined(Q_OS_LINUX)
    if (enabled) {
        return writeEntry(desktopentry::autoStartEntry(nameForEntry(), expectedExec()), error);
    }

    const QString path = entryPath();
    if (entryExists(path) && !QFile::remove(path)) {
        if (error != nullptr) {
            *error = translated("The autostart entry could not be removed: %1")
                         .arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    // Removing our file is not enough when a packaged entry has the same name:
    // without ours, that one becomes the effective entry again. The
    // specification's answer is a stub that masks it.
    if (systemEntryExists())
        return writeEntry(desktopentry::hiddenEntry(nameForEntry()), error);
    return true;
#else
    if (error != nullptr)
        *error = translated("Auto-start is only supported on Linux");
    Q_UNUSED(enabled)
    return false;
#endif
}

bool AutoStartRegistration::restore(const AutoStartSnapshot& previous, QString* error) {
#if defined(Q_OS_LINUX)
    if (!previous.valid) {
        if (error != nullptr)
            *error = translated("The previous autostart entry could not be read");
        return false;
    }
    if (previous.existed)
        return writeEntry(previous.contents, error);
    const QString path = entryPath();
    if (entryExists(path) && !QFile::remove(path)) {
        if (error != nullptr) {
            *error = translated("The autostart entry could not be removed: %1")
                         .arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    return true;
#else
    Q_UNUSED(previous)
    if (error != nullptr)
        *error = translated("Auto-start is only supported on Linux");
    return false;
#endif
}

AutoStartResult AutoStartRegistration::reconcile(bool enabled) {
    AutoStartResult result;
#if defined(Q_OS_LINUX)
    QString error;
    if (enabled) {
        if (matchesExpectedEntry())
            return result;
        if (!setEnabled(true, &error)) {
            result.success = false;
            result.error = error;
        }
        return result;
    }
    if (!isEnabled())
        return result;
    if (!setEnabled(false, &error)) {
        result.success = false;
        result.error = error;
    }
    return result;
#else
    Q_UNUSED(enabled)
    result.success = false;
    result.error = translated("Auto-start is only supported on Linux");
    return result;
#endif
}

} // namespace snow_shot::platform::linux
