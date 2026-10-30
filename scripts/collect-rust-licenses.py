#!/usr/bin/env python3
"""Collect original notices from one built Cargo package's runtime dependency graph."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess


def resolved_packages(cargo, manifest, package, target, features, no_default_features):
    options = ['--manifest-path', str(manifest), '--locked', '--offline']
    if features:
        options.extend(['--features', features])
    if no_default_features:
        options.append('--no-default-features')
    metadata = json.loads(subprocess.run(
        [cargo, 'metadata', *options, '--filter-platform', target, '--format-version', '1'],
        check=True, capture_output=True, text=True, encoding='utf-8',
    ).stdout)
    roots = [entry for entry in metadata['packages'] if entry['name'] == package]
    if len(roots) != 1:
        raise ValueError(f'Expected one Cargo package named {package}')
    tree = subprocess.run(
        [cargo, 'tree', *options, '--package', roots[0]['id'], '--target', target,
         '--edges', 'normal,build', '--prefix', 'none', '--format', '{p}'],
        check=True, capture_output=True, text=True, encoding='utf-8',
    ).stdout
    # Metadata unions workspace features and includes development dependencies.
    # The tree selects exactly the package, features and platform that CMake built.
    labels = set()
    for line in tree.splitlines():
        match = re.match(r'^(\S+ v\S+)(?: |$)', line)
        if not match:
            raise ValueError(f'Unexpected Cargo tree entry: {line}')
        labels.add(match.group(1))
    packages_by_label = {}
    for entry in metadata['packages']:
        label = f"{entry['name']} v{entry['version']}"
        packages_by_label.setdefault(label, []).append(entry)
    selected = []
    for label in sorted(labels):
        matches = packages_by_label.get(label, [])
        if len(matches) != 1:
            raise ValueError(f'Missing or ambiguous Cargo package: {label}')
        if matches[0]['source'] is not None or matches[0]['name'] == 'simd-paddle-ocr-rs':
            selected.append(matches[0])
    return selected


def license_files(package, fallback):
    directory = Path(package['manifest_path']).parent
    notices = sorted(path for path in directory.iterdir()
                     if path.is_file() and re.fullmatch(
                         r'(LICENSE|LICENCE|COPYING|NOTICE|UNLICENSE)(?:[._-].*)?',
                         path.name, re.IGNORECASE))
    if package['name'] == 'simd-paddle-ocr-rs':
        notices.extend(sorted((directory / 'licenses').glob('*.txt')))
    if not notices:
        upstream = fallback / 'cargo' / f"{package['name']}-{package['version']}"
        if upstream.is_dir():
            notices = sorted(path for path in upstream.iterdir() if path.is_file())
    if not notices:
        declared = package.get('license') or ''
        for identifier in ('Apache-2.0', 'WTFPL'):
            if re.search(r'(?:^|\s|\()' + re.escape(identifier) + r'(?:$|\s|\))', declared):
                canonical = fallback / f'{identifier}.txt'
                if canonical.is_file():
                    notices = [canonical]
                    break
    if not notices:
        raise ValueError(
            f"Resolved Rust package has no distributable license: "
            f"{package['name']} {package['version']} [{package.get('license')}]")
    return notices


def collect_licenses(packages, destination, fallback, package):
    # Resolve every notice before writing, so missing licenses stop packaging.
    plan = [(entry, license_files(entry, fallback)) for entry in packages]
    lines = [f'# Rust notices for {package}', '',
             'Resolved normal and build dependencies; development dependencies are excluded.', '',
             '| Package | Declared license | Notice |', '| --- | --- | --- |']
    for entry, notices in plan:
        label = f"{entry['name']}-{entry['version']}"
        directory = destination / label
        directory.mkdir(parents=True, exist_ok=True)
        for notice in notices:
            target = directory / notice.name
            if target.exists() and target.read_bytes() != notice.read_bytes():
                raise ValueError(f'Conflicting original Rust notices: {target}')
            if not target.exists():
                shutil.copyfile(notice, target)
            declared = (entry.get('license') or 'Not declared').replace('|', r'\|')
            lines.append(f'| {label} | {declared} | [{notice.name}]({label}/{notice.name}) |')
    destination.mkdir(parents=True, exist_ok=True)
    (destination / f'{package}.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cargo', default='cargo')
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--package', required=True)
    parser.add_argument('--target', required=True)
    parser.add_argument('--features', default='')
    parser.add_argument('--no-default-features', action='store_true')
    parser.add_argument('--destination', required=True, type=Path)
    parser.add_argument('--fallback-directory', required=True, type=Path)
    args = parser.parse_args()
    try:
        packages = resolved_packages(args.cargo, args.manifest, args.package, args.target,
                                     args.features, args.no_default_features)
        collect_licenses(packages, args.destination, args.fallback_directory, args.package)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            parser.exit(1, f'Cargo license collection failed: {error.stderr}\n')
        parser.exit(1, f'Cargo license collection failed: {error}\n')


if __name__ == '__main__':
    main()
