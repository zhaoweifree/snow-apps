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
| `snow_shot`（主程序） | 低（平台层全缺） | 中 | **大改** | 需从零实现全部 Linux 平台后端。**已起步**：物理光标走 X11，并建立了 Rust↔C++ 平台桥（见 9.4.8） |
| `snow_image_viewer` | 低 | **极低** | **重写或裁剪** | `QRhi*` 为 Qt6 专有公开 API |
| Rust `snow-capture` / `snow-cursor` | 低 | — | **已完成 X11 部分** | 采集与光标走 X11（见 9.4.3），经 MIT-SHM 零拷贝读像素（见 9.4.7）；PipeWire / portal 仍缺 |
| Rust 输入钩子 / 键帽渲染 | 低 | — | **已完成 X11 部分** | XInput2 观测（见 9.4.4）+ `ab_glyph` 光栅化（见 9.4.5）；Wayland / 无 XInput2 退路仍缺 |
| Rust 元素拾取 | 低 | — | **已完成窗口级** | X11 窗口树 + `_NET_*`（见 9.4.6）；控件级仍需 AT-SPI2 |
| Rust 音频 | 低（全为 `UnsupportedBackend`） | — | **范围外（决策 4）** | 不做：只在 Win/mac 提供录制音轨，Linux 上诚实上报不可用 |
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

**决策 4：Linux 不做音频采集。** 录屏在 UOS 上只录画面。理由不是"做不了"，而是代价与收益不成比例：
PulseAudio / PipeWire 的客户端库全是 C 绑定，一旦引入，「平台后端只用纯 Rust」这条铁律就破了，
此后音频路径**没有任何能在 Windows 上跑的门禁**，只能靠真机反复试。而 `snow-audio-recorder`
在 Linux 上本来就已经是 `UnsupportedBackend`，保持"如实上报不可用"比给一个无法验证的实现更诚实。

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
├── snow-crates/         (Apache-2.0)  Rust workspace，31 个 crate
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
| UIA/MSAA 元素拾取 | `snow-ui-selector/src/windows/*`、`snow-selected-text/src/platform/windows/*` | **X11 窗口树 —— 已落地窗口级**：`snow-linux/src/window.rs`（纯逻辑）+ `snow-linux/src/x11/window.rs`（见 9.4.6）。控件级仍需 AT-SPI2 (D-Bus)；跨进程读选中文字（`snow-selected-text`）同样仍缺 |
| 全局键鼠钩子 | `snow-recording-effects/src/{keyboard_hook.rs:124, mouse_hook.rs:90}` | **XInput2 raw events —— 已落地**：`snow-linux/src/input/`（见 9.4.4）。待补：无 XInput2 时的 XRecord / evdev 退路、Wayland portal |
| DirectWrite + Direct2D 键帽光栅化 | `snow-recording-effects/src/keyboard_rasterizer.rs:8`（macOS 走 CoreText + Quartz） | **`ab_glyph` + `ttf-parser` —— 已落地**：`snow-linux/src/text/`（见 9.4.5）。字体发现自建，**刻意不链 fontconfig** |
| Win32 窗口层 | `snow_shot/src/platform/windows/*`（30 文件）：`windowchrome.cpp:255` `SetWindowDisplayAffinity`、`pinnedwindownative.cpp` `WS_EX_*` | X11 属性 / 无对应 |
| Win32 物理光标 | `platform/physicalcursor.cpp:26-39`（`GetPhysicalCursorPos` / `SetPhysicalCursorPos`）、macOS 侧 `CGEventGetLocation` / `CGWarpMouseCursorPosition` | **XQueryPointer / XWarpPointer —— 已落地**（见 9.4.8）：`snow_linux::x11::pointer` + C ABI `snow_linux.h`。**刻意不用 XFixes**，因为这两个请求是核心协议，缺扩展的服务器也能用 |
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
| **新增** | `snow_shot/src/platform/linux/` | 对应 `windows/` 全部能力：全局快捷键、全局鼠标、物理光标、全屏窗口检测、窗口捕获排除、窗口 chrome、托盘、单实例、自启动、提权、剪贴板、系统通知、滚动注入。**例外**：物理光标没有落在这里 —— `physicalcursor.cpp` 的 Linux 分支直接调 `snow-linux-c`，因为那块逻辑的价值在于可离线单测，放进纯转发目录反而失去意义（见 9.4.8） |
| **修改** | `snow_shot/src/platform/focusedfullscreenwindow.cpp:39`、`globalmousebackend.cpp:5-38`、`physicalcursor.cpp:13-97` | 增加 `Q_OS_LINUX` 分支。**`physicalcursor.cpp` 已完成**（见 9.4.8）；其余两个仍是通用桩（全屏检测恒 false、全局鼠标 `Unavailable`） |
| **修改** | `snow_shot/src/presentation/{globalcanvas/globalcanvasplatform.cpp, pinned/pinnedwindowplatform.cpp}` | Win/Mac 分发 + 桩，需 Linux 实现 |
| **重构** | `snow_shot/include/snow_shot/platform/screenshotnative.h:3` | **无条件 include `windows/scrollinput.h`**，须拆出平台中立接口 |
| **修改** | `ant_design_qt/.../widgets/{context_menu,modal}.cpp` + `detail/*` | Win32/`.mm` 分发，需 X11 对应 |
| **重写/裁剪** | `snow_image_viewer/src/render/rhi_image_window.*` | 见 3.4 |
| **新增** | `snow_shot/resources/linux/*.desktop`、图标 | 应用入口、右键菜单 |
| **新增** | `cmake/SnowShotLinuxPackage.cmake` | CPack DEB/AppImage |

### 4.2 Rust 侧

| 动作 | 路径 | 说明 |
|---|---|---|
| **新增** | `snow-crates/crates/snow-linux/` | 替代 `snow-macos` 的平台 crate：X11/Wayland 屏幕、音频、光标、输入、文字。**已建**：`geometry`/`pixels`/`session`/`capture`/`window`/`pointer`（纯逻辑，任何平台可单测）+ `input::{keys,tracking}`（纯逻辑：keysym→虚拟键映射、键码表、点击/区域状态机）+ `text::{font,raster}`（纯逻辑：字体发现 + 键帽光栅化，`ab_glyph`/`ttf-parser`）+ `x11`/`display`/`cursor`/`input::x11`/`x11::{window,shm,pointer}`（X11，`x11rb` 纯 Rust 实现）。待补：PipeWire/Wayland capture、evdev 退路。**音频按决策 4 不做** |
| **新增** | `snow-crates/crates/snow-linux-c/`（`include/snow_linux.h`） | Rust↔C++ 平台桥：C ABI 头 + `staticlib`。**已建**（见 9.4.8）：`create`/`destroy` + 指针读写。刻意与 `snow_rust_ffi` **分成两个 archive** —— 后者经 `zstd-sys` 撞 C 源码，跨目标检查在依赖图里就断了，走不到这段 Linux-only 代码；这个 archive 只依赖纯 Rust 的 `snow-linux`，因此每次 `bash scripts/linux-cross-check.sh` 都覆盖它 |
| **新增** | `snow-crates/crates/snow-capture/src/platform/linux.rs` | **X11 已落地**（`LinuxBackend` + `X11Capturer`，见 9.4.3）：显示器枚举 / 主屏 / `MonitorLayout` / `inspect_target` + 全屏与区域采集。待补：PipeWire + XDG portal（Wayland）、X11 Damage 脏矩形、XShm 零拷贝 |
| **新增** | `snow-crates/crates/snow-audio-recorder/src/platform/linux.rs` | ~~PulseAudio / PipeWire~~ **不做（决策 4）**：保持 `UnsupportedBackend`，Linux 录制无音轨 |
| **新增** | `snow-crates/crates/snow-cursor/src/platform/linux.rs` | XFixes `GetCursorImage` + 核心 `QueryPointer`。**已完成**（含 image 缓存的瞬时失败保护，与 Windows 后端同策略） |
| **新增** | `snow-crates/crates/snow-ui-selector/src/linux.rs` | **窗口级已落地**（见 9.4.6）：X11 窗口树 + `_NET_CLIENT_LIST`，`ElementRegionService` 在 Linux 上导出，`snow-ui-selector-c` 恢复可编译。控件级仍需 AT-SPI2（D-Bus）——未实现，`HitTestMode::UiElement` 与 `Window` 目前返回同一个窗口矩形 |
| **新增** | `snow-crates/crates/snow-selected-text/src/platform/linux/` | AT-SPI2（D-Bus）取选中文本 + PRIMARY selection。未开始 |
| **修改** | `snow-recording-effects/src/{keyboard_hook,mouse_hook}.rs` 的 `#[cfg(not(any(windows,macos)))]` 空实现 | **已落地 X11 实现**（见 9.4.4）：`#[cfg(target_os="linux")] mod platform` 把 `snow_linux::input` 的观测翻译成 `KeyObservation` / `MouseClickObservation` / `MouseMovement` |
| **修改** | `snow-recording-effects/src/keyboard_rasterizer.rs:547` 的 `#[cfg(not(any(windows,macos)))]` 桩 | **已落地**（见 9.4.5）。**注意**：该文件不是纯逻辑 —— 每个平台各自实现 `mod platform`（Windows 是 DirectWrite+D2D，macOS 是 CoreText+Quartz），Linux 分支原先是硬返回 Err ⇒ 按键能采到但画不出来。现已接 `snow_linux::text` |
| **修改** | `snow_rust_ffi/{Cargo.toml,src/lib.rs}` | `snow-ui-selector-c` 的依赖与 `pub mod ui_selector` 门控从 `cfg(any(windows, target_os="macos"))` 放宽到 `cfg(any(windows, unix))`。不改的话，Linux 上 C++ 那头 `snow_ui_selector_service_*` 会在链接期全部 undefined。**本机无法验证**：该 archive 在 Linux 目标上连 `cargo check` 都过不去（见 9.4.2 A 类），只能到真机验 |
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
| 屏幕采集 | DXGI Duplication / WGC | ScreenCaptureKit | **X11 `GetImage` + MIT-SHM —— 已落地**（见 9.4.3 / 9.4.7）；**PipeWire**（Wayland）仍缺 |
| 窗口采集 | WGC per-window | SCK per-window | PipeWire `restore_token` / X11 Composite |
| 防截屏排除 | `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` | `NSWindowSharingNone` | **无等价**，X11 用 shape/`XComposite` 近似；建议降级说明 |
| 全局快捷键 | `RegisterHotKey` | Carbon/CGEventTap | **X11 `XGrabKey`** / Wayland `GlobalShortcuts` portal |
| 全局鼠标 | `WH_MOUSE_LL` | `CGEventTap` | **XInput2 raw events —— 已落地**（see 9.4.4）；无 XInput2 时诚实上报不可用 |
| 窗口 / 元素拾取 | UIA / MSAA | `AXUIElement` + `CGWindowList` | **X11 窗口树 + `_NET_CLIENT_LIST` —— 已落地窗口级**（见 9.4.6）。控件级需 AT-SPI2，仍缺 |
| 键帽文字渲染 | DirectWrite + Direct2D | CoreText + Quartz | **`ab_glyph` + `ttf-parser` —— 已落地**（见 9.4.5）。字体发现自建枚举（族名 → 文件），**刻意不链 fontconfig / FreeType**，以保住跨目标类型检查 |
| 物理光标位置 | `GetPhysicalCursorPos` | `CGEventGetLocation` | **XQueryPointer / XWarpPointer —— 已落地**（见 9.4.8）。不用 XFixes：这两个是核心协议请求，缺扩展的服务器也能用 |
| 音频采集 | WASAPI | CoreAudio | ~~PulseAudio / PipeWire~~ **不做（决策 4）**：Linux 录制无音轨 |
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
| **P3 Rust 平台后端** *(进行中)* | 补齐 L2/L3 平台能力 | `snow-linux` crate；capture(X11→PipeWire)、cursor、audio、input hooks、text、window picking | `cargo test -p snow-capture` 在 UOS 通过；能截到主屏。**已达成（slice 1–6）**：`snow-linux`（X11 会话 / RandR / 光标 / `GetImage` 解码 / XInput2 输入观测 / `ab_glyph` 键帽光栅化 / X11 窗口树拾取 / MIT-SHM 零拷贝采集）+ `snow-cursor` Linux 后端 + `snow-capture` X11 后端 + `snow-recording-effects` 键鼠观测与键帽渲染 + `snow-ui-selector` Linux 窗口级 provider，双架构跨目标类型检查全绿、纯逻辑单测在 Windows 上跑。**剩余**：PipeWire/Wayland、AT-SPI2 控件级、Damage 脏矩形（**音频按决策 4 不做**） |
| **P4 C++ 平台层** *(进行中)* | 主程序平台层 | `snow_shot/src/platform/linux/*`；3 个 dispatcher 增分支；`screenshotnative.h` 重构 | 截图→标注→贴图闭环在 X11 会话可用。**已起步（slice 1）**：`snow-linux-c` 平台桥建立 + 物理光标 X11 落地（见 9.4.8）。**剩余**：单实例、`screenshotnative.h` 拆分（全局快捷键 slice 2 → 9.4.9；全屏窗口检测 slice 3 → 9.4.10；全局鼠标手势 slice 4 → 9.4.11；自启动 XDG autostart slice 5 → 9.4.12；桌面通知 `org.freedesktop.Notifications` slice 6 → 9.4.13；托盘 StatusNotifierItem slice 7 → 9.4.14；首页卡片拖拽在 X11 上明确不实现，理由见 9.4.11 第 7 条） |
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

31 个 workspace 成员 × 2 架构 = 62 次检查，**首跑 46 通过（每架构 23/30）、当前 50 通过（每架构 25/31）**，9.4.1 的脚本已固化为
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
>
> **2026-10-17 复跑（slice 5 之后）**：**48/60**（每架构 24/30），**B 类清零**。新增通过的两项就是
> `snow-ui-selector` 与 `snow-ui-selector-c` —— 它们此前是同一个根因的两面（provider 未导出 /
> C 层用不到）。A 类仍是每架构 6 个、逐条未变（`snow-ocr-process` 在两个架构上撞的是不同依赖，
> 见下表）。**这就是本片选 X11 窗口树而不是直接上 AT-SPI2 的实际收益：一次把 2 个门禁项转绿，
> 且没有新增任何 C 依赖。**
>
> **2026-10-17 复跑（slice 6 之后）**：仍是 **48/60**，失败清单逐条未变。新增的两项依赖
> （`x11rb` 的 `shm` 特性、`memmap2`）都不含 C 源码，所以门禁范围没有任何变化 ——
> 这是选 `CreateSegment` + `mmap` 而不是 SysV `shmget` 的直接后果。

> **2026-10-17 复跑（P4 slice 1 之后）**：**50/62**（每架构 25/31）。分母从 60 变成 62 是因为
> `snow-linux-c` 进了 workspace —— 它自己在两个架构上都 PASS，所以这是一次**纯增量**的覆盖，
> 不是通过率被稀释。A 类仍是每架构 6 个、逐条未变（`snow-ocr-process` 在两个架构上撞的都是
> `clipper-sys` 要 `*-linux-gnu-g++`）。**新增的门禁价值**：这段 C++ 直接调用的代码以前根本
> 不存在，现在它每次都在 Windows 上被类型检查。

> **2026-10-20 复跑（P4 slice 2 之后）**：仍是 **50/62**（每架构 25/31），失败清单逐条未变。
> 本片新增的是 `snow-linux::hotkey`（纯逻辑）与 `snow-linux::x11::hotkey`（`x11rb` 的
> `grab_key` / `get_modifier_mapping` / `query_keymap`，全在已启用的 `xproto` 里），
> 以及 `snow-linux-c` 的六个新导出 —— 没有新增 workspace 成员，也没有新增 C 依赖，
> 所以分母不动、通过项不动，属于**纯代码增量**。

> **2026-10-20 复跑（P4 slice 3 之后）**：仍是 **50/62**（每架构 25/31），失败清单逐条未变
> （仍是那 6 个 A 类 crate × 2 架构）。本片新增的是 `snow-linux::fullscreen`（纯逻辑）与
> `snow-linux::x11::fullscreen`（`get_property` / `query_tree` / `get_geometry` /
> `translate_coordinates` / `get_input_focus`，全在已启用的 `xproto` 里）——
> 没有新增 workspace 成员，也没有新增依赖，分母与通过项都不动。

> **2026-10-20 复跑（P4 slice 4 之后）**：仍是 **50/62**（每架构 25/31），失败清单逐条未变
> （仍是那 6 个 A 类 crate × 2 架构）。本片新增的是 `snow-linux::globalmouse`（纯逻辑）、
> `snow-linux::x11::keymap`（从 `x11/hotkey` 提出来的表，没有引入新请求）、
> `snow-linux::x11::globalmouse`（`grab_button` / `ungrab_button`，同样在已启用的 `xproto` 里）
> 与 `snow-linux-c` 的五个新导出 —— 没有新增 workspace 成员，也没有新增依赖。

**(A) 需要交叉 C 编译器 —— 不是我们的代码问题（每架构 6 个）**

| crate | 卡住的 C 依赖 | 说明 |
|---|---|---|
| `rapid-ocr-rs` | `ring` | rustls 链引入 |
| `snow-ocr-process` | `clipper-sys` | 要 `*-linux-gnu-g++`（两个架构都一样） |
| `snow-recording-c` / `snow-recording-export` / `snow-recording-runtime` / `snow-screen-recorder` | `ffmpeg-sys-next` | FFmpeg 绑定；真机上还要 `pkg-config` 找 `libav*-dev` |

这一类的失败**与 Linux 移植无关**：同一份代码在 UOS 真机上只要有 `gcc` + `libav*-dev` 就能过。

**(B) 真实缺口 —— ~~必须在 P3 里补（每架构 1 个）~~ 已于 slice 5 清零（每架构 0 个）**

| crate | 报错 | 根因 | 处置 |
|---|---|---|---|
| ~~`snow-ui-selector-c`~~ | ~~`E0432 unresolved import snow_ui_selector::ElementRegionService`~~ | 该类型只在 `#[cfg(windows)]` / `#[cfg(target_os="macos")]` 下导出；`snow-ui-selector` 尚无 Linux provider | **已修（slice 5，见 9.4.6）**：新增 `snow-ui-selector/src/linux.rs` 导出 `ElementRegionService`，走 X11 窗口树。原判断「不放假桩」保留 —— 当时塞一个"返回空元素集"的实现会让 C 层以为无障碍"可用但没结果"，比编译失败更危险；本片给的是**真实现**（窗口级），不是空元素集 |

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

**性能**：同步往返 + 服务端拷贝的形态已在 slice 6 改掉——像素经 MIT-SHM 直接写进双方
共享的内存，回复里只剩下深度与字节数（见 9.4.7）。**仍缺**的是脏矩形：`is_duplicate` 恒为 false，
Windows 侧靠 DXGI 脏矩形跳过编码的优化在这里没有对应物；X11 的等价物是 Damage 扩展，属后续 slice。

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

#### 9.4.6 P3 slice 5：X11 窗口级拾取落地（2026-10-17）

slice 1–4 让 Linux「看上屏、看见手、画得出键帽」，这一片补的是**指针指向谁**：
`snow-ui-selector` 在 Linux 上第一次有了 `ElementRegionService`，`snow-ui-selector-c` 随之恢复可编译。
这就是 9.4.2 里唯一一个 B 类门禁失败的根因，本片把它清零（46/60 → **48/60**）。

**范围是窗口级，不是控件级。** 控件级要 AT-SPI2（另一个协议、另一条总线），本片没做，
也不假装做了 —— 见下面第 5 条判断。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 规则 | `snow-linux/src/window.rs` | **全平台** | `WindowKind` / `classify`（`_NET_WM_WINDOW_TYPE` 折叠）/ `selectable`（可选中判据）/ `DesktopWindows::hit_test`（叠放序命中）。**10 个单测在 Windows 上跑** |
| 对话 | `snow-linux/src/x11/window.rs` | linux | `desktop_windows_for`：`_NET_CLIENT_LIST` → 逐窗口 `GetWindowAttributes` + `_NET_WM_WINDOW_TYPE` + `GetGeometry` + `TranslateCoordinates` → 根坐标 frame 矩形 |
| 接线 | `snow-ui-selector/src/linux.rs` | linux | `ElementRegionService`：与 macOS provider 同形的 API（`new` / `with_backend*` / `refresh*` / `release_cache` / `window_snapshot` / `from_snapshot` / `query`），`!Send`/`!Sync` |

五个必须写下来的判断：

1. **`_NET_CLIENT_LIST` 是自下而上，命中测试必须倒着找**。这是本片最容易写错、且**错了不会崩**的地方：
   正序遍历命中第一个，拿到的是最底层窗口 —— 表现为"选择器总指向后面那个应用"，看起来像
   业务逻辑 bug 而不是遍历方向 bug。所以规则模块的文档注释和单测都把这条写在最前面
   （`the_topmost_window_wins` 用两个**完全重合**的矩形来断言：叠放序相同则无法靠几何区分）。
2. **`TranslateCoordinates` 给的是内区原点，边框要自己补回来**。X11 的 `GetGeometry` 里
   `width`/`height` 是内区尺寸，`border_width` 单独给出；而用户看到的、他要截的那块是**含边框**的外框。
   两者不一致时框会整体向内缩一圈，且窗口越有边框偏得越多。故 `bounds = (dst − border, w + 2·border)`。
   `translate_coordinates` 的 `same_screen == false` 一律丢弃候选：那表示窗口在另一个 screen 上，
   在本 screen 的根坐标里没有位置。
3. **属性的三种状态必须分开**：「读失败（窗口没了）」「属性不存在（`type_ == NONE`）」「存在但为空」。
   - `GetWindowAttributes` / `GetGeometry` / `TranslateCoordinates` 失败 ⇒ **丢弃这一个候选**，不让整次
     枚举失败。桌面在任何时刻都有应用被关掉，一次瞬时竞态不该让选择器整体报错。
   - `_NET_CLIENT_LIST` **属性不存在**才回退到 `QueryTree` 遍历根窗口子窗口（reparenting WM 下的
     frame 窗口）；**存在但为空**是窗口管理器在说"没有受管窗口"，此时回退会把 frame 当成窗口，是反的。
   - `_NET_WM_WINDOW_TYPE` 不存在 ⇒ `Normal`。该属性是**可选的**，绝大多数应用根本不设；
     若把"没写类型"当成"不可选中"，等于把几乎所有程序排除在外。
4. **`_NET_WM_WINDOW_TYPE` 与 `override_redirect` 两道过滤都要**，因为它们描述的是不同的东西。
   菜单/工具提示通常 `override_redirect`，于是**根本不会出现在 `_NET_CLIENT_LIST` 里**（受管窗口才进）；
   但回退路径读的是根窗口子窗口，那时它们就在。反过来，dock/desktop 是受管窗口、会进列表。
   单测 `only_plain_application_windows_are_selectable` 把七种"看似窗口但不可选"的形态列了一遍
   （不可见 / override-redirect / `InputOnly` / 零面积 / 桌面 / 面板 / 瞬态），逐条断言被拒。
5. **`HitTestMode::UiElement` 与 `Window` 返回同一个窗口矩形 —— 这是有意的，且写进了模块文档**。
   没有控件级 provider 时，把窗口矩形当控件矩形返回会让调用方以为拿到了控件；
   而返回"空元素集"会让 C 层以为无障碍**可用但没结果**（比编译失败更危险，与 9.4.2 原来的判断一致）。
   所以只承诺窗口级，并在 `query` 的文档里写明"需要控件时应把本 provider 当窗口级退路"。

**其余刻意的取舍**：

- **点落在窗口之间的空隙时报根窗口矩形**，不返回空 path。空 path 在 C ABI 里是 `path: None`，
  上层当作"这次没选中"；而 macOS provider 在同样情形返回显示器矩形。两者行为保持一致，
  用户在裸桌面上点一下也能框到一块区域。
- **`Point::window_id` 优先**：UI 线程已经用 `QueryPointer` 解析过命中窗口时，以它的答案收窄搜索范围。
  两次遍历的时差可能让重叠窗口的结论不一致，让先解析的一方说了算，比让两边各自猜更稳。
- **属性读取上限 `MAX_PROPERTY_ITEMS = 1024`**：一次 hover 里读几千项的窗口列表不是任何真实桌面，
  上限只是防止一个畸形属性把一次 hover 变成大分配。
- **`release_cache` 只清快照**：X11 provider 不持有长生命周期的服务器对象，连接随快照一起丢掉；
  这与 macOS provider 需要显式释放 AX 对象不同，因此"释放"在这里就是丢弃 `WindowSnapshot`。

**验证（2026-10-17，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux` | **101 通过**（slice 4 后是 91，本片 +10 = `window` 全部） |
| `cargo test -p snow-ui-selector` | **50 通过**，无回归 |
| `cargo check -p snow-ui-selector -p snow-ui-selector-c --all-targets` × {windows, x86_64-linux, aarch64-linux} | 三目标全绿（`-Dwarnings` 生效） |
| **反向验证**：往 `linux.rs` 末尾注入 `compile_error!` 后重跑 linux 目标检查 | 构建**如期失败** —— 证明该文件在 linux 目标下确实参与编译，而不是被 cfg 掉后"检查通过" |
| `bash scripts/linux-cross-check.sh`（双架构） | **48/60**（每架构 24/30）；`snow-ui-selector` 与 `snow-ui-selector-c` 均 **PASS**，B 类清零 |

反向验证这一步不是形式主义：`snow-ui-selector` 的 Linux 面全部在 `#[cfg(target_os = "linux")]` 之后，
如果 cfg 名写错（例如写成 `target_os = "linux "`），跨目标检查会**静默通过**，而真机上照样编译不过。
注入一个必然失败的语句，是把"这条路径真的被编译了"从假设变成事实的最便宜办法。

**本片未覆盖（诚实记账）**：

- **控件级拾取（AT-SPI2）**：`HitTestMode::UiElement` 拿不到按钮/输入框。已完成可行性探针：
  `zbus 6.0.0`（`--no-default-features --features default-rt,proxy`）能在 Windows 上为
  `x86_64-unknown-linux-gnu` 交叉编译通过、依赖树无 C 源码 ⇒ **不会破坏纯 Rust 铁律**。
  但代价是 1000+ 行协议代码 + 「在 Windows 上无法验证行为」—— 属于独立切片。
- **非矩形窗口的形状**：`Shape` 扩展裁剪过的窗口，这里按外接矩形处理，会多框一圈透明区域。
- **多 screen 的老式 X 配置**（`Xinerama` 之前的 `screen 0/1`）：跨 screen 的窗口被直接丢弃，不折算坐标。
- **窗口的最小化状态**：靠 `IsViewable` 判断（最小化窗口通常已 unmap）；不额外读
  `_NET_WM_STATE_HIDDEN`。
- **XWayland 下的缩放**：XWayland 会把逻辑坐标与物理像素的关系交给合成器，本层拿到的仍是
  X 空间坐标；Wayland 会话下的实际映射需真机验证。
- **真机待验**：UOS 20 的 `dde-daemon` / `dde-dock` 是否导出 `_NET_CLIENT_LIST`（不导出则走
  `QueryTree` 回退，选中粒度变成 frame 窗口）、以及 `_NET_WM_WINDOW_TYPE` 在国产应用上的实际设置率。

#### 9.4.7 P3 slice 6：MIT-SHM 零拷贝采集（2026-10-17）

slice 2 的 X11 采集能出画面，但每帧的像素要穿过整条连接：服务端分配回复、拷进去，客户端再拷出来。
1920×1080 一帧 ≈ 8 MB，三份拷贝，按录制帧率重复。本片把这条路换成 MIT-SHM：
服务端直接写进双方共享的内存，回复里只剩深度与字节数。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 尺寸规则 | `snow-linux/src/capture.rs::shm_segment_size` | **全平台** | 按服务端的 `scanline_pad` 规则算段大小，溢出/退化返回 `None`。**4 个单测在 Windows 上跑** |
| 对话 | `snow-linux/src/x11/shm.rs` | linux | `Segment`（`CreateSegment` 取 fd + `mmap`）、`prepare_shm`（分配/按需扩大）、`read_shm_image`（`ShmGetImage` + 用回复的 `size` 反推 stride） |
| 接线 | `snow-linux/src/x11.rs::capture_image` | linux | 先 `prepare_shm`（`&mut`）再 `read_image`（`&self`）；后者优先读共享段，不可用则 `GetImage` |

**为什么这仍然是"纯 Rust"且不缩门禁**：`CreateSegment`（MIT-SHM 1.2）由服务端分配段并把 fd 传回来，
客户端只用 `memmap2` 映射（它本身也不含 C 源码）。换成经典的 SysV `shmget` + `Attach` 也能做，
但那要直接调 libc，且段的生命周期要自己用 `shmctl(IPC_RMID)` 管；`CreateSegment` 的段随连接消失。

五个判断：

1. **`ShmGetImage` 没有回复数据，所以必须先分配再知道大小**。这是与 `GetImage` 最根本的差别：
   后者可以「读回复 → 从长度反推 stride」，前者必须在发请求前就算出字节数，
   算小了服务端就写越界。故用 `shm_segment_size`（与服务端同一套 padding 规则）估算，
   再用回复里的 `size` 反推真实 stride（复用 slice 2 的 `reply_stride`）——**不信任自己的估算**。
2. **失败是连接级属性，不是瞬时故障**。远程 TCP 显示、无 MIT-SHM 的服务器、拒绝映射 fd 的沙箱，
   这三种情况都不会因为下一帧而改变。所以第一次失败就把会话标成 `Unavailable` 并按 `GetImage` 走到底；
   若每帧重试，等于每帧多一次往返去确认同一件事。
3. **越界一律不读**。服务端已经在回复到达前写完了，所以 `size > 段长` 时数据可能已经越界；
   该帧直接交给 `GetImage`（它带回长度可校验）。另外 `mmap` 按页映射，小幅越界通常落在同一页里
   不会崩 —— 但这是"没崩"，不是"正确"，不能拿来当保证。
4. **准备与读取必须拆成两个函数**。分配需要 `&mut self`，而读取要把映射里的切片借出去；
   一个方法同时做两件事，就会出现「一条路径返回自身借用、另一条路径继续用自身」——
   可变借用的生命周期被返回类型钉死到整个函数体，编译器直接拒绝。这个拆分是编译器逼出来的，
   不是风格选择。
5. **`capture_access_active` 仍为 `false`**：共享段不是"采集访问句柄"，释放它并不会让别的程序
   重新获得采集能力。语义没变，就没有理由跟着改。

**验证（2026-10-17，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux` | **105 通过**（slice 5 后是 101，本片 +4 = `shm_segment_size`） |
| `cargo check -p snow-capture --target {x86_64,aarch64}-unknown-linux-gnu` | 双架构全绿（`-Dwarnings` 生效） |
| `bash scripts/check-rust.sh snow-linux`（fmt + clippy `-D warnings`） | 绿。**顺带修掉前几片遗留的 3 个 clippy 与全 crate 的 fmt 漂移**（单独一个 `style` 提交） |
| `bash scripts/linux-cross-check.sh`（双架构，slice 6 后复跑） | **48/60**，失败清单逐条未变；`snow-linux`、`snow-capture` 均 PASS |

**必须说清楚的一点**：这一片**在 Windows 上无法验证行为，只能说类型正确**。零拷贝是否真的更快、
`ShmGetImage` 在 UOS 的 Xorg 上是否按 `size` 报告长度、以及 `CreateSegment` 在 dde 会话下是否被允许，
都必须在真机上测。手册层面的依据是 MIT-SHM 规范与 Xorg 的实现，不是本机的观测。

**本片未覆盖（诚实记账）**：

- **Damage 脏矩形**：`is_duplicate` 仍恒为 false，录制器无法跳过未变化的帧。Damage 是纯协议
  （`x11rb` 的 `damage` 特性，零 C），但"根窗口的 Damage 在无 Composite 重定向时是否覆盖
  遮挡变化"必须真机确认，属独立 slice。
- **窗口采集**：仍 `BackendUnavailable`。读别的窗口像素需要 Composite 重定向，而**有合成器运行时
  重定向会被拒（BadAccess）**，届时要走合成器的截图 portal 或从根图像裁剪。这是设计决策，不是补丁。
- **合成器的 `XCompositeNameWindowPixmap` 路径**、`XShmPutImage`（本工程不往屏幕写像素，用不到）。
- **真机待验**：UOS 20 / Deepin 23 上 `CreateSegment` 的可用率、1440p 下每帧耗时与 CPU 占用
  相对 `GetImage` 的实际差距。

#### 9.4.8 P4 slice 1：Rust↔C++ 平台桥 + 物理光标（2026-10-17）

P4 的第一个问题不是"写什么"，而是**写在哪里才能被验证**。C++ 侧在这个仓库里**一行都没被编译过**
（本机没有 Linux C++ 交叉工具链，也没有 Qt5 的 Linux 头），所以如果直接把 Win32 调用逐条翻译成
Xlib 调用，产出的是一批**只能靠眼睛读的代码**。本片的取舍是：把逻辑压到纯 Rust 里（可跨目标类型检查
+ 可在 Windows 上跑单测），C++ 只剩转发，并**先用一个最小能力把桥建立起来**，后续 P4 各项复用同一座桥。

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 坐标规则 | `snow-linux/src/pointer.rs` | **全平台** | `RootPosition::new`（i32 → i16 越界即 `None`）、`PointerSample::position`（`same_screen == false` ⇒ 无位置）。**6 个单测在 Windows 上跑** |
| 对话 | `snow-linux/src/x11/pointer.rs` | linux | `pointer_sample`（`QueryPointer`）、`warp_pointer_to`（`WarpPointer` + 显式 `flush`） |
| C ABI | `snow-crates/crates/snow-linux-c/`（`include/snow_linux.h`） | linux | 进程级连接 + 空句柄；`create`/`destroy`/`pointer_position`/`set_pointer_position`。**整 crate 在 `#![cfg(target_os = "linux")]` 之后**，非 Linux 上导出为空 |
| 接线 | `snow_shot/src/platform/physicalcursor.cpp` | linux | `#elif defined(Q_OS_LINUX)` 分支，形状与 macOS 分支同构；CMake 用遍历把所有编译该文件的 target 都链上 `snow_shot_linux_platform` |

**为什么非要另开一个 archive**（实测依据，不是偏好）：把这段代码塞进 `snow_rust_ffi` 是更"自然"的做法，
但本机实测那个 archive 的 Linux 目标 `cargo check` 在 `zstd-sys` 就断了（A 类，需 C 交叉编译器），
**永远走不到我们的代码**。也就是说：塞进去 = 写了但没有门禁。新 archive 只依赖纯 Rust 的 `snow-linux`，
每次 `bash scripts/linux-cross-check.sh` 都会真的编译它。代价是 C++ 侧多链一个静态库。

六个判断：

1. **句柄不持有连接**。Qt 在热路径上构造临时 `PhysicalCursor`（`globalcanvascontroller.cpp:68` 与 `:387`
   都是 `platform::PhysicalCursor().logicalPosition()`），若一实例一条连接，就是**每次鼠标移动做一次
   X11 握手**。所以连接放进 `OnceLock<Option<Mutex<X11Session>>>`，句柄只是一个能力令牌 +
   以后挂 per-caller 状态的地方；`create` 返回 NULL 就等于"这个会话不支持"。
2. **第一次尝试决定终身**。会话打不开要么是永久的（纯 Wayland 无 XWayland），要么是暂时的
   （合成器还没起来）。每次调用重试等于每次花一次握手去确认同一个答案；而应用本身没有 X 就跑不起来，
   所以这个 latch 的代价可以接受 —— 但**必须写在文档里**，不能让人以为是随机失败。
3. **`WarpPointer` 没有回复，所以必须显式 `flush`**。否则请求躺在外发缓冲里，直到**下一次需要回复的
   请求**顺手把它冲出去，光标会在一个不可预测的时刻跳。这类 bug 不报错、不复现，只是"有时候不灵"。
4. **越界要拒绝，不要 clamp、更不要 `as` 转**。root 坐标是 `i16`，而 C ABI 收的是 `int32_t`；
   `as i16` 会把 32768 变成 -32768，把光标送到**对面角落**。`RootPosition::new` 返回 `Option`，
   C 侧拿 0（失败）。同样地，也不 clamp 到屏幕边缘 —— 那会把调用方的 bug 藏在一个"只是位置不对"的
   现象后面。
5. **`same_screen == false` 必须当成"没有位置"**。`QueryPointer` 的坐标是相对**持有指针的那个 root**；
   本后端只连一个 screen，把别的 root 的坐标当本 root 的读出来，结果是**光标跳到左上角**，而不是读取失败。
6. **不提供 `readLogicalPosition`，也不合成鼠标事件**。X11 的 root 坐标是设备像素，而 logical 坐标
   依赖屏幕缩放 —— 那是 `QCursor::pos()` 已经算过的事，在这里再算一遍是"用更少信息给出第二个答案"。
   反过来，X11 的 warp 对服务器而言是**真实的 motion 事件**（与 macOS 的 `CGWarpMouseCursorPosition` 相反），
   所以 `generatesMouseMoveEvents` 保持 `true`，不需要 `QWindowSystemInterface` 合成。

**两个附带判断**：

- **C++ 分支要先看 `QGuiApplication::platformName() == "xcb"`**。Wayland 会话下即使有 XWayland，
  Qt 的 platform 仍是 `wayland`；此时 `XQueryPointer` 读的是 XWayland 自己那份指针副本，
  **指针离开 X 窗口后就不再跟随**。那是一个"过期的位置"，比"不支持"更糟。
- **CMake 的链接不能只写一处，也要防匹配器静默失效**。静态库可以留 undefined 符号，最终可执行文件不行；
  而仓库里多个测试 target 会**各自重编** `physicalcursor.cpp`。所以沿用本文件中已有的
  `canvas_status_readout` 遍历写法，但加一条**配置期自检**：一个 target 都没匹配到就 `FATAL_ERROR`。
  否则匹配器写错的表现会是链接期的 undefined symbol，出现在离原因很远的日志里。

**验证（2026-10-17，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux` | **111 通过**（slice 6 后是 105，本片 +6 = `pointer`） |
| `cargo check -p snow-linux-c` 三目标（host / x86_64-linux / aarch64-linux） | 全绿。host 上是**空 crate**（整 crate 在 `#![cfg(target_os="linux")]` 之后），Linux 上是真实现 |
| `cargo fmt --check` + `cargo clippy --all-targets`（host 与 linux target） | 双绿，`-Dwarnings` 生效 |
| **反向验证** | 往 `snow-linux-c/src/lib.rs` 末尾注入 `compile_error!` 后重跑 Linux 目标检查，**如期失败**。理由同 slice 5：整 crate 都在 crate 级 `cfg` 之后，cfg 写错会让门禁**静默通过**而真机编译出空 archive |
| `bash scripts/linux-cross-check.sh`（双架构） | **50/62**（每架构 25/31），新增的 `snow-linux-c` 双架构 PASS，A 类失败逐条未变 |

**必须说清楚的一点**：**C++ 那一侧一行都没有被编译过**。本片能保证的是
(a) 它调用的 C ABI 在两个 Linux 架构上类型正确，(b) 逻辑与坐标规则有单测，
(c) CMake 的匹配错误会在 configure 阶段大声失败，而不是在链接期。
但 `#elif defined(Q_OS_LINUX)` 分支本身是否有语法/类型错误，**只有 UOS 上的第一次构建能回答**。
这是 P4 全阶段的共同约束，不是本片的疏漏。

**本片未覆盖（诚实记账）**：

- **全屏窗口检测**、**全局鼠标 backend**、**全局快捷键（`XGrabKey`）**、托盘 / 自启动 / 单实例 / 通知、
  `screenshotnative.h` 的平台中立化 —— 都属 P4 后续 slice，桥已就位。
- **`snow_rust_ffi` 的 selector 门控放宽**（`cfg(any(windows, unix))`）：slice 5 让
  `snow-ui-selector-c` 能在 Linux 编译，但那个 archive 仍把它排除在 Linux 之外，C++ 侧
  `snow_ui_selector_service_*` 会在链接期全部 undefined。本片一并改掉，**但本机无法验证**
  （该 archive 的 Linux 目标连 `cargo check` 都过不去），只能到真机验。
- **真机待验**：`XWarpPointer` 在 dde 下是否被合成的鼠标抓取拦截（拖动中 warp 的行为）、
  多屏负坐标布局下的读数、以及 `QCursor::pos()` 与 root 坐标在 `QT_SCALE_FACTOR` 非 1 时的对应关系。

#### 9.4.9 P4 slice 2：全局快捷键（`XGrabKey`）落地（2026-10-20）

Windows 用 `RegisterHotKey`，macOS 用 Carbon 的 `RegisterEventHotKey`；X11 的等价物是**被动键盘抓取**
（`XGrabKey`）：在本连接持握期间，该 keycode + 该修饰键状态的按键只发给本客户端，别的应用收不到。
没有它，截图快捷键只在窗口有焦点时有效 —— 那等于没有。

**分层照旧：翻译规则压在纯 Rust 里，X11 交互在 `#[cfg(target_os = "linux")]` 之后，C++ 只做转发。**

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 翻译规则 | `snow-linux/src/hotkey.rs` | **全平台** | Qt 键 → keysym；keysym → 可用的 keycode（含隐含 Shift）；修饰键组合 → 抓取掩码集合；修饰键映射 → Alt/Super 的 X11 mask。**18 个单测在 Windows 上跑** |
| 抓取服务 | `snow-linux/src/x11/hotkey.rs` | linux | 独立连接 + 工作线程；`register` / `unregister` / `key_down`；`KeyPress` 匹配后回调 |
| C ABI | `snow-linux-c`（`include/snow_linux.h`） | linux | `snow_linux_hotkeys_{create,destroy,register,unregister,key_down,validate_binding}` |
| 接线 | `snow_shot/src/platform/linux/globalshortcutbackend.cpp` | linux | `GlobalShortcutBackend` 的 Linux 实现；工厂 `createPlatformGlobalShortcutBackend()` 增 `#elif defined(Q_OS_LINUX)` |
| CMake | `snow_shot/CMakeLists.txt` | linux | `SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES` 增 `elseif(UNIX)` 分支；文末链接遍历增一条匹配器 + **自检 `FATAL_ERROR`** |

八个判断（每条都对应一个会静默出错的坑）：

1. **Qt 键值 → keysym 必须自己建表，不能凭记忆**。Qt 没有公开的 keysym 反查表。本片把 Qt 侧取值
   从 `qnamespace.h`（5.15.2 与 6.12.0 实测一致）读出、X11 侧从 xorgproto 的 `keysymdef.h` /
   `XF86keysym.h` 读出后逐条写死。**凭记忆写的初稿错了 6 处**（`Hyper_L` 写成 `Meta_R`、
   `MediaRecord` 指向了 `XF86Mail`、`PowerOff`/`WakeUp`/`Eject`/`ScreenSaver`/`Calculator` 全偏），
   而这类错误的表现是「按了快捷键弹出邮件客户端」—— 不像 bug，像灵异。
2. **Qt 5.15 把两个媒体键的常量写少了一位**：`Key_MediaPause = 0x1000085`、`Key_MediaTogglePlayPause
   = 0x1000086`（Qt 6 已修正为 `0x01000085/86`）。Linux 发的是 Qt5，所以表里**两种拼写都接受**。
3. **字母必须做大小写折叠**。Qt 的 `Key_A` 是 `0x41`（= `XK_A`），但 X 键盘映射把 `XK_a` 放在
   **未按 Shift 的那一层**。只查 `0x41` 会命中 shifted 层，于是 `Ctrl+A` 被悄悄注册成
   `Ctrl+Shift+A` —— 能注册、永远不触发。折叠到小写 keysym 才能命中 unshifted 层；
   同理 `+` 这类字符确实在 shifted 层，此时**必须把 Shift 加进抓取掩码**。
4. **抓取要覆盖所有锁定键组合**。`XGrabKey` 的修饰键匹配是**精确匹配**，只抓 `Control` 的话，
   CapsLock 一开快捷键就失效。按键盘上实际存在的锁定修饰键（`Lock` + 实测到的 NumLock /
   ScrollLock 掩码）做幂集，最多 8 个抓取。
5. **Alt / Super 的 X11 mask 要读回来**。协议只定义 `Mod1..Mod5`，谁是谁由布局决定。从
   `GetModifierMapping` + 键盘映射反查 `Alt_L/R`、`Super_L/R`，查不到才退回 `Mod1`/`Mod4`。
   写死 `Mod1` 在某些布局上会「注册成功但永不触发」。
6. **一个组合只能属于一个注册**。同一客户端对同一组合抓两次 X 是允许的，但那样一次按键会触发
   两个动作；Windows 的第二次 `RegisterHotKey` 会返回「已被占用」，所以这里也回
   `ALREADY_IN_USE`，让两端的提示一致。
7. **失败必须整条回滚**。多个 keycode（多布局）时若第 3 个抓取失败而前两个留着，绑定会变成
   「CapsLock 关着能触发、开着不能」这种「偶发」现象。失败即 ungrab 已发的全部。
8. **回调不能直接摸 Qt**。`KeyPress` 在 Rust 工作线程上，C++ 侧用 `QMetaObject::invokeMethod`
   投递到 Qt 线程，且以 `ActivationDispatcher` 作 context 对象 —— QObject 析构时 Qt 会丢弃
   尚未执行的 metacall，这正是「已经投递但后端刚被销毁」这一类崩溃的解药。同理
   `snow_linux_hotkeys_destroy` 会 join 工作线程后才返回，所以 `user_data` 不会在析构后被动到。

**另外补了一道本机门禁（这是本片真正的可验证性来源）**：`src/platform/linux/*.cpp` 在 Windows 构建里
一行都不会被编译，但它的 Qt 用法是平台中立的。于是
`bash scripts/syntax-check-linux-cpp.sh` 用 `cl -Zs`（只做前端，不产出目标文件）
加 `-DQ_OS_LINUX` 把这批文件过一遍 `-W4 -permissive-` 语法/类型检查。
**反向验证过**：注入一个改名的函数调用后如期报 `C3861`，删掉即恢复 `SYNTAX OK`。
这让「C++ 侧只能靠眼睛看」的范围缩小到只剩 ABI 与运行期行为。

**验证（2026-10-20，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux --lib hotkey` | **18 通过**（新增，全部纯逻辑） |
| `cargo check -p snow-linux -p snow-linux-c` 双架构 | 全绿，`-Dwarnings` 生效 |
| `cargo clippy --all-targets` / `cargo fmt --check` | 双绿 |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（+ 反向验证会红） |
| `bash scripts/linux-cross-check.sh` 双架构 | 见 9.4.2 的复跑行，分母再次 +0（本片没有新增 workspace 成员） |

**本片未覆盖（诚实记账）**：

- **Wayland 会话下不支持**：`QGuiApplication::platformName() != "xcb"` 时后端整体降级为
  「平台不支持」，与物理光标分支同一判据。等 P6/P7 的 portal（`org.freedesktop.portal.GlobalShortcuts`）
  再接。
- **真机待验**：dde 是否把某些组合先截走（`XGrabKey` 会返回 `BadAccess`，此时 UI 上应显示
  「已被占用」）；多布局下同一键产生多个 keycode 的实际行为；长按重复触发频率与 Windows 的差异；
  `QT_SCALE_FACTOR` 非 1 时 `inputState` 的读数（与缩放无关，但需实测确认）。
- **全局鼠标 backend / 全屏窗口检测 / 托盘 / 自启动 / 单实例 / 通知** 仍是 P4 后续 slice。

#### 9.4.10 P4 slice 3：全屏窗口检测（2026-10-20）

两个产品行为依赖它：**全屏应用获焦时暂停全局快捷键**（`GlobalShortcutManager`），**全屏时收起悬浮工具条**
（`FloatingToolbarController`）。Windows 用「前台窗口 + `DWMWA_EXTENDED_FRAME_BOUNDS` 等于某台显示器矩形」，
macOS 用 AX 的窗口列表；X11 的等价物是「`_NET_ACTIVE_WINDOW` 的顶层框架矩形等于某台 RandR 显示器矩形」。

**分层照旧，本片新增的都是「查什么 + 怎么比」的规则：**

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 判定规则 | `snow-linux/src/fullscreen.rs` | **全平台** | `covers_display`（±1px 容差）、`focused_is_fullscreen`（种类/可视图/空矩形过滤）、`logical_to_native`（Qt 逻辑坐标 × dpr）。**16 个单测在 Windows 上跑** |
| X11 读取 | `snow-linux/src/x11/fullscreen.rs` | linux | `_NET_ACTIVE_WINDOW`（回退 `GetInputFocus`）→ 爬到 parent 为 root 的顶层 → 顶层几何 + `_NET_WM_WINDOW_TYPE` |
| 原子复用 | `snow-linux/src/x11/window.rs` | linux | 把 `Atoms` 拆出 `TypeAtoms`（一次批量 intern），X11 拾取与全屏检测共用同一套类型原子 |
| C ABI | `snow-linux-c` | linux | `snow_linux_platform_focused_fullscreen` / `..._on_display(x,y,w,h,scale)` |
| 接线 | `snow_shot/src/platform/linux/focusedfullscreenwindow.cpp` | linux | 实现 `snow_shot::platform::{focusedFullscreenWindowExists, OnScreen}` |
| CMake | `snow_shot/CMakeLists.txt` | linux | `SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES` 的 `UNIX` 分支加第二源文件；文末链接遍历加**第三条匹配器 + 自检** |

九个判断（前三条任意一条错，症状都是「产品行为静默错位」而不是崩溃）：

1. **桌面窗口精确覆盖显示器，所以几何相等不足以判定全屏**。`_NET_WM_WINDOW_TYPE_DESKTOP` / `DOCK`
   平时就跟显示器一样大。若不先按类型排除，**空桌面上所有快捷键会失效**，而且屏幕上没有任何东西
   能解释为什么 —— 这是本片最贵的一条，已写成单测 `an_empty_desktop_is_not_fullscreen`。
2. **必须读顶层（parent == root 的祖先）的几何，不是客户区几何**。重定向窗口管理器（mutter/kwin/
   xfwm/openbox 都是）把客户窗口嵌进自己画的框架里，框架才是用户看到的边界；读客户区会**少一圈标题栏**，
   于是一个「最大化」窗口看起来比显示器矮一截、一个真正的全屏窗口看起来也差一点 —— 两侧都判错。
3. **类型属性要沿祖先链往上找**。`_NET_WM_WINDOW_TYPE` 由应用设在**客户窗口**上，框架窗口没有这个属性；
   只看顶层会把每个应用都当成 `Normal`（桌面窗口的 `DESKTOP` 也就丢了）。反过来只读焦点窗口则会在
   焦点落在子窗口时漏掉。
4. **比对的是「某一台显示器」，不是整个桌面**。跨两块屏铺满的窗口是最大化窗口，不是全屏；这条同时
   让两端判据一致（Windows 也是 `MONITOR_DEFAULTTONULL` 那台的 `rcMonitor`）。
5. **不要求 `_NET_WM_STATE_FULLSCREEN`**。Wine 下的游戏、自带尺寸的视频播放器很多不设这个属性；
   而真正设了它的窗口管理器也会把窗口改到显示器大小 —— 也就是说几何判据已经涵盖了它。
6. **焦点要有回退**。没有窗口管理器时 `_NET_ACTIVE_WINDOW` 可能根本不存在（属性 `type == NONE`），
   或者存在但为 0。此时读 `GetInputFocus`，并把 `None`(0) / `PointerRoot`(1) 视为「没有窗口」——
   这两个值是协议里的哨兵，不是窗口 id。
7. **Qt 逻辑坐标 → 根像素的换算由 Rust 做，dpr 由调用方传**。X11 根坐标是设备像素，Qt 的逻辑坐标在缩放会话下
   是它的一部分（9.4.8 的物理光标已经确立这个分工）。把乘法和舍入放进 `logical_to_native` 才能给
   1.25 / 1.5 这类分数比例写单测；放在 C++ 里就只剩真机手测。
8. **判不出来时的答案必须是「不是全屏」**。窗口可能在「读属性」和「读几何」之间被销毁（这是常态竞态），
   此时逐请求允许失败、整体返回 `false`，而不是把错误抛给调用方去猜。答错的代价是「快捷键照常工作、
   工具条照常显示」—— 功能仍在，只是没被抑制；反过来会是「快捷键在最需要的时候失灵」。
9. **Wayland 会话降级为 `false`**，判据与 9.4.9 相同（`platformName() != "xcb"`）。XWayland 下 X 服务器
   只看得见 X 客户端，焦点窗口不是用户正在看的那个应用，宁可让它答「没有全屏」。
   *（另外两处易漏的接线：共享的 `src/platform/focusedfullscreenwindow.cpp` 里的兜底桩必须同时排除
   `Q_OS_LINUX`，否则 Linux 上是**重复符号**；`SNOW_SHOT_GLOBAL_SHORTCUT_SOURCES` 加文件之后，
   文末那段「谁编译了它就得链 X11 归档」的遍历也要加匹配器，否则是**未定义符号**。）*

**顺手修掉的一个真 bug（由本片自己的极端坐标测试抓到）**：`covers_display` 初版写的是
`(lhs - rhs).abs() <= 1`。`i32::MIN` 与 `0` 相减会溢出，在 release 下绕回一个恰好在容差内的差值 ——
两个「绝不可能相等」的矩形被判为相等。改成 `abs_diff`（`u32` 语义，永不溢出）。
这类算术错误在正常多屏坐标下**永远跑不出来**，只有表格驱动的单测能抓到。

**验证（2026-10-20，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux --lib` | **147 通过**（slice 2 是 131，本片 +16，全部纯逻辑） |
| `cargo check` / `clippy --all-targets` 双架构 | 全绿，`-Dwarnings` 生效 |
| `bash scripts/linux-cross-check.sh` 双架构 | 见 9.4.2 的复跑行 |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（默认覆盖面从 1 个文件扩到 2 个；改名注入仍如期 `C3861`） |
| 共享桩文件的 Windows 分支 | `windows-qt5-smoke` **不编译** `snow_shot` 的源文件（它只开 `ant_design_qt` + `snow_image`），所以本片用等价的 `cl -Zs -W4 -permissive-` 单独过了 `src/platform/focusedfullscreenwindow.cpp` 的 Windows 分支：0 error 0 warning |

**本片未覆盖（诚实记账）**：

- **未在 X11 真机跑过**：`_NET_ACTIVE_WINDOW` 在 dde/kwin 上的刷新时机、全屏但未获焦点的窗口
  （多屏时很常见：一块屏全屏播放、另一块屏在打字）、`--scale` 参与的布局下 Qt 逻辑坐标与根像素的
  对应关系，都只能在 UOS 上确认。
- **本片没有任何「按显示器」的真机证据**：`focusedFullscreenWindowOnScreen` 的容差是按单屏场景设计的，
  跨屏混合 dpr 的落点需要真机。
- **全局鼠标 backend / 托盘 / 自启动 / 单实例 / 通知 / `screenshotnative.h` 拆分** 仍是 P4 后续 slice。

#### 9.4.11 P4 slice 4：全局鼠标手势（`XGrabButton`）落地（2026-10-20）

产品行为：**按住修饰键组合 + 拖鼠标按钮，就在桌面任意位置开始截图**（`GlobalMouseManager` → `dragEvent`
→ `ScreenshotController`）。Windows 用 `WH_MOUSE_LL` 低级钩子，macOS 用 `CGEventTap`；X11 的等价物是
**被动按钮抓取 `GrabButton`**。

**本片最核心的一条结论：这个功能必须由 X 服务器做匹配，而不是由我们观测后决定。**

| 方案 | 能不能「吞掉」按下 | 结论 |
|---|---|---|
| XInput2 raw events（`XI_RawButtonPress`） | **不能**。raw 事件按定义对投递是中立的，它只是「旁听」；应用照样收到按下并开始自己的拖拽 | 与 Windows 钩子语义不同，弃用 |
| 记录 `XI2` 普通事件 / 轮询按钮状态 | 同上，且漏掉「按下到轮询之间」的窗口 | 弃用 |
| **被动 `XGrabButton`（root 上，精确修饰键掩码）** | **能**。抓取期间该组合的按下只投递给本连接，其他客户端一个字节都收不到 | **采用** |

代价是「匹配权」交给了服务器：**落到我们事件循环里的按下，必然是某个已配置的组合**，于是 Rust 侧不需要
「这个按下是不是我的」这种运行期判断，也不需要 `XAllowEvents(ReplayPointer)` 回放 —— 这一片因此比
Windows/macOS 的后端**更简单**，代价是 C++ 侧共享的 `GlobalMouseGesture` 状态机在 X11 上不参与判断
（Windows/macOS 仍然用它）。

分层：

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 规则 | `snow-linux/src/globalmouse.rs` | **全平台** | Qt 按钮 → X 按钮号、组合展开成抓取集合、歧义组合整条丢弃。**12 个单测在 Windows 上跑** |
| 键盘表复用 | `snow-linux/src/x11/keymap.rs` | linux | 从 `x11/hotkey.rs` 提出来的 `Keymap`（键码表 + Alt/Super 掩码 + 锁掩码），快捷键与鼠标抓取共用 |
| X11 抓取 | `snow-linux/src/x11/globalmouse.rs` | linux | `GlobalMouseService`：独立连接 + 工作线程，按下/移动/松开 → `Begin/Update/Finish`，`cancel` 只让拖拽静音 |
| C ABI | `snow-linux-c` | linux | `snow_linux_global_mouse_{create,destroy,set_bindings,set_enabled,cancel}` |
| 接线 | `snow_shot/src/platform/linux/globalmousebackend.cpp` | linux | `GlobalMouseBackend` 的 Linux 实现；Wayland 会话报 `Unavailable` |
| 共享桩 | `snow_shot/src/platform/globalmousebackend.cpp` | 全平台 | `#elif defined(Q_OS_LINUX)` 分支（原本只有 Win/macOS/兜底） |
| CMake | `snow_shot/CMakeLists.txt` | linux | `snow_shot_global_mouse` 的 `UNIX` 分支加源文件；文末 X11 链接遍历**重构成清单驱动**（见下） |

七条判断（前三条错，症状同样是「行为静默错位」）：

1. **Qt 的按钮枚举和 X11 的按钮号不是一回事，而且顺序相反**。Qt 是位标志 `1/2/4/8/16`（左/右/中/后退/前进），
   X11 按协议分配顺序编号：左 1、**中 2**、**右 3**、滚轮 4–7、侧键 8/9。按位置翻译会把「右键拖拽」变成
   「中键拖拽」，且在只有左右键的配置下完全看不出来。
2. **`captureAvailable == false` 时必须撤掉抓取，而不是「忽略按下」**。忽略仍然吞掉了那一次点击
   （抓取已经激活），要还回去就得回放；而撤掉抓取是零成本的。C++ 侧 `setCaptureAvailable(false)`
   （另一次截图正在进行）就走到这条路径上。
3. **两个动作配了同一组合时，整条丢弃**。共享的 `GlobalMouseGesture` 对歧义按下的处理是「不匹配」，
   Windows/macOS 上应用照常收到点击；X11 若把抓取给了其中一个，同一份配置在两个平台行为不同。
4. **锁状态变体照旧要展开**（`grab_masks`，与快捷键同一个函数）。X11 的抓取匹配是**精确**的，
   CapsLock/NumLock 开着时 `Ctrl+左键` 的状态位多一位 —— 不展开就是「一开大写锁定，手势就失灵」。
5. **抓取失败是正常答案，不是错误**。窗口管理器会自己抓 `Super`+拖拽之类（移动窗口），
   `BadAccess` 在那里是预期结果；`set_bindings` 返回**成功装上的条数**，让调用方能看出「有几条没抢到」，
   而不是让一条冲突把其余全废掉。
6. **拖拽期间指针归本进程，指针不动的是「应用」，不是光标**。`GrabButton` 用 `pointer_mode: ASYNC`：
   光标继续跟随鼠标移动，事件进本连接；`owner_events: false` 保证事件不回流给指针下的窗口。
   于是拖拽过程中下层应用收不到任何鼠标事件（Windows 上它还能收到移动，只是没有拖拽语义）——
   这是 X11 语义带来的**更强**隔离，也是它「不会意外拖到下层窗口」的原因。
7. **`beginButtonDrag` 在 X11 上是空实现，并且写清了为什么**。这个入口服务于首页卡片的
   「按住卡片再拖到桌面」：按下**已经**投递给本程序自己的 Qt 窗口，Qt 连接持有隐式抓取，
   此时 `XGrabPointer` 会 `AlreadyGrabbed` 失败，移动事件继续进 Qt 窗口而不是本服务 ——
   结果是「报一个 Begin 然后什么都没有」。正确做法是走 Qt 自己的拖拽 + `QCursor::pos()` 跟随，
   那是另一套设计；在它落地前，卡片拖拽在 Linux 上是**明确不可用**，而不是半工作状态。

**顺手做的两处结构整理**（都在本片被真实踩到）：

- `x11/hotkey.rs` 里的 `Tables` 提成 `x11/keymap.rs` 的 `Keymap`。鼠标抓取需要**同一套** Alt/Super 掩码
  与锁掩码，复制一份意味着以后修「Alt 到底在 Mod1 还是 Mod3」要改两处。
- `snow_shot/CMakeLists.txt` 文末的「谁编译了平台源，谁就链 X11 归档」从**三条重复的 if** 重构成
  一份清单 + 两个循环：加文件现在是一行，而「没人编译它」的 `FATAL_ERROR` 自检保留。这段逻辑在
  Windows 上不执行（`if(UNIX AND NOT APPLE)`），所以本片用一个等价小工程做了**双向验证**：
  四条齐 → configure 通过；抽掉一条 → `FATAL_ERROR`。

**验证（2026-10-20，Windows + rustc 1.97.1）**：

| 检查 | 结果 |
|---|---|
| `cargo test -p snow-linux --lib` | **159 通过**（slice 3 是 147，本片 +12，全部纯逻辑） |
| `cargo check` / `clippy --all-targets` 双架构 | 全绿，`-Dwarnings` 生效 |
| `cargo fmt --check` / clang-format `--dry-run -Werror` | 干净 |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（默认覆盖面从 2 个文件扩到 3 个；把 `snow_linux_global_mouse_cancel` 改名注入后如期 `C3861` + `SYNTAX FAILED`） |
| `windows-qt5-full` configure + `snow_shot_global_mouse` | configure `EXIT=0`；该静态库 16 步全绿（共享桩的 `#elif` 分支参与编译） |
| `bash scripts/linux-cross-check.sh` 双架构 | 见 9.4.2 的复跑行 |

**本片未覆盖（诚实记账）**：

- **未在 X11 真机跑过**：`GrabButton` 在 dde/kwin 上的实际冲突情况（哪些组合已被 WM 占用）、
  侧键（8/9）在真实鼠标上的映射、拖拽期间合成器对光标的重绘。
- **没有「按下归属」的真机证据**：抓取确实让下层应用收不到按下的直接证据（例如在文本编辑器里
  用同类组合拖拽），只能在 UOS 上验证。
- **布局切换（`MappingNotify`）不刷新掩码**：与快捷键服务同一个取舍，重建服务时才重读。
- **托盘 / 自启动 / 单实例 / 通知 / `screenshotnative.h` 拆分** 仍是 P4 后续 slice。

#### 9.4.12 P4 slice 5：自启动走 XDG autostart 落地（2026-10-20）

**起点是一处假成功。** `platform::windows::AutoStartRegistration` 的每个函数都有非 Windows 兜底：
`isSupported()` 返回 false，于是设置页的开关**灰掉**；而 `reconcileStartupMode()` 的 `#else` 分支返回
`{true, false, {}}` —— 「成功」但一个字节都没写。后者是本项目反复强调的那种缺陷：调用方看到成功，
用户看到开关是灰的，中间没有任何一处说实话。

**XDG autostart 是这里唯一可用的基线。** 一个 `.desktop` 文件放在 `$XDG_CONFIG_HOME/autostart/`
（默认 `~/.config/autostart/`），桌面会话登录时读它。系统级对应 `$XDG_CONFIG_DIRS/autostart`
（默认 `/etc/xdg/autostart`），**同名时用户级优先**。这里没有 Windows 那种「一个注册表值就是全部状态」
的单一位置：用户级文件、系统级文件、`Hidden` 键三者共同决定实际行为，所以判断「是否启用」必须三处一起看。

分层：

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 纯逻辑 | `snow_shot/src/platform/linux/desktopentry.cpp` | **全平台** | `Exec` 转义与反解、条目生成、键读取、启用判定。**零文件系统访问**，因此在 Windows 上跑单测 |
| 文件与目录 | `snow_shot/src/platform/linux/autostartregistration.cpp` | linux | `$XDG_CONFIG_HOME` 解析、系统级扫描、`QSaveFile` 原子写、快照/回滚、`reconcile` |
| C++ 接线 | `main.cpp` / `settingsbackend.cpp` / `settingscatalog.cpp` | linux | 启动对账、开关读写、条目文案与可见性 |
| 测试 | `snow_shot/tests/linux_autostart_tests.cpp` | 全平台 | 纯逻辑的 20 余条断言，含「写→读」闭环 |

六条判断（前四条错了都是静默错位）：

1. **`Exec` 有两层转义，顺序不能反**。规范说 string 层的转义「在 quoting 规则之前应用」，于是：
   字面反斜杠在引号参数里要写 **四个**；字面 `$` 要写 `\$`。写少一层，路径里的反斜杠就会被
   shell 层吃掉一位。
2. **`%%` 是字面百分号，而且引号内也成立**。本条是本片测试自己抓出来的：初版按「引号内不展开
   field code，所以引号内的 `%` 不必双写」处理，结果往返失败 —— 读回来多一个 `%`。
   `%%` 属于**转义**而不是**展开**，规范对转义没有引号内例外。
3. **禁用时不能只删自己的文件**。如果打包时装了系统级条目，删掉用户级文件等于让系统级那条重新生效 ——
   用户点了「关」，重启后照样自启。规范的解法是写一个 `Hidden=true` 的存根把同名条目遮住。
4. **`X-GNOME-Autostart-enabled=false` 也必须认**。GNOME 的「启动应用程序」面板禁用时写这个键而**不**写
   `Hidden`。不认它，用户在面板里关掉、我们的开关还显示「开」。
5. **文件名跟 `productId()` 走，不跟显示名走**。文件名同时是「覆盖哪条系统级条目」的身份，
   用翻译过的显示名意味着切换语言就换了要遮的文件。
6. **比较「参数列表」而不是「文本」**。`matchesExpectedEntry()` 先用 `parseExec()` 反解再比 argv：
   打包者用不同但等价的引号写法时，不会每次启动都被判不匹配而重写。

**顺手修的两处 Linux 产品缺陷**（都是本片接线时才暴露的）：

- 设置项文案原样写着「Start Snow Shot in the background when **Windows starts**」——`autoStartItem()` 的
  `#else` 分支是 Windows 专用，被 Linux 借用。现改为三分支，Linux 用「登录桌面后启动」。
- 「Launch as administrator」与「Restart as administrator」两项在 Linux 上仍会构建出来：桌面会话没有提权
  概念，它们只能永远灰着，附带一句「需要 Administrators 组成员资格」的 Windows 解释。现改为只在
  Windows 构建；`switchEnabled` 同时对该 binding 返回 false，避免陈旧调用落到无意义的操作上。

**验证（2026-10-20，Windows + Qt 5.15.2 / MSVC 14.44）**：

| 检查 | 结果 |
|---|---|
| `snow-shot-linux-autostart-tests`（手工 `cl` 编译链接后运行） | **passed**；`-W4 -permissive-` 0 warning（STL4043 系 Qt5 头噪声除外） |
| 测试抓到的真 bug | 往返用例 `/opt/100%/snow_shot` 读回 `/opt/100%%/snow_shot` ⇒ 补 `parseExec` 的 `%%` 反解 + 引号内双写 |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（覆盖面 3 → **5** 个文件）；把 `desktopentry::parseExec` 改名注入后如期 `C2039` + `C3861` + `SYNTAX FAILED` |
| `windows-qt5-full` configure（tests 保持 OFF） | `EXIT=0`，`-- Generating done` —— 加进 `snow_shot_administrator` 的两个源文件没有破坏既有配置 |
| `windows-qt5-full` build `snow_shot_administrator` | 5 步全绿；两个新文件在 Windows 上编的是 `#else` 兜底分支，静态库链接成功 |
| Qt5 路线开 `SNOW_APPS_BUILD_TESTS=ON` | **仍不可用**，见下 |

**验证时拆掉的一层既有阻塞（并没能拆完）**：想在 Windows 上真正配置出 `snow-shot-linux-autostart-tests`
这个 CTest 目标，必须开 `SNOW_APPS_BUILD_TESTS=ON`，而 Qt5 路线在这个开关上是**从来没被走通过**的
（三档 `windows-qt5-*` 全部 `BUILD_TESTS=OFF`）。依次撞到两层：

1. `qt_import_plugins(... NO_DEFAULT ...)` —— `cmake/SnowQtVersionShims.cmake` 里那条垫片用的是
   `if(NOT COMMAND qt_import_plugins)` 守卫，而 **Qt 5.15 自带 versionless 的 `qt_import_plugins`**，
   守卫恒假、垫片根本没装；Qt 自带那版原样转发 `NO_DEFAULT` 给 `qt5_import_plugins`，后者
   `FATAL_ERROR`。已改为**无条件覆盖**（有意的例外，理由写在文件里），因为那一版对 `NO_DEFAULT`
   的处理是错的而不是风格差异；Qt5 的插件本来就是运行时加载，没有默认插件集可关，丢掉这个关键字
   是语义已满足而非降级。
2. `ant_design_qt/CMakeLists.txt` 的测试用 `$<TARGET_FILE_DIR:Qt::Core>` 拼 PATH —— Qt5 的 `Qt::Core`
   是 INTERFACE 目标而不是 alias，生成阶段直接报 "not an executable or library"（`add_test` 共 5 处）。
   **这一层没修**：它属于 Qt5 降级路线的测试基础设施，与本片无关，修它要改 5 处生成表达式并逐个验证。

因此本片的测试目标**没有被 CMake 配置验证过**：验证分成「源文件能被 MSVC 编译并链接成静态库」
（已做）与「测试逻辑正确」（手工 `cl` 编译链接后运行，已做）两段，CTest 接线留待 Qt5 测试基建打通后一并确认。

**本片未覆盖（诚实记账）**：

- **未在真实桌面会话验证**：dde（UOS 默认桌面）读 `~/.config/autostart` 的实际行为、以及它是否会在
  用户改设置后重写该文件；GNOME/KDE 的差异。
- **Flatpak / Snap 沙箱未检测**：沙箱里的 `$XDG_CONFIG_HOME` 被重定向，写进去的条目在宿主侧不生效，
  而 `isSupported()` 恒 true。打包阶段（P6）如果走 Flatpak，这里需要一条显式判据。
- **`OnlyShowIn` / `NotShowIn` / `TryExec` 不参与判定**：它们决定「这个桌面是否启动」，不是「用户是否
  要求启动」。判成「关」会让开关和我们写下的条目自相矛盾。
- **托盘 / 单实例 / 通知 / `screenshotnative.h` 拆分** 仍是 P4 后续 slice。单实例经查是纯 Qt
  （`QLocalServer` + `QLockFile`，`src/app/singleinstancecoordinator.cpp` 全文件无平台条件编译），
  在 Linux 上应当直接可用，但尚未在真机确认 socket 路径可用。

#### 9.4.13 P4 slice 6：桌面通知走 `org.freedesktop.Notifications` 落地（2026-10-20）

**起点与 9.4.12 同型：Linux 一直在用 Windows 那条 `#else` 分支。** `SystemTrayController` 的通知投递在
非 macOS 上统一是 `QSystemTrayIcon::showMessage()`，也就是**经 StatusNotifier 项转手**。这有两个后果：
桌面没有 SNI 宿主时（纯 X11 会话、部分轻量 WM）**什么都不显示**，而回调仍然 `completed({})` 报 Accepted；
`supportsMessages()` 在 Linux 上取决于 SNI 后端自报，不是「能不能显示」的真实判据。

**通知 daemon 是会话服务，与显示协议无关。** 所以这一片不需要 Wayland 分支（与采集、快捷键、全屏检测
都不同）：`org.freedesktop.Notifications` 在 X11 和 Wayland 会话里都由同一个 daemon 提供，
Deepin/UOS 上是 `dde-daemon` 的 notification 服务。判据只有「会话总线连得上」与「这个名字已注册」。

分层（与 9.4.12 同一套切法）：

| 层 | 位置 | 平台 | 内容 |
|---|---|---|---|
| 纯逻辑 | `snow_shot/src/platform/linux/notificationpayload.cpp` | **全平台** | 转义、urgency 字节、图标名、过期时长、`actions` 数组、`Notify` 八参数的顺序与类型。**不碰总线**，因此在 Windows 上跑单测 |
| 总线 | `snow_shot/src/platform/linux/systemnotificationservice.cpp` | linux | `QDBusConnection::sessionBus()`、`GetCapabilities` 探测、异步 `Notify`、`ActionInvoked` / `NotificationClosed` 回执 |
| C++ 接线 | `systemtraycontroller.cpp` 的 `#elif defined(Q_OS_LINUX)` | linux | 投递改用 daemon；仅在「后端缺失」时退回托盘 balloon |
| 测试 | `snow_shot/tests/linux_notification_tests.cpp` | 全平台 | 纯逻辑的 30 余条断言，含参数顺序与变体类型 |

七条判断（前四条错了都是**运行期**才暴露的静默失败）：

1. **`Notify` 的八个参数有类型要求，写错是整条消息被拒，不是「显示得不理想」**。`replaces_id` 必须是
   `uint`（写成 `int` 会以 `i` 上线，daemon 按内省签名 `u` 校验 ⇒ `InvalidArgs`），`expire_timeout`
   必须是 `int`，而 `urgency` 在 `a{sv}` 里必须是**字节**（`y`），不是 int。测试因此断言的是
   `snowMetaTypeOf()` 的类型名，而不只是值。
2. **body 是 HTML 子集，路径里的 `&` / `<` / `>` 必须转义** ——否则 `/home/u/a&b<c>.mp4` 被当成标签碎片
   吞掉或断行。但反转也要认：daemon 不支持 `body-markup` 时**原样发**，否则用户看到的是 `&amp;`。
   判据来自 `GetCapabilities`，异步取一次并缓存；取不到就按「支持」处理（现网 daemon 全都支持，
   猜错的代价只是某一处 `&` 多显示几个字符）。
3. **只发 `default` 键、标签留空**。这既保住「点气泡本体就触发」的交互，又不必给按钮文案新增 msgid ——
   本片一行 `tr()` 都没加，翻译目录（`catalog-dir` 那套 split/merge）不需要动。
4. **`replaces_id` 按 action 维度记上一枚 id**：同一类通知重复出现时替换旧气泡而不是叠第二枚；
   收到 `NotificationClosed` 就把这枚 id 忘掉（daemon 可能回收 id，继续拿它当 `replaces_id` 是错的）。
5. **只把「后端缺失」降级到托盘**。`AccessDenied` 来自 daemon 策略或用户的勿扰状态，穿透到托盘 balloon
   等于绕开用户意图。
6. **托盘开关不再屏蔽通知**。`enabled` 在 Windows 上同时管「图标」和「气球」，在 Linux 上现在只管图标；
   `activateMessage()` / `showBalloon()` 里那两道 `if (enabled)` 与 macOS 一样排除 Linux，
   否则「关了托盘图标就收不到导出完成提示」。
7. **`desktop-entry` hint 指向 `productId()`（`snow-shot`）**，供 daemon 找 `.desktop` 拿标题与图标。
   该文件由 P6 安装；未安装时 daemon 退到 `app_icon`（`dialog-information` 等标准主题名），不会报错。

**验证（2026-10-20，Windows + Qt 5.15.2 / Qt 6.12.0，MSVC 14.44）**：

| 检查 | 结果 |
|---|---|
| `snow-shot-linux-notification-tests`（手工 `cl` 编译链接后运行，Qt **5.15.2**） | **passed** |
| 同一份源与测试，手工 `cl` + Qt **6.12.0** | **passed**（需 `/Zc:__cplusplus`，Qt6 头文件硬要求） |
| 测试抓到的真 bug | `QMetaType::name()` 在 Qt 5.15 返回**按值的 `QByteArray`**、在 Qt6 返回 `const char*`；按 `const char*` 接临时对象 ⇒ 悬垂读取，实际读到 `"Q\0\0…"`。测试改用 `SNOW_QT6` 分流 |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（覆盖面 5 → **6** 个文件，新增 `systemnotificationservice.cpp`，并给门禁补了 `QtDBus` 头目录） |
| 托盘投递 lambda 的语法 | 单独写探针 TU（不引 ant_design）过 `-Zs`，`EXIT=0` —— `systemtraycontroller.cpp` 因 ant_design 依赖进不了门禁 |
| `windows-qt5-full` configure | `EXIT=0`（`-- Generating done`）：新库与四处链接改动没有破坏既有配置 |
| `windows-qt5-full` build `snow_shot_notification_payload` | 3 步全绿（MOC + 编译 + 静态库） |
| CTest 目标本身 | **仍未经 CMake 配置验证**，原因同 9.4.12：Qt5 路线开不了 `SNOW_APPS_BUILD_TESTS`；本机唯一的 Qt6 树是 smoke 档，根本没 `add_subdirectory(snow_shot)` |

**本片未覆盖（诚实记账）**：

- **未在真机 daemon 上跑过**：`dde-daemon` 的实际收包、勿扰状态下的返回码、过期时长是否被尊重、
  点击气泡是否真能回到 `ActionInvoked`。这些只能到 UOS 上验。
- **`GetCapabilities` 存在竞态**：探测是异步的，第一枚通知可能早于答复到达，此时按「支持 markup」
  发送。只在 body 含 `<`/`&` 且 daemon 恰不支持 markup 时有可见差异。
- **未做 `CloseNotification`**：动作触发后依赖 daemon 自行关闭；有的 daemon 会把气泡留着。
- **多显示器 / 锁屏 / 通知中心的历史记录** 的呈现差异未验。
- **单实例 / `screenshotnative.h` 拆分** 仍是 P4 剩余 slice。单实例经查是纯 Qt
  （`QLocalServer` + `QLockFile`，`src/app/singleinstancecoordinator.cpp` 全文件无平台条件编译），
  在 Linux 上应当直接可用，但尚未在真机确认 socket 路径可用。

#### 9.4.14 P4 slice 7：托盘导出 StatusNotifierItem（2026-10-20）

**问题**：托盘从头到尾是 `QSystemTrayIcon`。Qt 5 的托盘实现不在 `QtWidgets` 里，而在**平台插件**
里：`xcb` 插件带一个 `StatusNotifierItem`，并且在有 XEmbed 管理器时还能退回 XEmbed。于是：

- Wayland 会话（Qt5 `wayland` 插件）**没有**任何托盘后端，`show()` 是空操作；
- 跑在 XWayland 里时图标"存在"，但它属于 XWayland 的那棵 X 树，合成器的面板看不到；
- 而主程序是 `QApplication::setQuitOnLastWindowClosed(false)`。托盘不见了，用户关掉主窗口后
  进程还在、却没有入口 —— 这不是"少一个图标"，是把应用弄丢了。

**做法**：自己导出 `org.kde.StatusNotifierItem`。这个对象是**会话总线**上的，跟显示协议无关，
理由和 slice 6 完全一样 —— 也就是它天生在 X11 与 Wayland 上同时可用。

**分层**（沿用 slice 5/6 的切法）

| 层 | 文件 | 平台 |
|---|---|---|
| 纯逻辑 | `platform/linux/statusnotifierpayload.cpp` | 全平台（不碰总线，Windows 跑单测） |
| 总线 | `platform/linux/statusnotifierservice.cpp` | linux（QtDBus 对象导出） |
| 接线 | `systemtraycontroller.cpp` 的 `#elif Q_OS_LINUX` | linux |
| 测试 | `tests/linux_tray_tests.cpp`（34 条断言） | 全平台 |

**何时才导出，写成纯函数 + 断言**：`needsStatusNotifierItem(platformTrayAvailable,
waylandSession) = !platformTrayAvailable || waylandSession`。也就是**平台插件看不见就不用了它**，
其余情况交给插件（同一个进程两个图标比没有图标更糟）。第二条判据 `isWaylandSession(WAYLAND_DISPLAY,
XDG_SESSION_TYPE)`：`XDG_SESSION_TYPE` 优先 —— `WAYLAND_DISPLAY` 会跟着从 Wayland 会话里启动的
XWayland 客户端一起活下来，只看它会把 x11 会话误判成 Wayland。

**图标必须逐字节重排**。`IconPixmap` 的类型是 `a(iiay)`：ARGB32、**网络字节序**（A,R,G,B 依次）。
`QImage::Format_ARGB32` 在任何目标平台上都是 host byte order 的 `0xAARRGGBB`，所以每个像素都要按
qAlpha/qRed/qGreen/qBlue 取出来重写，并且按 `bytesPerLine` 遍历 —— 外部缓冲、子图这类 stride 大于
`width * 4` 的图像不能直接 `memcpy`，否则之后每一行都错位。预乘也不做：宿主自己合成，Qt 自己的托盘
同样发直通 alpha。

**菜单走 `/NO_DBUSMENU`**。`Menu` 属性填 `/NO_DBUSMENU`，宿主于是调 `ContextMenu` 方法，我们把
`AdContextMenu`（换肤菜单，不是 `QMenu`）弹在指针处。代价写在明处：只认 D-Bus 菜单、完全不理
`ContextMenu` 的宿主会**没有右键菜单**。选这个是因为另一条路要放弃菜单皮肤、把每一项翻译成
`com.canonical.dbusmenu` 的 label/icon-name/toggle-state，那等于把设置菜单做两套。

**面板晚于应用启动是常态**，所以注册不是"构造时试一次"：先看
`org.kde.StatusNotifierWatcher` 在不在，不在就订阅 `NameOwnerChanged` 等它出现；出现后补注册。
注册参数用 `baseService()`（唯一名）而不是对象路径 —— Qt 与 GTK 的既有实现都发这个形态，宿主对它的
测试覆盖最多。

**验证（2026-10-20，Windows + Qt 5.15.2 / Qt 6.12.0，MSVC 14.44）**

| 检查 | 结果 |
|---|---|
| `snow-shot-linux-tray-tests`（手工 `cl` 编译运行，Qt **5.15.2**） | **passed**（34 条断言） |
| 同一份源与测试，Qt **6.12.0** | **passed** |
| 测试抓到的真问题 | `QImage(w,h,ARGB32)` 的未写像素是**分配器残留**，不是 0 —— 判据必须 `fill()` 后再断言，否则"逐行紧凑"这条会随机失败 |
| 判据修正 | `one</p><p>two` 是**两个**边界 ⇒ 两个换行（HTML 渲染本来也是空一行）。一开始按一个换行断言，是测试错了不是实现错了 |
| `moc` 解析头文件 | `MOC_EXIT=0`，生成的 `qt_metacall` 里出现 `qRegisterMetaType<QVector<…IconPixmap>>()`；moc 产物再过 `-Zs` 也 `EXIT=0` |
| `bash scripts/syntax-check-linux-cpp.sh` | `SYNTAX OK`（覆盖面 6 → **7** 个文件） |
| 接线 lambda 的语法 | 探针 TU（`-DQ_OS_LINUX`，不引 ant_design）过 `-Zs`，`EXIT=0` |
| `systemtraycontroller.cpp` 的 **Windows 分支** | 用 `compile_commands.json` 里的真实编译命令 + `-Zs` 跑，`EXIT=0`（0 error）—— 重构出 `showContextMenu()` / `applyToolTip()` 后原语义未变 |
| `windows-qt5-full` configure | `EXIT=0` |
| `windows-qt5-full` build `snow_shot_status_notifier_payload` | 5 步全绿（MOC + 编译 + 静态库） |
| 六个改动/新增文件 clang-format | 全部 0 违规 |

**本片未覆盖（诚实记账）**

- **D-Bus 线上签名没有任何离线验证**。这条不是懒，是**没有路**：`QDBusMetaType::typeToSignature`
  在 Qt 5.15 对**任何**用户类型都返回空串 —— 用一个最小案例（`struct Simple { int; QString; }` +
  `qDBusRegisterMetaType`）单独复核过，先初始化 `QDBusConnection` 也一样。Qt5 只在真正 marshal
  时推导签名，而 marshal 需要连接；本机没有会话总线。于是 `(iiay)` / `(sa(iiay)ss)` 是否被宿主接受，
  只能到真机用 `busctl --user introspect` / `gdbus introspect` 看。**我们与 Qt 自己的
  `qdbustrayicon.cpp` 用的是同一套写法**（`Q_PROPERTY` + `qDBusRegisterMetaType`），这是当前能拿到的
  最强保证。
- **真机一个都没跑**：`dde-dock` 是否显示、左/中/右键是否分别触发 `Activate`/`SecondaryActivate`/
  `ContextMenu`、图标尺寸是否被采纳（我们发 16/22/24/32/48/64）、tooltip 的标题/副标题排版。
- **`dde` 的右键菜单**：如果它只渲染 D-Bus 菜单而不调 `ContextMenu`，右键就是空的（见上文取舍）。
  这一条要优先验，验不出来就得补 `com.canonical.dbusmenu`。
- **watcher 重启后的状态**：已订阅 `NameOwnerChanged` 并会补注册，但"宿主重启时正在弹的菜单"、
  "重启后 `Status` 回到 `Active` 是否需要重发 `NewStatus`"未验。
- **`NeedsAttention` 未使用**：`showWarningMessage` 之类的告警没有让图标闪烁/高亮，仍是普通通知。
- **CTest 目标本身仍未进 CMake 配置验证**，原因同 slice 5/6：Qt5 路线开不了 `SNOW_APPS_BUILD_TESTS`，
  本机唯一的 Qt6 树是 smoke 档、不含 `snow_shot`。

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
