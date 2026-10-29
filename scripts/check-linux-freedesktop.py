#!/usr/bin/env python3
"""Validate the Linux desktop integration files without a Linux machine.

`desktop-file-validate` and `appstreamcli validate` only exist on the target,
which means the two files most likely to be wrong in a way nobody notices until
install time -- the entry and the AppStream metadata -- would otherwise never be
checked. This script re-implements the subset of their rules that this project
actually depends on, plus the cross-file checks a generic validator cannot do:

  * the entry file name is ``app::edition::desktopEntryId()``, which has to be a
    reverse-DNS name for AppStream, while the icon name is ``productId()`` --
    two different strings on purpose;
  * the AppStream id has to satisfy the rules ``appstreamcli`` enforces on a
    ``desktop-application`` component, which is the check whose absence let CI
    run 42 ship ``snow-shot.desktop`` and fail half an hour later;
  * ``StartupWMClass`` is ``app::edition::applicationName()``;
  * the release version in the AppStream file matches ``SNOW_SHOT_VERSION``.

Usage: ``python scripts/check-linux-freedesktop.py`` (the shell entry point
``scripts/check-linux-packaging.sh`` runs this and the CMake harness).
"""

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PACKAGING = REPO / "snow_shot" / "packaging" / "linux"
# Named after the reverse-DNS component id, which is what AppStream requires of a
# desktop application (see the RDNS check in main()). `appstreamcli` also wants
# the metainfo file name to be `<id>.metainfo.xml`.
DESKTOP = PACKAGING / "com.snowshot.snow_shot.desktop"
METAINFO = PACKAGING / "com.snowshot.snow_shot.metainfo.xml"
ICON = REPO / "snow_shot" / "resources" / "app-icon.svg"
EDITION = REPO / "snow_shot" / "include" / "snow_shot" / "app" / "edition.h"
ROOT_CMAKE = REPO / "CMakeLists.txt"

# The reverse-DNS rule `as_validator_validate_component_id()` applies to a
# desktop-application component, read out of appstream's `src/as-validator.c`:
#
#  * fewer than three dot-separated parts is `cid-desktopapp-is-not-rdns`;
#  * with three or more, the first part has to be a known top-level domain or it
#    is `cid-maybe-not-rdns` (`as_utils_is_tld()` against the bundled IANA list);
#  * the first two parts have to be lowercase (`cid-domain-not-lowercase`);
#  * a hyphen outside the last part is `cid-rdns-contains-hyphen`, and an
#    uppercase letter anywhere is `cid-contains-uppercase-letter`;
#  * the character set is alphanumerics plus `.`, `-` and `_`.
#
# Every one of those is a warning, and `appstreamcli validate` exits 3 on any
# warning. The TLD list below is the subset that can plausibly start a project
# domain -- it is a gate, not an IANA mirror, and a name that is not in it is
# either a typo or an invented TLD, both of which appstreamcli rejects too.
KNOWN_TLDS = {
    "com", "org", "net", "io", "dev", "app", "edu", "gov", "info", "biz",
    "de", "fr", "uk", "cn", "jp", "kr", "ru", "nl", "se", "no", "fi", "dk",
    "it", "es", "pt", "pl", "cz", "at", "ch", "be", "ca", "au", "nz", "br",
    "in", "za", "mx", "ar", "tr", "gr", "hu", "ie", "il", "sg", "tw", "ua",
}

# The main categories freedesktop.org defines. An entry has to name at least one
# of these or it is only reachable by searching for its name.
MAIN_CATEGORIES = {
    "AudioVideo", "Audio", "Video", "Development", "Education", "Game",
    "Graphics", "Network", "Office", "Science", "Settings", "System", "Utility",
}
# AppStream requires a permissive metadata licence; the content has to be
# reusable by software centres.
ALLOWED_METADATA_LICENSES = {"CC0-1.0", "MIT", "FSFAP", "0BSD"}

failures: list[str] = []


def check(label: str, condition: bool, detail: str = "") -> bool:
    if condition:
        print(f"ok   {label}")
        return True
    failures.append(label)
    print(f"FAIL {label}" + (f": {detail}" if detail else ""))
    return False


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def parse_desktop(path: Path) -> dict[str, str]:
    """Return the ``[Desktop Entry]`` keys, keeping only the unlocalised ones."""
    values: dict[str, str] = {}
    duplicates: list[str] = []
    group = None
    for raw in read_text(path).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            group = line[1:-1]
            continue
        if group != "Desktop Entry" or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if "[" in key:  # localised variant, e.g. Comment[zh_CN]
            continue
        if key in values:
            duplicates.append(key)
        values[key] = value.replace("\\s", " ").strip()
    if duplicates:
        failures.append(f"duplicate keys in {path.name}: {', '.join(sorted(set(duplicates)))}")
        print(f"FAIL duplicate keys in {path.name}: {', '.join(sorted(set(duplicates)))}")
    return values


def quote_value(source: str, function: str) -> str | None:
    """Return the string a ``productId()``-style function returns.

    These functions pick between the mini and the full edition with a ternary, so
    the value that matters is the one in the false branch -- the first literal in
    the body belongs to the mini build.
    """
    body = re.search(
        rf"inline QString {function}\(\)\s*\{{(.*?)\n\}}", source, re.DOTALL
    )
    if not body:
        return None
    branch = re.search(
        r'\?\s*QStringLiteral\("[^"]+"\)\s*:\s*QStringLiteral\("([^"]+)"\)',
        body.group(1),
    )
    if branch:
        return branch.group(1)
    literals = re.findall(r'QStringLiteral\("([^"]+)"\)', body.group(1))
    return literals[-1] if literals else None


def main() -> int:
    for path in (DESKTOP, METAINFO, ICON, EDITION, ROOT_CMAKE):
        if not path.exists():
            check(f"{path.relative_to(REPO)} exists", False, "missing")
    if failures:
        return 1

    edition = read_text(EDITION)
    product_id = quote_value(edition, "productId")
    desktop_id = quote_value(edition, "desktopEntryId")
    application_name = quote_value(edition, "applicationName")
    check("edition.h exposes a product id", product_id is not None)
    check("edition.h exposes a desktop entry id", desktop_id is not None)
    check("edition.h exposes an application name", application_name is not None)
    if product_id is None or desktop_id is None or application_name is None:
        return 1
    check(
        "entry file name is the desktop entry id",
        DESKTOP.name == f"{desktop_id}.desktop",
        f"expected {desktop_id}.desktop, found {DESKTOP.name}",
    )

    # --- Encoding: a CRLF or BOM here makes the entry invisible to the menu ----
    raw_desktop = DESKTOP.read_bytes()
    check("desktop entry has no BOM", not raw_desktop.startswith(b"\xef\xbb\xbf"))
    check("desktop entry has no CRLF", b"\r\n" not in raw_desktop)
    raw_metainfo = METAINFO.read_bytes()
    check("metainfo has no BOM", not raw_metainfo.startswith(b"\xef\xbb\xbf"))
    check("metainfo has no CRLF", b"\r\n" not in raw_metainfo)

    # --- The entry ------------------------------------------------------------
    entry = parse_desktop(DESKTOP)
    for key in ("Type", "Name", "Exec", "Icon", "Categories", "Version"):
        check(f"desktop entry has {key}", bool(entry.get(key)), "missing or empty")
    check("desktop entry type is Application", entry.get("Type") == "Application",
          entry.get("Type", ""))
    check("desktop entry version is 1.5", entry.get("Version") == "1.5",
          entry.get("Version", ""))

    exec_argv = entry.get("Exec", "").split()
    check("Exec names a program", bool(exec_argv))
    if exec_argv:
        # No field code: the application takes no file arguments, and advertising
        # %f/%U would make desktop shells try to hand it one.
        check("Exec passes no field codes",
              not any(arg.startswith("%") for arg in exec_argv), entry.get("Exec", ""))
        check("TryExec matches Exec",
              entry.get("TryExec", exec_argv[0]) == exec_argv[0],
              f'{entry.get("TryExec")} vs {exec_argv[0]}')
        check("StartupWMClass is the Qt application name",
              entry.get("StartupWMClass") == application_name,
              f'{entry.get("StartupWMClass")} vs {application_name}')
    check("Icon is the product id", entry.get("Icon") == product_id, entry.get("Icon", ""))

    categories = [c for c in entry.get("Categories", "").split(";") if c]
    check("Categories names a main category",
          any(c in MAIN_CATEGORIES for c in categories), str(categories))
    check("Categories is semicolon terminated", entry.get("Categories", "").endswith(";"))
    check("Terminal is false", entry.get("Terminal", "false").lower() == "false")
    check("entry is translated to zh_CN", "Comment[zh_CN]" in read_text(DESKTOP))

    # --- The AppStream file ---------------------------------------------------
    try:
        component = ElementTree.parse(METAINFO).getroot()
    except ElementTree.ParseError as error:
        check("metainfo parses as XML", False, str(error))
        return 1
    check("metainfo root is a component", component.tag == "component")
    check("metainfo declares a desktop application",
          component.get("type") == "desktop-application", component.get("type", ""))

    def text(tag: str) -> str:
        node = component.find(tag)
        return (node.text or "").strip() if node is not None else ""

    metainfo_id = text("id")

    # --- The component id has to be reverse-DNS -------------------------------
    # CI run 42 is why this block exists. `Validate the packaged metadata` ran
    # `appstreamcli validate` on `snow-shot.desktop` and got
    #
    #     W: snow-shot.desktop:3: cid-desktopapp-is-not-rdns snow-shot.desktop
    #     Validation failed: warnings: 1, infos: 2     (exit 3)
    #
    # -- a "warning" that fails the command, half an hour into a build. Nothing
    # local looked at the shape of the id: desktop-file-validate has no opinion on
    # it, and the checks below used to compare the id to the file name, which is
    # exactly the form that fails. Each rule appstreamcli applies is spelled out
    # here so a rename has to be a name this gate accepts.
    id_parts = metainfo_id.split(".")
    check("metainfo id is reverse-DNS (three or more parts)", len(id_parts) >= 3,
          f"{metainfo_id!r}: appstreamcli says cid-desktopapp-is-not-rdns")
    check("metainfo id starts with a known top-level domain",
          bool(id_parts) and id_parts[0] in KNOWN_TLDS,
          f"{metainfo_id!r}: appstreamcli says cid-maybe-not-rdns")
    check("metainfo id domain parts are lowercase",
          [p for p in id_parts[:2] if p != p.lower()] == [],
          f"{metainfo_id!r}: appstreamcli says cid-domain-not-lowercase")
    check("metainfo id has no hyphen outside the last part",
          not any("-" in p for p in id_parts[:-1]),
          f"{metainfo_id!r}: appstreamcli says cid-rdns-contains-hyphen")
    check("metainfo id has no uppercase letters",
          metainfo_id == metainfo_id.lower(),
          f"{metainfo_id!r}: appstreamcli says cid-contains-uppercase-letter")
    check("metainfo id has no part starting with a digit",
          not any(p[:1].isdigit() for p in id_parts),
          f"{metainfo_id!r}: appstreamcli says cid-has-number-prefix")
    check("metainfo id is the entry file name without its suffix",
          metainfo_id == DESKTOP.stem, f"{metainfo_id} vs {DESKTOP.stem}")
    check("metainfo file is named after the id",
          METAINFO.name == f"{metainfo_id}.metainfo.xml",
          f"{METAINFO.name} vs {metainfo_id}.metainfo.xml")
    check("metainfo metadata licence is reusable",
          text("metadata_license") in ALLOWED_METADATA_LICENSES, text("metadata_license"))
    check("metainfo declares a project licence", bool(text("project_license")))
    for tag in ("name", "summary", "developer_name"):
        check(f"metainfo has {tag}", bool(text(tag)))
    check("metainfo has a description",
          component.find("description") is not None)
    homepage = component.find('url[@type="homepage"]')
    check("metainfo has a homepage url", homepage is not None and bool((homepage.text or "").strip()))
    launchable = component.find('launchable[@type="desktop-id"]')
    check("metainfo launchable is the desktop entry",
          launchable is not None and (launchable.text or "").strip() == DESKTOP.name,
          (launchable.text or "").strip() if launchable is not None else "missing")
    binary = component.find("provides/binary")
    check("metainfo provides the packaged binary",
          binary is not None and (binary.text or "").strip() == (exec_argv[0] if exec_argv else ""),
          (binary.text or "").strip() if binary is not None else "missing")

    # The release version is the one the package will actually be built from.
    version = re.search(r'set\(SNOW_SHOT_VERSION "([^"]+)"\)', read_text(ROOT_CMAKE))
    check("CMakeLists declares SNOW_SHOT_VERSION", version is not None)
    if version:
        released = [
            release.get("version", "")
            for release in component.findall("releases/release")
        ]
        check("metainfo lists the current version",
              version.group(1) in released, f"{version.group(1)} not in {released}")
        # `appstreamcli validate` fails on a release without a date:
        #
        #     E: snow-shot.desktop:37: release-time-missing date
        #     Validation failed: errors: 1, ...
        #
        # and that error used to surface in `Validate the packaged metadata`,
        # half an hour into a build. Neither `desktop-file-validate` nor any XML
        # well-formedness check can see it, because it is a rule about the
        # AppStream schema rather than about the document -- so it is spelled out
        # here, where it costs a second.
        releases = component.findall("releases/release")
        for release in releases:
            version_attr = release.get("version", "?")
            date = (release.get("date") or "").strip()
            check(f"metainfo release {version_attr} has a date",
                  bool(re.fullmatch(r"\d{4}-\d{2}-\d{2}", date)), date or "missing")

    # --- The icon -------------------------------------------------------------
    try:
        icon = ElementTree.parse(ICON).getroot()
        check("icon parses as XML", True)
        check("icon is an svg", icon.tag.endswith("svg"), icon.tag)
        check("icon has a viewBox or size",
              "viewBox" in icon.attrib or ("width" in icon.attrib and "height" in icon.attrib))
        # The icon is installed under a different name than its file name (the
        # same art is the Windows .ico source), so the name to compare is the one
        # the install rule renames it to.
        rename = re.search(r'RENAME\s+"([^"]+\.svg)"', read_text(REPO / "snow_shot" / "CMakeLists.txt"))
        check("install rule renames the icon", rename is not None)
        if rename:
            check("installed icon name matches Icon=",
                  Path(rename.group(1)).stem == entry.get("Icon"),
                  f"{rename.group(1)} vs {entry.get('Icon')}")
    except ElementTree.ParseError as error:
        check("icon parses as XML", False, str(error))

    # The fixed-size icons are a second guess at the same name, and a rename of
    # either one would leave `Icon=` pointing at a file that is not installed.
    # Their contents are checked by scripts/gen-linux-icons.py --check; here the
    # only question is whether the lookup name still resolves.
    png_icons = sorted((REPO / "snow_shot" / "resources" / "icons" / "hicolor").glob("*/apps/*.png"))
    check("hicolor tree ships fixed-size icons", bool(png_icons))
    if png_icons:
        mismatched = [str(p.relative_to(REPO)) for p in png_icons if p.stem != entry.get("Icon")]
        check("hicolor icons are named after Icon=", not mismatched,
              f"Icon={entry.get('Icon')} but {', '.join(mismatched)}")

    if failures:
        print(f"\n{len(failures)} problem(s)")
        return 1
    print("\nfreedesktop integration check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
