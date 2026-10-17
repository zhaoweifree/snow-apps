# Snow Apps 移植统信 UOS / Linux 完整方案

> 目标平台：统信 UOS（Deepin 系，Debian 衍生），**arm64 + amd64 双架构**
> 硬约束：**必须兼容 Qt 5 及以下**
> **决策已定（见 §0.4）**：维持 Qt5 路线，但改造形态改为「**一套源码，Qt 5.15 / Qt 6.12 双版本皆可编译**」，
> 并**保留 Windows 分支**用于本地验证；具体执行见 **[`docs-qt5-downgrade-plan.md`](docs-qt5-downgrade-plan.md)**
> 基线版本：`SNOW_SHOT_VERSION = 1.2.8`（根 `CMakeLists.txt:18`）
> 文档定位：与 `docs-macos-build.md`、`docs-windows-arm64-build.md` 同级的移植设计文档

---

## 0. 结论摘要与可行性判定

### 0.1 一句话结论

工程具备**清晰的三平台抽象骨架**（Windows / macOS 两条腿已实现），但 **Linux 支线几乎为零**；
真正的移植难点不在 Linux，而在于**硬性 Qt6 → Qt5 降级**——现有代码不是"停留在 Qt5 老 API"，
而是**全面拥抱 Qt 6 专有 API**，且 `snow_image_viewer` 直接依赖 Qt 6 才公开的 RHI 渲染抽象。

### 0.2 可行性分级

| 模块 | Linux 可移植性 | Qt5 可降级性 | 综合判定 | 主要阻塞 |
|---|---|---|---|---|
| `snow_draw_engine_qt`（Rust 部分） | 高（纯 Rust，无平台 crate） | — | **可直接复用** | 仅需 aarch64 性能补强 |
| `snow_image` | 高 | 中 | **小改** | `_M_X64` 内联汇编/SSE → NEON |
| `snow_memory` | 高（`mmap` 已实现） | — | **可直接复用** | `pages.rs:123-137` 已就绪 |
| `ant_design_qt` | 中 | 中高 | **中改** | 4 个 `.mm`、`enterEvent`、`position()` |
| `snow_shot`（主程序） | 低（平台层全缺） | 中 | **大改** | 需从零实现全部 Linux 平台后端 |
| `snow_image_viewer` | 低 | **极低** | **重写或裁剪** | `QRhi*` 为 Qt6 专有公开 API |
| Rust `snow-capture` / `snow-cursor` | 低 | — | **已完成 X11 部分** | 采集与光标走 X11（见 9.4.3）；PipeWire / portal 仍缺 |
| Rust 输入钩子 / 键帽渲染 | 低 | — | **已完成 X11 部分** | XInput2 观测（见 9.4.4）+ `ab_glyph` 光栅化（见 9.4.5）；Wayland / 无 XInput2 退路仍缺 |
| Rust 音频 / 元素拾取 | 低（全为 `UnsupportedBackend`） | — | **从零实现** | PulseAudio / PipeWire、AT-SPI2 全缺 |
| Rust OCR（`snow-ocr-process`） | 中 | — | **中改** | 默认 `directml-provider` 为 Windows 专有 |

### 0.3 必须向决策方澄清的前置问题

1. **Qt5 的下限版本**：建议锁定 **Qt 5.15 LTS**。UOS 20 出厂为 Qt 5.11，`deepin-shared-libs` 另提供
   Qt 5.15.10 GLES 供 UOS v20 amd64/aarch64 使用；Deepin 23 基线为 Qt 5.15 + 6.x。
   若被迫降至 5.12/5.11，`QStringDecoder` / `QLatin1StringView` / `QByteArrayView` / `qsizetype` /
   `QColorSpace` / `QImageReader::allocationLimit` 等将再增一大层替换工作。
2. **`snow_image_viewer` 是否在移植范围内**：它是唯一使用 `QRhi` 的模块，Qt5 下无公开等价 API。
   建议 **Linux 首版不交付查看器**，或将其渲染后端重写为 `QOpenGLWidget`。
3. ~~**Qt5 约束的来源**~~ → **已定，见 §0.4。**

### 0.4 决策记录（已确认）

**决策 1：维持 Qt5 路线。** 不做 AppImage + Qt6 的绕行（该路线作为备选保留在附录 A，已否决）。

**决策 2：保留 Windows 分支，先本机验证再上 UOS。** 即先在 Windows 用 Qt5 打通编译与冒烟测试，
确认降级本身没引入回归，再进 UOS 做平台后端适配。

**决策 3（由决策 2 推导出的改造形态）：不删除 Qt6 支持，改为双版本兼容。**
依据是 Qt 官方的 versionless 机制——`Qt::Core` 这类无版本前缀的目标**在 Qt 5.15 与 Qt 6 中同时存在**，
因此 `Qt6::X` → `Qt::X` 是一次双向生效的官方重命名，而非单向降级。
这带来两个直接收益：现有 Qt6 构建全程保持可用（可作二分对照基线，快速区分"降级引入的 bug"与"原有 bug"），
且回滚成本极低。

**降级工作量实测**（当前 `main` 快照 grep 统计）：需改造 **112 个 C++ 文件** + **882 行 CMake**，
占非测试 C++ 文件总量的 8.6%。完整清单、兼容层代码、阶段计划与 Windows 环境前置
全部落在 **[`docs-qt5-downgrade-plan.md`](docs-qt5-downgrade-plan.md)**。

> ⚠️ **新增前置阻塞**：经实测，本机**未安装 Qt / CMake / MSVC / Ninja / Rust / vcpkg，且仓库从未在本机构建过**。
> 因此 Windows 验证前必须先完成环境 bootstrap（约 10–12 GB 下载、数小时），这是不可绕过的门槛。

---

## 1. 工程整体架构

### 1.1 顶层构成

```
snow-apps/                             (cmake_minimum_required 4.2, project snow_apps 0.2.0)
├── CMakeLists.txt                     顶层编排：模块开关、IPO/LTO、CPack(NSIS)
├── CMakePresets.json                  16 个 configure preset：windows-msvc-* / macos-*
├── vcpkg.json                         清单依赖：snow-shot / full-codecs / image-viewer features
├── vcpkg-configuration.json           builtin registry, baseline 4497409a
│
├── snow_shot/           (GPL-3.0)     主程序：截图/贴图/OCR/录屏；1084 个 C++ 文件
├── ant_design_qt/       (Apache-2.0)  Qt Widgets 组件库；369 个 C++ 文件 + 4 个 .mm
├── snow_draw_engine_qt/ (Apache-2.0)  绘图引擎：C++ 薄壳 + 9 个 Rust crate
├── snow_image/          (GPL-3.0)     图像编解码库（PNG/JPEG/GIF/HEIF/JXL/WEBP/AVIF...）
├── snow_image_viewer/   (GPL-3.0)     看图器：QRhi 渲染（Qt6 专有）
├── snow_memory/                       显式 OS 页所有权的大缓冲
├── snow-crates/         (Apache-2.0)  Rust workspace，29 个 crate
├── snow_rust_ffi/       (Apache-2.0)  汇总 C ABI staticlib，供 CMake 链接
└── cmake/ scripts/                    共享 CMake 模块、vcpkg overlay、PowerShell/sh 入口
```

### 1.2 依赖分层（自下而上）

| 层 | 组件 | 平台耦合 |
|---|---|---|
| L0 内存/基础 | `snow-memory`、`snow-core`、`snow-media`、`snow-diagnostics` | 低（Linux `mmap` 已有） |
| L1 图像 | `snow_image`、`snow-stitch-images`、`visual-region-detector`、`snow_draw_engine_qt`(Rust) | 低-中（SIMD 为 x86） |
| L2 平台后端 | `snow-capture`、`snow-cursor`、`snow-audio-recorder`、`snow-ui-selector`、`snow-selected-text`、`snow-recording-effects` | **极高**（Win/macOS only） |
| L3 录制/导出 | `snow-recording-runtime`、`snow-recording-export`、`snow-screen-recorder`、`snow-pipeline` | 高（GPU 路径 Windows only） |
| L4 OCR | `rapid-ocr-rs`、`snow-ocr-process`、onnxruntime | 中（DirectML Windows only） |
| L5 C ABI | `snow-*-c` × 8 → `snow-shot-rust-ffi`(staticlib) | — |
| L6 C++ UI | `ant_design_qt` → `snow_shot` | 高（平台层、.mm、Win32） |

### 1.3 构建与打包链路

- **Qt 供给**：**不使用 aqtinstall / vcpkg**。由 `scripts/qt-toolchain.json` 单一真源锁定 `6.12.0`，
  `cmake/SnowQt.cmake:29` 执行 `find_package(Qt6 <ver> EXACT REQUIRED)`，版本不符即 `FATAL_ERROR`；
  `SnowQt.cmake:39` 注入 `QT_DISABLE_DEPRECATED_UP_TO=0x060c00`（Qt 6.5+ 专有宏）。
  静态 Qt 由 `scripts/build-static-qt.sh:143-160` 从官方 tarball 自建，子模块仅
  `qtbase,qtsvg,qttools,qttranslations`，并 `-skip qtdeclarative`。
- **原生依赖**：vcpkg manifest 模式，triplet 仅 `{x64,arm64}-{windows,osx}-*`（**无 Linux triplet**）。
- **Rust 集成**：`cmake/RustStaticLibrary.cmake` 生成 `cargo build --locked --package ... --target ...` 自定义目标，
  产物以 `STATIC IMPORTED` 挂到 CMake；`snow_rust_ffi` 单库聚合全部 C ABI。
- **打包**：Windows `NSIS`（根 `CMakeLists.txt:426-485`），macOS `DragNDrop`
  （`cmake/SnowShotMacOSPackage.cmake`）。**无 Linux/deb/AppImage 配置。**

---

## 2. 跨平台兼容性问题清单

### 2.1 Linux 现状：几乎为零

全仓库源码级 `Q_OS_LINUX` 仅 **2 处**（`snow_image_viewer/src/ui/viewer_window.cpp:94,126`）。
X11 / Wayland / PipeWire / portal / D-Bus / PulseAudio / ALSA / EGL 在应用代码中 **0 处**。
缺平台实现时当前行为是「静默降级为桩」：

```cpp
// snow_shot/src/platform/globalmousebackend.cpp:35-37
#else
    return std::make_unique<UnsupportedGlobalMouseBackend>();   // 权限状态 Unavailable
#endif
```

```rust
// snow-crates/crates/snow-capture/src/platform/mod.rs:17-22
#[cfg(not(any(target_os = "windows", target_os = "macos")))]
fn unsupported_error() -> CaptureError {
    CaptureError::platform(anyhow::anyhow!("screen capture is only supported on Windows"))
}
```

> 风险：若不改造，Linux 版会**编译通过但核心功能全部失效**（截不到屏、录不了音、抓不到光标）。
> 这与用户对"误导性误报"的敏感点直接冲突（不允许出现"工具已安装但无法获取屏幕"这类误导提示），
> 因此在接口层必须显式上报 `Unsupported`，而非伪造成功。

### 2.2 Windows 专属代码（必须替换）

| 类别 | 代表位置 | Linux 对应 |
|---|---|---|
| DXGI Desktop Duplication / WGC | `snow-capture/src/platform/windows/{duplication.rs,wgc.rs,duplication_gpu.rs}` | PipeWire / X11 XShm |
| D3D11 / GPU tonemap | `snow-crates/crates/snow-d3d11/*`；`snow-capture/.../gpu_tonemap.rs`（≈180 处） | Vulkan/EGL 或纯 CPU |
| GDI 回退 | `snow-capture/src/platform/windows/gdi.rs` | X11 `GetImage` —— **已落地**：`snow-linux/src/x11.rs` + `snow-capture/src/platform/linux.rs`（见 9.4.3） |
| WASAPI 音频 | `snow-audio-recorder/src/platform/windows/*` | PulseAudio / PipeWire |
| UIA/MSAA 元素拾取 | `snow-ui-selector/src/windows/*`、`snow-selected-text/src/platform/windows/*` | AT-SPI2 (D-Bus) |
| 全局键鼠钩子 | `snow-recording-effects/src/{keyboard_hook.rs:124, mouse_hook.rs:90}` | **XInput2 raw events —— 已落地**：`snow-linux/src/input/`（见 9.4.4）。待补：无 XInput2 时的 XRecord / evdev 退路、Wayland portal |
| DirectWrite + Direct2D 键帽光栅化 | `snow-recording-effects/src/keyboard_rasterizer.rs:8`（macOS 走 CoreText + Quartz） | **`ab_glyph` + `ttf-parser` —— 已落地**：`snow-linux/src/text/`（见 9.4.5）。字体发现自建，**刻意不链 fontconfig** |
| Win32 窗口层 | `snow_shot/src/platform/windows/*`（30 文件）：`windowchrome.cpp:255` `SetWindowDisplayAffinity`、`pinnedwindownative.cpp` `WS_EX_*` | X11 属性 / 无对应 |
| 注册表自启动 | `platform/windows/autostartregistration.cpp:14` | `~/.config/autostart/*.desktop` |
| ShellExecute 提权 | `platform/windows/administratorlaunch.cpp:25-31` | `pkexec` / polkit |
| 资源管理器右键菜单 | `platform/windows/shellcontextmenuregistration.cpp:20-21` | `~/.local/share/applications/*.desktop` |
| 命名管道 | `app/main.cpp:379`、`app/mcp/screenshotmcpserver.cpp:31` | QLocalServer abstract socket |
| crashpad | `diagnostics/crashcollector.cpp`、`app/main.cpp:440-453` | 移除 / Breakpad-Linux |
| DirectML | `rapid-ocr-rs/src/runtime/{provider.rs,session.rs}` | CPU EP / OpenVINO / CUDA(amd64) |
| WIC/D3D11 打印 | `platform/windows/nativeprintdocument.cpp`、`snow_image_viewer/src/platform/windows_print.cpp` | CUPS / QPrinter |
| `winres` 无条件 build-dep | `snow-ocr-process/build.rs:35-36` | 需 target-gate |

### 2.3 macOS 专属代码（移植时可整目录排除）

- 19 个 `.mm`：`snow_shot/src/platform/macos/*`（`NSWindow`/`CGEvent`/`NSPasteboard`/`AVFoundation`...）；
- 4 个 `.mm`：`ant_design_qt/.../widgets/{context_menu_mac,modal_mac,detail/*_mac}.mm`；
- 整 crate `snow-crates/crates/snow-macos/`（22 文件，依赖 16 个 `objc2-*`）；
- `snow-capture/src/platform/macos.rs`、`snow-audio-recorder/src/platform/macos.rs` 等。

> 这些在 Linux 构建中可以 `if(APPLE)` 排除，但 **`snow_shot/CMakeLists.txt:1-8` 目前在 `APPLE` 下
> 全局启用 `LANGUAGE OBJCXX`**，需确保 Linux 分支不触发。

### 2.4 硬件 / 架构差异

| 项 | 现状 | arm64 影响 |
|---|---|---|
| SIMD 内核 | 全部 AVX2/SSE（`snow-capture/src/convert/simd_x86.rs` ≈600 处；`snow-stitch-images/{orb,region,estimator}.rs`；`rapid-ocr-rs/src/det/{pre,post}process.rs`；`snow-recording-export/src/{resize,editing}.rs`；C++ 侧 `snow_canvas_filter_avx2.cpp` ≈188 处 `_mm256_*`、`snow_image/src/raster.cpp:135,168`） | **均有标量回退可编译，但性能显著下降**，需补 NEON |
| 已有 NEON 范式 | `snow-media/src/convert.rs:46-76` | 可作为移植模板 |
| CMake SIMD | `cmake/SnowSimd.cmake:7-23`（MSVC `/arch:AVX2`、GCC/Clang `-mavx2`） | 需加 aarch64 分支（`-march=armv8-a+simd`） |
| 内存大页 | `snow-memory/src/pages.rs:30-44` `VirtualAlloc`+`MEM_LARGE_PAGES` | Linux 需 `mmap(MAP_HUGETLB)`，现有分支未用大页 |
| 指针宽度 | 已强制 64 位单架构（根 `CMakeLists.txt:55-86`） | — |

### 2.5 Qt 6 专有特性（**本方案的最大阻塞**）

> 全仓库**没有一处 `#if QT_VERSION_CHECK` 兼容分支**，即无法"一套代码双版本编译"，必须新增兼容层或分叉。

| 类别 | 代表 API / 文件 | Qt5 替代 |
|---|---|---|
| **RHI 渲染** | `snow_image_viewer/src/render/rhi_image_window.{h:23-48,222,cpp}`（`QRhi*`/`QVulkanInstance`/`QOffscreenSurface`） | **Qt5 无公开 RHI** → `QOpenGLWidget` 重写 |
| 键序列 | `QKeyCombination`：`shortcutbinding.h:4`、`shortcutbinding.cpp:375,432,623,650`、`windowshortcutmanager.cpp:46-47,508-596` 等 15+ 文件 | Qt5 `QKeySequence` 用 `int` |
| 原生接口 | `QNativeInterface::QWindowsScreen/QCocoaScreen`：`monitorgeometry.cpp:18`、`focusedfullscreenwindow.cpp:61`、`screenshotgeometry.cpp:373,873` | Qt5 `QPlatformNativeInterface` |
| 进入事件 | `enterEvent(QEnterEvent*)`：`titlebarwidget.cpp:20,97,154`、`tag.h:13,220` 等 | Qt5 `enterEvent(QEvent*)` |
| 指针事件 | `QMouseEvent::position()/globalPosition()`：`screenshotoverlaywindow.cpp:1100-1220`、`screenshotpinnedwindow.cpp` 等 15+ 文件 | `localPos()/globalPos()` |
| 字符串视图 | `QStringView`（11 文件）、`QLatin1StringView`(`configurationarchive.cpp:132,403-408`)、`QByteArrayView`(`worker_protocol.cpp:61,137`) | `QString`/`QLatin1String`/`QByteArray` |
| 编解码 | `QStringDecoder`（`screenshotclipboardcontent.cpp:38,549-551`） | `QTextCodec` |
| 元类型 | `QMetaType::fromType` / `QVariant::metaType()`：`mcpsettingsadapter_p.h:47-76`、`image.cpp:443`、`select_option_utils.cpp:18,37` | `QVariant::type()` / `QMetaType::Type` |
| 容器 | `QList::removeIf`：`settingsadapters.cpp:112`、`configurationschema.cpp:37-2273` | 手写 `std::remove_if`+`erase` |
| 协程 | `QPromise`（`mcpasyncwork_p.h:8,25`，Qt6 移入 QtCore） | Qt5 在 QtConcurrent 中 |
| 色彩 | `QImage::convertedToColorSpace`、`QColorSpace::Primaries::*`：`snowimageqtcodec.cpp:264-312` | Qt5.14 部分可用，命名不同 |
| 图像限制 | `QImageReader::allocationLimit()/setAllocationLimit()`：`snowimageqtcodec.cpp:598,619` | Qt5 无此静态 API |

---

## 3. Qt5 降级专项方案

### 3.1 Qt 模块可用性对照

| 现用 Qt6 模块 | Qt5 对应 | 结论 |
|---|---|---|
| Core / Gui / Widgets / Network / Svg / Test | `Qt5::*` | 直接可用 |
| Concurrent | `Qt5::Concurrent` | 可用（注意 `QPromise` 位置） |
| LinguistTools | `Qt5::LinguistTools` | 可用，但构建期 API 不同（见 3.3） |
| GuiPrivate | `Qt5::GuiPrivate` | 可用但私有 ABI，需逐点核对 |
| **未使用**：Quick / Qml / Multimedia / DBus / Sql / PrintSupport / OpenGL(Widgets) | — | 无需处理 |

> 有利面：**无 QML / Qt Quick、无 `.pro`、无 `.ui`**，`CMAKE_AUTOMOC/AUTORCC` 已启用。
> 这消除了 Qt5↔Qt6 差异中最大的两块（QML 引擎与 qmake）。

### 3.2 CMake 改写表

| Qt6 专有 | 出现位置 | Qt5 改写 |
|---|---|---|
| `find_package(Qt6 EXACT)` | `cmake/SnowQt.cmake:29` | `find_package(Qt5 5.15 EXACT)` |
| `QT_DISABLE_DEPRECATED_UP_TO` | `cmake/SnowQt.cmake:39` | `QT_DISABLE_DEPRECATED_BEFORE` |
| `Qt6::qtpaths` | `SnowQt.cmake:61` | `Qt5::qmake` / `Qt5Core_VERSION` |
| `qt_standard_project_setup()` | `snow_draw_engine_qt/CMakeLists.txt:36` | 手工设 `CMAKE_AUTOMOC` 等 |
| `qt_add_library()` | `snow_draw_engine_qt/CMakeLists.txt:64` | `add_library()` |
| `qt_add_executable()` | `snow_shot/CMakeLists.txt:1596`、`SnowShotMini.cmake:113` | `add_executable()` |
| `qt_add_resources(PREFIX/BASE/FILES)` | `snow_shot/CMakeLists.txt:1153,1664,1685`、`SnowShotTranslations.cmake:78` | `qt5_add_resources(<qrc>)` |
| `qt_add_translations/lupdate/lrelease` | `ant_design_qt/CMakeLists.txt:153`、`SnowShotTranslations.cmake:96`、`SnowShotTranslationRelease.cmake:18-31` | 直接调 `lupdate`/`lrelease`，手工实现合并 |
| `qt_import_plugins()` | `SnowQtOffscreenTest.cmake:31`、`snow_shot/CMakeLists.txt:1792,2975` | `Q_IMPORT_PLUGIN(...)` 宏 |
| `qt_generate_deploy_app_script()` / `qt_finalize_executable()` | `SnowShotMini.cmake:276,283` | Linux 无对应（用 CPack/手工） |
| 静态插件名 `Qt6::QSvgPlugin` 等 | `snow_shot/CMakeLists.txt:153-172` | Qt5 静态插件需 `Q_IMPORT_PLUGIN` + 手工链接 |

### 3.3 i18n 构建管线

运行时（`QTranslator` + `.qm` 资源）Qt5/6 一致，**可原样保留**。构建期需改写：
`MERGE_QT_TRANSLATIONS` / `QT_TRANSLATION_CATALOGS` / `QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR`
（Qt6 专有）→ 改为脚本显式调用 `lrelease`；Qt 自带翻译文件名由 `qtbase_*.qm` 改为 `qt_*.qm`
（`SnowShotTranslationRelease.cmake:8-14`）。`lrelease -fail-on-unfinished` 策略保留。

### 3.4 RHI 重写（查看器）

`rhi_image_window.{h,cpp}` 使用 `QRhiBuffer/QRhiGraphicsPipeline/QRhiSwapChain/QVulkanInstance`。
Qt5 仅有私有 RHI，**公开 API 无法编译**。三条路径：

| 方案 | 成本 | 说明 |
|---|---|---|
| **A. QOpenGLWidget 重写**（推荐） | 中 | 复用现有 GLSL 着色器（`snow_image_viewer/resources/shaders/*.frag|vert`），改写为 GL 3.3 Core |
| B. QPainter/QImage 软渲染 | 低 | 舍弃 GPU 加速，仅保证功能 |
| C. Linux 首版不交付查看器 | 最低 | 从 preset 关闭 `SNOW_APPS_BUILD_IMAGE_VIEWER` |

### 3.5 兼容层设计（推荐落地形态）

新增 `snow_shot/include/snow_shot/compat/qt5compat.h`（+ 同名 `.cpp` 可选），集中收敛差异：

```cpp
// 设计示意（非最终代码）
#pragma once
#include <QtGlobal>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#  include <QKeyCombination>
#  include <QStringDecoder>
#  define SNOW_HAS_QT6 1
#else
#  define SNOW_HAS_QT6 0
#endif

namespace snow_shot::compat {
#if SNOW_HAS_QT6
inline int keySequenceKey(const QKeySequence& s) { return s[0].toCombined(); }   // 占位
#else
inline int keySequenceKey(const QKeySequence& s) { return s[0]; }
#endif
} // namespace
```

约定：**所有平台差异集中在 `compat/`，业务代码只调用 `compat::` 前缀 API**，避免把
`#if QT_VERSION` 散落到 1000+ 文件。

---

## 4. 需要修改 / 新增的模块清单

### 4.1 C++ 侧

| 动作 | 路径 | 说明 |
|---|---|---|
| **新增** | `snow_shot/src/platform/linux/` | 对应 `windows/` 全部能力：全局快捷键、全局鼠标、物理光标、全屏窗口检测、窗口捕获排除、窗口 chrome、托盘、单实例、自启动、提权、剪贴板、系统通知、滚动注入 |
| **修改** | `snow_shot/src/platform/focusedfullscreenwindow.cpp:39`、`globalmousebackend.cpp:5-38`、`physicalcursor.cpp:13-97` | 增加 `Q_OS_LINUX` 分支 |
| **修改** | `snow_shot/src/presentation/{globalcanvas/globalcanvasplatform.cpp, pinned/pinnedwindowplatform.cpp}` | Win/Mac 分发 + 桩，需 Linux 实现 |
| **重构** | `snow_shot/include/snow_shot/platform/screenshotnative.h:3` | **无条件 include `windows/scrollinput.h`**，须拆出平台中立接口 |
| **修改** | `ant_design_qt/.../widgets/{context_menu,modal}.cpp` + `detail/*` | Win32/`.mm` 分发，需 X11 对应 |
| **重写/裁剪** | `snow_image_viewer/src/render/rhi_image_window.*` | 见 3.4 |
| **新增** | `snow_shot/resources/linux/*.desktop`、图标 | 应用入口、右键菜单 |
| **新增** | `cmake/SnowShotLinuxPackage.cmake` | CPack DEB/AppImage |

### 4.2 Rust 侧

| 动作 | 路径 | 说明 |
|---|---|---|
| **新增** | `snow-crates/crates/snow-linux/` | 替代 `snow-macos` 的平台 crate：X11/Wayland 屏幕、音频、光标、输入、文字。**已建**：`geometry`/`pixels`/`session`/`capture`（纯逻辑，任何平台可单测）+ `input::{keys,tracking}`（纯逻辑：keysym→虚拟键映射、键码表、点击/区域状态机）+ `text::{font,raster}`（纯逻辑：字体发现 + 键帽光栅化，`ab_glyph`/`ttf-parser`）+ `x11`/`display`/`cursor`/`input::x11`（X11，`x11rb` 纯 Rust 实现）。待补：PipeWire/Wayland capture、PulseAudio、evdev 退路 |
| **新增** | `snow-crates/crates/snow-capture/src/platform/linux.rs` | **X11 已落地**（`LinuxBackend` + `X11Capturer`，见 9.4.3）：显示器枚举 / 主屏 / `MonitorLayout` / `inspect_target` + 全屏与区域采集。待补：PipeWire + XDG portal（Wayland）、X11 Damage 脏矩形、XShm 零拷贝 |
| **新增** | `snow-crates/crates/snow-audio-recorder/src/platform/linux.rs` | PulseAudio / PipeWire |
| **新增** | `snow-crates/crates/snow-cursor/src/platform/linux.rs` | XFixes `GetCursorImage` + 核心 `QueryPointer`。**已完成**（含 image 缓存的瞬时失败保护，与 Windows 后端同策略） |
| **新增** | `snow-crates/crates/snow-ui-selector/src/linux/`、`snow-selected-text/src/platform/linux/` | AT-SPI2（D-Bus）+ PRIMARY selection。**注意**：`snow-ui-selector-c` 依赖 `ElementRegionService`，在 Linux provider 落地前该 crate 无法编译（见 9.4.2 B 类） |
| **修改** | `snow-recording-effects/src/{keyboard_hook,mouse_hook}.rs` 的 `#[cfg(not(any(windows,macos)))]` 空实现 | **已落地 X11 实现**（见 9.4.4）：`#[cfg(target_os="linux")] mod platform` 把 `snow_linux::input` 的观测翻译成 `KeyObservation` / `MouseClickObservation` / `MouseMovement` |
| **修改** | `snow-recording-effects/src/keyboard_rasterizer.rs:547` 的 `#[cfg(not(any(windows,macos)))]` 桩 | **已落地**（见 9.4.5）。**注意**：该文件不是纯逻辑 —— 每个平台各自实现 `mod platform`（Windows 是 DirectWrite+D2D，macOS 是 CoreText+Quartz），Linux 分支原先是硬返回 Err ⇒ 按键能采到但画不出来。现已接 `snow_linux::text` |
| **修改** | `snow-recording-runtime/src/{capture_policy.rs:18-90,direct_capture.rs:4-90}` | 放开 Windows-only 门控，接入 CPU 合成路径 |
| **修改** | `rapid-ocr-rs/src/model_store.rs:94-102` | `LOCALAPPDATA` → XDG (`$XDG_DATA_HOME`) |
| **修改** | `snow-ocr-process/build.rs:35-36` | `winres` 改为 `[target.'cfg(windows)'.build-dependencies]` |
| **重写/排除** | `snow_shot/rust/snow-shot-updater`、`snow-shot-mcp` | 纯 Windows crate（注册表/服务/`LoadLibrary`） |
| **修改** | `rust-toolchain.toml` | `targets` 增加 `x86_64-unknown-linux-gnu`、`aarch64-unknown-linux-gnu` |

---

## 5. 依赖库替换 / 调整

### 5.1 vcpkg → UOS 系统包 / 自建

| 依赖 | 现状 | Linux 方案 |
|---|---|---|
| Qt | 源码自建 6.12.0 | **系统 Qt 5.15**（deepin-shared-libs Qt5.15.10 / apt `qtbase5-dev`） |
| FFmpeg | vcpkg overlay port 9.0 | UOS 源 `libavcodec-dev` 等，或自建（Deepin23 自带 6.1.1） |
| OpenCV | vcpkg `opencv4`(contrib/dnn/fs/intrinsics) | `libopencv-dev` |
| onnxruntime | vcpkg overlay + DirectML | 官方预编译 `libonnxruntime.so`（CPU EP）或 OpenVINO |
| libpng/libjpeg-turbo/libwebp/giflib | vcpkg | UOS 源同名 `-dev` |
| libheif / libjxl / openexr / lunasvg | vcpkg | UOS 源或自建（注意 aom/hevc 特性） |
| tinyxml2 / minizip-ng / flatbuffers / abseil | vcpkg | UOS 源 |
| **crashpad** | `platform: windows \| osx` | Linux **移除**，或替换为 Breakpad/Breakpad-Linux |
| **DirectML** | `ort/directml` | **移除**（Windows 专有） |
| **Vulkan** | `vulkan-headers`(image-viewer) | `libvulkan-dev`（若走 RHI→GL 则不需要） |

### 5.2 需新增的 Rust 依赖

| 用途 | crate |
|---|---|
| 屏幕采集（Wayland） | `pipewire-rs`（`libspa`）、`ashpd` 或 `zbus`（xdg-desktop-portal ScreenCast） |
| 屏幕采集（X11） | `x11rb` 或 `xcb`、`xfixes`、`xrecord` |
| 音频 | `libpulse-binding` / `pipewire-rs` |
| 输入钩子 | `evdev`、`input-linux`、或 `x11rb` XRecord |
| 底层系统 | `nix`、`libc` |
| 可访问性 | `atspi` / `zbus`（AT-SPI2 over D-Bus） |
| 硬件编码（amd64） | FFmpeg `h264_vaapi`/`h264_nvenc` 经由 `ffmpeg-next` 绑定 |

> 目前上述依赖**一个都没有**（`Cargo.lock` 中无 `nix`/`wayland`/`x11rb`/`pipewire`/`zbus`）。

---

## 6. 系统调用 / 硬件差异适配对照表

| 能力 | Windows 实现 | macOS 实现 | **Linux 目标实现** |
|---|---|---|---|
| 屏幕采集 | DXGI Duplication / WGC | ScreenCaptureKit | **PipeWire**（Wayland） / **X11 XShm** |
| 窗口采集 | WGC per-window | SCK per-window | PipeWire `restore_token` / X11 Composite |
| 防截屏排除 | `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` | `NSWindowSharingNone` | **无等价**，X11 用 shape/`XComposite` 近似；建议降级说明 |
| 全局快捷键 | `RegisterHotKey` | Carbon/CGEventTap | **X11 `XGrabKey`** / Wayland `GlobalShortcuts` portal |
| 全局鼠标 | `WH_MOUSE_LL` | `CGEventTap` | **XInput2 raw events —— 已落地**（see 9.4.4）；无 XInput2 时诚实上报不可用 |
| 键帽文字渲染 | DirectWrite + Direct2D | CoreText + Quartz | **`ab_glyph` + `ttf-parser` —— 已落地**（见 9.4.5）。字体发现自建枚举（族名 → 文件），**刻意不链 fontconfig / FreeType**，以保住跨目标类型检查 |
| 物理光标位置 | `GetPhysicalCursorPos` | `CGEventGetLocation` | **XQueryPointer** / XFixes |
| 音频采集 | WASAPI | CoreAudio | **PulseAudio** / PipeWire |
| 自启动 | 注册表 Run 键 | SMAppService | **`~/.config/autostart/*.desktop`** |
| 提权 | `ShellExecute(runas)` | Authorization Services | **`pkexec`**（polkit） |
| 剪贴板图像 | DIB / CF_DIBV5 | `NSPasteboard` | **X11 `image/png` 选择** / Wayland `wl_data_device` |
| 托盘 | `Shell_NotifyIcon` | NSStatusItem | **StatusNotifierItem**（D-Bus） |
| 系统通知 | Windows Toast | UNUserNotification | **`org.freedesktop.Notifications`** |
| 单实例 | 命名管道 | 命名端口 | **QLocalServer abstract socket**（Qt 抽象，改名字即可） |
| 内存大页 | `VirtualAlloc(MEM_LARGE_PAGES)` | 无 | **`mmap(MAP_HUGETLB)`** |
| 进程 QoS | `SetProcessInformation` | `pthread_set_qos_class_self_np` | **无等价**（返回 0，`snow-core/src/qos.rs:92-134` 已有分支） |
| 打印 | WIC + D3D11 | NSPrintOperation | **CUPS / `QPrinter`** |
| 崩溃捕获 | crashpad | crashpad | 移除或 Breakpad |
| 滚动注入 | `WS_EX_TRANSPARENT` + wheel | CGEvent | **XTest** / `libei`（Wayland） |

---

## 7. arm64 / amd64 双架构方案

### 7.1 架构支持矩阵

| 架构 | UOS 典型终端 | 构建方式 | Rust target |
|---|---|---|---|
| amd64 (x86_64) | 主流 PC | 原生 | `x86_64-unknown-linux-gnu` |
| arm64 (aarch64) | 飞腾/鲲鹏/麒麟 990 | 原生优先，交叉为备 | `aarch64-unknown-linux-gnu` |

`cmake/RustStaticLibrary.cmake:120-125` **已内置** Linux 架构解析，无需改动即可识别上表。

### 7.2 SIMD 补强优先级

| 优先级 | 位置 | 说明 |
|---|---|---|
| P0 | `snow-capture/src/convert/simd_x86.rs` ↔ 新增 `simd_neon.rs` | 像素格式转换热路径，影响采集帧率 |
| P1 | `snow_image/src/raster.cpp:135,168`、`snow_canvas_filter_avx2.cpp` | 绘制/滤镜 |
| P1 | `snow-stitch-images/src/{orb,region,estimator}.rs` | 滚动拼接（可先容忍标量） |
| P2 | `snow-recording-export/src/{resize,editing}.rs` | 录制缩放 |
| P2 | `rapid-ocr-rs/src/det/{pre,post}process.rs` | OCR 后处理 |

`cmake/SnowSimd.cmake` 需新增 aarch64 分支（Neon 默认开启，`-march=armv8-a+simd`）。

### 7.3 交叉编译工具链

```bash
# arm64 交叉（在 amd64 上构建 arm64 包）
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
# Rust: rustup target add aarch64-unknown-linux-gnu
# .cargo/config.toml:
# [target.aarch64-unknown-linux-gnu]
# linker = "aarch64-linux-gnu-gcc"
```

> 注意：ARM64 交叉构建**必须在真机（飞腾/鲲鹏）做终验**，尤其是 PipeWire/portal/GPU 路径。

---

## 8. 分步骤实施计划

| 阶段 | 目标 | 关键产出 | 验收标准 |
|---|---|---|---|
| **P0 预研基线** | 验证 Qt5.15 能否编译纯逻辑模块 | 在 UOS 上构建 `snow_image` + `snow_draw_engine_qt` + `ant_design_qt`（暂关平台功能） | 三库链接通过；`snow_image` 单元测试在 x86_64+arm64 通过 |
| **P1 Qt5 兼容层** | 建立 `compat/` + CMake 双版本开关 | `snow_shot/include/snow_shot/compat/qt5compat.h`；`SnowQt.cmake` 增加 `SNOW_QT_MAJOR` 分支 | 一个不含平台代码的目标可在 Qt5/Qt6 下分别编译 |
| **P2 构建系统 Linux 化** | preset / 依赖 / 工具链 | `CMakePresets.json` 增 `linux-*`；`cmake/vcpkg-overlay-triplets/{x64,arm64}-linux.cmake`；`rust-toolchain.toml` 补 target | `cmake --preset linux-x64-debug` 配置成功 |
| **P3 Rust 平台后端** *(进行中)* | 补齐 L2/L3 平台能力 | `snow-linux` crate；capture(X11→PipeWire)、cursor、audio、input hooks、text | `cargo test -p snow-capture` 在 UOS 通过；能截到主屏。**已达成（slice 1–4）**：`snow-linux`（X11 会话 / RandR / 光标 / `GetImage` 解码 / XInput2 输入观测 / `ab_glyph` 键帽光栅化）+ `snow-cursor` Linux 后端 + `snow-capture` X11 后端 + `snow-recording-effects` 键鼠观测与键帽渲染，双架构跨目标类型检查全绿、纯逻辑单测在 Windows 上跑。**剩余**：PipeWire/Wayland、音频、AT-SPI2 |
| **P4 C++ 平台层** | 主程序平台层 | `snow_shot/src/platform/linux/*`；3 个 dispatcher 增分支；`screenshotnative.h` 重构 | 截图→标注→贴图闭环在 X11 会话可用 |
| **P5 双架构 + SIMD** | arm64 可用可跑 | NEON 内核；`SnowSimd.cmake` aarch64 分支 | 飞腾真机截屏成功，帧率 ≥ 目标准 |
| **P6 打包与系统集成** | 可交付安装包 | `cmake/SnowShotLinuxPackage.cmake`；`.desktop`；deb + AppImage | 双架构 deb 在 UOS 20/Deepin 23 安装并可启动 |
| **P7 硬化与 CI** | 稳定与回归 | Wayland 会话适配；CI 增 Linux job | 双会话（X11/Wayland）功能对等；关键用例回归通过 |

**建议节奏**：P0→P2 可并行（构建立即受阻于 Qt5 决策，应最先定论）；P3 与 P4 是最大工作量，建议
**先打通 X11 路径**（技术成熟、调试简单），**再补 Wayland/PipeWire**（UOS 20 默认 X11，Deepin 23 起 Wayland 占比上升）。

---

## 9. 编译构建建议

### 9.1 依赖安装（UOS 20 / Deepin 23 通用）

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config git
# Qt5 开发环境
sudo apt install -y qtbase5-dev qt5-qmake qttools5-dev-tools libqt5svg5-dev
# 图形/输入/媒体
sudo apt install -y libgl1-mesa-dev libxcb-xinerama0-dev libxkbcommon-dev libxkbcommon-x11-dev \
    libfontconfig1-dev libfreetype6-dev libharfbuzz-dev \
    libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxi-dev libxtst-dev libxrecord1-dev \
    libpipewire-0.3-dev libpulse-dev libasound2-dev libdbus-1-dev libegl1-mesa-dev
# 图像/编解码
sudo apt install -y libpng-dev libjpeg-dev libwebp-dev libgif-dev libtiff-dev \
    libheif-dev libjxl-dev zlib1g-dev libtinyxml2-dev
# 可选：OCR
sudo apt install -y libopencv-dev
```

### 9.2 CMake preset（已落地，不再是草案）

以下预设已写入仓库根 `CMakePresets.json`（对应上面 §8 的 **P2**，可直接用）：

| 预设 | 用途 |
|---|---|
| `linux-base`（hidden） | 所有 Linux 预设的公共父级：Ninja、`SNOW_QT_MAJOR=5`、无 `toolchainFile`、`VCPKG_MANIFEST_INSTALL=OFF`、`SNOW_APPS_BUILD_IMAGE_VIEWER=OFF`、移植期 `SNOW_APPS_STRICT_COMPILE=OFF` |
| `snow-shot-linux-x64-debug` / `-release` | UOS 20 / Deepin 23 x86_64 原生构建 |
| `snow-shot-linux-arm64-debug` / `-release` | 飞腾 / 鲲鹏 / 麒麟 990 原生构建 |
| `build-snow-shot-linux-{x64,arm64}-{debug,release}` | 对应的 build preset |

```bash
# 在 UOS 机器上
bash scripts/uos-preflight.sh --probe          # 先跑前置探测（§6.1 的决策点）
cmake --preset snow-shot-linux-x64-debug
cmake --build --preset build-snow-shot-linux-x64-debug          # 或 --target snow_image
ctest --preset test-...                                         # 测试目标见下
```

设计上有三处和常规 preset 不同，都是为了不把 Windows/macOS 主线的假设带进 Linux：

1. **不设 `toolchainFile`**（与 `windows-qt5-base` 同思路）。不挂 vcpkg 工具链 ⇒ manifest 步骤
   根本不运行 ⇒ Linux 用发行版包（§9.1），Windows 用 vcpkg，两边互不干扰。
2. **不写死 `CMAKE_PREFIX_PATH`**。Debian 系（含 UOS）把 Qt5 的 CMake 包放在
   `/usr/lib/<triplet>/cmake/Qt5`，本来就在 CMake 默认搜索路径里；若 UOS 装在自定义前缀，
   再显式 `-DCMAKE_PREFIX_PATH=<prefix>`。
3. **`SNOW_APPS_BUILD_IMAGE_VIEWER=OFF`**，直到 §6.1 的甲/乙路线拍板（见
   `docs-qt5-downgrade-plan.md` §6.1，探测脚本 `scripts/uos-preflight.sh`）。

`SNOW_APPS_STRICT_COMPILE=OFF` 是**移植期**设定：新平台代码加上 Qt5 自带头的告警会在 `-Werror`
下把真正的错误淹掉。Linux 目标全绿后应逐目录打开，最终与 Windows 侧对齐。

#### 9.2.1 源码侧的配套改动（P2 一并完成）

| 文件 | 改动 | 为什么必需 |
|---|---|---|
| `rust-toolchain.toml` | `targets` 增加 `x86_64-unknown-linux-gnu`、`aarch64-unknown-linux-gnu` | 让**任何 Windows 机器**都能对 Linux 目标做静态验证：`cargo check --target x86_64-unknown-linux-gnu` 只需要 std 不需要链接器，等于把跨平台语法/cfg/API 检查前移到写代码的当下 |
| `cmake/SnowWorkspace.cmake` | 补 `Linux` 分支，默认 triplet `x64-linux` / `arm64-linux` | 原来非 Windows/非 macOS 一律落到 `x64-windows`，会把 `SNOW_VCPKG_INSTALLED_DIR` / `SNOW_FFMPEG_ROOT` 指到 Windows 树 |
| `snow_shot/CMakeLists.txt` | `OpenCV_DIR` 的强制赋值加 `if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)` 守卫 | 原来无条件指向 `vcpkg_installed/.../share/opencv4`，在 Linux 上会让 `find_package(OpenCV CONFIG REQUIRED)` 直接失败 |
| `cmake/vcpkg-overlay-triplets/x64-linux.cmake`、`arm64-linux.cmake` | 新增 | 为「Linux 上仍需 vcpkg 补几个发行版没有的库」这条可选路线准备；`arm64` 那份显式带 `-march=armv8-a+simd` |
| `scripts/uos-preflight.sh` | 新增（§6.1 的可执行形式） | 见下 |

`cmake/SnowDiagnostics.cmake` 首行是 `if(WIN32 OR APPLE)`，`snow_shot/CMakeLists.txt` 里 DirectML
的相关段落也都在 `if(WIN32)` 内，因此 Linux 天然不会去找 crashpad / DirectML —— 这两处**无需改动**，
写在这里是为了避免下次重新排查一遍。

### 9.3 需改动的 CMakeLists 关键点

1. `cmake/SnowQt.cmake`：`SNOW_QT_MAJOR` 分支；`find_package(Qt${major})`；Qt5 无 `EXACT` 语义时放宽为 `>=5.15`。
2. 根 `CMakeLists.txt:55-86`：现有 macOS 专用校验需补 Linux 分支；`_snow_mini_default` 对 Linux 置 OFF。
3. `snow_shot/CMakeLists.txt:1-8`：`OBJCXX` 仅在 `APPLE` 生效（确认 Linux 不触发）。
4. `snow_shot/CMakeLists.txt:404-451`：OCR 目标移除 `directml-provider`，Linux 用 `dynamic-onnx-runtime`。
5. `cmake/SnowDiagnostics.cmake`：Linux 下跳过 crashpad。
6. `cmake/SnowSimd.cmake`：新增 aarch64 分支。

### 9.4 Rust 构建

```bash
cd snow-crates
cargo build -p snow-capture -p snow-cursor --target x86_64-unknown-linux-gnu
cargo build -p snow-capture -p snow-cursor --target aarch64-unknown-linux-gnu
cargo test  -p snow-capture      # 按 AGENTS.md：只跑相关 crate 测试
```

> 注意 `.cargo/config.toml` 的 **`-Dwarnings`**：Linux 新平台代码的任何 warning 都会直接 build failure，
> 移植期建议临时对该目录放开或严格清零。

#### 9.4.1 在 Windows 上预先检查 Linux 目标（已实测，不必等 UOS 机器）

`rust-toolchain.toml` 已声明 `x86_64-unknown-linux-gnu` 与 `aarch64-unknown-linux-gnu`，
`cargo check` 只需要目标 std、不需要链接器，所以 Windows 工作站可以直接做跨目标检查：

```bash
cargo check -p snow-cursor -p snow-memory --target x86_64-unknown-linux-gnu
cargo check --workspace --target aarch64-unknown-linux-gnu \
    --config 'build.rustflags=["-Awarnings"]'      # 先看结构错误，别被 -Dwarnings 淹没
```

实测边界（2026-10-15，Windows + rustc 1.97.1）：

| 范围 | 结果 |
|---|---|
| 纯 Rust crate（`snow-core` / `snow-media` / `snow-memory` / `snow-cursor`） | ✅ 23.8s 全绿，可直接当 Linux/arm64 的语法 + cfg + 类型门禁 |
| 依赖树里含 **C 源码** 的 crate | ❌ 卡在 `cc-rs`：`failed to find tool "x86_64-linux-gnu-gcc"`（实测触发者是 `ring`，由 rustls 链条引入）。这类 crate 必须上 UOS 或装交叉 C 工具链才能验 |
| 整个 workspace 一把梭 | 不适用，会被上一条挡住 |

用法建议：新写 Linux 平台代码时先对**目标 crate** 跑跨目标 `cargo check`（秒级反馈），
再在 UOS 上跑真正的 `cargo build` / `cargo test`。这样能把"Linux 语法/cfg 错误"和
"Linux 环境缺失"两类问题分开，后者在真机上一眼可见，前者在真机上代价高得多。

由于"整个 workspace 一把梭"会被 `ring` 挡住，跨目标体检改成**逐个 crate** 跑：

```bash
bash scripts/linux-cross-check.sh                                  # x86_64
SNOW_CHECK_TARGETS="x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu" \
    bash scripts/linux-cross-check.sh                              # 双架构
SNOW_CHECK_FILTER=capture bash scripts/linux-cross-check.sh        # 只跑名字含 capture 的
```

报告写入 `.workbuddy/qt5-port/logs/linux-cross-check.txt`，每个 crate 一份明细日志。
逐 crate 跑的价值第一次就兑现了：它把 `snow-capture` 一个**既有的** Linux 阻塞点
揪了出来（`MonitorId::raw_handle` 在非 Win/mac 下是 dead code，撞 `-Dwarnings`），
这个问题在 Windows 与 macOS 构建里完全不可见。

> **已知的跨目标不可验边界**：依赖树含 C 源码的 crate（`ring` ← rustls 链）需要
> `x86_64-linux-gnu-gcc`。要彻底解锁整个 workspace 的 Linux 类型检查，可以在
> Windows 上装 Zig 并设 `CC_x86_64_unknown_linux_gnu="zig cc -target x86_64-linux-gnu"`
> （`AR_*`/`CXX_*` 同理）。FFmpeg 绑定（`ffmpeg-next` 走 `pkg-config`）即使有 C 编译器
> 也仍需真机，除非手工给 `FFMPEG_*` 环境变量。

#### 9.4.2 逐 crate 体检现状（2026-10-15 实测，x86_64 + aarch64）

30 个 workspace 成员 × 2 架构 = 60 次检查，**46 通过（每架构 23/30）**，9.4.1 的脚本已固化为
`bash scripts/linux-cross-check.sh`。缺口分两类，性质完全不同：

> **2026-10-17 复跑（slice 3 之后）**：仍是 **46/60**，失败项与分类**逐条未变**。差别在覆盖范围：
> `snow-recording-effects` 这一项现在真正编译了新增的 Linux 键鼠适配代码（`#[cfg(target_os="linux")]`
> 在该目标下是激活的），所以输入后端从"只在真机验"变成了"Windows 上每次都能类型检查"。
> 失败原因在 A 类内部会漂移（`zstd-sys` / `ffmpeg-sys-next` 谁先撞取决于并行调度），不是新问题。
>
> **2026-10-17 复跑（slice 4 之后）**：依旧 **46/60**，失败清单与上面第二次复跑**逐条相同**。
> 本片新增的 `snow-linux::text`（`ab_glyph` + `ttf-parser`）**没有给任何 crate 增加新的失败项**，
> 因为它只往依赖树里加了两个纯 Rust crate —— 这正是选它们而不选 fontconfig / FreeType 的原因：
> 若链了 C 字体栈，`snow-linux` 自己就会掉进 A 类，本片代码将不再有任何自动门禁。

**(A) 需要交叉 C 编译器 —— 不是我们的代码问题（每架构 6 个）**

| crate | 卡住的 C 依赖 | 说明 |
|---|---|---|
| `rapid-ocr-rs` | `ring` | rustls 链引入 |
| `snow-ocr-process` | `clipper-sys` | — |
| `snow-recording-c` / `snow-recording-export` | `zstd-sys` / `ffmpeg-sys-next` | 视 crate 并行调度，先撞哪个不定 |
| `snow-recording-runtime` / `snow-screen-recorder` | `ffmpeg-sys-next` | FFmpeg 绑定还需真机上的 `pkg-config` |

这一类的失败**与 Linux 移植无关**：同一份代码在 UOS 真机上只要有 `gcc` + `libav*-dev` 就能过。

**(B) 真实缺口 —— 必须在 P3 里补（每架构 1 个）**

| crate | 报错 | 根因 | 处置 |
|---|---|---|---|
| `snow-ui-selector-c` | `E0432 unresolved import snow_ui_selector::ElementRegionService` | 该类型只在 `#[cfg(windows)]` / `#[cfg(target_os="macos")]` 下导出；`snow-ui-selector` 尚无 Linux provider | **不放假桩**。AT-SPI2 provider 落地前，此 crate 就是 Win/macOS-only；塞一个"返回空元素集"的 Linux 实现会让 C 层以为无障碍"可用但没结果"，比编译失败更危险 |

**(C) 顺手修掉的两个既有 Linux 阻塞（本来编译不过，与本次改动无关）**

| 位置 | 问题 | 修法 |
|---|---|---|
| `snow-capture/src/monitor.rs` | `MonitorId::raw_handle()` 只被 Win/mac 后端读 ⇒ Linux 下 dead code ⇒ 撞 `-Dwarnings` | `#[cfg_attr(not(any(windows, macos)), allow(dead_code))]` |
| `snow-ui-selector/src/{lib.rs,query.rs}` | `ElementRect::new` 与 `QueryControl` 的字段同样只在 Win/mac 被消费 | 同上，平台范围内 `allow(dead_code)` |

这类问题在 Windows 与 macOS 构建里**完全不可见**，正是 9.4.1 那套跨目标检查第一次跑就抓出来的。

> 检查报告：`.workbuddy/qt5-port/logs/linux-cross-check.txt`；逐 crate 明细
> `.workbuddy/qt5-port/logs/linux-check-<target>-<crate>.log`。
> 复跑：`bash scripts/linux-cross-check.sh`（加 `SNOW_CHECK_TARGETS` 可扩到 arm64）。

#### 9.4.3 P3 slice 2：X11 采集后端落地（2026-10-15）

`platform/mod.rs` 里的 `UnsupportedBackend` 在 Linux 上已有真实现：`platform/linux.rs`
（`LinuxBackend` + `X11Capturer`）经 `snow-linux` 的 X11 会话读根窗口 `GetImage`。

**分层照 8.1 的铁律切**：协议与像素全在 `snow-linux`，`snow-capture` 只做类型翻译。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 协议 | `snow-linux/src/x11.rs` | linux | `capture_image(Rect)`：裁剪到根窗口 → `GetImage(ZPixmap, AllPlanes)` → 根 visual + 该深度的 pixmap format |
| 解码 | `snow-linux/src/capture.rs` | **全平台** | `ImageLayout` / `ChannelMask` / `decode_into`：掩码驱动的通用解码 + 原生 BGRX 快路径；14 个单测**在 Windows 上跑** |
| 接线 | `snow-capture/src/platform/linux.rs` | linux | `CaptureBackend`（枚举 / 主屏 / 布局 / inspect）+ `MonitorCapturer`（`capture`、`set_output_pixel_format`） |

三个必须写下来的判断（都是跨目标类型检查逼出来的）：

1. **`bits_per_pixel` 不在 visual 里**。X11 的 `Visualtype` 只有 `red_mask/green_mask/blue_mask`；
   每个深度的位宽与扫描线填充在 `Setup.pixmap_formats`。所以位宽按**回复的 `depth`** 去查
   `pixmap_formats`，而不是从 visual 猜 32 —— 猜错就是整幅画面通道错位。
   同理 `GetImage` 回复**没有** `bytes_per_line` 字段：优先用回复长度反推（`reply_stride`），
   退路才是按 `Format.scanline_pad` 现算（`scanline_bytes`）。
2. **X11 像素没有 alpha**。32bpp 的第 4 字节是填充（常见 0，也可能残留脏数据），
   解码一律写成不透明的 `0xff` —— 直接透传会得到带洞的截图。
3. **根窗口读不到指针，也读不到别的窗口**。指针由 X 合成在根窗口"之上"而非"之中"，
   所以 `create_target_capturer` 保持默认（不提供原生采集器），光标交给会话的通用路径
   （`attach_cursor_to_frame` → `snow_cursor` 的 X11 采样器）来画；窗口目标直接
   `BackendUnavailable` —— 读别的窗口像素需要 Composite 重定向，这是硬缺口而非待办。

`CaptureBackendKind` 新增 `X11` 变体（`as_str() == "x11"`），C ABI 里是 `5`
（`snow_capture.h` 同步）。穷尽 match 的更新点：Windows 侧两个 `create_*_by_kind`
（**显式拒绝而非替换**）、三个 benchmark example、`snow-capture-c` / `snow-recording-c`
的 `capture_backend_value`。

**已知的性能缺口（P5 处理）**：整条路径是同步服务器往返 + 服务端拷贝，**没有脏矩形**，
`is_duplicate` 恒为 false。Windows 侧靠 DXGI 脏矩形跳过编码的优化在这里没有对应物；
X11 的等价物是 Damage 扩展，属后续 slice。

#### 9.4.4 P3 slice 3：X11 输入观测后端落地（2026-10-17）

P3 slice 1/2 让 Linux 能「看上屏」，这一片让它能「看见手」：`snow-recording-effects`
的键盘/鼠标观测在 Linux 上不再是空实现。分层仍照 8.1 的铁律切，且**新增的纯逻辑面比 X11 面还大**
（这也是它能在 Windows 上被完整单测的原因）。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 翻译 | `snow-linux/src/input/keys.rs` | **全平台** | `KeyMap`（键码→keysym，含 shift/AltGr 层选择与 `NoSymbol` 回退）、`virtual_key`（keysym→虚拟键）、`label`、`pressed_from_keymap`。**20 个单测在 Windows 上跑** |
| 规则 | `snow-linux/src/input/tracking.rs` | **全平台** | `Region` / `MotionTracker`（连续性）/ `ClickTracker`（按下命中区域、抬起跟随）/ `PointerButton` / `modifiers_from_pressed`。**16 个单测** |
| 对话 | `snow-linux/src/input/x11.rs` | linux | `X11InputObserver`：XI2 原始事件订阅 + 事件泵 + `QueryPointer`/`QueryKeymap`/`GetKeyboardMapping` 往返 |
| 接线 | `snow-recording-effects/src/{keyboard_hook,mouse_hook}.rs` | linux | `#[cfg(target_os = "linux")] mod platform`，翻译成既有的 `KeyObservation` / `MouseClickObservation` / `MouseMovement` |

**先修掉一个既有 Linux 硬阻塞**：mouse/hook 的空实现里 `start()` 返回 `Ok`（一个永远不上报的观察器），
只有 `start_with_movement()` 返回 `Err`。而 `deferred.rs`（Linux 目标下确实参与编译）走的正是后者 ⇒
**Linux 上 `InputRecorder::new` 必然失败，录制根本起不来**。空实现里"假装成功"的那一半正是问题所在，
两处都已换成真实现，`not(any(windows, macos))` 门控随之改为 `not(any(windows, macos, linux))`。

四个必须写下来的判断（都是跨目标类型检查/交叉比对逼出来的）：

1. **观测值必须说 Windows 虚拟键的空间**。overlay 的 `KeyObservation::event()` 与
   `keyboard_overlay::modifier()` 用**硬编码**的 `0xa2/0xa3/0xa4/0xa5/0xa0/0xa1/0x5b/0x5c` 索引
   `pressed` 表，F1–F24 靠 `0x70..=0x87`、小键盘靠 `0x60..=0x69` 直接格式化。所以 Linux 后端不能透传
   X11 键码，必须做 keysym→虚拟键映射（`keys.rs`）。macOS 走的是 `0x100+原生键码` 的第三空间，Linux 只在
   **映射不到**时才退回 `0x100+键码`（见第 4 点）。
2. **keysym 描述的是"字符"，不是"键"**。US 布局下同一个物理键给出 `0x31`(`1`) 与 `0x21`(`!`)，
   而 Windows 虚拟键只有一个身份。故映射表建在**字符**上：`!` 与 `1` 归一、`?` 与 `/` 归一、`;`/`:` 归一，
   与 Windows 的 `VK_OEM_*` 语义一致。钥匙帽显示什么则由 `label(keysym)` 决定 —— 它返回**按下的那个字符**
   （`!`、`Д`），因此比"虚拟键→字符"反查更准（后者拿不到 Shift 层）。
3. **不用原始事件的坐标，用 `QueryPointer`**。RawMotion 的位置在设备 valuator 里，`axisvalues` 与
   `axisvalues_raw` 哪个含加速值、相对设备按 FP1616 还是 FP3232 上报，**没有真机就无法证伪**，
   而猜错的结果是"看起来合理但整体偏移"的光标轨迹 —— 最坏的一类错误。`QueryPointer` 在任何服务器/设备上
   都无歧义。代价是每次采样一次往返，故运动采样上限 `MOTION_SAMPLE_INTERVAL = 8ms`（125 Hz）并在其间合并
   ——消费端本来就只保留最新一条 movement，合并零损失。
4. **观测器自己持有 pressed 表和键盘映射**，于是按键事件**零往返**：shift/AltGr 层选择直接用本地
   `pressed` 表算，不需要每次查询修饰键。映射在 `MappingNotify` 时重建（换布局后标签才不会继续描述旧布局）；
   通道溢出后 `generation` 自增，并用 `QueryKeymap` 重建 pressed 表 —— 丢掉的批次里可能含 keyup，
   不修就会让一个修饰键**永久画在屏幕上**。

**其余刻意的取舍**：

- **poll 而不是阻塞 `wait_for_event`**：连接的 socket 归工作线程独占，`Drop` 没有任何办法打断阻塞读，
  而录制收尾必须能随时结束观测器。改成 `poll_for_event` + 空闲 10ms 休眠（约 100 次/秒唤醒，比采集路径
  自身的帧定时器低一个量级），代价是**空闲时首事件最多在 socket 缓冲里等 10ms** —— 抖动上限，远小于一帧。
- **`alt_gr` 恒为 false**：X11 的 AltGr 是**独立的键**（映射到 `0xa5`，标签直接就是 `AltGr`），
  不是 Windows 那种"Ctrl+Alt 合成"的伪修饰。该标志的作用是隐藏伪装的 Ctrl 芯片，这里不存在。
- **无 XInput2 时诚实失败**：`LinuxError::UnsupportedInputBackend`（分类 `InvalidConfig`，不重试），
  而不是塞一个"返回空事件流"的实现 —— 后者会让录制器承诺一个永远不出现的键帽。
- **`scan` 字段在 Linux 上存 keysym**：X11 没有硬件扫描码可报，而 keysym 正是 `printable_label`
  需要的（`scan` 的语义本来就是每平台不同：Windows 是扫描码，macOS 是 `keyboard_type`）。
  `layout` 字段在 Linux 上未使用（映射由观测器单点持有）。

**验证（2026-10-17，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux` | **65 通过**（其中 47 个是本次新增：keys 20 + tracking 16 + error 1 + …） |
| `cargo test -p snow-recording-effects` | **74 通过**，1 ignored —— 与改动前逐条一致，无回归 |
| `cargo check -p snow-linux --target {x86_64,aarch64}-unknown-linux-gnu` | 双架构全绿（`-Dwarnings` 生效） |
| `cargo check -p snow-recording-effects --target {x86_64,aarch64}-unknown-linux-gnu` | 双架构全绿 —— 该 crate 依赖树**无 C 源码**，所以整条适配路径都能在 Windows 上类型检查 |

**本片未覆盖（诚实记账）**：Wayland（无特权客户端拿不到全局输入，只能走 `GlobalShortcuts` / 远程桌面
portal）、无 XInput2 的 XRecord/evdev 退路、音频采集。**真机待验**：XInput2 原始事件在 UOS 20 的
Xorg 与 XWayland 下的实际投递行为、以及 `QueryPointer` 采样在 125 Hz 上限下的实测开销。

#### 9.4.5 P3 slice 4：键帽光栅化落地（2026-10-17）

slice 3 让 Linux「采得到键」，这一片让它「画得出键帽」：`snow-recording-effects` 的
`keyboard_rasterizer::create` 在 Linux 上不再返回 `Err`，录制时的按键叠加终于能合成进画面。

**先纠正上一片文档的一处错误断言。** §4.2 原先写着「`keyboard_rasterizer.rs` 无需改动（纯逻辑）」，
这是错的：该文件是 `#[cfg(...)] mod platform` 结构，Windows 走 DirectWrite + Direct2D、macOS 走
CoreText + Quartz、其余平台是**硬返回 Err** 的桩。**判据**：凡是每个平台都各自实现一份 `mod platform`
的模块，就不是纯逻辑，第三方平台的适配必须在清单里，不能因为「文件名不含平台字样」就跳过。
本片的起点正是这个被漏掉的桩 —— 功能上它让 Linux 的按键叠加整条链路缺最后一环。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 字体发现 | `snow-linux/src/text/font.rs` | **全平台** | `scan_faces`（递归枚举 + `ttf-parser` 读 `name` 表）、`select_face`（族名归一 + 字重最近邻）、`resolve`（主族 + CJK 回退）。**10 个单测** |
| 光栅化 | `snow-linux/src/text/raster.rs` | **全平台** | `KeycapRenderer`：`ab_glyph` 取轮廓 → 灰度覆盖；自写圆角矩形 4×4 超采样；premultiplied RGBA 合成。**15 个单测** |
| 接线 | `snow-recording-effects/src/keyboard_rasterizer.rs` | linux | `#[cfg(target_os = "linux")] mod platform`，包成既有的 `KeycapRasterizer` |

五个必须写下来的判断：

1. **不调 fontconfig**。这是本片最重要的约束，与 x11rb 的选型同源：`cargo check --target
   <linux-triple>` 是本项目**唯一**能在 Windows 上验证 Linux 代码的门禁，一旦链接 C 库（fontconfig /
   FreeType），该门禁立刻需要 C 交叉工具链与 target 头文件，机制整个失效。所以族名解析改成
   **枚举字体文件 + 直接读 `name` 表**（`ttf-parser`，纯 Rust），光栅化用 `ab_glyph`。代价是
   fontconfig 的 alias / 替换规则（`~/.config/fontconfig/fonts.conf`）**不生效**，只做族名精确匹配。
2. **只实现 `rasterize`，`rasterize_glyph` 沿用 trait 默认实现** —— 与 macOS 分支一致（macOS 也只实现
   前者）。默认实现把 `font_pixels / 32` 当作 scale，`playback.rs` 的 `TIMER_FONT_SIZE = 24` ⇒ scale
   0.75。**这个不能自己另算**：`playback` 靠扫描返回位图的 alpha 通道来定位字形边界并推出基线
   （`glyph_top` / `digit_top`），换一套度量就会让计时器数字的基线飘。
3. **产物契约对齐 macOS 而不是 Windows**。两边都能画成「带边框的圆角键帽」，但构造不同：macOS 是
   **border 铺满整图 + background 内缩 `scale` 像素**（圆角 `10 × scale`），Windows 是矩形内缩 1.0 +
   圆角 12 + 2px 描边。选 macOS 的形态，因为自绘一遍 fill 两遍更简单，且 Linux 与 macOS 同属
   `not(windows)` 分支。尺寸公式与两边一致：`ceil(fit(measured).0 × scale)` by `round(64 × scale)`。
4. **文本度量用 advance 累加，不做 kerning**。主字体与 CJK 回退字体是两套坐标，跨字体的一对
   kern 没有意义；同字体下对一两个字符的键帽图例影响不到 1px。macOS 的 `CTLineGetTypographicBounds`
   本质也是 advance 宽度，所以这仍与 macOS 同源。
5. **缺字回退的判据是 `glyph_id(c) == 0`**。`ab_glyph` 对无轮廓的字符返回 `.notdef`（id 0），
   这正是 DirectWrite / CoreText 触发 cascade 的同一个条件。**主字体缺失且没有回退字体时不报错**，
   退化成 `.notdef` 方框 —— 少一个汉字不该让整帧合成失败。但**请求了一个未安装的族名要报错**
   （`TextError::FontUnavailable`）而不是换成别的字体：录出来的水印字体必须与应用程序其它地方一致。

**其余刻意的取舍**：

- **圆角与边缘用 4×4 超采样覆盖率**（16 样本/像素）而不是解析式 SDF：确定性、能写进断言、
  没有浮点近似的争议。一个 64×144 的键帽约 15 万次 `inside` 测试，微秒量级，而 overlay 侧本来就有
  256 项、32 MB 上限的键帽缓存。
- **不做 hinting / 子像素定位**：灰度抗锯齿与系统库同一档次，键帽是小字号大字重，差异不可见。
- **默认族名候选表**（`Noto Sans CJK SC` → `Source Han Sans SC` → `WenQuanYi Micro Hei` → `Noto Sans`
  → `DejaVu Sans` → `Liberation Sans`）只在请求未命名族名时使用；UOS / Deepin 默认 UI 字体是前两个。
- **扫描跳过符号链接并限深 6 层**，否则字体目录里一个指回父目录的链接会把一次枚举变成无边界遍历。
  文件按路径排序后解析，保证同一台机器上字重平局的判定每次一样。

**验证（2026-10-17，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux` | **91 通过**（slice 3 后是 65，本片 +26：`text::font` 10 + `text::raster` 16） |
| `cargo test -p snow-recording-effects` | **74 通过**，1 ignored —— 与改动前逐条一致，无回归 |
| `cargo check -p snow-recording-effects --all-targets` × {windows, x86_64-linux, aarch64-linux} | 三目标全绿（`-Dwarnings` 生效） |
| `cargo check -p snow-linux --target {x86_64,aarch64}-unknown-linux-gnu` | 双架构全绿 |
| `bash scripts/linux-cross-check.sh`（双架构，slice 4 后复跑） | **46/60**，失败清单与 slice 3 复跑逐条相同；`snow-linux` 与 `snow-recording-effects` 均 PASS |

单测**不是**在真机上跑的：验证素材是 `test-support/fonts` 里那三套刻意造的测试字体。它们的设计正好
支撑本片的断言 —— Latin 家族**故意不含汉字轮廓**（"fallback 测试不能靠基础字体蒙混过关"）、
Bold 的笔画是 Regular 的两倍多（能区分「真加载了粗体」与「又用了一次常规体」）、每个非 ASCII 字形的
advance 都比 Latin 的 `M` 宽（能区分「量了回退字体」与「没量」）。

测试字体是 fontTools 生成的**单 face TTF**，与真实安装仍有距离（集合 `.ttc`、CFF 轮廓、本地化 name
记录），所以另有一个 `#[cfg(windows)]` 的冒烟测试直接枚举宿主 `C:/Windows/Fonts`（约 3.8 秒），
用 **Arial 画拉丁图例、微软雅黑做汉字回退**，两者都要求真有墨迹落地；目录不存在时不执行。
这条测试是「发现层不是只对生成字体成立」的实证。

**本片未覆盖（诚实记账）**：fontconfig 的 alias / 替换规则与按语言选字形（`lang=` 匹配）；
颜色字体（COLR / CBDT emoji）；可变字体的 `wght` 轴插值（只按静态 face 选字重）；
`Xft` / X core 位图字体。**真机待验**：UOS 20 上实际的默认族名（候选表按 Noto / 文泉驿写的，
若镜像用的是别的族名，`create` 会在录制启动时报 `FontUnavailable` 而不是静默画错），
以及汉字键帽在真实字体下的墨迹居中效果。

### 9.5 打包

```cmake
# cmake/SnowShotLinuxPackage.cmake（新增）
set(CPACK_GENERATOR "DEB;External")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")   # 或 arm64
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libqt5widgets5, libqt5svg5, libqt5network5, libpipewire-0.3-0, libpulse0")
```

产出：`snow-shot_<ver>_amd64.deb`、`snow-shot_<ver>_arm64.deb`，附 `.desktop` 与图标。

---

## 10. 工作量 / 优先级矩阵

| 编号 | 工作项 | 规模 | 风险 | 优先级 |
|---|---|---|---|---|
| W1 | Qt5 兼容层 + CMake 双版本 | 大 | 高 | **P0** |
| W2 | `snow_image_viewer` RHI 重写/裁剪 | 大 | 高 | P1 |
| W3 | Rust 屏幕采集（X11 / PipeWire） | 大 | 高 | **P0** |
| W4 | C++ `platform/linux/*` | 大 | 中高 | **P0** |
| W5 | 音频 / 输入钩子 / AT-SPI | 中 | 中 | P1 |
| W6 | NEON SIMD 补强 | 中 | 中 | P1 |
| W7 | 打包（deb/AppImage/.desktop） | 小 | 低 | P1 |
| W8 | 全局快捷键 + portal 适配 | 中 | 中高 | P1 |
| W9 | onnxruntime CPU EP 接入 | 小 | 低 | P1 |
| W10 | crashpad 移除/替换 | 小 | 中 | P2 |

---

## 11. 未决问题与建议

1. **Qt5 下限**：建议 **5.15 LTS**。若必须 5.12 或更低，W1 规模再增约 30%–50%。
2. **查看器取舍**：建议 Linux 首版不交付 `snow_image_viewer`，把 RHI 重写排到 P5 之后。
3. **防截屏排除能力**：Linux 无系统级等价 API，需在 UI 明确降级说明（避免误导性提示）。
4. **crashpad**：Linux 下建议直接移除，避免引入不稳定第三方崩溃链路；或后续评估 Breakpad。
5. **Wayland vs X11**：UOS 20 默认 X11 可先行；Deepin 23 / UOS 后续版本 Wayland 占比上升，
   建议从一开始就把采集/输入抽象成 `LinuxBackend` 接口下的 `x11` / `wayland` 两个实现，避免二次重构。
6. **真机验证**：arm64（飞腾/鲲鹏/麒麟 990）必须真机终验，交叉编译不可替代。

---

## 附录 A：AppImage 打包路线与「保留 Qt6」可行性

### A.1 直接回答

**技术上可行，但约束只是换了地方。**

AppImage 把应用**连同 Qt 一起**打进单个可执行文件，运行时由 `AppRun` 设置 `LD_LIBRARY_PATH` /
`QT_PLUGIN_PATH`，因此**不依赖宿主系统的 Qt 版本**——这也意味着**不再需要把代码降级到 Qt5**：
`QKeyCombination`、`QNativeInterface`，乃至 `snow_image_viewer` 的 `QRhi` 都可以原样保留。

但它同时引入一个**比 Qt5 更硬的约束：glibc 基线**。AppImage **无法打包 glibc**
（glibc 不可重定位，官方亦明确不支持声明最低 glibc 要求），宿主的 glibc 必须 ≥ 构建基线。

> 结论：**能否在 UOS 上保留 Qt6，不取决于 AppImage，而取决于「目标机 glibc 版本」与「本工程 Qt 6.12 的构建基线」的相对关系。**

### A.2 AppImage 能打包什么 / 不能打包什么

| 能打进 AppImage | 必须由宿主提供（打不进） |
|---|---|
| Qt6 全套库 + 平台插件（`libqxcb.so` / `libqwayland-*.so`） | **glibc**（不可重定位） |
| `libstdc++` / `libgcc_s` | **GPU 驱动**：`libGL` / `libEGL` / `libdrm`（必须用宿主的） |
| FFmpeg / OpenCV / onnxruntime / 图像编解码库 | **xdg-desktop-portal**（Wayland 采集入口） |
| 自研 Rust 静态库、`.qm` 翻译、字体 | **PipeWire / PulseAudio 守护进程** |
| 应用本体与资源 | D-Bus session bus、AT-SPI、polkit、CUPS、X11/Wayland 合成器 |

> ⚠️ 关键推论：**AppImage 解决不了本方案的主要工作量（L2 平台后端）。**
> 屏幕采集（portal + PipeWire）、音频、全局输入钩子、UI 元素拾取全部依赖宿主服务与内核接口，
> 打包行为对此毫无帮助——**第 8 章的 P3 / P4 工作量一项都省不掉。**

### A.3 核心约束：glibc 基线（决定性因素）

Qt 官方数据（[Qt for Linux](https://doc-snapshots.qt.io/qt6-dev//linux.html)）：

| Qt 版本 | 官方预编译包最低 glibc |
|---|---|
| Qt 6.8+ | glibc **2.28** |
| Qt 6.10+ | glibc **2.34** |
| Qt 6.11+ | 官方在线安装包基于 Ubuntu 24.04（glibc **2.39**） |

目标系统基线（已核实）：

| 目标系统 | 基础发行版 | glibc | 内核 | Qt 6.12 预编译包 | 说明 |
|---|---|---|---|---|---|
| **UOS 20（V20 1070）** | Debian 10 buster | **2.28** | 4.19 | **✗ 不可用** | 会报 `GLIBC_2.34 not found` |
| Deepin 20 | Debian 10 | 2.28 | 4.19 | **✗ 不可用** | 同上 |
| Deepin 23 | 自研 | 2.38 | 6.6 | ✓ 可用 | |
| UOS 25 | 较新 | 需实测 | 较新 | 大概率可用 | 以实机 `ldd --version` 为准 |

**本工程锁定 Qt 6.12.0**，其官方预编译基线为 glibc ≥ 2.34（实测构建于 2.39），
而 **UOS 20 只有 glibc 2.28** → 预编译 Qt6 AppImage 在 UOS 20 上**直接跑不起来**。

> 注意：Qt 官方称"**从源码构建则不受 glibc 版本限制**"——但真实限制会转移到*构建主机*：
> 在 glibc 2.38 / 2.39 的机器上编出的 Qt 会引用高版本符号，同样跑不了 2.28。
> 必须**在 glibc ≤ 2.28 的环境内构建**（UOS 20 / Debian 10 容器，或使用老 sysroot）。

### A.4 三条可行路线对比

| | 路线 A：AppImage + Qt6 预编译 | 路线 B：AppImage + Qt6 自建于 2.28 环境 | 路线 C：维持 Qt5 路线（原方案） |
|---|---|---|---|
| 覆盖 UOS 20（glibc 2.28） | ✗ | ✓（需实测验证） | ✓ |
| 覆盖 Deepin 23 / UOS 25 | ✓ | ✓ | ✓ |
| 是否保留 Qt6 | ✓ | ✓ | ✗（须降级） |
| 关键难点 | 无（但 UOS 20 用不了） | Qt 6.12 需 CMake ≥ 3.22 + 较新编译器，而 Debian 10 仅 GCC 8.3 → 需"老 glibc + 新编译器"组合（老 sysroot / 容器） | Qt5 兼容层 + RHI 重写 |
| 主要风险 | glibc 不达标 | 构建链复杂；符号版本需实测 | 代码改写量大 |

> **路线 B 的隐藏地雷**：在 glibc ≥ 2.38 且 GCC 14 下构建会引入 `__isoc23_*` 符号
> （如 `__isoc23_strtol`），该符号在旧 glibc 上不存在。必须**使用旧版 glibc 头文件**编译，
> 而不是只换一个旧 linker。

### A.5 对本工程工作量的连锁影响

若最终选择「AppImage + Qt6」，原方案中两大战线**整体消失**：

| 原方案工作项 | AppImage + Qt6 后 | 变化 |
|---|---|---|
| W1 Qt5 兼容层 + CMake 双版本（P1） | **取消** | ↓↓↓ |
| W2 `snow_image_viewer` RHI 重写 | **取消**（Qt6 下 `QRhi` 原生可用） | ↓↓↓ |
| 第 3 章全部 Qt6→Qt5 API 改写清单 | **取消** | ↓↓↓ |
| W3 Rust 平台后端（采集 / 光标 / 音频 / 输入） | 保留 | — |
| W4 C++ `platform/linux/*` | 保留 | — |
| W5 / W6 / W8 / W9 / W10 | 保留 | — |
| AppImage 打包 + glibc 基线控制 | **新增** | ↑ |

**净效果**：工程总量显著下降，但**前置条件变为"目标机 glibc ≥ 构建基线"**，
且**信创合规风险上升**（见 A.7）。

### A.6 AppImage 落地注意事项（与本工程相关）

1. **FUSE2 依赖**：AppImage 需要 `libfuse2`（**不是** fuse3）。UOS 可能未预装，
   需 `sudo apt install libfuse2`；否则用 `--appimage-extract-and-run` 兜底（体验下降，需解压约 1GB）。
2. **切勿打包 `libGL` / `libEGL` / `libdrm`**：`linuxdeploy` 默认排除这些是正确行为——
   打包会导致 GPU 驱动失配、渲染异常。
3. **Qt 平台插件链必须完整**：`platforms/libqxcb.so` 依赖一长串 `libxcb-*`、`libxkbcommon`
   与 `xcbglintegrations/*`，建议用 `linuxdeploy-plugin-qt` 自动收集，并逐个 `ldd` 核对。
4. **Wayland 会话**：需同时打包 `libqwayland-*.so` 与 `libwayland-client`；
   但**采集仍须走宿主 portal**，打包无济于事。
5. **桌面集成缺口**：AppImage 不会自动创建菜单项、托盘、开机自启。本工程的"开机自启"会写
   `~/.config/autostart/*.desktop` 并指向 AppImage **绝对路径**——用户一旦移动文件即失效，
   需在 UI 中显式提示（避免"设了自启却没生效"的误导）。
6. **单实例**：`QLocalServer`（`snow_shot/src/app/singleinstancecoordinator.cpp`）在 AppImage 下正常，无需改动。
7. **提权**：`pkexec` 走宿主 polkit，正常工作。
8. **更新**：AppImage 不受系统包管理器管理，需自建更新通道（`AppImageUpdate` / zsync 或应用内自更新）。
9. **arm64**：需 aarch64 版 `appimagetool` 与 arm64 `libfuse2`；同样准备 `--appimage-extract-and-run` 兜底。
10. **静态 Qt 变体**：本工程已支持 `SNOW_APPS_QT_STATIC`。`snow_shot` 为 **GPL-3.0**，
    静态链接 Qt 在许可上成立，且能规避插件路径问题——可作为路线 B 的简化形态。

### A.7 合规提醒（重要）

在**信创采购 / 等保 / 国产化适配认证**场景中，AppImage 通常**不被认可**：
它不在信创软件目录内，且认证审查一般要求使用**系统提供的基础库**而非自带副本。
若本项目的 Qt5 约束来自认证要求，则 **AppImage 打包 Qt6 极可能通不过评审**，
必须维持 Qt5 路线（折中方案：采用 `deepin-shared-libs` 提供的 Qt 5.15.10）。

### A.8 基线验证方法（动手前必做）

```bash
# 1) 确认目标机 glibc
ldd --version | head -1

# 2) 解包后检查打包库引用的最高 GLIBC 符号版本（必须 <= 目标机）
./SnowShot.AppImage --appimage-extract
readelf -V squashfs-root/usr/lib/libQt6Core.so.6 \
  | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -3
# 期望 <= GLIBC_2.28（UOS 20）

# 3) 复查全部打包 .so
find squashfs-root -name '*.so*' -exec sh -c \
  'readelf -V "$1" 2>/dev/null | grep -o "GLIBC_[0-9.]*"' _ {} \; | sort -Vu | tail -3

# 4) 确认未引入 __isoc23_* 符号
nm -D squashfs-root/usr/lib/libQt6Core.so.6 | grep isoc23 || echo "OK: no __isoc23"
```

---

*本文档基于对 `snow-apps` 仓库（`SNOW_SHOT_VERSION 1.2.8`）的静态分析编写，所有文件路径与行号均对应当前快照。*
