#!/usr/bin/env python3
"""A public header's Qt module has to be a *public* link dependency.

`statusnotifierservice.h` is a public header of the `snow_shot_status_notifier`
static library and it includes `<QDBusObjectPath>`. The library linked QtDBus as
`PRIVATE`. A PRIVATE dependency of a static library still reaches the linker -
CMake wraps it in `$<LINK_ONLY:...>` on the consumer's interface - so nothing
complains at configure time and the link would have been fine. What does not
propagate is the *usage requirements*: the consumer never gets
`-isystem .../qt5/QtDBus`, and compiling the header fails:

    statusnotifierservice.h:6:10: fatal error: QDBusObjectPath:
    No such file or directory                 (CI run 23, both architectures)

The header was only ever compiled on Linux (`systemtraycontroller.cpp` guards
its includes), so a Windows or macOS build could not have seen it - the class of
bug is "the platform that can build it is not the platform that consumes it".

The rule this gate enforces: for every STATIC/OBJECT library, each Qt module
that its public headers include must appear in its PUBLIC or INTERFACE link
libraries. `Core` counts as covered when a dependent module is public, because
`Qt::Gui` and `Qt::Widgets` bring it along; the table below is Qt's own module
dependency graph, not a guess.

A Qt header that is not in the table is a failure rather than a skip: a gate
whose blind spots are silent is the reason this bug shipped.

Run it directly; the Linux packaging gate runs it as well.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Qt's own module dependency graph, for the modules this project links. Used to
# answer "is Core covered?" without demanding a redundant `Qt::Core` entry.
QT_MODULE_IMPLIES: dict[str, set[str]] = {
    "Core": set(),
    "Gui": {"Core"},
    "Widgets": {"Gui", "Core"},
    "Network": {"Core"},
    "DBus": {"Core"},
    "Svg": {"Gui", "Core"},
    "SvgWidgets": {"Svg", "Widgets", "Gui", "Core"},
    "Concurrent": {"Core"},
    "PrintSupport": {"Widgets", "Gui", "Core"},
    "OpenGL": {"Gui", "Core"},
    "Sql": {"Core"},
    "Xml": {"Core"},
    "Multimedia": {"Network", "Gui", "Core"},
}

# Which module a public Qt header belongs to. Covers every header that currently
# appears in a static library's public headers; a new one has to be added here,
# and the gate says so. Note the split that matters: QImage/QColor/QPixmap are
# Gui while QRect/QPointF/QSize are Core, and QKeySequence is Gui in Qt 5.
QT_HEADER_MODULES: dict[str, str] = {
    # Core
    "Qt": "Core",
    "QByteArray": "Core",
    "QCoreApplication": "Core",
    "QDateTime": "Core",
    "QDeadlineTimer": "Core",
    "QFile": "Core",
    "QFlags": "Core",
    "QHash": "Core",
    "QJsonArray": "Core",
    "QJsonObject": "Core",
    "QJsonValue": "Core",
    "QList": "Core",
    "QLocale": "Core",
    "QMap": "Core",
    "QMetaType": "Core",
    "QMutex": "Core",
    "QObject": "Core",
    "QPointer": "Core",
    "QPointF": "Core",
    "QRect": "Core",
    "QRectF": "Core",
    "QRegularExpression": "Core",
    "QSet": "Core",
    "QSharedPointer": "Core",
    "QSize": "Core",
    "QSizeF": "Core",
    "QString": "Core",
    "QStringList": "Core",
    "QStringView": "Core",
    "QTemporaryDir": "Core",
    "QThread": "Core",
    "QTimer": "Core",
    "QUrl": "Core",
    "QVariant": "Core",
    "QVariantList": "Core",
    "QVariantMap": "Core",
    "QVector": "Core",
    # Gui
    "QColor": "Gui",
    "QColorSpace": "Gui",
    "QCursor": "Gui",
    "QFont": "Gui",
    "QIcon": "Gui",
    "QImage": "Gui",
    "QKeySequence": "Gui",
    "QPalette": "Gui",
    "QPen": "Gui",
    "QBrush": "Gui",
    "QPixmap": "Gui",
    "QTransform": "Gui",
    # Widgets
    "QAction": "Widgets",
    "QApplication": "Widgets",
    "QLabel": "Widgets",
    "QMenu": "Widgets",
    "QWidget": "Widgets",
    # Network
    "QNetworkReply": "Network",
    "QNetworkRequest": "Network",
    # DBus
    "QDBusArgument": "DBus",
    "QDBusConnection": "DBus",
    "QDBusMessage": "DBus",
    "QDBusObjectPath": "DBus",
    "QDBusVariant": "DBus",
}

ADD_LIBRARY = re.compile(r"add_library\s*\(\s*([A-Za-z0-9_]+)\s+(STATIC|OBJECT)\b")
TARGET_LINK = re.compile(r"target_link_libraries\s*\(\s*([A-Za-z0-9_]+)")
QT_INCLUDE = re.compile(
    r"""^[ \t]*#[ \t]*include[ \t]*[<"](Q[A-Za-z0-9_]+|Qt[A-Za-z]+/[A-Za-z0-9_/]+)[>"]""",
    re.MULTILINE,
)
HEADER_IN_SOURCES = re.compile(r"([A-Za-z0-9_./\-]*\binclude/[A-Za-z0-9_./\-]+\.h)")
QT_TARGET = re.compile(r"Qt::([A-Za-z0-9_]+)")
LINK_MODES = ("PUBLIC", "PRIVATE", "INTERFACE")

_failures = 0


def fail(message: str) -> None:
    global _failures
    print(f"FAIL: {message}")
    _failures += 1


def balanced_call(text: str, start: int) -> str:
    """The parenthesised argument list of the call that starts at `start`."""
    index = text.find("(", start)
    if index < 0:
        return ""
    depth = 0
    for offset in range(index, len(text)):
        char = text[offset]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return text[index : offset + 1]
    return text[index:]


def qt_link_modes(cmakelists: Path) -> dict[str, dict[str, set[str]]]:
    """target -> {PUBLIC|PRIVATE|INTERFACE: {Qt module names}}.

    The plain `target_link_libraries(t foo)` signature means PUBLIC for a target
    that is not imported, so an unkeyworded run of arguments starts out PUBLIC.
    """
    text = cmakelists.read_text(encoding="utf-8", errors="replace")
    result: dict[str, dict[str, set[str]]] = {}
    for match in TARGET_LINK.finditer(text):
        target = match.group(1)
        body = balanced_call(text, match.start())
        modes = {mode: set() for mode in LINK_MODES}
        current = "PUBLIC"
        for token in body.replace("(", " ").replace(")", " ").split():
            if token in modes:
                current = token
                continue
            for module in QT_TARGET.findall(token):
                modes[current].add(module)
        entry = result.setdefault(target, {mode: set() for mode in LINK_MODES})
        for mode in LINK_MODES:
            entry[mode] |= modes[mode]
    return result


def required_modules(header: Path) -> tuple[dict[str, set[str]], set[str]]:
    """{Qt module: {header names that pulled it in}}, plus unknown header names."""
    modules: dict[str, set[str]] = {}
    unknown: set[str] = set()
    text = header.read_text(encoding="utf-8", errors="replace")
    for include in QT_INCLUDE.findall(text):
        # `QtDBus/QDBusConnection` names the module directly; a bare `QIcon`
        # needs the table.
        if "/" in include:
            module = include.split("/", 1)[0]
            if module.startswith("Qt"):
                module = module[2:]
            if module not in QT_MODULE_IMPLIES:
                unknown.add(include)
                continue
        else:
            module = QT_HEADER_MODULES.get(include)
            if module is None:
                unknown.add(include)
                continue
        modules.setdefault(module, set()).add(include)
    return modules, unknown


def static_libraries(cmakelists: Path) -> list[tuple[str, list[Path]]]:
    """(target, public headers listed among its sources) for STATIC/OBJECT libs."""
    text = cmakelists.read_text(encoding="utf-8", errors="replace")
    libraries = []
    for match in ADD_LIBRARY.finditer(text):
        body = balanced_call(text, match.start())
        headers = []
        for relative in HEADER_IN_SOURCES.findall(body):
            path = cmakelists.parent / relative
            if path.is_file():
                headers.append(path)
        if headers:
            libraries.append((match.group(1), headers))
    return libraries


def main() -> int:
    cmakelists_files = [
        path for path in sorted(ROOT.glob("*/CMakeLists.txt"))
        if ".tools" not in path.parts
    ]
    if not cmakelists_files:
        fail("no module CMakeLists.txt found -- this gate has lost its subject")
        return 1

    libraries_checked = 0
    headers_checked = 0
    includes_classified = 0

    for cmakelists in cmakelists_files:
        links = qt_link_modes(cmakelists)
        for target, headers in static_libraries(cmakelists):
            modes = links.get(target, {mode: set() for mode in LINK_MODES})
            public_modules = modes["PUBLIC"] | modes["INTERFACE"]
            # Everything a public module drags along, so `Qt::Widgets` covers Gui
            # and Core without a redundant entry.
            effective = set(public_modules)
            for module in public_modules:
                effective |= QT_MODULE_IMPLIES.get(module, set())

            libraries_checked += 1
            for header in headers:
                headers_checked += 1
                modules, unknown = required_modules(header)
                includes_classified += sum(len(names) for names in modules.values())
                relative = header.relative_to(ROOT).as_posix()

                for include in sorted(unknown):
                    fail(
                        f"{relative} includes <{include}>, which this gate cannot map "
                        "to a Qt module; add it to QT_HEADER_MODULES so the visibility "
                        "of its module can be checked"
                    )

                for module in sorted(modules):
                    if module in effective:
                        continue
                    listed = (
                        "linked PRIVATE"
                        if module in modes["PRIVATE"]
                        else "not linked at all"
                    )
                    fail(
                        f"{target}: public header {relative} includes "
                        f"{sorted(modules[module])}, but Qt::{module} is {listed}; "
                        "consumers that compile this header never get its include "
                        "directory -- make it PUBLIC"
                    )

    if libraries_checked == 0:
        fail("no static library with public headers was inspected")
    if _failures:
        print(f"{_failures} Qt module visibility check(s) failed")
        return 1
    print(
        f"Qt module visibility check passed ({libraries_checked} static libraries, "
        f"{headers_checked} public headers, {includes_classified} Qt includes classified)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
