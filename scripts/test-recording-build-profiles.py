"""Focused Cargo feature-graph contracts for application and compatibility recording."""

import json
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
CARGO = shutil.which('cargo')


@unittest.skipUnless(CARGO, 'Cargo is required')
class RecordingBuildProfiles(unittest.TestCase):
    def cargo(self, *arguments):
        result = subprocess.run([CARGO, *arguments], cwd=ROOT, capture_output=True,
                                text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_application_excludes_capture_compatibility_and_retains_recovery(self):
        metadata = json.loads(self.cargo(
            'metadata', '--manifest-path', 'snow_rust_ffi/Cargo.toml', '--locked',
            '--offline', '--format-version', '1'))
        packages = {package['id']: package['name'] for package in metadata['packages']}
        nodes = {packages[node['id']]: node for node in metadata['resolve']['nodes']}
        for package in ('snow-recording-c', 'snow-screen-recorder', 'snow-recording-runtime'):
            self.assertNotIn('legacy-capture', nodes[package]['features'])
        self.assertNotIn('snow-pipeline', nodes)
        self.assertIn('snow-recording-export', nodes)
        self.assertIn('snow-recording-model', nodes)
        # The Windows direct GPU startup probe reads encoded packets through FFmpeg.
        runtime_dependencies = {packages[dependency['pkg']]
                                for dependency in nodes['snow-recording-runtime']['deps']}
        self.assertIn('ffmpeg-next', runtime_dependencies)

    def compatibility_packages(self, *features):
        output = self.cargo(
            'tree', '--manifest-path', 'snow-crates/Cargo.toml', '--locked', '--offline',
            '-p', 'snow-recording-c', '--edges', 'normal,build', '--prefix', 'none',
            '--format', '{p}', *features)
        return {line.split()[0] for line in output.splitlines()}

    def test_standalone_defaults_retain_legacy_capture(self):
        self.assertIn('snow-pipeline', self.compatibility_packages())

    def test_standalone_can_select_the_application_capture_surface(self):
        packages = self.compatibility_packages('--no-default-features')
        self.assertNotIn('snow-pipeline', packages)
        self.assertIn('snow-recording-export', packages)
        self.assertIn('snow-recording-model', packages)


if __name__ == '__main__':
    unittest.main()
