# Snow Apps

Snow Apps repository, providing source code for Snow Shot and Snow Image Viewer.

For native Windows ARM64 builds and packages, see
[Windows ARM64 build support](docs-windows-arm64-build.md), including cross compilation,
OCR preparation, and release validation prerequisites.

<div style="font-size: 128px">🏗️🚧🦺</div>

## Install Snow Shot on Windows

Download the installer from [Snow Shot releases](https://github.com/mg-chao/snow-apps/releases).
Install the offline Windows x64 package from the WinGet community repository with:

```powershell
winget install --exact --id mg-chao.snow-shot --source winget
```

This package includes published beta versions and the default OCR resources. See
[WinGet release support](docs/snow-shot-releases.md#winget) for upgrades, removal,
and maintainer setup.

Once [Snow Shot's submission](https://github.com/ScoopInstaller/Extras/issues/18870)
is accepted into Scoop's official
[Extras bucket](https://github.com/ScoopInstaller/Extras), install the portable
Windows x64 package with [Scoop](https://scoop.sh):

```powershell
scoop bucket add extras
scoop install extras/snowshot
```

The Scoop package follows stable releases and includes the default OCR resources.
Quit Snow Shot before running `scoop update snowshot`; use Scoop rather than the
app's built-in updater. Settings and history persist across upgrades and ordinary
uninstall. See [Scoop release support](docs/snow-shot-releases.md#scoop) for removal,
data locations, and maintainer setup.

## Install Snow Shot on macOS

Apple Silicon Macs running macOS 15 or later can install the stable release with:

```sh
brew update
brew install --cask mg-chao/tap/snow-shot
brew upgrade --cask snow-shot
brew uninstall --cask snow-shot
```

Installation reuses Snow Shot's local signing identity and may request Keychain
access and macOS privacy permissions. See [Homebrew installation and migration](docs-macos-build.md#homebrew-installation)
for existing installations, custom app directories, and recovery.

## Maintain the OCR submodule

[`snow-crates/crates/simd-paddle-ocr-rs/`](snow-crates/crates/simd-paddle-ocr-rs/)
is a Git submodule with its own
[repository](https://github.com/mg-chao/simd-paddle-ocr-rs), history, branches,
and releases. Snow Apps records the exact OCR commit to check out.
The submodule is excluded from the Snow Crates Cargo workspace so it retains
its own pinned nightly toolchain and lockfile. Run its Cargo commands from
the submodule directory.

After cloning Snow Apps, initialize the submodule from the repository root:

```sh
git submodule update --init --recursive
```

For a new clone, `git clone --recurse-submodules` initializes it automatically.
Submodule updates normally check out a detached HEAD. Before editing OCR code,
switch to `main` or a feature branch inside the submodule:

```sh
git -C snow-crates/crates/simd-paddle-ocr-rs switch main
```

Follow the submodule's README and AGENTS.md for its build and validation commands.
Commit and push OCR changes to its own repository first, then record the new
revision in Snow Apps:

```sh
git add snow-crates/crates/simd-paddle-ocr-rs
git commit -m "build(ocr): update simd paddle ocr submodule"
```

## Open Source Licenses

This is a multi-license repository. See [LICENSE.md](LICENSE.md) for the
repository-level scope rules and third-party material policy.

| Project | License |
| --- | --- |
| `ant_design_qt/` | [Apache License 2.0](ant_design_qt/LICENSE) |
| `snow-crates/` | [GNU GPL v3.0 or later](snow-crates/COPYRIGHT) |
| `snow_draw_engine_qt/` | [Apache License 2.0](snow_draw_engine_qt/LICENSE) |
| `snow_rust_ffi/` | [GNU GPL v3.0 or later](snow_rust_ffi/COPYRIGHT) |
| `snow-crates/crates/simd-paddle-ocr-rs/` | [Apache License 2.0](snow-crates/crates/simd-paddle-ocr-rs/LICENSE) |
| `snow_image/` | [GNU GPL v3.0 or later](snow_image/COPYRIGHT) |
| `snow_image_viewer/` | [GNU GPL v3.0 or later](snow_image_viewer/COPYRIGHT) |
| `snow_shot/` | [GNU GPL v3.0 or later](snow_shot/COPYRIGHT) |

Synchronized and bundled third-party materials retain their upstream licenses.
See [Ant Design Qt third-party notices](ant_design_qt/THIRD_PARTY_NOTICES.md)
and [Snow Shot third-party notices](snow_shot/THIRD_PARTY_NOTICES.md).

## Contributing / 贡献指南

Only fix PRs will be reviewed. Feature PRs are not accepted directly; please start
a community discussion first if you need a specific feature.
See the [contribution guidelines](CONTRIBUTING.md) for details.

仅审查修复问题的 PR，不直接接受新增功能或功能增强的 PR。如有功能需求，请先在社区发起讨论。
详情请阅读[贡献指南](CONTRIBUTING.md)。

## Community

[![LINUX DO](https://img.shields.io/badge/LINUX%20DO-Community-blue)](https://linux.do)
