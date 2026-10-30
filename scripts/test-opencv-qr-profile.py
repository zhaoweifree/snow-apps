#!/usr/bin/env python3
"""Compile the patched DNN registry offline; no OpenCV build or models are downloaded.

Run with --source-tree PATH (or SNOW_OPENCV_SOURCE_TREE) pointing to the pinned
OpenCV source, either pristine or with the Snow Shot patch already applied.
--models-directory defaults to artifacts/qr-models-wechat when those files exist.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'cmake/vcpkg-overlay-ports/opencv4'
PATCH = PORT / '0025-dnn-snow-shot-qr-layers.patch'
PROFILE_LAYERS = {
    'Identity', 'BatchNorm', 'Concat', 'Convolution', 'Crop', 'Deconvolution',
    'DetectionOutput', 'Eltwise', 'Flatten', 'Permute', 'Pooling', 'PriorBox',
    'ReLU', 'Reshape', 'Scale', 'Softmax',
}
OPTIONS = None


def model_layer_types(text):
    # Only direct fields of a top-level layer count. Nested weight fillers also
    # have a "type" field and must not be mistaken for registered DNN layers.
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|#[^\n]*|//[^\n]*|[A-Za-z_]\w*|[{}:]', text)
    tokens = [token for token in tokens if not token.startswith(('#', '//'))]
    stack = []
    types = set()
    for index, token in enumerate(tokens):
        if token == '{':
            stack.append(tokens[index - 1])
        elif token == '}':
            stack.pop()
        elif token == 'type' and stack == ['layer']:
            if tokens[index + 1] != ':':
                raise ValueError('Caffe layer type is missing its field separator')
            types.add(json.loads(tokens[index + 2]))
    if stack or not types:
        raise ValueError('Caffe definition has no complete layer graph')
    return types


class OpenCVQrProfile(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not OPTIONS.source_tree:
            raise unittest.SkipTest('Set --source-tree or SNOW_OPENCV_SOURCE_TREE for the offline fixture')
        source = Path(OPTIONS.source_tree).resolve()
        version_header = source / 'modules/core/include/opencv2/core/version.hpp'
        version = version_header.read_text(encoding='utf-8')
        actual = '.'.join(re.search(rf'#define\s+CV_VERSION_{field}\s+(\d+)', version).group(1)
                          for field in ('MAJOR', 'MINOR', 'REVISION'))
        expected = json.loads((PORT / 'vcpkg.json').read_text(encoding='utf-8'))['version']
        if actual != expected:
            raise ValueError(f'Expected pinned OpenCV {expected}, found {actual}')

        compiler = OPTIONS.cxx or shutil.which('c++') or shutil.which('clang++') or shutil.which('cl')
        if not compiler:
            raise unittest.SkipTest('The registry fixture requires a native C++ compiler')
        cls.compiler = compiler
        temporary = tempfile.TemporaryDirectory(prefix='snow OpenCV registry ')
        cls.addClassCleanup(temporary.cleanup)
        cls.directory = Path(temporary.name)
        tree = cls.directory / 'tree'
        for relative in ('modules/dnn/CMakeLists.txt', 'modules/dnn/src/init.cpp'):
            destination = tree / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / relative, destination)

        def apply(*flags):
            return subprocess.run(['git', 'apply', *flags, str(PATCH)], cwd=tree,
                                  text=True, capture_output=True, timeout=30)

        if apply('--check').returncode:
            reverse = apply('--reverse', '--check')
            if reverse.returncode:
                raise ValueError('The QR patch does not apply to this source tree:\n' + reverse.stderr)
            reverse = apply('--reverse')
            if reverse.returncode:
                raise ValueError(reverse.stderr)
        baseline = (tree / 'modules/dnn/src/init.cpp').read_text(encoding='utf-8')
        result = apply()
        if result.returncode:
            raise ValueError(result.stderr)
        patched = (tree / 'modules/dnn/src/init.cpp').read_text(encoding='utf-8')
        cls.baseline_registry = cls.compile_registry('baseline', baseline)
        cls.default_registry = cls.compile_registry('default', patched)
        cls.minimal_registry = cls.compile_registry('minimal', patched, minimal=True)

    @classmethod
    def compile_registry(cls, name, source, minimal=False):
        directory = cls.directory / name
        include = directory / 'opencv2/dnn'
        include.mkdir(parents=True)
        (include / 'layer.details.hpp').touch()
        (directory / 'precomp.hpp').write_text('''
#include <cstddef>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#define CV__DNN_INLINE_NS_BEGIN
#define CV__DNN_INLINE_NS_END
#define CV_TRACE_FUNCTION() do {} while (false)
#define CV_DNN_REGISTER_LAYER_CLASS(name, implementation) record(#name, #implementation);
namespace cv {
struct Mutex {};
namespace dnn {
std::vector<std::pair<std::string, std::string>> registrations;
void record(const char* name, const char* implementation) {
    registrations.emplace_back(name, implementation);
}
}}
''', encoding='utf-8')
        (directory / 'registry.cpp').write_text(source + '''
int main() {
    cv::dnn::initializeLayerFactory();
    for (const auto& entry : cv::dnn::registrations)
        std::cout << entry.first << '\\t' << entry.second << '\\n';
}
''', encoding='utf-8')
        executable = directory / ('registry.exe' if os.name == 'nt' else 'registry')
        msvc = Path(cls.compiler).name.lower() in ('cl', 'cl.exe')
        arguments = ([cls.compiler, '/nologo', '/std:c++17', '/EHsc', '/I' + str(directory),
                      '/Fe:' + str(executable)] if msvc else
                     [cls.compiler, '-std=c++17', '-I', str(directory), '-o', str(executable)])
        if minimal:
            arguments.append(('/D' if msvc else '-D') + 'OPENCV_DNN_SNOW_SHOT_QR=1')
        arguments.append(str(directory / 'registry.cpp'))
        result = subprocess.run(arguments, cwd=directory, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise ValueError('Registry fixture compilation failed:\n' + result.stdout + result.stderr)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30,
                                check=True)
        return [tuple(line.split('\t')) for line in result.stdout.splitlines()]

    def test_unconfigured_registry_preserves_upstream_behavior(self):
        self.assertEqual(self.default_registry, self.baseline_registry)
        self.assertTrue({'LSTM', 'GRU', 'Attention', 'Scatter', 'ScatterND', 'Reduce'}
                        <= dict(self.default_registry).keys())

    def test_profile_registers_only_the_required_upstream_constructors(self):
        baseline = dict(self.baseline_registry)
        self.assertEqual(len(self.minimal_registry), len(PROFILE_LAYERS))
        self.assertEqual(dict(self.minimal_registry),
                         {layer: baseline[layer] for layer in PROFILE_LAYERS})

    def test_profile_covers_the_pinned_caffe_graphs_without_extra_layers(self):
        directory = Path(OPTIONS.models_directory)
        if not all((directory / name).is_file() for name in ('detect.prototxt', 'sr.prototxt')):
            self.skipTest('The pinned prototxt files are absent; no network fetch is attempted')
        pins = dict(re.findall(r'"([^"|]+\.prototxt)\|([0-9a-f]{128})"',
                               (ROOT / 'cmake/FetchSnowShotQrModels.cmake').read_text(encoding='utf-8')))
        layers = set()
        for name in ('detect.prototxt', 'sr.prototxt'):
            data = (directory / name).read_bytes()
            self.assertEqual(hashlib.sha512(data).hexdigest(), pins[name], name)
            layers.update(model_layer_types(data.decode('utf-8')))
        self.assertIn('Input', layers)
        # Input is importer-owned; Net::registerOutput additionally requires Identity.
        self.assertEqual((layers - {'Input'}) | {'Identity'}, dict(self.minimal_registry).keys())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--source-tree', default=os.environ.get('SNOW_OPENCV_SOURCE_TREE'))
    parser.add_argument('--models-directory', default=ROOT / 'artifacts/qr-models-wechat')
    parser.add_argument('--cxx', default=os.environ.get('CXX'))
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
