# Qt6 → Qt5 降级实施方案（Windows 优先验证 → UOS）

> 决策：**维持 Qt5 路线**；**保留 Windows 分支**，先在 Windows 用 Qt5 编译冒烟测试，通过后再上 UOS。
> 策略：**不删除 Qt6 支持**，把工程改造成「一套源码，Qt 5.15 / Qt 6.12 双版本皆可编译」。
> 基线：`SNOW_SHOT_VERSION = 1.2.8`（根 `CMakeLists.txt:18`）｜当前 Qt 契约：`6.12.0`（`scripts/qt-toolchain.json`）
> 文档定位：与 `docs-uos-linux-port.md`（Linux 移植总方案）、`docs-windows-arm64-build.md` 同级

---

## 0. 方案形态：为什么是「双版本」而不是「纯 Qt5」

这是本方案与前一版最大的区别，也是它成立的前提。

Qt 公司官方自 **Qt 5.15** 起引入了 **versionless 目标与命令**，并且 **Qt 6 同样提供**：

> 每个被加载的 Qt 模块都会定义库目标，名称以 `Qt6::` 开头……**这些目标也可使用 `Qt::` 前缀：`Qt::Core`、`Qt::Gui` 等。**
> —— Qt 6 官方文档 *Imported targets*

> Qt 5.15 引入了 *versionless targets*……可以同时引用 `Qt6::Core` 或 `Qt::Core`。独立于使用 Qt 6 还是 Qt 5，都可以使用导入的 `Qt::Core` 目标。
> —— Qt 官方文档 *Qt 5 and Qt 6 compatibility*

**结论：`Qt6::X` → `Qt::X` 是一次官方背书、双向生效的重命名。** 于是改造可以做成一棵分支树而非重写：

| | 纯 Qt5 重写（前一版方案） | **双版本改造（本方案）** |
|---|---|---|
| 现有 Qt6 构建 | 失效，不可回归 | **保持可用**，作为对照基线 |
| 定位「降级引入的 bug」 | 无法区分 | 可二分对比 |
| 回滚成本 | 高（跨 112 文件） | 低（每次一个 commit） |
| 迁移期工作量 | 一次性大 | **增量、可中断** |
| UOS 目标 | 达成 | 达成 |

**落地含义**：同一份源码在 `SNOW_QT_MAJOR=6` 与 `=5` 下都能配置、编译、运行。所有版本差异被收敛到
**3 个新增文件 + 1 个 CMake 模块**里，不进业务代码。

---

## 1. 实测改造面（grep 证据，对应当前 `main` 快照）

以下数据均由实仓 grep 统计得出，可复现。

### 1.1 分类总览

| # | 类别 | 典型 API | 命中 | 涉及文件 | 处理方式 |
|---|---|---|---|---|---|
| A | **官方版本无关** | `Qt6::Core` 等 CMake 目标 | **882 行** | 全部 CMakeLists | 机械改名 `Qt6::` → `Qt::` |
| B | **CMake 命令差异** | `qt_add_resources` 等 11 种 | **83 处** | CMakeLists/`cmake/*.cmake` | compat 模块提供同名封装 |
| C | **需兼容层** | `QKeyCombination` | 52 | 16 | L1 兼容头 |
| C | | `QEnterEvent` / `enterEvent()` 签名 | 52 / 99 | 45 / 51 | L1 兼容头 + 宏 |
| C | | `QList::removeIf` | 49 | 22 | L1 自由函数 + 脚本改写 |
| C | | `QLatin1StringView` | 25 | 7 | L1 一行别名 |
| C | | `QVariant::metaType()` | 9 | 2 | L1 内联函数 |
| C | | `QMetaType::fromType<T>()` | 8 | 2 | L1 内联模板 |
| C | | `QStringDecoder` | 4 | 1 | 人工改写单文件 |
| D | **无需改动**（Qt5.15 已有） | `QImage::sizeInBytes()` | 135 | 42 | 保留（Qt5 返回 `int`，见 1.3） |
| D | | `QAction` / `QShortcut` | 322 / 15 | 20 / 4 | 保留（Qt5 在 QtWidgets） |
| D | | `Qt::SkipEmptyParts` | 5 | 4 | 保留（5.14+ 已提供） |
| D | | `QWindow::startSystemResize/Move` | 9 | 7 | 保留（5.15+ 已提供） |
| E | **平台专属，Linux 不参与编译** | `QNativeInterface` | 5 | 4 | Windows/macOS 专用，保持原样 |
| E | | `QPromise` | 7 | 2 | 仅 viewer 的 `windows_print.cpp` |
| F | **换 include + 少量 API 分支**（见 1.5，**不再需要整文件重写**） | `QRhi*` | 246 | 3 | viewer 渲染后端；Qt 5.15 **以私有 API 形式自带 QRhi** |
| G | **构建契约** | `QT_DISABLE_DEPRECATED_UP_TO=0x060c00` | 1 | `cmake/SnowQt.cmake` | Qt6 专有宏，需条件化 |

### 1.2 去重后的真实工作量

```
需改造 C++ 文件（不含 tests 与平台专属目录）: 112
    snow_shot              67
    ant_design_qt          39
    snow_image_viewer       4
    snow_draw_engine_qt     2
CMake 需改动行数:                              882
```

占全部 1306 个非测试 C++ 文件（含头文件）的 **8.6%**。

### 1.3 三个容易误判的点（已逐条核实，避免做无用功）

1. **`QImage::sizeInBytes()` 不用改。** 经查 Qt 5.15 文档，该方法存在，只是返回 `int`（Qt6 返回 `qsizetype`）。
   135 处调用在 Qt5 下**可编译**；风险仅在超大位图（>2GB）时的溢出，而截图工具不会触及。
   → **处理：保留，仅在 code review 时留意 `qint64` 累加场景。**
2. **`Qt::SkipEmptyParts` 不用改。** 它是 Qt 5.14 引入的（`QString::SkipEmptyParts` 的替代名）。5 处调用在 5.15 下合法。
3. **`QAction` / `QShortcut` 不用改。** 虽然 Qt6 把它们从 QtWidgets 移到了 QtGui，但 Qt5 里它们本就在 QtWidgets，
   `#include <QAction>` 两种版本都能解析。

反过来，**`QKeyCombination` 是必须处理的硬点**：它**自 Qt 6.0 引入，Qt5 中无等价类**——Qt5 时代的写法是把
`Qt::Key` 与 `Qt::KeyboardModifiers` 按位或成 `int` 直接传。`QKeyCombination::fromCombined()/toCombined()`
这对 API 的存在本身就证明了这一点。

### 1.4 逐条实测的 Qt5 / Qt6 API 差异（编译迭代中确认，均带证据）

本机 Qt 套件为 `C:\Qt\5.15.2\msvc2019_64`，下列结论**全部读该套件头文件或改写前备份里的 Qt6 原始源码得出**，
不是凭记忆写的。

| # | 差异 | Qt5.15 事实 | Qt6 事实 | 证据 |
|---|---|---|---|---|
| 1 | `QLayoutItem::widget()` const 性 | `virtual QWidget *widget();`（非 const） | `virtual QWidget *widget() const;` | Qt 5.15.2 `QtWidgets/qlayoutitem.h:78-82` **自身写着版本分支** |
| 2 | `QWheelEvent::position()/globalPosition()` | **已回移**，`inline QPointF position() const` | 同 | `QtGui/qevent.h:243-244`（旧名 `pos()/globalPos()` 标着 "Use position()"） |
| 3 | `QMouseEvent::localPos()/globalPos()` | 无 `position()/globalPosition()` | 有 | `QtGui/qevent.h` 读源确认 |
| 4 | `QHoverEvent::posF()` | 无全局坐标访问器 | `position()/globalPosition()` | 同上 |
| 5 | `QTouchEvent::TouchPoint` 全局坐标 | `screenPos()`（**无 `globalPosF()`**） | `QEventPoint::globalPosition()` | `QtGui/qevent.h` TouchPoint 段 |
| 6 | `QColor::getHslF()` 指针与返回 | `void getHslF(qreal*,...` | `void getHslF(float*,...` | `QtGui/qcolor.h:202`；返回 **void**（改写前 Qt6 源码亦按语句调用） |
| 7 | `QColor::alphaF()` 返回类型 | `qreal` | `float` | 破坏 `std::clamp(x, 0.0F, 1.0F)` |
| 8 | `QWidget::setScreen()` | **不存在**（grep `qwidget.h` 为 0） | 存在 | Qt5 只有 `QWindow::setScreen`，`QtGui/qwindow.h:258` |
| 9 | `QWidget::setAccessibleIdentifier()` | 不存在 | 存在 | 全文 grep 仅 `QWidget` 侧调用 |
| 10 | `QFont` resolve 掩码 | `uint resolve()`（**public**，无参） | `resolveMask()` | `QtGui/qfont.h:300` |
| 11 | `QEvent` 屏幕/DPR 变化 | `ScreenChangeInternal = 216` | `DevicePixelRatioChange` | `QtGui/qevent.h` 枚举 |
| 12 | `qHash` 几何类型重载 | QSize/QPoint/QRect/QMargins/QLine **及其 float 变体全无** | 有 | `grep -rhoE "qHash\((const )?Q[A-Za-z0-9_]+" QtCore/ QtGui/` |
| 13 | 自定义 `qHash` 的 seed 与返回类型 | `uint`，且**必须带默认值** | `size_t`，无需默认值 | Qt5 兜底模板 `QtCore/qhashfunctions.h:117` 会二次派发到单参 `qHash(t)` |
| 14 | `QAbstractItemModel::dataChanged` 第三参 | `QVector<int>` | `QList<int>` | 信号签名，`qOverload<>` 模板实参必须逐字一致 |
| 15 | `QString::size()/length()` 返回 | `int` | `qsizetype` | `QtCore/qstring.h:277-279`；混合进 `std::clamp` 直接推导失败 |
| 16 | `QFlags::toInt()` | 无（但已有 `int` 转换） | 有 | 用于 `drawText(..., alignment)` |
| 17 | `QLocale::positiveSign()/negativeSign()` | `QChar`（**无 `isEmpty()`**） | `QString` | 用于数字输入前/负号 |
| 18 | `QEvent::type()` 派生 | 无 `typeId()` | `QEvent::typeId()` | 映射到 `userType()` |
| 19 | `QAction::associatedObjects()` | 无（只有 `associatedWidgets()`） | 有 | `QWidgets/qaction.h` |
| 20 | `QAccessible::Event::Alert` | **存在**（`0x0002`） | — | 用于通告事件的 Qt5 近似 |
| 21 | `QStyleHints::colorScheme()` / `Qt::ColorScheme` | **完全不存在**（`QtGui/qstylehints.h`、`QtCore/qnamespace.h` grep 均为 0） | Qt 6.5 引入 | 真实调用点：`thememanager.cpp:38/241`、`snow_image_viewer/src/app/application_setup.cpp:18` ＋ 若干测试 |
| 22 | `QStyleHints::colorSchemeChanged` 信号 | 不存在 | Qt 6.5 | 需改用 `QGuiApplication::paletteChanged` |
| 23 | `QChronoTimer` | **头文件不存在**（`QtCore/QChronoTimer` 无） | Qt 6.8 引入 | `screenshotpresentationframescheduler.cpp` 1 处实现 + 2 处测试 |
| 24 | `QTimeZone::fromSecondsAheadOfUtc` | 不存在（`qtimezone.h` grep 为 0） | Qt 6.5 引入 | `screenshot_pdf_export_tests.cpp:121` |
| 25 | `QStringDecoder` / `QStringConverter` 家族 | 不存在 | Qt6 才有 | `screenshotclipboardcontent.cpp:548-550`（3 处），改用 `QTextCodec` |
| 26 | `QPromise` | 不存在 | Qt6 才有 | `windows_print.cpp`（4 处）+ `mcpasyncwork_p.h`（1 处） |
| 27 | QRhi 渲染栈 | **以私有 API 形式存在**（`QtGui/5.15.2/QtGui/private/qrhi_p.h` + `qshader_p.h` + D3D11/GLES2/Vulkan/Metal/Null 全部后端，均已核实） | `QtGui/rhi/*` 公开 | 261 处，集中在 `snow_image_viewer/src/render/rhi_image_window.cpp`；**结论见 1.5** |
| 28 | `QByteArrayView` | **不存在**（无 `QtCore/QByteArrayView` 转发头，`qbytearray.h` 0 次提及） | Qt 6.0 引入 | 13 处：7 处是 `addData()` 的临时包装（R29），6 处是 `worker_protocol.cpp` 里的真实类型（第 15 节垫片） |
| 29 | `QScreen::nativeInterface<T>()` / `QNativeInterface` 命名空间 | **完全不存在** | Qt6 才有 | 15 处，`focusedfullscreenwindow.cpp` / `monitorgeometry.cpp` / `screenshotgeometry.cpp` / `screenshotoriginalimagepreviewwindow.cpp` ＋ 测试 |
| 30 | `QPointingDevice` | **不存在**（仅有 `QTouchDevice`） | Qt6 才有 | 5 处，均在 `screenshot_pinned_window_tests.cpp` 等测试里 |
| 31 | `QNativeGestureEvent::position()` | **不存在**（只有 `localPos/windowPos/screenPos/globalPos`） | 继承 `QSinglePointEvent` 故有 | Qt 5.15.2 `QtGui/qevent.h:343-362`；真阳性 1 处 |
| 32 | `QTextDocument::toRawText()` | **存在**（`qtextdocument.h:172`） | 存在 | 扫描器一度误报，已核实为**无需处理** |
| 33 | `QImage::sizeInBytes()` | **存在**（`qimage.h:223`） | 存在 | 同上，195 处全部**无需处理**（`byteCount()` 才是弃用的那个） |

**第 13 条最隐蔽**：不是简单地换个类型别名就完事。Qt5 的 `qHash` 兜底模板长这样：

```cpp
template<typename T> inline uint qHash(const T &t, uint seed)
    noexcept(noexcept(qHash(t)))
{ return qHash(t) ^ seed; }          // QtCore/qhashfunctions.h:117
```

它通过**单参** `qHash(t)` 二次派发，因此用户自定义重载在 Qt5 侧必须写成
`SnowHashSeed qHash(const Key& key, SnowHashSeed seed = 0)`——**默认值是必需的**，
否则 Qt5 找不到单参可调用版本（Qt6 直接调 `qHash(key, seed)`，不需要）。加默认值对 Qt6 无害。

第 21–27 条本轮已**全部落垫片**（兼容头第 10–15 节，见 L3b），不再只是「待处理清单」。
第 28 条已用「R29 + 第 15 节垫片」两条腿解决；第 29/30 条的 Windows 侧已收敛（见 1.5），
剩余 macOS 侧显式延后；第 31 条是真阳性，已补 `QNativeGestureEvent` 的 `snowPos/snowGlobalPos` 重载；
第 32/33 条是扫描器的**假阳性**，核实后确认两版同名、无需处理——这正是
「每条结论都要读头文件核实」的价值。

**第 27 条（QRhi）的原判断是错的，本轮已纠正**：原先按「私有头未公开 → 必须整文件
重写为 `QOpenGLWidget`」处理，实测 Qt 5.15.2 的套件里 QRhi 是**完整存在**的（虽为私有
API）。详见 1.5，那 2977 行的重写**不必做**。

### 1.5 两个改变计划的实测结论（本轮新增）

#### (1) Qt 5.15 自带 QRhi —— viewer 渲染后端不必重写

`C:/Qt/5.15.2/msvc2019_64/include/QtGui/5.15.2/QtGui/private/` 下实际存在：

```
qrhi_p.h  qrhi_p_p.h  qshader_p.h
qrhid3d11_p.h  qrhigles2_p.h  qrhimetal_p.h  qrhinull_p.h  qrhivulkan_p.h  (+ 各自 _p_p.h)
```

且**工程用到的调用点签名完全对得上**（逐条读 Qt5 的 `qrhi_p.h` 核实）：

| 工程调用 | Qt 5.15 声明 | 结论 |
|---|---|---|
| `QRhi::create(QRhi::D3D11, &params)` | `static QRhi *create(Implementation, QRhiInitParams *, Flags = {}, QRhiNativeHandles * = nullptr)` | 形参是**指针**，与工程一致 |
| `QRhi::D3D11 / OpenGLES2 / Vulkan / Null` | `enum Implementation { Null, Vulkan, OpenGLES2, D3D11, Metal }` | 四个取值全在 |
| `newTexture(format, size, 1, flags)` | `QRhiTexture *newTexture(Format, const QSize &, int, Flags, ...)` | 一致 |
| `newBuffer/newSampler/newShaderResourceBindings/newGraphicsPipeline` | 同名同形参 | 一致 |
| `QShader::fromSerialized(ba)` | `qshader_p.h:150` 存在 | 一致 |

**仍需处理的两处真差异**：

1. **include 路径**：Qt6 的公开头是 `<rhi/qrhi.h>` / `<rhi/qrhi_platform.h>` / `<rhi/qshader.h>`；
   Qt5 只有私有头 `<QtGui/private/qrhi_p.h>` / `<QtGui/private/qshader_p.h>`，且各后端
   （`QRhiVulkanInitParams` 等）要按需包含 `<QtGui/private/qrhivulkan_p.h>` 之流。
2. **Qt5 无 HDR**：`qrhi_p.h` 里 `Hdr`/`HDR` **grep 为 0**，因此工程里
   `QRhiSwapChain::HDRExtendedSrgbLinear`、`swapChain_->hdrInfo()`、`QRhiSwapChainHdrInfo`、
   `luminanceBehavior`、`limitsType` 全部不存在（`rhi_image_window.cpp` 中 3 组、约 10 行）。
   Qt5 侧必须显式降级为 SDR 分支，并在代码里标注这是有损降级。

**代价与风险**：私有 API 在不同 5.15.x 补丁间无语义保证；UOS 上需要
`qtbase5-private-dev` 才能拿到 `qrhivulkan_p.h` 等头文件（**部署前置条件，需在 UOS 侧确认**）。
若该包在 UOS 上不可得，才回退到原「重写为 `QOpenGLWidget`」方案。

#### (2) QSB 版本不兼容 —— 这是走 QRhi 路线的**真正硬卡点**

`.qsb` 是预编译并入库的（`snow_image_viewer/resources/shaders/*.qsb`，
由 `snow_image_viewer/scripts/compile-shaders.ps1` 调 Qt6 的 `qsb.exe` 生成，
`scripts/test-qt-shader-contract.ps1` 强制其版本匹配 `scripts/qt-toolchain.json` 的 6.12.0）。
运行时经 `:/shaders/*.qsb` 从 qrc 读取，交给 `QShader::fromSerialized()`。

实测两条硬事实：

| | Qt 6.12（现状） | Qt 5.15.2（目标） |
|---|---|---|
| `QShaderPrivate::QSB_VERSION` | **9** | **5**（白名单 1–5） |
| 版本不符时 | — | `qWarning("Attempted to deserialize QShader with unknown version %d.")` 后返回**空 `QShader`** |
| 套件自带烘焙工具 | `qsb.exe`（契约要求存在） | **无 `qsb.exe`**（`C:/Qt/5.15.2/msvc2019_64/bin/` 下不存在） |

即：现有 `.qsb`（v9）在 Qt5 下会被**静默丢弃成空 shader**（表现为黑屏/渲染异常），
必须在 Qt5 路线下**重新烘焙成 ≤ v5**。而 Qt6 的 `qsb` 只提供
`--qsbversion 64|65`（Qt 6.4 / 6.5）这类回退档，**没有 v5 档**，所以需要
Qt 5.15 自身的 Shader Tools（`qtshadertools`，5.15 分支）来出包。

**顺带澄清一个容易误判的格式问题**：`.qsb` 文件开头不是 `QSB` 魔数，而是
`4 字节大端原始长度 + zlib 流`（实测 `00 00 11 A0 78 9C ...`，解压后正好 4512 字节）。
这不是坏文件——Qt 的 `QShader` 序列化本身就是 `qCompress` 封装，
Qt5/Qt6 两侧一致，**不需要手工解包**。


---

## 2. 三层改造策略

### L1 — 兼容层（新增文件，业务代码零侵扰）

新增 `snow_shot/include/snow_shot/compat/qt_compat.h`，被所有模块统一 include（可在 PCH 或
`target_precompile_headers` 中注入）。核心内容：

```cpp
#pragma once
#include <QtGlobal>
#include <QEvent>
#include <QList>
#include <QMetaType>
#include <QPointF>
#include <QVariant>

// ── 版本判定 ──────────────────────────────────────────────
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#  define SNOW_QT6 1
#else
#  define SNOW_QT6 0
#endif

// ── 1. QKeyCombination（Qt 6.0 引入，Qt5 无） ──────────────
#if SNOW_QT6
#  include <QKeyCombination>
#  include <QKeySequence>
#else
namespace snow::compat {
class QKeyCombination
{
public:
    constexpr QKeyCombination(Qt::KeyboardModifiers mods, Qt::Key key) noexcept
        : m_key(key), m_modifiers(mods) {}
    constexpr explicit QKeyCombination(Qt::Key key = Qt::Key_unknown) noexcept
        : m_key(key), m_modifiers(Qt::NoModifier) {}
    [[nodiscard]] constexpr Qt::Key key() const noexcept { return m_key; }
    [[nodiscard]] constexpr Qt::KeyboardModifiers keyboardModifiers() const noexcept { return m_modifiers; }
    [[nodiscard]] constexpr int toCombined() const noexcept
    {
        return static_cast<int>(m_key) | static_cast<int>(m_modifiers);
    }
    [[nodiscard]] constexpr bool operator==(const QKeyCombination&) const noexcept = default;
private:
    Qt::Key m_key;
    Qt::KeyboardModifiers m_modifiers;
};
} // namespace snow::compat
using snow::compat::QKeyCombination;   // 仅 Qt5 分支存在，无冲突
#endif

// Qt5 的 QShortcut/QAction 只接受 QKeySequence，需要一次桥接
[[nodiscard]] inline QKeySequence snowKeySequence(const QKeyCombination& combination)
{
#if SNOW_QT6
    return QKeySequence(combination);
#else
    return QKeySequence(combination.toCombined());
#endif
}

// ── 2. enterEvent 签名（Qt 6.0 改为 QEnterEvent*） ────────
#if SNOW_QT6
#  include <QEnterEvent>
using SnowEnterEvent = QEnterEvent;
#else
using SnowEnterEvent = QEvent;   // Qt 5.14+ 已存在 QEnterEvent，但虚函数签名仍是 QEvent*
#endif
// 用法：void enterEvent(SnowEnterEvent* event) override;

[[nodiscard]] inline QPointF snowEnterPosition(const QEvent* event)
{
#if SNOW_QT6
    return static_cast<const QEnterEvent*>(event)->position();
#else
    // Qt 5.14+ 实际投递的仍是 QEnterEvent 实例，可安全下转型
    if (event && event->type() == QEvent::Enter) {
        if (const auto* enter = dynamic_cast<const QEnterEvent*>(event))
            return enter->position();
    }
    return {};
#endif
}

// ── 3. QList::removeIf（Qt 6.1 引入） ─────────────────────
namespace snow::compat {
template <typename Container, typename Predicate>
qsizetype removeIf(Container& container, Predicate predicate)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 1, 0)
    return container.removeIf(predicate);
#else
    qsizetype removed = 0;
    for (auto it = container.begin(); it != container.end();) {
        if (predicate(*it)) { it = container.erase(it); ++removed; }
        else { ++it; }
    }
    return removed;
#endif
}
} // namespace snow::compat
// 用法：snow::compat::removeIf(list, [](auto& v){ ... });

// ── 4. QLatin1StringView（Qt 6.4 改名） ───────────────────
#if SNOW_QT6
#  include <QLatin1StringView>
#else
#  include <QLatin1String>
using QLatin1StringView = QLatin1String;
#endif

// ── 5. QMetaType / QVariant 类型查询 ──────────────────────
template <typename T>
[[nodiscard]] inline QMetaType snowMetaType()
{
#if SNOW_QT6
    return QMetaType::fromType<T>();
#else
    return QMetaType(qMetaTypeId<T>());
#endif
}

[[nodiscard]] inline QMetaType snowMetaTypeOf(const QVariant& value)
{
#if SNOW_QT6
    return value.metaType();
#else
    return QMetaType(value.userType());
#endif
}
```

### L2 — 机械改写（脚本驱动，逐条可 diff 审查）

新增 `scripts/port-qt6-to-qt5.py`，执行确定性规则，**支持 `--dry-run` 先出差异清单**：

| 规则 | 匹配 | 替换 |
|---|---|---|
| R1 | `Qt6::` (CMake) | `Qt::` |
| R2 | `qt_<name>(` (CMake) | 由 compat 模块接管，**不改** |
| R3 | `#include <QKeyCombination>` | `#include "snow_shot/compat/qt_compat.h"` |
| R4 | `void enterEvent(QEnterEvent*` | `void enterEvent(SnowEnterEvent*`；并删除 `class QEnterEvent;` 前向声明 |
| R5 | `<receiver>.removeIf(` | `snow::compat::removeIf(<receiver>, ` |
| R6 | `#include <QLatin1StringView>` | compat 头 |
| R7 | `QMetaType::fromType<` | `snowMetaType<` |
| R8 | `<expr>.metaType()` (QVariant) | `snowMetaTypeOf(<expr>)` |
| R9 | `setShortcut(<QKeyCombination>)` | 进人工清单，加 `snowKeySequence()` 包裹 |
| R10/R11 | `<ev>->position()` / `->globalPosition()` | `snowPos(<ev>)` / `snowGlobalPos(<ev>)`（保守：接收者须在本文件被声明为事件指针） |
| R12 | `nativeEvent(..., qintptr*)` | `snowNativeEventResult*` |
| R13 | `std::clamp(<c>.alphaF(), 0.0F, 1.0F)` | `std::clamp(snowAlphaF(<c>), 0.0F, 1.0F)` |
| R14 | `qHashMulti(` | `snowHashMulti(` |
| R15 | `<c>.deviceIndependentSize()` | `snowDeviceIndependentSize(<c>)` |
| R16 | `<v>.typeId()` | `<v>.userType()` |
| R17 | `QEvent::DevicePixelRatioChange` | `snowDevicePixelRatioChange` |
| R18 | `QPalette::Accent` | `snowAccentRole` |
| R19 | `<w>->setScreen(x)` / `<w>.setScreen(x)` / `setScreen(x)` | `snowSetWidgetScreen(<w>\|&<w>\|this, x)` |
| R20 | `<w>->setAccessibleIdentifier(id)` | `snowSetAccessibleIdentifier(<w>, id)` |
| R21 | `<a>->associatedObjects()` | `snowAssociatedObjects(<a>)` |
| R22 | `QAccessible::AnnouncementPoliteness` | `SnowAnnouncementPoliteness` |
| R22b | `QAccessibleAnnouncementEvent` | `SnowAccessibleAnnouncementEvent` |
| R23 | `<f>.resolveMask()` | `snowFontResolveMask(<f>)` |
| R24 | `Qt::ColorScheme[::Dark\|Light\|Unknown]` | `SnowColorScheme[Dark\|Light\|Unknown]` |
| R25 | `QTimeZone::fromSecondsAheadOfUtc(` | `snowTimeZoneFromSecondsAheadOfUtc(` |
| R26 | `QPromise<` | `SnowPromise<` |
| R27 | `QChronoTimer` | `SnowChronoTimer`（**跳过 `#include` 行**） |
| R28 | `<styleHints>->colorScheme()` / `X::styleHints()->colorScheme()` | `snowColorScheme(<styleHints>)`（接收者名字不像 style hints 则转人工） |
| R29 | `addData(QByteArrayView(a, b))` | `addData(a, static_cast<int>(b))`（Qt5 无视图重载，见第 15 节垫片） |
| R30 | `QNativeInterface::Q*Screen` | 登记人工清单（Qt5 无 `QScreen::nativeInterface<>()`）。**Windows 侧已收敛**：13 处全部改为 `snow_shot/platform/windows/monitorgeometry.h` 的 `nativeMonitorHandle()`，Qt6 分支保留原写法、Qt5 分支走 `QPlatformNativeInterface::nativeResourceForScreen("handle", screen)`；剩 2 处 macOS cocoa 为已文档化的延后项 |
| R31 | `QPointingDevice` | **已收敛**：4 处（均在 `screenshot_pinned_window_tests.cpp`）改为调用兼容头新增的工厂 `snowNativeGestureEvent(type, localPos, windowPos, screenPos, value)`；Qt6 分支补 `QPointingDevice::primaryPointingDevice()` + `fingerCount=2` + 尾参 `delta`，Qt5 分支传 `nullptr` 设备 + 尾参 `(0, 0)`。工厂有探针覆盖（`compat-shim-probe.cpp` 第 33 段）证明 Qt5 分支真实可编译 |
| R32 | `<recv>.sliced(pos[, n])` | `.mid(pos[, n])`。**这是本轮新发现的真缺口**：`.sliced()` 是 Qt 6.0 引入的成员，Qt 5.15.2 的 `qstring.h` / `qbytearray.h` / `qlist.h` / `qvector.h` 里 grep `sliced` 全部 0 命中。共 6 处（`settingssearchindex.cpp` ×2、`screenshottoolpalette.cpp`、`screenshottoolpalettestylecontrols.cpp`、`settingsadapters.cpp`、`angle_annotation_integration_tests.cpp`），接收者类型逐一确认为 `QString` ×4 / `QByteArray` ×1 / `QVector<QStringList>` ×1，均可用 `mid()` 精确等价替换（`n<0` 语义都是"截到末尾"）|
| R33 | `QLibraryInfo::path(...)` | `snowLibraryInfoPath(...)`。**本轮新发现的真缺口**：Qt 5.15.2 只有 `location()`，它的头文件自己就写着 `// ### Qt 6: consider renaming it to path()`（`QtCore/qlibraryinfo.h:105`），grep `path(` 无命中。垫片做成**模板**以回避 Qt6 侧 `LibraryLocation`（`path()` 用）与 `Qt6LibraryLocation`（已弃用的 `location()` 用）双枚举的命名问题。1 处，在剪贴板性能基准里 |
| R34 | `QFont::Tag(...)` | `SNOW_FONT_SET_VARIABLE_AXIS` / `SNOW_FONT_SET_FEATURE` 宏。**本轮新发现的真缺口**：Qt 6.7 的 `setVariableAxis(QFont::Tag, float)` 与 Qt 6.1 的 `setFeature(QFont::Tag, quint32)` 在 Qt 5.15 **完全没有**（grep `QtGui/qfont.h`：`Tag` / `setVariableAxis` / `setFeature` / `featureTags` 全 0）。且**无等价物可回落**：Qt5 的 `QFont::Weight` 是 0..99 旧刻度，承载不了任意 `wght` 轴值（如 650），Qt5 也没有任何字体特性 API。故垫片在 Qt5 展开成 no-op（2 行，在 `snow_draw_engine_qt/tests/snow_canvas_filter_safety_tests.cpp`）。**为什么用宏而不是 inline 函数**：Qt5 展开时不能出现 `QFont::Tag`（不存在），Qt6 展开又必须与现有已编译通过的表达式逐字一致——宏是唯一同时满足两者的形态，且不必猜 `QFont::Tag` 的构造函数重载 |

R24–R28 的实现要点：替换目标都是**类名本身**，所以必须跳过 `#include <QChronoTimer>`
这类行（换成 `#include <SnowChronoTimer>` 会直接编译失败）。R28 又是另一个假阳性陷阱：
`colorScheme()` 在本工程里**同时是 adqt 自有 API**（`AdContextMenu::colorScheme()`、
`AdTag::colorScheme()`），无条件改写会把
`menu->colorScheme() != AdContextMenu::ColorScheme::Inherit` 变成 `snowColorScheme(menu)`。
因此 R28 只对「形如 `styleHints` 的接收者」自动改写，其余送人工清单——实测命中数
从 11 收窄到 1（真阳性），10 处假阳性被挡下。**R28 还漏了第二种形态**
`QGuiApplication::styleHints()->colorScheme()`（接收者是**调表达式**而不是标识符），
它既不匹配 `_MEMBER_RECV` 也不进人工清单，属于**静默漏改**，实测 2 处；
现已在 R28 内单独加一条 `\w+(?:::\w+)*\(\)->colorScheme()` 分支补上。

R5/R8/R10/R11/R19/R20 需要括号配对与接收者表达式提取——脚本只自动处理**可静态判定**的接收者，
其余**打印到 `manual-review.txt` 交人工**，绝不猜。

#### 规则实现中的三个真实陷阱（本轮实测踩到，已修）

1. **负向后行断言会废掉整个「带接收者」分支。** R19 最初写成
   `(?:((RECV))\s*->\s*)?(?<!->)(?<!\.)setScreen...`，本意是挡住
   `windowHandle()->setScreen()`（接收者带 `()`，`RECV` 吃不下，可选组不参与匹配，
   于是退化成「裸调用」被误改）。但 lookbehind 的判定位置正好落在 `->` 之后，
   结果**连 `popup->setScreen()` 这类合法调用也被一并挡住**——规则整体失效
   （实测 R19 只命中 4 处裸调用，`popup->/popup_->/popupSurface_->` 全部漏改）。
   改为：正则只切「可选接收者 + 实参」，是否受保护由 Python 端按接收者名字、
   以及裸匹配前一个字符显式判定。

2. **`[^()]*` 匹配不了含括号的实参。** `setScreen(anchor->screen())`、
   `setAccessibleIdentifier(button->objectName())` 这类实参本身带 `()`，
   原模式直接失配（R20 漏改的第二个原因）。改用允许一层嵌套的
   `(?:[^()]|\([^()]*\))*`。

3. **R5 的截断点用错运算符位置，产出语法错误代码（最严重的一个）。**
   R5 原文是 `replaced = replaced[:m.start()] + new + replaced[m.end():]`，
   而 `m.start()` 指向 `.` / `-`，于是**接收者被原样留下**：

   ```cpp
   paths.removeIf(pred)
       ->  pathssnow::compat::removeIf(paths, pred)   // 语法错误
   ```

   这是**编译不过但肉眼极难发现**的损坏，而且 `ant_design_qt` 里没有 removeIf
   调用点，所以「ant_design_qt 全绿」并不能暴露它。实测波及 **22 个文件、49 处**。
   修法：截断点必须用 `extract_receiver()` 返回的**接收者起点** `recv_start`。
   已写一次性修复脚本 `.workbuddy/qt5-port/repair-r5-prefix-duplication.py`
   （只在能证明前缀确为重复时才删，其余原样保留并打印），另在 `collect_manual`
   里加**回归哨兵**：出现 `[A-Za-z0-9_]snow::compat::` 即无条件报出。

   顺带：`it->removeIf(...)` 里 `it` 是 `QHash::iterator`（`->` 返回 `T*`），
   兼容头的 `removeIf` 取的是**容器引用**，因此 `->` 形态必须显式解引用成
   `snow::compat::removeIf(*it, pred)`；`&&operator` 已按运算符分支处理。

#### 已复核抑制表：让人工清单重新变得可读

`manual-review.txt` 的价值在于「列出来的都还没人看过」。本轮一开始它挂着 64 条，
逐条核对后发现**只有 1 条是真阳性**（`gesture->position()`，`QNativeGestureEvent`
在 Qt5 没有 `position()`）——真阳性正是被假阳性埋掉的。因此：

- 建立 `REVIEWED_SAFE` 表（`scripts/port-qt6-to-qt5.py`），按
  `(规则, 路径, 命中文本)` 登记**已复核-无需处理**的项，每条写明判定依据与证据行号，
  输出到独立的 `manual-reviewed-safe.txt`（可审计，不会静默消失）。
- 同时修掉两类**假阳性的产生源**：
  1. `EVENT_TYPE_ALT` 漏登记 `QWheelEvent` → 16 处 QWheelEvent 的
     `position()/globalPosition()` 被误判成待改（Qt 5.15 早已回移这两个成员）；
  2. 残留检测把「标识符接收者」也重复报了一遍 → `sink->position()` 被 标识符路径
     与 残留路径 各记一次，清单条目凭空翻倍。现在残留检测只覆盖**非简单标识符**
     接收者（如 `block.layout()->position()`，即 `QTextLayout::position()`）。
- 结果：待人工清单 **64 → 0**（24 条转入已复核文件），此后新增的 R30/R31 共 20 条
  才是真正待办；R30 的 Windows 侧 13 处与 R31 的 4 处收敛后，清单落到 **2 条**，
  且这 2 条都是已文档化的 macOS cocoa 延后项。所有已复核项的判定依据都写进了代码注释与下表。

| 族 | 条数 | 判定 | 依据 |
|---|---|---|---|
| R10 `wheel->position()` / QWheelEvent `->globalPosition()` | 16 | 无需改 | Qt 5.15 已回移 `QWheelEvent::position()/globalPosition()` |
| R10 `sink->position()` | 8 | 无需改 | `RasterWriterSink::position()` 返回 `Result<std::uint64_t>` |
| R10 `m_physicalCursor->position()` | 4 | 无需改 | `snow_shot::platform::PhysicalCursor::position()` |
| R10 `block.layout()->position()` | 5 | 无需改 | `QTextLayout::position()`，两版同名 |
| R28 `->colorScheme()`（工程自有） | 11 | 无需改 | `AdContextMenu/AdNavigationMenu/AdTag` 各自类内 `enum class ColorScheme` |
| R9 `setShortcut(shortcut)` | 1 | 无需改 | 形参本就是 `const QKeySequence&` |

### L3 — 人工改写（无法机械化的部分）

| 项 | 文件 | 方案 | 量级 |
|---|---|---|---|
| **viewer RHI 渲染** | `snow_image_viewer/src/render/rhi_image_window.cpp`（2977 行）+ `.h`（345 行） | **不再重写**：Qt 5.15 自带 QRhi（私有 API）。改为 ① `#include <rhi/qrhi.h>` → `<QtGui/private/qrhi_p.h>` 等；② Qt5 无 HDR，把 `HDRExtendedSrgbLinear` / `hdrInfo()` / `QRhiSwapChainHdrInfo` 显式降级为 SDR 分支；③ **重新烘焙 `.qsb` 为 QSB ≤ v5**（见 1.5(2)，这是真正卡点） | **中**，但依赖 UOS 侧 `qtbase5-private-dev` 与 Qt 5.15 Shader Tools；若二者不可得才回退到 `QOpenGLWidget` |
| **Windows 打印** | `snow_image_viewer/src/platform/windows_print.cpp`（用 `QPromise`） | Qt5 用 `QtConcurrent::run` + `QFutureInterface` 或自实现 promise | 小，仅 viewer |
| ~~**QStringDecoder**~~ | ~~1 个文件~~ | **已完成**：改为版本无关的 `snowTextDecoder(bytes, isHtml)`（垫片第 11 节），Qt5 侧走 `QTextCodec::codecForHtml/codecForUtfText`，并**显式以 UTF-8 作为回落**以对齐原 `value_or(QStringDecoder::Utf8)` | 已闭环 |
| **QNativeInterface（R30）** | 原 15 处 | **Windows 侧 13 处已收敛**为 `snow_shot/platform/windows/monitorgeometry.h` 的 `nativeMonitorHandle()`：Qt6 走 `QNativeInterface::QWindowsScreen::handle()`，Qt5 走 `QPlatformNativeInterface::nativeResourceForScreen("handle", screen)`（读 Qt5 上游 `qwindowsnativeinterface.cpp` 确认其 `resourceType` 表的 `HandleType` 即 `"handle"`，返回 `QWindowsScreen::handle()` → `HMONITOR`，语义完全一致）。剩 2 处 macOS `QCocoaScreen` 延后：Qt 5.15 的 cocoa 插件**并未**实现 `nativeResourceForScreen`（读上游 `qcocoascreen.mm` 确认），`NSScreen*` 只能经插件私有头取得，而 macOS 不在交付范围 | Windows 侧已完成 |
| **QPointingDevice（R31）** | 4 处，均在 `screenshot_pinned_window_tests.cpp` | **已完成**：Qt5 侧 `QPointingDevice` **完全不存在**（Qt 5.15 只有 `QTouchDevice`，且 `QInputEvent` 不带设备指针），`QNativeGestureEvent` 构造形参个数也不同（Qt6：`type, device, fingerCount, local, scene, global, value, delta`；Qt5：`type, dev, local, window, screen, value, sequenceId, intArgument`）。改为调用兼容头新增工厂 `snowNativeGestureEvent()` 吸收差异，4 处调用点不再出现 `QPointingDevice` | 已闭环 |
| **CMake 插件体系** | `ant_design_qt/CMakeLists.txt` 等 61 处 `qt_import_plugins` | Qt5.15 提供 `qt5_import_plugins()`（语义相同，自 5.14 引入），由 compat 模块转发 | 中，机械 |
| **构建契约** | `cmake/SnowQt.cmake:29` `find_package(Qt6 ... EXACT)`、`QT_DISABLE_DEPRECATED_UP_TO` | 抽象为按 `SNOW_QT_MAJOR` 分派 | 小 |

### L3b — 兼容头新增垫片（第 10–15 节 + 第 6/8 节新增，本轮）

| 节 | 垫片 | Qt6 侧 | Qt5 侧 |
|---|---|---|---|
| 10 | `SnowPromise<T>` | `QPromise<T>` | 类内持 `QFutureInterface<T>`，析构未 finish 时补 `reportFinished()` |
| 11 | `SnowStringDecoder` / `snowTextDecoder(data, html)` | `QStringDecoder` | `QTextCodec::codecForHtml/codecForUtfText` + `ConverterState::invalidChars`，显式 UTF-8 回落 |
| 12 | `snowTimeZoneFromSecondsAheadOfUtc(int)` | `QTimeZone::fromSecondsAheadOfUtc` | `QTimeZone(int offsetSeconds)`（`qtimezone.h:94`） |
| 13 | `SnowChronoTimer` | `QChronoTimer` | `QTimer` 子类，chrono 纳秒向上取整到毫秒（Qt5 的 QTimer 只有毫秒精度） |
| 14 | `SnowColorScheme` / `snowColorScheme(hints)` | `Qt::ColorScheme` / `QStyleHints::colorScheme()` | 类内枚举 + 按 `QPalette::Window` 亮度推断 |
| 14b | `snowConnectColorSchemeChanged(hints, ctx, fn)` | `QStyleHints::colorSchemeChanged` 信号 | `QGuiApplication::paletteChanged`（Qt 5.15.2 `qguiapplication.h:191`）+ 重新推断；**有损降级**，非幂等的调用点不可用 |
| 15 | `QByteArrayView`（最小子集） | 原生 | 自实现类：`(const char*, qsizetype)` / `const char[N]` / `QByteArray` 构造 + `size/data/empty/operator[]/begin/end` |
| 6（新） | `SNOW_FONT_SET_VARIABLE_AXIS` / `SNOW_FONT_SET_FEATURE` | `QFont::setVariableAxis/setFeature(QFont::Tag, ...)` | **no-op 宏**（Qt5 无字体变体轴 / OpenType 特性 API，且无等价物：`QFont::Weight` 是 0..99 旧刻度）|
| 8（新） | `snowNativeGestureEvent(type, local, window, screen, value)` | `QNativeGestureEvent(type, QPointingDevice::primaryPointingDevice(), 2, local, scene, global, value, QPointF())` | `QNativeGestureEvent(type, nullptr, local, window, screen, value, 0, 0)`（Qt5 收 `QTouchDevice*` + 尾参对）|
| 8（新） | `snowLibraryInfoPath(Location)` | `QLibraryInfo::path(loc)` | `QLibraryInfo::location(loc)`（模板转发，回避 Qt6 双枚举命名）|

三个新垫片都有探针覆盖（`compat-shim-probe.cpp` 第 33/34/35 段），因此它们的 Qt5 分支
不是"纸面正确"而是**编译验证过**的。

同时修正了两处**此前编造的 API**（已用 Qt5/Qt6 双份头文件交叉验证）：
`snowGetHslF` 返回类型 `bool → void`（`QColor::getHslF` 两版都返回 `void`），
`snowFirstTouchGlobalPos` 的 Qt5 分支 `globalPosF() → screenPos()`
（Qt5 `QTouchEvent::TouchPoint` 无 `globalPosF()`）。

### L3c — 事件坐标访问器（第 2b 节）补全

`QNativeGestureEvent` 必须计入事件类型表：Qt6 里它继承 `QSinglePointEvent` 因而有
`position()/globalPosition()`，而 Qt5 只有 `localPos()/windowPos()/screenPos()/globalPos()`
（`QtGui/qevent.h:343-362`）。漏登记会让这处**真阳性**沉进人工清单——
`screenshotpinnedinteraction.cpp:546` 的 `gesture->position()` 就是这么漏掉的，
现已补 `snowPos/snowGlobalPos(const QNativeGestureEvent*)` 重载与
`snowPos(const QEvent*)` 运行时分发里的 `QEvent::NativeGesture` 分支。

---

## 3. Windows 测试路径——以及必须先解决的硬前置

### 3.1 本机环境实测（这是当前真正的阻塞）

| 组件 | 实测结果（首轮快照） | 现况 |
|---|---|---|
| Qt（任意版本） | **未安装**（无 `C:\Qt`、无 `D:\Qt`） | 已装 `C:\Qt\5.15.2\msvc2019_64` |
| CMake / Ninja | **未安装**（`cmake`、`ninja` 均不在 PATH，全盘抽样未找到 `cmake.exe`） | 已装 `C:\snow-tools\cmake`（4.2.3）与 `C:\snow-tools\ninja` |
| MSVC / Visual Studio | **未安装**（`vswhere.exe` 不存在，常见安装目录均不存在） | 已装 `C:\snow-tools\vs2022bt`（v143 + Windows SDK） |
| Rust（1.97.1，`rust-toolchain.toml` 要求） | **未安装**（无 `~/.cargo/bin`） | 仍未装 |
| vcpkg | **未安装** | 已在 `.tools/vcpkg`（baseline `4497409a`，promisor 局部克隆）；32 包安装计划已推进到 **30/32**——`ffmpeg` / `libheif` / `x264` / `x265` 等历史上最难的包已装成，剩余 `onnxruntime[directml]` / `opencv4` / `tinyxml2`（见 §3.6 说明为何 snow_shot 的 CMake 面不等它也能验） |
| 既有构建产物 | **无**——仓库内不存在任何 `build/` 目录，从未在本机构建过 | 已有 `build/windows-qt5-smoke`（Qt5 冒烟）、`build/windows-qt5-preset-proof`（全新目录全量证明）、`.workbuddy/qt5-probe`（Qt5 路径探针） |
| 磁盘 | C: 68 GB 可用 / D: 5.6 GB 可用 | 基本不变，构建产物需注意占用 |

**含义**：编译器与 Qt5 套件已就位，Qt5 冒烟链已能在本机全量构建（见 §3.5）。
现在唯一未打通的是 **vcpkg 第三方依赖**（OpenCV/ffmpeg/onnxruntime 等），其网络可行性实测见 §6.2。

### 3.2 必须安装的清单（Windows）

| # | 组件 | 用途 | 下载体积 | 备注 |
|---|---|---|---|---|
| 1 | Visual Studio 2022 Build Tools（**v143 工具集**）+ Windows SDK | C++/Rust 编译 | ~7 GB | **不要用 VS2026 v14.5x**，见 3.3 |
| 2 | CMake ≥ 3.22（建议最新） | 配置/生成 | ~100 MB | `CMakePresets.json` 目前写的是 VS2026 生成器，需加 preset |
| 3 | Ninja | 快速生成器 | ~5 MB | 可选，但强烈建议 |
| 4 | rustup + Rust 1.97.1 | Rust 静态库 | ~1.5 GB | `rustup toolchain install 1.97.1` |
| 5 | **Qt 5.15.x 套件** | 降级目标 | 见 3.3 | 关键决策点 |
| 6 | vcpkg + 依赖（OpenCV 等） | 第三方库 | 数 GB / 1–3 h | 已有 `cmake/vcpkg-overlay-triplets/*-windows*.cmake`，但**无 Linux 三元组** |

总计约 **10–12 GB 下载 + 数小时**。C: 的 68 GB 够用；**D: 只有 5.6 GB，构建目录建议全部放 C: 或外接盘**。

### 3.3 Qt5 套件的两个选项（需你拍板）

| | A. 官方预编译动态 Qt 5.15.2 | B. 自建静态 Qt 5.15.2 |
|---|---|---|
| 获取 | Qt 官方归档 `qt-opensource-windows-x86-5.15.2.exe` / 模块化 `msvc2019_64` | `scripts/build-static-qt.ps1` 改版本号后自建 |
| 体积 / 时间 | ~1.5 GB / **10 分钟** | ~18 GB / **1.5–3 小时** |
| 与发布形态一致 | ✗（发布是静态 Qt） | ✓（`SNOW_SHOT_QT_STATIC` 要求静态套件） |
| 编译器要求 | MSVC 2019+（v14x 二进制兼容，可用 v143） | 需 MSVC 2019/2022；**Qt 5.15.2 源码不支持 VS2026 的 v14.5x 工具集**，必须回退 v143 |
| 适合 | **本机冒烟测试（推荐先走这条）** | 后续出包 |
| 许可 | LGPLv3 | LGPLv3（静态链接需提供可重链接目标文件） |

> **许可提示（重要）**：Qt 5.15.2 是 5.15 系列**最后一个开源版本**；5.15.3 起 LTS 仅对商业订阅开放。
> 官方对 5.15 的公共支持已于 **2025-05-26** 结束；5.15.19 于 **2026-05-19** 转为公开。
> 若采用 5.15.2 + 静态链接，需按 LGPLv3 履行"可重链接"义务（提供 `.lib`/`.obj` 或目标文件），
> 或改用动态链接。**这是信创/合规审查会问到的点，建议提前准备说明。**

### 3.4 Windows 冒烟测试验收清单（P5 出口）

配置 → 编译 → 逐项手测。**"起得来"这一步已脚本化**，不必靠双击试：
`bash .workbuddy/qt5-port/launch-qt5-app.sh <preset> <exe 相对路径> [保持秒数]` ——
它按这条路线的方式装配 `PATH`（Qt 5.15.2 的 `bin` + `vcpkg_installed/x64-windows/bin`）、
设 `QT_PLUGIN_PATH`，然后在观察窗口内判活：进程活了算 PASS，两秒内退出即 FAIL，
并区分"平台插件没加载"（环境问题）与"起来了但秒退"（应用问题）——这两种处置完全不同，
混在一起看日志会误判。它只证明"没当场崩"，功能项仍须按下表手测：

**手测务必带隔离实例参数**（原因见 §3.8(2)：已安装的正式版会抢单实例键与全局热键，
不带的话构建产物会直接转发给它然后 `exit=0`）：

```bash
bash .workbuddy/qt5-port/launch-qt5-app.sh windows-qt5-full snow_shot/snow_shot.exe 20 \
    --e2e-instance-id=qt5smoke
```

这个脚本在观察窗口结束时会**主动结束进程**，只适合冒烟判活；要坐下来逐项手测就用
下面这个（不设观察窗口、不杀进程，顺带把截图热键播种成 F24 并打印数据目录）：

```bash
bash .workbuddy/qt5-port/start-qt5-manual.sh qt5manual
```

它同样带 `--e2e-instance-id`，所以不会和正式版抢单实例键与热键；日志与崩溃转储落在
`%APPDATA%` / `%LOCALAPPDATA%` 下的 `SnowShot/snow_shot-e2e-qt5manual/`。
**必须在你自己的终端里跑**：从自动化工具里起的话，工具调用一结束子进程就被回收了。

| 项 | 验收要点 | 实测（R44） |
|---|---|---|
| 启动 | 无 `This application failed to start because no Qt platform plugin could be initialized` | ✅ |
| 截图 | 全屏 / 区域 / 窗口三种模式各成一次 | ✅ 三种模式均成 |
| 贴图 | 贴图窗口置顶、拖动、缩放、关闭 | ✅ 含拖边缩放（HTRIGHT）、关闭按钮 |
| 快捷键 | **重点**——`QKeyCombination` 改造的正确性直接体现在这里（含 Ctrl+Shift+Tab 区域类型循环） | ✅ 含区域类型循环 |
| 工具栏悬停 | **重点**——`enterEvent` 签名改造的正确性（悬停高亮、弹出菜单） | ✅ 高亮与弹出菜单均正常 |
| 设置页 | 各页可切换。`removeIf` 的 Qt5 缺陷已修（§3.8(4)），这里要重点看工具栏布局的读写 | ✅ 各页可切换，布局读写正常 |
| OCR | 中文识别出文字 | ✅ 中文识别出文字 |
| 录屏 | 能起录、能停、产物可播 | ✅ 起停与回放正常 |

**R44 结论：全表通过，P5 出口判据达成。** 到此为止，Windows Qt5 构建在
「真实用户手测」意义上与 Qt6 版功能等价；本文档记录过的 12 个 Qt5 专属硬阻塞
（§3.5 起逐节的 (1)~(7) 各条）全部关闭，且没有遗留的产品级缺陷——两条长期挂着的
告警经正式版交叉对照证实是上游既有行为（§3.12(7)）。

**自动化已经覆盖到的部分**（R38 起）：热键 → overlay → 拖框选中 → Ctrl+C 落剪贴板，
连跑 5 轮全绿，入口见 §3.9。贴图链路另有 `probe-pin.sh`（事件级）与
`probe-pin-observe.sh`（窗口级）两个可复跑入口。

**`probe-qt5-modes.sh` 覆盖的剧本**（R38 追加，判据全走窗口枚举 + 日志事件，
不依赖 UIA）：

| 剧本 | 判据 | 实测 |
|---|---|---|
| fullscreen（F22 → `screenshot_full_screen`） | `export.image_finished` + `clipboard.finished` | PASS |
| copy（F19 → 绑到 `screenshot_copy`） | `clipboard.finished` + 剪贴板里出现 1920×1080 位图 | PASS |
| 拖框 + Ctrl+C（`uia-ctrl-c.exe`） | 剪贴板里是合法 PNG，5 轮 | PASS |

注意两件事，否则会把环境问题当成产品缺陷：

- **跑之前不能有别的 `snow_shot.exe` 在跑**。残留实例会先注册同一批全局热键，
  后起的实例拿到 `code=1409`，按键被前一个进程吃掉 —— 表现是"窗口就是不出现"。
  `probe-qt5-modes.sh` 会先检查并直接退出，注册日志里出现 1409 也会单独报出来。
- **设置页（F21）不能和它们放在同一条命令里自动判**：只要应用是这条命令的子进程，
  按 F21 后 20 s 都不出现设置窗口，随后整棵进程树被沙箱 SIGTERM（应用没有崩溃记录：
  emergency 为空、无 `.dmp`）；用后台任务起应用、再从另一条命令发 F21，5 s 就出现在
  734×550 的位置（见 `logs/qt5-settings.png`）。所以设置页按手测处理，别指望脚本判。

**一条命令跑完上面全部自动化项**（构建 + 三个剧本，最后给一张表）：

```bash
bash .workbuddy/qt5-port/verify-qt5-windows.sh            # 含增量构建
bash .workbuddy/qt5-port/verify-qt5-windows.sh --skip-build
```

它只做编排，判据全部复用各脚本自己的退出码；实测输出：

```
================ verify summary ================
  build              PASS
  overlay+drag       PASS
  ctrl+c x5          PASS
  screenshot modes   PASS
===============================================
verify: ALL PASS
```

注意：全新实例的默认 `screenshot` 热键是 **F1**。**手测**用
`start-qt5-manual.sh` 已经播好 **F8 截图 / F9 全屏直接复制 / F10 打开设置** ——
自动化和注入脚本之所以用 F24，是因为它不撞任何常见绑定，但**物理键盘通常没有这个键**，
所以人工测试不要沿用 F24 那套绑定（见 §3.9(6)）。

### 3.5 本轮新增：一个真缺陷修复 + 三个可复跑的验证入口

#### (1) 修复了 Qt5 翻译管线的构建顺序缺陷（`cmake/SnowQtVersionShims.cmake`）

**症状**：在**全新**构建目录里 `cmake --build` 会在构图阶段直接失败：

```
ninja: error: 'ant_design_qt/i18n/ant_design_qt_zh_CN.qm',
  needed by 'ant_design_qt/qrc_ant_design_qt_translations.cpp',
  missing and no known rule to make it
```

**根因**：`qt_add_translations` 的 Qt5 垫片用 `add_custom_target(... COMMAND lrelease ...)`
跑 lrelease，**没有把 `.qm` 声明成产物**。而下面生成的 `.qrc` 会把 `.qm` 列进资源清单，
`qt5_add_resources` 于是把它写进 `qrc_*_translations.cpp` 这条边的**输入**里。Ninja 在
加载构图阶段就要求每个输入都有"产出它的边"，找不到即报错——**且发生在任何命令执行之前**，
所以 `add_dependencies(${target} ${lrelease_target})` 给的先后顺序救不了它。

这个缺陷此前一直没暴露，原因很具体：`build/windows-qt5-smoke` 里 `.qm` **恰好已经躺在
磁盘上**（上一轮构建的残留），Ninja 见到文件存在就不再抱怨。**只有全新目录才暴露它**。

**修复**：改为 `add_custom_command(OUTPUT "${_snow_qm}" COMMAND lrelease ... DEPENDS "${_snow_ts}")`
+ `add_custom_target(${t} DEPENDS ${_snow_qm_files})`。附带收益：每个 `.qm` 只依赖自己的
`.ts`，改一个翻译文件只重跑一次 lrelease，增量更准。

**验证**：全新目录 `build/windows-qt5-preset-proof` 全量构建 **153 目标 / 0 error / EXIT=0**。

#### (2) `CMakePresets.json` 新增 Qt5 预设（此前只能靠命令行直配）

| preset | 作用 |
|---|---|
| `windows-qt5-base`（hidden） | Qt5 路线的公共面：Ninja、`CMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64`、`SNOW_QT_MAJOR=5`、`CMAKE_POLICY_VERSION_MINIMUM=3.5`、`CMAKE_CXX_SCAN_FOR_MODULES=OFF`。**刻意不继承 `windows-msvc-base`**，也**刻意不设 toolchainFile** —— 那个预设用 vcpkg toolchain 把 Qt 钉在 Qt6 上，正是本路线要避开的；不设 toolchain 则 vcpkg manifest 步骤根本不执行，于是第三方依赖通道没打通也能配置、编译 |
| `windows-qt5-smoke` | 只开 `ant_design_qt` + `snow_image`，其余全 OFF（与历轮回归所用配置逐项对齐） |
| `build-windows-qt5-smoke` | 对应 build 预设 |

配套包装：`bash .workbuddy/qt5-port/run-qt5-preset.sh {configure|reconfigure|build} [目标...]`。
**为什么需要包装**：Ninja 从 **PATH** 取 `cl.exe`（不像 VS 生成器能靠 toolset 字段解析），
而本机 MSVC 在 VS2022 BuildTools 下、不在 PATH，且 `vcvarsall.bat` 因 `reg.exe` 被安全策略
拦截而无法定位 Windows SDK —— 必须经 `run-vcvars.py` 注入环境，所以预设不能在别处直接
`cmake --preset`。

**验证**：`cmake --list-presets` 能识别；全新目录仅靠预设配置 + 构建，**0 error / 0 warning / EXIT=0**。
（顺带修掉一个死变量：预设里原先写的 `VCPKG_MANIFEST_INSTALL` 在工程侧无人引用，
只由 vcpkg 自己的 toolchain 读取，每次 configure 都会报 "Manually-specified variables were not used"。
既然本路线不加载 vcpkg toolchain，它既是死变量、又会误导将来继承该 base 的人，已删除。）

#### (3) 兼容头探针扩为两个 TU（`run-compat-probe.sh`）

`build/windows-qt5-smoke` 不含 `snow_shot`，所以兼容头里的**模板**平时只被解析、从未被
实例化，R30 改的 Windows 屏幕句柄解析更是**完全没有编译证据**。探针现在编两个 TU：

1. `compat-shim-probe.cpp` —— 显式实例化全部垫片（`SnowPromise` / `snowTextDecoder` /
   `QByteArrayView` / `SnowChronoTimer` / `snowConnectColorSchemeChanged` …），
   并在第 33 段构造 `QNativeGestureEvent` 以覆盖 **R31** 的 Qt5 分支
   （`QTouchDevice*` + 尾参 `(0, 0)`），同时验证 `&pinch` 能隐式转成测试访问器要的
   **非 const** `QEvent*`（`gesture(ScreenshotPinnedWindow&, QEvent*)`）；
2. `snow_shot/src/platform/windows/monitorgeometry.cpp` —— 覆盖 R30 的 Qt5 分支：
   `#include <qpa/qplatformnativeinterface.h>` 能否解析（Qt5 把它装在
   `include/QtGui/5.15.2/QtGui/qpa/` 而不是顶层 `include/QtGui/qpa/`）、
   `nativeResourceForScreen("handle", QScreen*)` 的实参类型、`const_cast` 与 `void*` 返回是否自洽。

两者均 `EXIT=0`。这两条命令是当前**唯一**能覆盖"vcpkg 依赖之外的 Qt5 代码"的编译级证据。

#### (4) 兼容头的 include 面本身也是一类缺口（本轮新增的排查方法）

R32/R33 不是靠读代码发现的，而是靠**反向扫描**：先按"Qt6 新增 API"清单在全仓 grep 计数，
再逐个拿本机 `C:/Qt/5.15.2/msvc2019_64/include` 的头文件去证伪。这一轮扫出两个真缺口
（`.sliced()` 6 处、`QLibraryInfo::path()` 1 处），也顺手把 3 个**曾被怀疑但实际存在**的
成员钉死为"无需处理"，避免以后重复排查：

| 被怀疑的 API | Qt 5.15.2 判定 | 证据 |
|---|---|---|
| `.sliced(pos[, n])` | **不存在**（真缺口，R32） | `qstring.h`/`qbytearray.h`/`qlist.h`/`qvector.h` grep `sliced` 全 0 |
| `QLibraryInfo::path()` | **不存在**（真缺口，R33） | 只有 `location()`，`qlibraryinfo.h:105` 自带 `### Qt 6` 注释 |
| `QFont::Tag` / `setVariableAxis` / `setFeature` | **不存在**（真缺口，R34） | `QtGui/qfont.h` grep 全 0；Qt5 无任何字体变体轴 / 特性 API |
| `QPointingDevice` | **完全不存在**，连类型都没有 | `QtGui/` 下无 `qpointingdevice.h`；`QInputEvent` 也不带设备指针 |
| `QEventPoint` | **不存在** | `QtGui/` 下无 `qeventpoint.h`；仅 `QPointingDeviceUniqueId` |
| `QLatin1StringView` | **不存在**，但已有别名垫片 | Qt5 是 `QLatin1String`；兼容头第 4 节 `using` 别名覆盖 26 处 |
| `QFileInfo::isJunction()` | 存在 | `qfileinfo.h:116` |
| `QNetworkRequest::setTransferTimeout(int)` | 存在 | `qnetworkrequest.h:194`（6 处调用无需改）|
| `QString::toStdU16String()` / `toStdU32String()` | 存在 | `qstring.h:939/941`（13 + 1 处调用无需改）|
| `QList::assign` | **不存在**，但全仓 43 处 `.assign(` 都是 `std::vector`/`std::string` | 逐站点核对接收者类型后确认 |
| `QTouchDevice` | 存在（R31 的 Qt5 落点） | `QtGui/qtouchdevice.h`，`qevent.h:57` 直接包含它 |

**方法论要点**：`QLatin1StringView` 这类"名字变了但东西还在"的成员最容易被误报成缺口——
它在 `qt_compat.h` 里是 `using QLatin1StringView = QLatin1String;`，而该头由
`snow_find_qt()` 以**目录级 `/FI`** 强制注入，所以 `QLatin1StringView` 的 26 处调用点
无需逐个加 include。判断"是不是缺口"必须同时看**符号存不存在**和**垫片是否已注入**。

---

### 3.6 本轮新增：`snow_shot` 的 CMake 面首次过 Qt5（R35）

**为什么此前没被发现**：`windows-qt5-smoke` 只开 `ant_design_qt` + `snow_image`，
`snow_shot` 那份 9400+ 行的 `CMakeLists.txt` **从未在 Qt5 路线下被解析过**；而完整
configure 又被第 330 行的 `find_package(OpenCV CONFIG REQUIRED)` 挡在前面（vcpkg
依赖尚未齐）。也就是说，这一整面处于"既没验过、也还没轮到"的盲区。

本轮用探针把这条路径**单独**打通：同一份 `cmake/` 脚本、真实 Qt 5.15.2 套件、
不引入任何第三方依赖。一次跑出 **5 个必然失败的 Qt5 阻塞**，其中 4 个只能在这条
路径上暴露（`ant_design_qt` 那条路径走到它们的概率为零）：

| # | 位置 | 症状 | 根因 | 处置 |
|---|---|---|---|---|
| 1 | `snow_shot/CMakeLists.txt:1148` | Qt5 下 configure 硬失败 | 裸的 `find_package(Qt6 REQUIRED COMPONENTS Concurrent)`——本路线**刻意不加载** Qt6 kit（不设 toolchainFile），所以找不到 | 改走共享解析入口 `snow_find_qt(COMPONENTS Concurrent)`；探针断言 `Qt::Concurrent` 与 `Qt5::Concurrent` 均可解析、可链接 |
| 2 | `cmake/SnowShotTranslationRelease.cmake` | 紧随其后的 `qtbase_zh_CN.qm` 检查必然 FATAL | 目录解析整段用 Qt6 专有变量（`QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR` / `QT6_INSTALL_PREFIX` / `QT6_INSTALL_TRANSLATIONS`），Qt5 下三者皆空 ⇒ 路径退化成 `"/"` | 按版本分支：Qt5 依次取 `SNOW_QT5_TRANSLATIONS_DIR` → 由 `Qt5Core_DIR` 反推 `<prefix>/translations` → 由 `SNOW_QT_BIN_DIR` 反推 |
| 3 | 同上（**命名**问题） | 即便目录修对，检查仍永远不可能通过 | 官方 Qt 5.15.2 套件**没有 `qtbase_zh_CN.qm`**：实测该套件 `qtbase_*.qm` 共 23 个，含 `qtbase_zh_TW.qm`，**不含** `qtbase_zh_CN.qm`（简体只以聚合目录 `qt_zh_CN.qm` 发布） | Qt5 分支接受套件自己的命名：`zh_CN` ← `qtbase_zh_CN.qm` 或 `qt_zh_CN.qm`；`zh_TW` ← `qtbase_zh_TW.qm` 或 `qt_zh_TW.qm`。守卫仍在（一个都找不到才失败），只是不再假设 Qt6 的命名 |
| 4 | `cmake/SnowQtVersionShims.cmake` 的 `qt_add_lrelease` 垫片 | **一个 `.qm` 都不产出**，只留一条 WARNING | 垫片签名为 `function(qt_add_lrelease target)`（位置参数形态），而仓库调用用的是 Qt6 6.7+ 的**关键字形态**（`TS_FILES ... LRELEASE_TARGET ...`）⇒ 形参 `target` 绑到字面量 `"TS_FILES"`，`cmake_parse_arguments` 的关键字/值配对整体错位，`TS_FILES` 解析为空 | 兼容两种形态：首个实参不是关键字才当目标名；目标名按 `LRELEASE_TARGET` → `TARGET` → 位置参数 的顺序取值 |
| 5 | 同上（**门禁**） | 翻译目标必然构建失败 | 工程依赖 Qt6 lrelease 的 `-fail-on-unfinished`（AGENTS.md 要求"不得留下 unfinished"），而 **Qt 5.15.2 的 lrelease 没有这个开关**（`-help` 只列 `-nounfinished`/`-removeidentical`/…），原样透传会让 lrelease 打印用法并非零退出 | 新增 `cmake/SnowQt5LreleaseEnforce.cmake`：把开关从 Qt5 命令行摘掉，改为逐 `.ts` 做等价校验 |

#### (1) 第 5 条的坑中坑：Qt5 的 lrelease **不会**因为未完成而失败，统计行也看不出来

同一份 `.ts`（1 条已完成 + 1 条 `type="unfinished"`）在 Qt 5.15.2 下的**原始输出**：

```
Updating 'mix.qm'...
    Generated 1 translation(s) (1 finished and 0 unfinished)
    Ignored 1 untranslated source text(s)
$ echo $?
0
```

`(X finished and Y unfinished)` 里的 `Y` 描述的是**已生成条目**的完成度，未完成条目
被归到另一行；加不加 `-nounfinished` 都不改变这两行，**退出码恒为 0**。
所以"照抄统计行做门禁"会得到一条**永远通过**的假门禁——比构建失败危险得多
（残缺目录会照常进安装包）。`SnowQt5LreleaseEnforce.cmake` 因此用两个独立信号：

1. **主判据**：源 `.ts` 里是否存在 `type="unfinished"`（与 AGENTS.md 的措辞逐字对应，
   不依赖任何工具的文案）；
2. **交叉校验**：lrelease 是否报了 `Ignored N untranslated source text(s)`（`N > 0`）。

实测两者在同一条件下触发（都只针对 `type="unfinished"`）；一个**空但没有**
`type="unfinished"` 的 `<translation></translation>` 两个信号都不报——这与 Qt6 行为一致，
故不额外收紧。

#### (2) 验证入口（可复跑，不依赖 vcpkg）

```bash
bash .workbuddy/qt5-port/run-qt5-probe.sh
```

四段：Qt5 configure（断言 `Qt::Concurrent` 可解析）→ 构建翻译目标（真实 lrelease，
断言产出 3 个 locale 的 `.qm`）→ **反向**注入 `type="unfinished"`（断言构建**失败**）
→ 恢复夹具复检。第三段是这条链的价值所在：没有它，"门禁"只是个看起来像门禁的东西。

另有回归测试扩为 8 例（原有 4 例 Qt6 夹具 + 3 例 Qt5 套件命名 + 1 例需真机的 skip）：

```bash
PATH="/c/snow-tools/cmake/cmake-4.2.3-windows-x86_64/bin:$PATH" \
  python scripts/test-qt-translation-release.py
```

> **`PATH` 必须写 MSYS 风格（`/c/...`）**：写成 `C:/...` 时 Windows 版 python 的
> `shutil.which('cmake')` 返回 `None`，测试会以 `TypeError: ... not NoneType` 报错——
> 那是环境问题，不是测试失败。

**回归证据**：`windows-qt5-smoke` 全量构建在垫片改动后仍 `EXIT=0`（只重建了 MOC 与
`ant_design_qt.lib`，翻译目标无需重跑——`ant_design_qt` 传的 `LRELEASE_OPTIONS` 本来就
是空的，故第 5 条的门禁对它不生效，行为逐字不变）。

#### (3) Qt5 缺口的 stock 对话框翻译 —— 已用「运行时装载」收口（R45）

**原缺口**：Qt6 的 `MERGE_QT_TRANSLATIONS` 会把 `qtbase_<locale>.qm` 并进
`snow_shot_<locale>.qm`；Qt5 垫片没有对应实现，只打一条 STATUS 说"由打包步骤处理"
（而**没有任何打包步骤真的处理它**）。而 `LanguageManager::discoverCatalogs()` 只从
`:/i18n` 装载 `snow_shot_*.qm`，**没有**第二个翻译器读 Qt 自带的 stock 目录。结果是
Qt5 路线下 `QColorDialog`、`QMessageBox` 的标准按钮等会退回英文
（`QFileDialog` 在 Windows 上默认走原生对话框，由系统语言决定，不受影响；
**但在 Linux/UOS 上 `QFileDialog` 也是 Qt 自绘，所以这个缺口在上线平台上更明显**）。

**取方案甲（运行时装载）**，理由：改动局限在一个类里，不需要 `lconvert` 往返，
也不碰打包清单；方案乙（构建期合并）成本高且 `.qm` 往返会丢元信息。

实现（`languagemanager.cpp` / `.h`）：

- 新增私有 `installQtStockTranslator(const QLocale&)` 与成员 `m_qtTranslator`；整体包在
  `#if !SNOW_QT6` 里，**Qt6 行为逐字不变**（Qt6 已经靠合并拿到同样内容）。
- 搜索顺序：`QLibraryInfo::location(TranslationsPath)` → `<exe>/translations`；
  文件名顺序 `qtbase_<locale>.qm` → `qt_<locale>.qm`。
  第二条是必须的：**Qt 5.15.2 套件没有 `qtbase_zh_CN.qm`，简体中文只有聚合文件
  `qt_zh_CN.qm`**（`qtbase_zh_TW.qm` 则存在）。
- **安装时序有讲究**：`QTranslator` 是「后装的优先」被查找，所以先装 stock、再装
  `snow_shot_*.qm`，保证本应用自己的条目盖住 stock 的同名条目。
- 英文（源语言）不装；找不到 catalog 只 WARN **一次**（语言切换会反复走到这里）。

实测（实例 `snow_shot-e2e-i18n`）：

```
Installed Qt stock translation "C:/Qt/5.15.2/msvc2019_64/translations/qt_zh_CN.qm"
```

**打包时的待办**（P7）：`qt_zh_CN.qm` 需要随安装包带上（或在安装脚本里从套件目录拷
一份到 `<exe>/translations`）。开发构建直接读套件的 `TranslationsPath`，不需要处理。

顺带记录两处 Qt6 命名的既有假设，进 UOS 前需要一并按版本分流：
`scripts/snow-build-environment.ps1:252` 与 `scripts/validate-static-qt.py:192`
都写死了 `translations/qtbase_$language.qm`，在 Qt5 套件（无 `qtbase_zh_CN.qm`）上会判失败。

### 3.7 本轮新增：Qt5 Windows 三档阶梯与首次真编译（R36）

前几轮的证据都停在 `ant_design_qt` + `snow_image` 这两个**库**上：`snow_shot` 因为
`find_package(OpenCV CONFIG REQUIRED)` 挡在 configure 阶段，一行都没进过编译器。
本轮先补齐第三方依赖通道，再把"能不能编译"拆成三档，逐档回答一个更窄的问题。

#### (1) 三档预设：每一档只回答一个问题

| 预设 | 编译对象 | 回答的问题 | 依赖 |
|---|---|---|---|
| `windows-qt5-smoke` | `ant_design_qt` + `snow_image` | Qt5 底座还在吗（历轮回归基线，**范围未动**） | 无 |
| `windows-qt5-engine` | 上面的 + `snow_draw_engine_qt` + demo | 真正吃 Qt API 的那层 C++ 过得了 Qt5 吗 | OpenCV |
| `windows-qt5-full` | 再加 `snow_shot` 本体 | 应用整体能不能构建 | OpenCV + ffmpeg + onnxruntime + libjxl/libheif |

**为什么 engine 档不是"无依赖档"**：`snow_draw_engine_qt/CMakeLists.txt:33` 的
`find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc)` 是无条件的，所以
engine 继承 `windows-qt5-full` 而不是 `windows-qt5-smoke`；它隔离的是**代码层**
（draw engine 相对 snow_shot 本体），不是依赖层。把这一点写清楚，是为了避免下一个人
按"smoke 之上、依赖之前"去理解它，然后奇怪为什么它要 OpenCV。

`windows-qt5-full` 相对 `windows-msvc-*` 只差两处：Qt 来自独立 5.15.2 套件而不是 vcpkg，
且 **`VCPKG_MANIFEST_MODE=OFF`** —— configure 只**查找**已装好的依赖树，不启动安装。
后者是刻意的：一个"configure 悄悄开始编译一小时端口"的构建系统，比找不到依赖更难排查。

配套 `bash .workbuddy/qt5-port/run-qt5-preset.sh` 支持 `SNOW_QT5_PRESET=<档名>`，
每档独立 build 目录，互不污染。

#### (2) 三个环境前置（都不在源码里，缺了都会以误导性的错误暴露）

| 前置 | 症状 | 处置 |
|---|---|---|
| `libclang.dll` | `bindgen` panic：`Unable to find libclang: "couldn't find any valid shared libraries matching: ['clang.dll', 'libclang.dll']"` | 本机**没有任何 LLVM**（VS BuildTools 未装 Clang 组件，`C:/Program Files/LLVM` 不存在）。用 PyPI 的 `libclang` wheel 取单个 dll（18.1.1），不装整套 LLVM、不碰注册表；`run-qt5-preset.sh` 在未显式设置 `LIBCLANG_PATH` 时自动指向它 |
| MSVC 的 `INCLUDE`/`LIB` | 上一条修好后紧接着出现 `libavutil/samplefmt.h:22:10: fatal error: 'stdint.h' file not found` —— bindgen 找不到系统头 | 走 CMake 时 `run-vcvars.py` 已注入，无需处理；**单独手跑 `cargo build` 时必须先 source `.workbuddy/qt5-port/msvc-local/env-msvc-v143.sh`** |
| QR 模型下载 | `cmake/FetchSnowShotQrModels.cmake` 在 configure 期从 `raw.githubusercontent.com` 取 4 个模型，失败即 `FATAL_ERROR` | 该主机在本机不可达。vcpkg 的 opencv 端口早已把同样 4 个文件缓存在 `.tools/vcpkg/downloads/opencv-cache/wechat_qrcode/`，逐个核对 SHA512 **全部命中**，拷进 `artifacts/qr-models-wechat/` 后 `file(DOWNLOAD ... EXPECTED_HASH)` 直接跳过传输 |

OCR 资产（`cmake/StageSnowShotOcrAssets.cmake`）只 `WARNING` 不 `FATAL_ERROR`，
下载失败时**OCR 功能不可用但构建继续**——这是既有的诚实降级，冒烟清单里的
"OCR 中文识别"一项在无网络构建下应预期失败而不是误判为回归。

#### (3) draw engine 首次过 Qt5：7 处差异，全部是"Qt6 把两个类型合并了"

编译到链接之前一共 7 处，集中在 `snow_draw_engine_qt`。它们的共同根因只有两条：
**Qt6 让 `QList` 与 `QVector` 变成同一类型**，以及**一批 API 是 Qt 6.x 才有的**。

| # | 位置 | Qt5 报错 | 处置 |
|---|---|---|---|
| 1 | `src/rendering/snow_canvas_image.cpp` `copyMetadata` | `QColorSpace` 无 `colorModel`、无 `ColorModel::Cmyk`、无 `Format_CMYK8888`、无 `reinterpretAsFormat` | Qt6.8 以下是整段编译掉：CMYK 图像根本不存在，因此"保留原模型"这件事无对象，`setColorSpace` 就是同一终态 |
| 2 | 同上 `exactQtFallback` | `Format_CMYK8888` 未定义 | 用 `#if QT_VERSION >= 6.8` 折成一个 `bool cmykSource`，版本外的分支里恒 `false` |
| 3 | 同上，float 格式的 `switch` 标签 | `Format_RGBX32FPx4` / `RGBA16FPx4` 等未定义（Qt6.2 才有） | 标签整体包进 `#if QT_VERSION >= 6.2` |
| 4 | 同上，`QImage::convertedToColorSpace(space, format, flags)` 与 `colorTransformed(transform, format, flags)`（共 5 个调用点） | 重载不存在 | 新增两个 helper `convertColorAndFormat` / `transformColorAndFormat`：Qt6 走上表原路，Qt5 拆成"先转色彩空间、再转格式"两步。**像素与色彩空间一致，代价是多一个临时图** |
| 5 | 同上 `working.flip(mirrored)` | `QImage` 无 `flip`（Qt6.9 才有） | `#if >= 6.9` 保留原地翻转；否则 `working = working.mirrored(mirrored)`（仍在 `working` 独占时不复制像素） |
| 6 | 同上 `ImageFormatInfo::defaultColorTable` | `setColorTable(const QVector<QRgb>)` 不接受 `QList` | 声明改成 `QVector<QRgb>`：Qt6 里两者同型，Qt5 里正是 `setColorTable` 要的那个 |
| 7 | `src/rendering/snow_canvas_watermark_renderer.cpp`、`src/text/snow_canvas_text_layout.cpp`、`src/text/snow_canvas_text_edit_geometry.cpp`、`src/core/snow_canvas_input_adapter.cpp`、`qt-demo/demo_window.cpp` | `QList<quint32> = run.glyphIndexes()`、`QVector<QGlyphRun> = layout.glyphRuns()`、`qMin(int, qsizetype)`、`QMouseEvent::position()`、静态调用 `QFontDatabase::families()` | 逐条按"哪个版本来回传递就用哪个类型"修正；鼠标位置与字体枚举按版本分支（`localPos()` / `QFontDatabase().families()`） |

一句话总结这 7 处的性质：**没有一处是业务逻辑差异，全是 Qt 类型/API 的年代差**。
因此每一处都用 `QT_VERSION` 分支保持 Qt6 路径逐字不变，而不是把代码改成"两边都能接受
的第三种写法"——后者会让 Qt6 的行为无法验证。

#### (4) 编译过了、链接不过：opencv4 的 CRT 与共享 Qt5 套件冲突

第 (3) 节全部修完后，engine 档 **C++ 全部编译通过**，失败前移到链接：

```
opencv_imgproc4d.lib(morph.dispatch.cpp.obj) : error LNK2038:
  检测到 RuntimeLibrary 的不匹配项: 值(MTd_StaticDebug)不匹配值(MDd_DynamicDebug)
```

根因在端口而不是代码：`cmake/vcpkg-overlay-ports/opencv4/portfile.cmake:9` 无条件
`set(VCPKG_CRT_LINKAGE static)`，因为**发布配置**链接的是静态 Qt 套件 + 静态 CRT，
`std::string` / `std::vector` 跨 CRT 堆传递会直接崩。而 Qt5 验证路线用的是**共享** Qt
5.15.2 套件（`msvc2019_64`，`/MDd`），`/MT` 的 OpenCV 根本进不了它的链接行——
这是 `LNK2038`，不是可以忽略的警告。

处置：默认保持静态 CRT 不变，另开一个**只给 Qt5 验证路线用**的变体端口：
`.workbuddy/qt5-port/vcpkg-mirror/overlay-ports/opencv4/`，它只有两行——`set(SNOW_OPENCV_DYNAMIC_CRT TRUE)`
然后 `include()` 仓库里的同一份 portfile。仓库 portfile 里那行改成 `if(NOT SNOW_OPENCV_DYNAMIC_CRT)`。

**为什么不是环境变量**（第一版就是环境变量，白跑了一次 49 分钟）：vcpkg 求值 portfile 时用的是
**被清洗过的环境**，`$ENV{SNOW_OPENCV_DYNAMIC_CRT}` 恒为空——实测导出 `=1` 后重装，
`BUILD_WITH_STATIC_CRT` 仍是 `1`、`CMAKE_CXX_FLAGS_DEBUG` 仍是 `/MTd`。CMake 变量在同一次
`include()` 里可见，所以包装端口是唯一既能生效、又不必改 triplet 的办法（改 triplet 文件会改变
**所有**端口的 ABI 哈希，等于全量重建）。

镜像 overlay 目录本来就排在仓库目录之前（`crashpad` 已经在这么用），所以同名端口会后者被前者
覆盖，且 ABI 因 portfile 文本变化而必然重建——这正是需要的。

**必须记住的副作用**：OpenCV 在同一个 `vcpkg_installed` 里只能有一个变体。跑完 Qt5 验证、要回到
静态 CRT 的发布配置时，把该目录移除（或排到仓库目录之后）重装一次 `opencv4` 即恢复 `/MT`。
这不是"两者共存"的开关。

#### (5) 换了包装端口还是 `/MT`：vcpkg 工具链的 CRT 缓存项不做 FORCE

包装端口被 vcpkg 采纳（日志里能看到 `installing overlay port from here`）、`BUILD_WITH_STATIC_CRT`
也确实变成 `0` 了，但编译命令行**仍然是** `/MTd`：

```
BUILD_WITH_STATIC_CRT:BOOL=0                             ← portfile 的改动生效了
CMAKE_CXX_FLAGS_DEBUG:STRING='/MTd /Z7 /Ob0 /Od /RTC1'   ← 可是标志没变
CMAKE_MSVC_RUNTIME_LIBRARY:STRING=MultiThreaded$<$<CONFIG:Debug>:Debug>$<$<STREQUAL:static,dynamic>:DLL>
```

第二行里那个 `$<STREQUAL:static,dynamic>` 是证据：它是
`.tools/vcpkg/scripts/toolchains/windows.cmake:3` 用**上一次**的 `VCPKG_CRT_LINKAGE`（即 `static`）
展开出来的。该文件里 `CMAKE_MSVC_RUNTIME_LIBRARY` 与 `CMAKE_C/CXX_FLAGS_{DEBUG,RELEASE}` 全都是
`set(... CACHE STRING "")` —— **没有 FORCE**。于是同一个
`buildtrees/opencv4/x64-windows-{dbg,rel}` 目录只要先按静态 CRT 配过一次，旧值就一直存活，
后续配置既不改写也不报错。**构建是成功完成的**，问题只在链接期以 `LNK2038` 出现。

这是一类值得单独记住的坑：**"portfile 已经生效"不等于"编译选项已经生效"**，判据必须取
命令行的实际标志（或 `dumpbin //directives`），不能取 portfile 传给 CMake 的 `-D` 值。

处置（不删构建目录）：在 portfile 里对动态 CRT 路线显式覆盖这些缓存项。命令行 `-D` 的优先级高于
`set(... CACHE ...)`，所以能盖掉陈旧值；而这段被 `if(SNOW_OPENCV_DYNAMIC_CRT)` 包住，
静态 CRT 的发布配置仍完全走 vcpkg 默认值。

第二版写法又踩了一个坑：把 `CMAKE_MSVC_RUNTIME_LIBRARY` 覆盖成和 vcpkg 一样的生成器表达式
`MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`，configure 直接失败——

```
ninja: error: build.ninja:5: bad $-escape (literal $ must be written as $$)
```

经 `-D` 传进去的生成器表达式在这个位置不会被求值，原样落进 `build.ninja` 的规则变量里。
最终写法是**置空**（= 不要自动加 `/M` 标志），CRT 由四条显式的 `*_FLAGS_*` 决定：

```cmake
if(SNOW_OPENCV_DYNAMIC_CRT)
  list(APPEND ADDITIONAL_BUILD_FLAGS
      "-DCMAKE_MSVC_RUNTIME_LIBRARY="
      "-DCMAKE_C_FLAGS_DEBUG=/MDd /Z7 /Ob0 /Od /RTC1"
      "-DCMAKE_CXX_FLAGS_DEBUG=/MDd /Z7 /Ob0 /Od /RTC1"
      "-DCMAKE_C_FLAGS_RELEASE=/MD /O2 /Oi /Gy /DNDEBUG /Z7"
      "-DCMAKE_CXX_FLAGS_RELEASE=/MD /O2 /Oi /Gy /DNDEBUG /Z7")
endif()
```

这套组合先用一个最小工程验过（`.workbuddy/qt5-port/tmp-crt-probe/`：同一条 vcpkg 工具链 +
同一条 overlay triplet + 一个 `main.cpp`），`dumpbin //directives` 给出
`/DEFAULTLIB:MSVCRTD`（动态调试 CRT），且不再有任何 `RuntimeLibrary=` 的 `FAILIFMISMATCH`。

**副作用要记住**：`CMAKE_MSVC_RUNTIME_LIBRARY` 置空后，目标不再发 `/FAILIFMISMATCH:RuntimeLibrary`
指令，也就是说**这类库的 MT/MD 混用不会在链接期被 LNK2038 抓到**。所以"链接通过"对这条路线来说
不再是 CRT 一致性的证明，判据只能是 `dumpbin //directives` 看实际 `/DEFAULTLIB`。

#### (6) engine 档闭环：链接通过 + 窗口起得来

CRT 修好后 engine 档一次通过：

```
[4/7] Linking CXX executable snow_draw_engine_qt\qt-demo\snow-draw-engine-qt-demo.exe
[run-vcvars] EXIT=0
```
产物 36.9 MB，随后 `launch-qt5-app.sh` 的判定是 `PASS —— 存活 10s 后被脚本结束`。
**这是 P5 第一个真正闭环的里程碑**：Qt5 + 共享套件 + 静态库 OpenCV + Rust 桥 + 全量 C++ 都跑通了，
而且窗口能起来（不只是"编过了"）。

`launch-qt5-app.sh` 自身还有一个路径坑，值得单独记：**PATH 里的条目必须写 MSYS 风格**。
写 `C:/Qt/5.15.2/msvc2019_64/bin` 时，MSYS 不会把它转换进给原生子进程的 Windows `PATH`，
Windows 加载器找不到 `Qt5Svgd.dll`，报出来的是伪装成 Unix 的
`error while loading shared libraries: Qt5Svgd.dll` —— 看着像缺库或代码问题，其实是脚本写法。
改成 `/c/Qt/...` 立刻 PASS。（`QT_PLUGIN_PATH` 反过来用 Windows 风格即可，Qt 自己能认。）

#### (7) full 档 configure：三个垫片缺口（都是"签名/语义错位"，不是缺功能）

`windows-qt5-full` 的 configure 从 1 处错误降到 0，中间修了三处：

1. **`clang-format` 不在 PATH**（环境，不是 Qt5）：snow_shot 的图标包生成器在 configure 期
   `shutil.which('clang-format')`，找不到就 `icon manifest error` 直接失败。它随 VS BuildTools 的
   LLVM 组件在 `VC/Tools/Llvm/bin`，但不在默认 PATH。`run-qt5-preset.sh` 里自动补上
   （同样必须是 `/c/...` 的 MSYS 形式）。
2. **`qt_add_resources` 的垫片从来没生效**。Qt 5.15 的 `Qt5CoreMacros.cmake:355` **自己定义了**
   `qt_add_resources`，只是 `qt5_add_resources` 的**变量形式**别名。于是
   `if(NOT COMMAND qt_add_resources)` 守卫在 5.15 上恒为假，目标形式的调用被 Qt5 当成
   "一串 .qrc 文件名"处理，把 `BASE resources/about` 里的**目录**当 qrc 去读，报
   `file failed to open for reading (Permission denied): .../snow_shot/resources/about`。
   这是最像"资源路径写错"、实际是命令签名错位的一类错误。处置：在 Qt5 分支无条件覆盖该命令
   （变量形式原样转发给 `qt5_add_resources`，行为不变）。
   顺带修掉 BASE 为**相对路径**时的 `file RELATIVE_PATH must be passed a full path`。
3. **`qt_add_lupdate` 不能只打日志**。实仓紧接着
   `add_dependencies(snow_shot_update_translations ...)` 与 `add_custom_command(TARGET 同名 POST_BUILD)`，
   空实现会让 configure 死在"Cannot add target-level dependencies to non-existent target"。
   改为按 `LUPDATE_TARGET` 建一个空的 custom target（接口保住，语义仍如实说明要手工跑 lupdate）。
4. **`qt_generate_deploy_app_script` 的 `OUTPUT_SCRIPT` 是变量名，不是路径**。早先把它当路径
   `file(GENERATE OUTPUT ...)`，变量始终为空，`install(SCRIPT "")` 于是报
   `install given a directory as value of SCRIPT argument`。改为生成一个 no-op 脚本到 build 目录
   并把**路径写回该变量**。

configure 收尾：`-- Configuring done (31.0s)` / `-- Generating done (6.7s)` / `EXIT=0`。

### 3.8 本轮新增：full 档链接闭环 + 首次真机启动（R37）

**结论先说**：`windows-qt5-full` 全量 `550/550` 目标、`EXIT=0`、产出 154 MB 的
`build/windows-qt5-full/snow_shot/snow_shot.exe`；真机启动不再崩溃，日志里
`application.ready` + `capture.backend_ready` 都出现，emergency 日志为空。
剩下的是 §3.4 的**手测**项。

#### (1) 链接期的两个未定义符号 —— 都是 `qt_add_resources` 垫片的语义错位

`LNK2019: qInitResources_snow_shot_about_assets` 与 `qInitResources_snow_shot_microtex`。
看起来像"资源没编进去"，实际是垫片两处独立的错：

- **形式 B（`qt_add_resources(<VAR> <file.qrc>)`）的结果没有回传**。垫片内部调
  `qt5_add_resources`，而后者用 `set(... PARENT_SCOPE)` 回传——它的 parent 是**垫片自己**，
  不是真正的调用方。于是 `_resource_sources` 在 `MicroTeX.cmake` 里恒为空，rcc 一次都没跑。
  修法：垫片里显式 `set(${_snow_var} ... PARENT_SCOPE)`。
- **形式 A 的 qrc 文件名决定了 rcc 的 `--name`**。Qt5 的 `qt5_add_resources` 用
  `get_filename_component(outfilename NAME_WE)` 当 `--name`，而 `Q_INIT_RESOURCE(x)` 展开成
  `qInitResources_x`。早先命名 `${target}_${name}.qrc`，于是符号成了
  `qInitResources_<target>_<name>`，与代码要的对不上。修法：qrc 就叫 `<资源名>.qrc`
  （实仓 6 处目标形式的资源名全局唯一，不撞名）；同时把 `file(GENERATE)` 换成
  `file(CONFIGURE)`——`qt5_add_resources` 在**配置期**解析 qrc 算 DEPENDS，文件那时还不存在
  就等于没有依赖，改 `.svg` 不会重新 rcc。

#### (2) 真机首次启动：三次失败，三种完全不同的原因

| 现象 | 真实原因 | 判定依据 |
|---|---|---|
| `exit=2` | 10 月 15 日遗留的旧进程（PID 5356）持着 storage lock，且无法被联系 → `SingleInstanceOutcome::Failed` | 应用日志 `Another Snow Shot process owns the storage lock but could not be contacted` |
| `exit=127` + 崩溃转储 | 启动期 `qFatal` → 我们的 panic 桥 → Crashpad | `emergency` 日志 `event=qt.fatal` / `runtime.panic` |
| `exit=0` 秒退 | 转发给已安装的正式版（`D:\Program Files\SnowShot\bin\snow_shot.exe --autostart`），它占着单实例键 | 进程命令行 + 无 `application.ready` |

**这三种都不能靠"改代码"解决**，分别属于环境残留、真缺陷、环境共存。所以工具先补：
`launch-qt5-app.sh` 现在支持第 4 个参数起**原样透传**给被测程序，配合应用自带的
`--e2e-instance-id=<id>`（换应用名 → 换单实例键与 AppData 目录），可以做到
**不动用户的正式配置**地跑构建产物：

```bash
bash .workbuddy/qt5-port/launch-qt5-app.sh windows-qt5-full snow_shot/snow_shot.exe 20 \
    --e2e-instance-id=qt5smoke
```

#### (3) 诊断层的真缺陷：`qFatal` 的正文被丢了

`diagnostics.cpp` 的 `messageHandler` 对 `QtFatalMsg` 是"记事件名 + 立刻 panic"，
**正文一个字节都不留**，于是崩溃现场只剩 `event=qt.fatal` 四个字。这不是 Qt5 的问题，
Qt6 同一份代码同样会丢，只是之前没人在这个路径上崩过。

修法：panic 之前把正文写进 emergency 通道。三个细节都不是随意选的：

- 走 `snow_diag_emergency`（`FILE_FLAG_WRITE_THROUGH` 同步写）而不是常规异步队列，
  因为紧接着 `DumpAndCrash` 会终止进程，排队的记录多半丢；
- `level` 写 `FATAL` 但**不能**把 `QtFatalMsg` 传给 `record()`——`record()` 对 Fatal 的处理是
  **再次 panic**，会自递归；
- 用 `QJsonDocument` 序列化而不是手拼字符串，正文里的引号/换行/反斜杠不会弄坏日志行。

#### (4) 移植引入的真缺陷：`removeIf` 把 `end()` 缓存在循环外

修完上面两项后的崩溃点，靠转储解出的调用链是：

```
ConfigurationStore::load → materializeConfiguration → ConfigurationSchema::normalize
  → normalizeToolbarLayout → snow::compat::removeIf<QVector<QStringList>, λ>
  → QVector<QStringList>::erase → qFatal
    ASSERT failure in QVector::erase: "The specified iterator argument 'aend' is invalid"
```

`qt_compat.h` 里 Qt5 分支写的是：

```cpp
const auto end = container.end();   // 快照
while (it != end) { ... it = container.erase(it); ... }
```

`erase` 让容器变短，快照立刻变成"当前 end 的下一格"；当 `it` 走到**新的** `end()` 时
`it != 旧end` 仍成立，于是解引用一过尾位置并 `erase(it)`——`erase(iterator)` 展开成
`erase(it, it + 1)`，那个 `it + 1` 就是非法 `aend`。Qt6 分支走容器自带的 `removeIf`，
完全不经过这段，所以这是**移植引入**的。

修法：每轮重新取 `container.end()`。验证入口：

```bash
bash .workbuddy/qt5-port/run-removeif-probe.sh   # 期望 PASS
```

探针**不真的崩**，而是镜像 Qt5 的 `isValidIterator` 判据主动检测"是否即将以非法 aend 调
erase"。原因是实测出来的环境限制：这台机器上进程一崩就拉起 Windows 错误报告
（WerFault → `reg.exe`），`reg.exe` 在安全中心黑名单上，命中后**整棵进程树被 SIGTERM**，
外层脚本连输出都拿不到——第一版探针就是这样"什么都没打印就退出"，很容易误判成探针没跑。

#### (5) 没有 cdb / WinDbg 时怎么读转储

本机没有 Debugging Tools，`dumpbin` 也不是符号化工具。补了两个脚本（都在
`.workbuddy/qt5-port/`）：

- `read-minidump.py`：只取异常码、异常地址、模块表。够回答"是访问违例还是 fail-fast、
  崩在哪个模块的哪个偏移"。本轮靠它认出了 `0xE0534E4F` 这个自定义码（`'SNO'`），
  从而定位到 `snow_diag_panic` 这条 Rust/C++ 共用的崩溃桥。
- `stack-from-minidump.py`：读 `MINIDUMP_THREAD.Stack` 的栈内存，线性扫描落在 exe
  `.text` 内的返回地址，再用 **`dbghelp.dll`**（系统组件，不需要装调试器）翻符号。

两个坑值得记下来：

- `SymFromAddr` 在本机（MSVC 14.44 的 766 MB PDB + 系统自带 dbghelp）**恒返回失败且
  GetLastError 为 0**，但 `SymEnumSymbols` 能完整枚举 54 万个符号。所以脚本退一步：
  枚举全量符号 → 自己二分查找。`SymLoadModuleEx`/`SymEnumSymbols` 的 **`argtypes` 必须显式声明**，
  否则 ctypes 按 32 位 int 传参，64 位地址被截断，表现同样是"一个符号都翻不出来"。
- 符号表里混着 vftable / RTTI 描述符等**数据**符号，最近符号查询会给出
  `kRecordLimit+0x410` 这种一眼假的结果。必须先解析 PE 拿 `.text` 的虚拟范围做过滤。

#### (6) 现状与仍未验证的部分

启动期零崩溃已由日志证实（`application.ready`、`capture.backend_ready`、
emergency 为空）。以下两条告警**不是缺陷**，别当回归处理：

- `shortcut.registration` `code=1409`（`ERROR_HOTKEY_ALREADY_REGISTERED`）×4：
  已安装的正式版占着同一批全局热键，关掉正式版即消失；
- `Snow Shot updater service exited unexpectedly 1`：开发树里没有待应用的更新包。

仍未验证的是 §3.4 的手测项（截图三模式、贴图、快捷键、工具栏悬停、设置页、OCR、录屏）。


### 3.9 本轮新增：拖框即崩的根因是 Qt 5.15.2 自己的 `tryStart(std::function)`（R38）

这是 P5 出口前最后一个阻塞缺陷，现已修复并端到端验证。结论放在最前面：

> **Qt 5.15.2 的 `QThreadPool::tryStart(std::function<void()>)` 忘了给 `QRunnable::ref` 做 +1。
> 而池线程在跑任务前断言 `!del || r->ref == 1`，且 5.15 里 `autoDelete()` 就是 `ref != -1`。
> 于是「池子收下了、但 ref 仍是 0」必然触发断言。** 修法是绕开那条重载，改走一直正确的
> `tryStart(QRunnable*)`。

#### (1) 现象与第一层定位

拖框松开后 30 ms 内进程消失，emergency 日志给出：

```json
{"event":"qt.fatal.message","level":"FATAL",
 "message":"ASSERT: \"!del || r->ref == 1\" in file thread\\qthreadpool.cpp, line 92",
 "thread":"Thread (pooled)"}
```

R37 加在 Fatal 记录里的 `thread` 字段这次**没能缩小范围**，原因值得记下来：
`QThreadPoolPrivate::startThread()` 对每个池线程**无条件**执行
`thread->setObjectName("Thread (pooled)")`，所以给 `QThreadPool` 对象命名不会传到线程上，
线程名对所有池都一样。顺着这条线只会白跑。

换思路：用**串行闸门**做二分。在 `snow_canvas_image.cpp` 的 `processChunks()` 里加
`SNOW_QT5_SERIAL_CHUNKS=1` 让分块工作走串行分支，同一条复现路径**立刻不再崩**。
再在 `tryStart` 前后打点，最后一条日志是：

```
[pooltrace] chunks=23 pixels=361201 workers=4 serial=0
[pooltrace] pool before active=0 max=8
[pooltrace] tryStart #1 accepted=1
                    <- 下一行就是 FATAL
```

`pixels=361201` 正好是 601×601 的选区，说明这是**选区确认时把选区图像做格式转换**那条路径，
而崩在新起的第一个池线程上。

#### (2) 用最小探针把「本项目用法」与「Qt 套件缺陷」分开

本机没有调试器，池线程的调用栈里也只有 `QThreadPoolThread::run()`（断言在 `run()` 之前，
拿不到提交方），所以改用**纯净探针**：`.workbuddy/qt5-port/qthreadpool-probe.cpp`，
只依赖 cl + Qt5 头/库（无 moc；必须 `/MDd` + `Qt5Cored.lib`，否则链接到 release 的 Qt5Core，
`Q_ASSERT` 根本不存在，探针等于什么都没验）。四种模式，同一台机器、同一个 Qt 5.15.2：

| 模式 | 调用 | 结果 |
|---|---|---|
| `start` | `pool->start(std::function)` ×3 | 正常，3 个任务都跑完 |
| `trystart-runnable` | `tryStart(QRunnable::create(...))` ×3 | 正常，3 个任务都跑完 |
| `worker` / `main` | `tryStart(std::function)` ×3 + `QSemaphore` 等待 | **挂死**：`accepted=1/1/1`，任务一个都不跑 |
| `worker-runnable` | 工作线程里 `tryStart(QRunnable::create(...))` ×3 | 正常 |

`qVersion()` 打印确认运行时就是 5.15.2。对照 qtbase 源码可以看清差别：

```cpp
// v5.15.2 —— 没有 ++ref
QRunnable *runnable = QRunnable::create(std::move(functionToRun));
if (d->tryStart(runnable)) return true;
delete runnable;
return false;

// 5.15 分支 head（补上了，同时补了失败路径的 --ref）
QRunnable *runnable = QRunnable::create(std::move(functionToRun));
Q_ASSERT(runnable->ref == 0);
++runnable->ref;
if (d->tryStart(runnable)) return true;
--runnable->ref; Q_ASSERT(runnable->ref == 0); delete runnable; return false;
```

而 `start(std::function)` 是转调 `start(QRunnable*)` 的，那条重载一直有
`Q_ASSERT(ref == 0); ++ref;`，所以**只有 `tryStart(std::function)` 这一条重载不可用**。
本仓正好只有一处用到它（`snow_canvas_image.cpp` 的并行分块）。

顺带解释两种症状为什么不一样：应用里断言先炸（`ref=0` 被池线程抓到），
探针里表现为「任务永不执行」—— 同一处 `ref` 记账缺失，在不同线程时序下的两种表现。

#### (3) 修法与验证

```cpp
QRunnable* const job = QRunnable::create([&] { consume(); finished.release(); });
if (pool->tryStart(job)) {
    ++started;
} else {
    delete job;   // tryStart 失败时所有权仍在调用方（Qt5/Qt6 同约定）
}
```

`tryStart(QRunnable*)` 两个大版本语义一致，Qt6 上同样是正确写法，不需要 `#if QT_VERSION`。

两个可复跑入口：

```bash
# 1) 最小探针：四种模式的对照（挂死的两个模式 20s 超时即可判定）
bash .workbuddy/qt5-port/build-qthreadpool-probe.sh
.workbuddy/qt5-port/tmp-uia-build/qthreadpool-probe.exe worker-runnable   # 期望 exit=0

# 2) 真机剧本：起隔离实例 -> 播种 F24 -> 发热键 -> 拖框 -> 判活
bash .workbuddy/qt5-port/probe-pool-crash.sh <instance-id> drag            # 期望 PASS
```

修复前后同一条 `drag` 剧本：修复前 `probe: FAIL`（emergency 里是上面那条断言），
修复后 `probe: PASS`；窗口枚举能看到拖框后三扇可见窗口（overlay 1536×864、
绘制工具栏 994×114、选区工具栏 128×171），视觉核对见 `.workbuddy/qt5-port/logs/qt5-toolbar.png`。

#### (4) 端到端：Ctrl+C 5 轮全绿

`uia-ctrl-c.exe`（`build-uia-ctrl-c.sh` 独立编译，不进 CMake）现在能跑完 5 轮：

```
round 1..5: hotkey+Ctrl+C recognized
screenshot Ctrl+C end-to-end test passed
```

覆盖链路：全局热键 → overlay 出现并取得前台 → 拖 500×500 选区 → Ctrl+C → 剪贴板里是合法 PNG。

#### (5) 两个把「误判」变成「结论」的改动

**(a) 驱动不再把崩溃报成「控件没暴露」**。原来只要等不到工具栏元素就报
`did not expose its toolbar`，而 R37 那轮应用其实**已经崩了**，于是这条缺陷被误记成
「UIA automationId 没暴露，属测试基础设施限制」，白绕了一整轮。现在先查退出码：

```cpp
const std::string status = application.exitStatus();
if (status != "the application is still running") throw ... status ...
```

**(b) Qt5 确实不暴露 UIA automationId，工具栏断言降级为提示**。Qt5 的 Windows 无障碍
只走 MSAA → UIA 桥，元素的 `automationId` 一律为空；把 `objectName` 映射到
`UIA_AutomationIdPropertyId` 是 Qt6 才有的行为。所以驱动现在先数一遍
「有 automationId 的元素个数」，为 0 就打印明确提示并**跳过工具栏元素断言**，
以剪贴板结果作为判据 —— 视觉核对交给 §3.4 手测。这条比「直接失败」诚实，
也比「静默跳过」安全。

#### (6) 新坑：全新实例的截图热键是 F1

隔离实例首次启动时 `global_shortcuts.screenshot` 的默认值是 **F1**，不是 F24。
注入工具发的是 F24，于是「热键发出去了、overlay 就是不出现」，而
`shortcut.registration` 里 `code` 一律是 0（注册成功）—— 极容易被读成注入失效或
热键注册失败。播种 `config.json` 时必须显式写：

```json
{"storage":{"schema_version":1},"global_shortcuts":{"screenshot":["F24"]}}
```

`probe-pool-crash.sh` 已经把这一步做进去了。

**人工测试另用一套绑定**：F24 在绝大多数键盘上没有这个键。`start-qt5-manual.sh`
播的是 **F8 截图 / F9 全屏直接复制 / F10 打开设置**，人能按到才有意义。

#### (7) 追加的自动化剧本与两个"环境陷阱"

`probe-qt5-modes.sh` 把 §3.4 里能自动判的项变成本机可复跑，判据全部基于
**窗口枚举 + 日志事件**（Qt5 没有 UIA automationId，见 (5)(b)）：

| 剧本 | 判据 | 实测 |
|---|---|---|
| `fullscreen`（VK_F22 → `screenshot_full_screen`） | `export.image_finished` + `clipboard.finished`；剪贴板里 1920×1080 位图 | PASS |
| `copy`（VK_F19 → 绑到 `screenshot_copy`） | `clipboard.finished`；剪贴板里 1920×1080 位图 | PASS |
| `all`（上面两条连跑） | 外加 emergency 里零 FATAL | `ALL PASS`（exit=0） |

探索这一层时撞到两个坑，都会伪装成产品缺陷：

1. **残留实例抢全局热键**。前一个实例没退干净时，后起的实例注册 F21/F22 拿到
   `code=1409`（`ERROR_HOTKEY_ALREADY_REGISTERED`），按键被**前一个进程**吃掉，
   本实例什么都不做 —— 表现就是"窗口怎么都不出现"。当时 qm9 还在跑，qm10 的 4 个
   热键全 1409，设置窗口实际上是开在了 qm9 身上（截图里能看到，但 `enum-windows`
   按 qm10 的 pid 查就是 0）。现在脚本启动前先查残留进程，注册日志里出现 1409 也单独报。
2. **设置页不能和自动化命令同处一棵进程树**。应用是该命令的子进程时，按 F21 后
   20 s 都不出现设置窗口，随后整棵树被沙箱 SIGTERM（应用侧无崩溃记录：emergency
   为空、无 `.dmp`）。用后台任务起应用、再从另一条命令发 F21，5 s 就出现在 734×550。
   这是本机沙箱的限制，不是产品缺陷（两次截图都对得上），所以 `all` 里**不含**
   `settings`，设置页回归按手测 + 截图留证处理。

顺带固定的两个小事实：`screenshot_full_screen` 的行为是「截全屏并直接进剪贴板」，
**不弹 overlay**（所以判据不能是"有没有可见窗口"，否则会把正常工作判成失败）；
`send-vk.exe` 只会发单键（外加内置的 `ctrl+c`），要测某个组合键就把它单独绑到一个
空闲 F 键上，比给注入器加组合键支持划算。

### 3.10 手测反馈：「贴图无效」——客户区矩形 vs 窗口矩形（R39）

> **本节 (3) 的修法已被 §3.11 证伪并回退。** 现场判据与工具用法仍然有效，
> 结论请看 §3.11。

真机上点贴图完全没反应。日志里三步就把范围锁死了：

```
pin.started                                          （INFO）
pin.finished  outcome=failed stage=first_frame       （WARN，6ms 后）
pin.rejected  outcome=failed stage=presentation      （WARN）
```

`stage=first_frame` 是「首帧没出来」，`pin.rejected` 是 `present()` 返回 false。
但 `present()` 里每个失败分支都会打 `Pinned window presentation failed: stage=...`，
日志里**一条都没有** —— 说明失败发生在没打日志的那几条静默分支上。

#### (1) 先把静默失败变成具名 stage

`ScreenshotPinnedWindow::present()` 里有 6 处 `finishPresentation(false); return false;`
是不打日志的。全部补上 stage 名（这本身就该常驻，静默失败是这次绕远路的直接原因）：

```cpp
qWarning("Pinned window presentation failed: stage=restore_canvas_document");
qWarning("Pinned window presentation failed: stage=platform_attach");
qWarning("Pinned window presentation failed: stage=initial_geometry");
...
```

重跑一次就得到 `stage=initial_geometry` —— `applyInitialGeometry()` 返回 false。

#### (2) 再拆 `applyAndVerifyNativeGeometry`

它内部有四种失败可能（目标非法 / 重入 / 平台层拒绝 / 尺寸对不上），原来一律返回 false。
全部具名后，日志直接给出答案：

```
Pinned native geometry mismatch: requested=184,184 533x533  actual=192,192 517x517  window=184,184 533x533
Pinned native geometry: platform rejected target=184,184 533x533
```

窗口矩形**正好**是请求的 533x533，客户区却内缩成 517x517（每边 8px）。

#### (3) 根因：`SetWindowPos` 收的是窗口矩形，代码传的是客户区矩形

`screenshot_pinned_window_native::applyClientGeometry()` 的参数叫 `geometry`、语义是
**客户区**，但它直接把 `geometry` 塞进 `SetWindowPos` —— 而 `SetWindowPos` 设置的是
**窗口矩形**。只要窗口带 `WS_THICKFRAME`（`applySystemResizeStyle()` 给贴图窗口加的，
用来支持拖边缩放），两者就差一圈边框，`currentClientGeometry() == geometry` 必然不成立。

为什么 Qt6 上一直没暴露：Qt6 在 `WM_NCCALCSIZE` 里把无边框窗口的客户区撑满整个窗口，
于是客户区矩形恰好等于窗口矩形，错误写法"正好"成立；**Qt5 不做这件事**，
客户区被内缩 8px（与 `QWindowsWindow::setGeometry` 那条告警里的 `margins: 8,8,8,8` 对得上）。

修法：Set 之前先把客户区矩形换算成窗口矩形，按当前 style/exstyle 与 DPI 计算。
没有边框时 `AdjustWindowRectEx*` 是恒等变换，所以 Qt6 路径行为不变：

```cpp
RECT frameRect{...};                      // 客户区矩形
const DWORD style = GetWindowLongPtrW(hwnd, GWL_STYLE);
const DWORD exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
const UINT dpi = GetDpiForWindow(hwnd);
if (!(dpi && AdjustWindowRectExForDpi(&frameRect, style, FALSE, exStyle, dpi)) &&
    !AdjustWindowRectEx(&frameRect, style, FALSE, exStyle)) { /* 退回原矩形 */ }
SetWindowPos(hwnd, nullptr, frameRect.left, frameRect.top, w, h, flags);
```

#### (4) 复跑与仍然待确认的一点

新增 `probe-pin.sh`（F8 截图 → 拖框 → F7 贴图，F7 是把 overlay 内默认的 `Ctrl+F`
重绑成单键，因为 `send-vk.exe` 只发单键）。修复前后：

```
修复前：pin.succeeded=0 pin.rejected=1  → FAIL
修复后：pin.succeeded=1 pin.rejected=0  → PASS
```

窗口枚举能看到贴图窗口：439×439、`layered=1`、`affinity=0x0`（**没有**被排除出截图），
持久化目录里的 `source.png` 就是拖框那块内容，说明导出链路是对的。

**但本机无法用截图判断"屏幕上那张图对不对"**：贴图正好贴在它自己截的那块区域上，
内容与背景 1:1 重合，任何截图对照都会和背景混在一起（最初我把这读成了"内容空白"，
是误判）。这一项只能靠肉眼：贴图应该是一张带边框和阴影的浮层，且**能拖动**。

#### (8) 本轮删除的临时代码

R37 为定位「overlay 全透明」加的 4 处 `TEMP-QT5-DIAG` 全部删除
（`screenshotoverlaywindow.cpp` 的 `window.paint` 与 `grab()` 落盘、
`screenshotcanvasrenderer.cpp` 的 `covers`/`frame`、`screenshotoverlaycanvaswidget.cpp` 的
`canvas.paint`），`snow_canvas_image.cpp` 的 `[pooltrace]` 也一并清掉。
诊断结论已固化在本节，不需要留在代码里。

### 3.11 §3.10 的修法是错的：真正的根因是「客户区 ⊂ 窗口矩形」让逐像素透明失效（R40）

§3.10 (3) 把 `applyClientGeometry()` 改成「先 `AdjustWindowRectExForDpi` 换算成窗口矩形
再 `SetWindowPos`」。手测结果：贴图**仍然无效**，而且进程直接退出。日志给出新的判据：

```
Pinned native geometry mismatch: requested=284,284 633x483  actual=640,270 640x480
pin.rejected  outcome=failed stage=presentation
```

`actual` 和 `requested` 差了一整圈，说明换算方向和 Qt5 自己的取整打架了：写进去的是
"请求 + 边框"，读回来 Qt5 又按它自己的非客户区语义算一遍客户区，永远对不上，
`applyAndVerifyNativeGeometry` 的校验必然失败 → `stage=initial_geometry` → 关窗。

#### (1) 用最小复现隔离「透明」与「几何」

`qpin-min.exe`（`.workbuddy/qt5-port/qpin-min.cpp`，纯 Qt5 + 逐像素透明 + 无第三方依赖）
四档对照，把两个嫌疑彻底分开：

| 模式 | 做法 | 结果 |
|---|---|---|
| `plain` | 半透明无边框窗口 | 正常上屏 |
| `prewarm` | `plain` + 预热建 HWND | 正常上屏 |
| `thickframe` | `plain` + `WS_THICKFRAME` | **完全不显示**（`IsWindowVisible=1`、`paintEvent` 照跑） |
| `nccalc` | `thickframe` + `WM_NCCALCSIZE` 撑满客户区 | 正常上屏 |

结论：逐像素透明本身没问题，预热建 HWND 也没问题；**只要客户区 ≠ 窗口矩形，整块分层表面
就被判成全透明**。逐像素透明窗口的合成面是按**窗口矩形**提交的，客户区小了，
外面那圈"没有客户区覆盖"的区域就让整个 surface 的 alpha 判定失效。

#### (2) 真正的修法：Qt5 上干脆不给贴图窗口加 `WS_THICKFRAME`

`applySystemResizeStyle()` 加这圈边框的唯一目的是"让系统认为窗口可缩放"，从而支持拖边。
但 Qt5 把这圈边框当作**真实非客户区**（每边内缩 8px），Qt6 才在 `WM_NCCALCSIZE` 里
把无边框窗口的客户区撑满整个窗口矩形 —— 所以同一行代码只有 Qt5 出事。

```cpp
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (m_role == Role::Image && !screenshot_pinned_window_native::applySystemResizeStyle(id))
            return false;
#else
        // Qt5 上不给贴图窗口加 WS_THICKFRAME：客户区会每边内缩 8px，
        // 逐像素透明的合成面按窗口矩形提交，整块表面被判全透明。
#endif
```

`applyClientGeometry()` 里 §3.10 加的 `AdjustWindowRectEx*` 换算同步删除 —— 既然
客户区 == 窗口矩形，再换算就是把不存在的边框算两遍。**§3.10 (3) 的做法作废。**

#### (3) 副作用：Qt5 不投递 `WM_NCHITTEST`，拖边缩放要靠应用级过滤器补投

去掉 `WS_THICKFRAME` 之后窗口不可缩放，而贴图窗口的拖边命中码全在
`PinnedWindowWindowsEvents::handle()` 里，靠 `QWidget::nativeEvent` 收消息。
Qt5 的 windows 平台插件**不把 `WM_NCHITTEST` / `WM_SETCURSOR` / `WM_GETMINMAXINFO`
投给 `QWidget::nativeEvent`**（Qt6 会）。

修法：在 `WindowsPinnedWindowPlatform::attach()` 里装一个
`QAbstractNativeEventFilter`（`NativeMessageRelay`），把这三种消息补投给
`handleNativeEvent()`，**只有处理函数认领时才吞掉**（`return true`），其余一律放行：

```cpp
if (msg == nullptr || msg->hwnd == nullptr ||
    msg->hwnd != reinterpret_cast<HWND>(m_owner.m_attached))
    return false;
switch (msg->message) {
case WM_NCHITTEST: case WM_SETCURSOR: case WM_GETMINMAXINFO: break;
default: return false;
}
snowNativeEventResult nativeResult = 0;
if (!m_owner.handleNativeEvent(type, message, &nativeResult))
    return false;
if (result != nullptr) *result = static_cast<long>(nativeResult);
return true;
```

`detach()` 里 `removeNativeEventFilter` + `m_relay.reset()`，否则过滤器持有悬空 owner。
注意 `WId` 与 `HWND` 之间没有隐式转换（`error C2446`），必须显式 `reinterpret_cast`。

#### (4) 验证

```
贴图（Ctrl+F，走真实默认绑定）   426x346 → 上屏、内容在、进程不退出
拖动                             窗口矩形跟着走，冻结帧一起走（不是"透过去看桌面"）
拖右边框                         533x433 → 734x594，命中码 11 = HTRIGHT
                                 等比缩放（updateResize 的设计行为，不是 bug）
probe-pin.sh                     pin.succeeded=1 pin.rejected=0 → PASS
```

`Pinned native geometry mismatch` 与 `QWindowsWindow::setGeometry` 的告警同时消失。

#### (5) 一个把人带偏的工具坑：DPI 虚拟化

`enum-windows.exe` 早先没声明 DPI 感知，在 125% 缩放的机器上 `GetWindowRect` 拿到的是
**虚拟化坐标**（物理 ÷ 1.25）。于是同一个窗口：

```
enum-windows（虚拟化）: rect=(307,227)-(733,573) size=426x346
Qt 内部 GetClientRect :  rect=(384,284) 533x433
```

看起来像"Qt 自己算的客户区和系统不一致"，实际两边都对，只是坐标系不同
（533/1.25=426.4、384/1.25=307.2、284/1.25=227.2 全部吻合）。
按虚拟化矩形去算"右边框在 x≈731"是错的，实测那一点在物理图片正中，
命中码 `HTCAPTION=2` 完全正确 —— 拖边缩放本来就没坏，是探针瞄错了地方。
修法：`main()` 首行 `SetProcessDPIAware()`（`drag-rect.exe` 一直是这样，
所以它是物理坐标、窗口移动量 150 与鼠标位移 150 精确相等，反而成了佐证）。

### 3.12 贴图后做 OCR 就崩：Qt5 的 `QWidget::mapTo()` 对非祖先 target 直接 abort（R41/R42）

**(1) 现场**

贴图本身成功（`pin.started` / `pin.finished` 均 succeeded、内容上屏），随后点「识别文字」，
`ocr.finished operation=4` 之后 0.6 秒进程消失。没有 `session.clean_shutdown`，
`emergency-*.log` 里是 `qt.fatal` + `runtime.panic`：

```
ASSERT failure in QWidget::mapTo(const QWidget *parent, const QPoint &pos):
"parent must be in parent hierarchy", kernel\qwidget.cpp:4059
```

**(2) 根因**

Qt5 的 `QWidget::mapTo(parent, pos)` 在 `parent` 不是自己祖先时**不是返回错值，而是
`Q_ASSERT_X` 直接 abort 整个进程**（Qt6 是同一契约，只是这条路径在 Qt6 上没被走到）。
凡是「`watched` 是任意接收者」的坐标换算都踩在这条上：贴图窗口会把输入转发给识别窗口，
而**识别窗口是独立的顶层窗口，不在贴图窗口的父子链上**。

修复后的日志拿到确证：

```
WARN snowMapToWithinHierarchy: ScreenshotPinnedWindow -> ScreenshotRecognitionWindow
     is not an ancestor of the receiver; using global coordinates
```

**(3) 修法**

`qt_compat.h` 新增 `snowMapToWithinHierarchy(receiver, target, point)`：target 真是祖先就走
`mapTo`，否则退回 `mapToGlobal` / `mapFromGlobal`，并且**只打一次** WARN（打印双方类名，
定位完不需要再加临时日志）。替换的四处都是「target 不保证是祖先」：

| 位置 | 场景 |
|---|---|
| `popup_geometry.cpp` `PopupWidgetRect::mappedTo()` | 同一顶层窗口的**兄弟子树**（弹层/提示） |
| `carousel.cpp` `AdCarousel::eventFilter` | `watched` 是任意安装的接收者 |
| `screenshotrecognitionwindow.cpp` `handleSelectionResizeEvent` | 贴图窗口转发来的鼠标事件 |
| `screenshotpinnedwindow.cpp` `windowPositionForEvent` | 顶层工具窗口（编辑/识别浮层）的事件 |

用 `window()` / `parentWidget()` / `this` / 构造期 `new X(..., this)` 当 target 的调用点
**可证安全**，一个都没动 —— 不给按钮绘制之类的热路径加 `isAncestorOf`。
`ant_design_qt` 侧通过 `ant_design_qt_pch.h` 拿到该 helper。

**(4) 定位手段（结论：只剩「断言文案 + 二分」可用）**

- minidump 符号解析在本机不可用：`stack-from-minidump.py` 对这份 2.1MB dump 吐出的是
  Rust/OpenCV 乱码符号（PDB 与二进制不匹配的老问题），返回地址翻不出函数名。
- 试过链接期 `/MAP` + `RtlCaptureStackBackTrace` 落 `stack` 字段：**能落，但没必要** ——
  断言文案里已经写了 `QWidget::mapTo` 和断言内容，直接二分四处调用点更快。诊断代码已全部删除。

**(5) 同时踩到的两个构建坑**

1. **`LINK=/MAP` 环境变量会毁掉链接命令行**（报 `LNK1104: 无法打开文件 D:\Program.obj`）。
   MSVC 的 `LINK` 不是简单追加；要加链接器参数得走
   `cmake ... -DCMAKE_EXE_LINKER_FLAGS=/MAP`。
2. **`snow-hang-watchdog.sh` 判据太激进，会误杀正常编译**：`cpu=0s && idle=6 samples`
   就杀，而大 TU 前 30 秒本来就 0 CPU。误杀的特征是 ninja 报 `subcommand failed`
   但日志里**一条 FAILED / error 都没有**。它只适合 vcpkg 安装循环，别给普通 ninja 构建挂。
   真挂死长这样：单个 `cl.exe` CPU 冻结在 0.1s、内存个位数 MB、持续 10 分钟以上 ——
   杀掉重跑即可（本轮撞了 3 次，靠 `for i in 1..5` 重试循环越过）。

**(6) 验证**

manual3（清理诊断后的干净二进制）：贴图 → 识别文字 → 鼠标在识别浮层上晃/点 → 拖边缩放，
`Q_ASSERT` 不再触发，进程从前台正常活到 `session.clean_shutdown`（用户从托盘退出），
无 dump、无 panic。

**(7) 两条「疑似迁移遗留」经正式版交叉对照，确认是上游既有行为**

两条在 Qt5 构建里反复出现的告警，用**已安装的正式版（Qt6 构建）日志**做了对照，
两边一字不差，因此**不是 Qt5 降级引入的**，不列为本次迁移的待办：

| 告警 | Qt5 构建（隔离实例） | 正式版 Qt6（`%APPDATA%\SnowShot\snow_shot\logs`） |
|---|---|---|
| `Canvas session restoration failed: stage=deserialize status=1 bytes=408` | 每次选区复制后 1 条 | 4 条，**`status=1 bytes=408` 完全相同** |
| `QObject::disconnect: No such slot QWidget::_q_overrideMenuActionDestroyed()` | 35 条（多实例累计） | 有，`qt.core.qobject.connect` 类别 |

第一条的真实来路（本轮更正）：告警由 `RuntimeSession::restoreDocumentSession()`
→ `runtimeFromSerializedSession()` 打出（`snow_canvas_runtime_session.cpp:103`），
**不是**导出渲染路径。`status=1` = `SNOW_ERROR_INVALID_ARGUMENT`，即引擎拒绝这份
408 字节 payload。时间轴上也对不上：`export.image_finished` → `clipboard.finished`
之后 4 ms 才出现，此时导出已经完成，所以它不阻塞复制/导出，此前担心的「带标注导出
被丢空」没有被证据支持。**结论：上游既有问题（空文档 session 的序列化/反序列化
不对称），与 Qt 版本无关，留给上游单独处理。**

第二条是 Qt5 走**运行期字符串槽查找**（Qt6 用函数指针编译期解析）时对已销毁对象的
disconnect 告警，对象随即销毁，无功能影响。

### 3.13 Qt6 基线补齐：装 6.12.0 套件，并抓出 4 个「只改 Qt5 却改坏 Qt6」的缺陷（R45）

**为什么必须补**：本轮为了打通 Qt5，动了四类**同时在 Qt6 编译路径上**的东西：

| 文件 | 改动 | Qt6 侧是否被 Qt5 构建覆盖 |
|---|---|---|
| `cmake/SnowQt.cmake` | 兼容头注入、`snow_qt_apply_compat_include` | 否（Qt5 构建只跑 Qt5 分支） |
| `snow_shot/include/snow_shot/compat/qt_compat.h` | 大量垫片 | 否 |
| `ant_design_qt/.../popup_geometry.cpp`、`carousel.cpp` | 坐标换算改用 `snowMapToWithinHierarchy` | 是（smoke 档） |
| `pinnedwindowplatform.cpp` / `screenshotpinnedwindow.cpp` / `languagemanager.cpp` | Qt5 专属行为，Qt6 分支保留原逻辑 | 否（要 full 档） |

没有 Qt6 基线，上表前三行等于「改完没验」。这与此前确立的判据一致：**从未被解析过的代码面 = 盲区**。

**(1) 套件怎么装：官方安装器与 aqtinstall 都不通**

- Qt Online Installer 需要交互登录 Qt 账号，不适合脚本化；
- 本机唯一可得的 `aqtinstall 3.3.0` 仍按 **6.12 之前的仓库布局**拼路径
  （`.../desktop/qt6_6120/Updates.xml`），而 Qt 6.12 改成**按架构分目录**：
  `.../desktop/qt6_6120/qt6_6120_msvc2022_64/Updates.xml`。表现是
  `ERROR: Failed to download checksum for the file 'Updates.xml'`，且 `list-qt --arch` 同样失败。
- 官方预编译套件只有 `win64_msvc2022_64`（无 msvc2026 变体）；本机 toolset 是 14.44/v143，
  与之一致（Qt5 路线也是用 msvc2019_64 套件在 v143 下编译，跨工具集链接已验证可行）。

于是改成直接讲仓库协议：`scripts/install-qt6.py` 读 `Updates.xml` → 取主包
`qt.qt6.6120.win64_msvc2022_64` 的 archive 列表 → 下载 → **校验仓库发布的 `.sha1`**
→ `py7zr` 解压到 `C:/Qt/6.12.0/msvc2022_64`。

只下项目真正需要的 4 个 archive（`qtdeclarative`/`qtdoc`/`d3dcompiler`/`opengl32sw` 不装）：

| archive | 下载体积 | 用途 |
|---|---|---|
| `qtbase` | 40.2 MB | Core/Gui/Widgets/Network/Test/Concurrent/PrintSupport + **private headers** |
| `qttools` | 27.4 MB | `LinguistTools`（`lrelease`/`lupdate`） |
| `qttranslations` | 2.0 MB | `qt_zh_CN.qm`（P7 打包要用） |
| `qtsvg` | 0.7 MB | `ant_design_qt` / `snow_shot` 图标 |

装好 411 MB（解压后），`qmake -v` 报 `Qt version 6.12.0`，`scripts/install-qt6.py --check`
七项资产全在（含 `QtCore/6.12.0/QtCore/private`、`QtWidgets` private、`Qt6LinguistToolsConfig.cmake`）。
镜像默认清华，`--mirror https://download.qt.io` 可切官方源。

**(2) 新增预设：Qt6 与 Qt5 只差一个 Qt 前缀**

`windows-qt6-base` / `windows-qt6-smoke` / `windows-qt6-full`（+ 对应 build preset），
与 `windows-qt5-*` **逐项对称**：同一个 Ninja 生成器、同样不设 `toolchainFile`
（smoke 档 vcpkg-free）、同样 `SNOW_APPS_STRICT_COMPILE=OFF`、同样的目标集。
入口 `.workbuddy/qt5-port/run-qt6-smoke.sh`（vcvars 注入 + `LIBCLANG_PATH` + `clang-format`，
与 Qt5 的 `run-qt5-preset.sh` 同构）。这样 Qt5/Qt6 的编译日志可以逐行对拍，
diff 出来的一定是源码差异，不是工具链差异。

**(3) 基线第一跑就红了：4 个真缺陷**

| # | 报错 | 根因 | 修法 |
|---|---|---|---|
| 1 | `qt_compat.h(245): error C2039: "setKeyCombination": 不是 "QKeySequence" 的成员` | Qt 6.12 的 `QtGui/qkeysequence.h` 只有 `setKey(QKeyCombination,int)` 和 4 参 `QKeyCombination` 构造，**没有** `setKeyCombination`。这段代码从没被 Qt6 编译过 | Qt6 分支改用 4 参构造（与 Qt5 分支同构，只差元素类型），且 6.0–6.12 都稳定 |
| 2 | `qt_compat.h(1768): error C2440: 无法从 QStringConverter::Encoding 转换为 SnowStringDecoder` | `QStringDecoder(Encoding)` 是 **explicit**，`SnowStringDecoder(QStringDecoder)` 也是 explicit ⇒ 一次隐式转换序列里不能连做两次 | 显式写出内层 `QStringDecoder(...)` |
| 3 | `checkbox.h(107): error C2061: 语法错误: 标识符 "SnowEnterEvent"` 等（70 个文件） | Qt6 构建**不注入**兼容头（原设计假设 Qt6 走原生 API），但机械改写已把业务代码改成使用垫片符号 | **Qt6 也强制注入**兼容头；Qt6 分支几乎全是原生类型的薄别名，无行为开销 |
| 4 | `qt_compat.h(116): fatal error C1083: 无法打开包括文件 "QEnterEvent"`（`snow_shot_diagnostics`） | 注入后，QtCore-only 目标（无 QtGui include 路径）仍无条件 `#include <QEnterEvent>`；Qt5 分支早有 `__has_include` 保护，**Qt6 分支漏了** | Qt6 分支的 GUI 头改为受 `SNOW_COMPAT_HAS_GUI` 保护；`SnowEnterEvent` 在无 GUI 时退化为 `QEvent` 别名 |

第 3、4 条是这次补基线的主要收获：**「兼容头只在 Qt5 注入」这个设计假设，被机械改写本身推翻了**。

**(4) 结果**

| 配置 | 目标数 | 结果 |
|---|---|---|
| `windows-qt6-smoke`（ant_design_qt + snow_image） | 148 | **全绿**，0 error 0 warning（`ant_design_qt.lib` 133 MB 链接完成） |
| `windows-qt5-smoke` 回归（兼容头改动后） | 147 | **全绿**，0 error |

**(5) full 档：本机不可行，如实记为环境限制**

`windows-qt6-full` 能 configure 成功（3 min），但完整构建不可行，两条硬原因：

1. **磁盘**：Qt5 full 构建树已占 **22.6 GB**，Qt6 full 首次尝试涨到 **13 GB** 时把 C 盘吃满（0 字节可用，
   连 shell 都起不来）。qt6-full 目录已删除，C 盘恢复 12.9 GB。
2. **snow_shot 的 autogen 依赖 Rust FFI bundle**：`snow_shot_autogen` 会拉起
   `cargo build --package snow-shot-rust-ffi --features selected-text`，在该构建树里 **exit 101**。
   这是既有的 Rust/Qt6 构建路径问题（与本次改动无关），修它属于另一个议题。

因此 `pinnedwindowplatform.cpp` / `screenshotpinnedwindow.cpp` / `languagemanager.cpp` 的
**Qt6 分支目前只有语义等价检查，没有编译证据**。补齐的前提是：磁盘 ≥ 40 GB，或把 Qt5 full
构建树挪到别的盘；以及 Rust FFI 在 Qt6 full 树里能编过。工具已备好：
`.workbuddy/qt5-port/compile-qt6-objs.py` 用 `ninja -t commands` 取该预设下**原生正确**的编译命令
（不做 Qt5→Qt6 路径替换），只编这几个 TU，避免整树构建 —— 前置（PCH + autogen）仍是上面第 2 条。

---

## 4. 分阶段实施计划

| 阶段 | 内容 | 出口判据（可验证） |
|---|---|---|
| **P0** | Windows 环境 bootstrap：装 MSVC v143 / CMake / Ninja / Rust / Qt5 套件 | `cmake --version`、`qmake -v`（或 `qtpaths --qt-version`）、`rustc -V` 三者均返回预期版本 |
| **P1** | 落地 L1 兼容层（`qt_compat.h` + `SnowQtVersionShims.cmake`），**Qt6 构建保持不变** | Qt6 分支仍可 configure；新增文件零警告 |
| **P2** | CMake 双版本化：`Qt6::`→`Qt::`(882)、`qt_*` 命令封装、`QT_DISABLE_DEPRECATED_UP_TO` 条件化、新增 `windows-qt5-*` preset | **已完成**：`windows-qt5-base/smoke` + `build-windows-qt5-smoke` 三个预设已入库，全新目录仅靠预设即 `EXIT=0`；出口判据（能走到编译阶段）已超额满足——已能全量编译链接 `ant_design_qt` + `snow_image` |
| **P2b** | CMake 面的 **`snow_shot` 覆盖**（§3.6）：`Concurrent` 包解析、翻译目录解析与套件命名、`qt_add_lrelease` 垫片签名、Qt5 的未完成翻译门禁 | **本轮完成**：5 个 Qt5 硬阻塞已修；探针 `run-qt5-probe.sh` 四段全绿（含反向验证）；`snow_shot` 完整 configure 仍等 vcpkg（OpenCV/onnxruntime），但**它挡不住的那部分已验完** |
| **P3** | L2 机械改写 8 条规则，按规则逐条提交（每条一个 commit） | 每条规则后都能 `git diff --stat` 复核；Qt5 编译错误数单调下降 |
| **P4** | L3 人工改写：viewer RHI 后端、`QStringDecoder`、Windows 打印 | viewer 可编译；若决策为"首版不交付"，则改为在 CMake 中整模块 `option()` 关闭 |
| **P5** | **Windows Qt5 编译打通 + 3.4 冒烟清单全绿** | **✅ 完成（R44）**：full 档 `550/550` 目标全绿、链接通过、真机启动零崩溃；3.4 清单**八项全部手测通过**（含设置页、录屏、工具栏悬停、`Ctrl+Shift+Tab` 区域类型循环、窗口截图）；拖框崩溃（Qt 5.15.2 `tryStart(std::function)`，§3.9）、贴图不可见（`WS_THICKFRAME` 让客户区小于窗口矩形，§3.11）、`mapTo` 非祖先 abort（§3.12）三个 Qt5 专属缺陷全部修复并回归。自动化入口：`verify-qt5-windows.sh`（build/overlay+drag/ctrl+c×5/modes 全 PASS）、`probe-pin.sh`、`probe-pin-observe.sh` |
| **P6** | UOS 编译：amd64 → arm64，按 `docs-uos-linux-port.md` 的 P2–P4 推进平台后端 | UOS amd64 能起界面；arm64 真机（飞腾/鲲鹏）终验 |
| **P7** | 收尾：changelog、版本号、双架构打包 | 产物齐全，`snow-shot-installation.json` 校验通过 |

**顺序上的硬依赖**：P0 → P1 → P2 不可调换；P3 依赖 P2 的 CMake 能配置；P5 依赖 P0。
**P6 的平台后端工作与 P1–P5 完全独立，可并行**（不碰 Qt 版本的部分）。

---

## 5. 风险控制与回滚

| 风险 | 控制手段 |
|---|---|
| 机械改写引入隐蔽错误 | 每条规则独立 commit；Qt6 构建全程保持可编译，作为对照基线做二分定位 |
| `removeIf` / `metaType()` 接收者提取错位 | 脚本只处理简单接收者，复杂表达式输出到 `manual-review.txt` 人工确认 |
| Qt5 静态套件构建失败（v14.5x 工具集） | P0 阶段就用 **v143** 工具集；若仍有障碍，先用选项 A（预编译动态）推进 P1–P5，打包留到后面 |
| `qt_*` 命令在 Qt 5.15 的 versionless 覆盖不全 | P2 第一步先跑一个探测脚本，打印 Qt5 下实际存在的 `qt_*` 命令清单，再补齐 compat 模块（**不靠记忆猜**） |
| C 盘空间 | 构建目录全部指向 C:（68 GB 可用），**不要放 D:**（仅 5.6 GB） |
| 回滚 | 全流程在 `qt5-dual` 分支；`main` 不动。最坏情况 `git checkout main` 即完全恢复 |

---

## 6. 待你确认的三项

1. **Qt5 套件走 A（预编译动态，10 分钟，先测）还是 B（自建静态，1.5–3 小时，与发布一致）？**
   建议 **A 先走**，把 P5 冒烟跑通再补 B。
2. **是否同意安装 P0 清单（约 10–12 GB 下载、数小时）？** 这是本机测试不可绕过的前置。
3. **`snow_image_viewer` 首版是否交付？** 建议 P4 先在 CMake 中用 `option(SNOW_BUILD_IMAGE_VIEWER)` 关闭，
   把 viewer 渲染后端排到 Windows 冒烟通过之后，避免它阻塞主线。

### 6.1 本轮新增的**唯一**决策点（第 3 项已被 1.5 改写，需要重新拍板）

原先的 "RHI → `QOpenGLWidget` 重写" 计划已被 1.5 推翻。现在 viewer 有两条路：

| 路线 | 做法 | 成本 | 风险 |
|---|---|---|---|
| **甲：保留 QRhi** | 换 include 到 Qt5 私有头；把 3 组 HDR 调用显式降级为 SDR；把 `.qsb` 重新烘焙成 QSB ≤ v5 | **中**（改动局限在 `rhi_image_window.cpp` 的 include 与 HDR 分支） | UOS 侧需 **`qtbase5-private-dev`**（拿 `qrhivulkan_p.h` 等）与 **Qt 5.15 Shader Tools**（出 QSB v5）。二者若有任一个在 UOS 上不可得，此路不通 |
| **乙：重写为 `QOpenGLWidget`** | 2977 行重写，GLSL 需从 QSB 目标改成手写着色器 | **大** | 不依赖私有 API 与 Shader Tools；但工作量大且行为等价性难验证 |

**建议先做一次 10 分钟的前置探测**（在 UOS 上确认 `qtbase5-private-dev` 与 `qsb` 是否可得），
再决定走甲还是乙。在此之前 viewer 保持 `SNOW_APPS_BUILD_IMAGE_VIEWER=OFF`，不阻塞主线。

**这次探测已经脚本化**，不用手工逐条查：

```bash
bash scripts/uos-preflight.sh --probe      # 在 UOS / Deepin 真机上跑，直接给结论
```

它输出 10 段，最后一段就是上面的甲/乙判定，输入全部来自实测而非假设：

| 段 | 采集内容 | 对应决策输入 |
|---|---|---|
| 1 | 发行版 / glibc / gcc / cmake ≥ 3.20 / ninja | 方案附录 A.3 的 glibc 基线 |
| 2 | `Qt5Core/Gui/Widgets/Svg/...` pkg-config 版本与 include 目录 | Qt 5.15 下限 |
| 3 | `qrhi_p.h`、`qshader_p.h` 是否在 `.../QtGui/5.15.x/QtGui/private/`；实际编了哪些后端（GLES2/Vulkan/...）；**Qt5 `qrhi_p.h` 里是否真的没有 HDR** | **甲** 的可行性 |
| 4 | `qsb` 是否存在；从**私有头**读 `QSB_VERSION` / `QSB_VERSION_MIN`（这才是 Qt5 真正接受的版本白名单，不是工具自报的版本）；仓库里已有 `.qsb` 的个数与样例 | **甲** 的第二个硬条件 |
| 5 | xcb/xkbcommon/wayland/pipewire/pulse/dbus/fontconfig、opencv4（含 `wechat_qrcode`、`objdetect/barcode` 两个本工程真用的模块）、libheif/libjxl/onnxruntime/minizip-ng | §9.1 依赖清单 |
| 6 | `XDG_SESSION_TYPE` / `WAYLAND_DISPLAY` / `DISPLAY` / `QT_QPA_PLATFORM` | 先 X11 后 Wayland 的路线（§5 未决问题 5） |
| 7 | rustc/cargo 版本、已装 target、libclang | §9.4 |
| 8 | CPU 型号与该架构的 SIMD 标志（x86: avx2/avx512f/fma；arm64: asimd/sve） | §7.2 SIMD 补强优先级 |
| 9 | `--probe`：现场编译一个同时 include 私有 `qrhi_p.h` + 调 `QShader::fromSerialized()` 读**本仓库真实 `.qsb`** 的小程序 | 把"头文件在不在"升级成"我们的 shader 资源 Qt5 到底能不能读" |

第 9 段是关键：QSB 版本号不在 `.qsb` 前缀里（前 4 字节是大端原始长度，后面直接是 zlib 流
`78 9c`），所以没法靠读文件头部判断，只能真跑一次反序列化。`QShader::fromSerialized()`
返回空对象时 Qt5 只打一句 `qWarning`，不抛异常 —— 若不在 UOS 上先测，症状会表现为
**viewer 编译全绿但上屏一片黑**，排查成本远高于这 10 分钟。

### 6.2 vcpkg 第三方依赖通道：镜像可行性实测

问题只问一句「vcpkg 能不能配国内镜像」，实测答案要分三截：

**(1) 网上流传的三条说法都不成立**

| 说法 | 实测 |
|---|---|
| 设 `VCPKG_DOWNLOAD_MIRROR` 换源 | 该变量**在 vcpkg 里不存在**（全仓 grep 0 命中） |
| 清华有 `mirrors.tuna.tsinghua.edu.cn/vcpkg/assets/` | **404**，没有这个镜像 |
| 清华 `github-release/` 能兜住 GitHub Release 附件 | 只镜像约 40 个白名单仓库；**没有** `Kitware/`、**没有** `ninja-build/`（均 404） |

**(2) vcpkg 官方只给四条杠杆**（`X_VCPKG_ASSET_SOURCES` 的后端就 `clear` / `x-azurl` / `x-block-origin` / `x-script` 四个）

| 杠杆 | 说明 | 本机适配度 |
|---|---|---|
| `x-script,<模板>` | 把下载交给任意外部命令，vcpkg 传 `{url}`/`{sha512}`/`{dst}` | **已用**。vcpkg 走 WinHTTP 拿 **502**，同 URL 上 `curl` 能拿到字节 → 换成 curl 立刻见效 |
| `x-azurl,<url>,\|file://<dir>` | Azure Blob 兼容端点 / 本地或 UNC 目录做资产缓存 | 适合团队共享盘 |
| 预置 `downloads/` | 按 vcpkg 打印的目标名放文件，哈希过则不再联网 | **已用**，把 CMake 4.2.3 从 `C:\snow-tools\cmake.zip` 预置成功 |
| `VCPKG_BINARY_SOURCES` | 二进制缓存（编译产物），与源码下载**两回事** | 建议开，跨机器零编译 |

**(3) 本机真正有用的两条改写**

- **源码包走 `codeload`**：`github.com/o/r/archive/<ref>.tar.gz` → `codeload.github.com/o/r/tar.gz/<ref>`
  （注意**去掉扩展名**）。实测 `EXIT=0`，一次拉全 1,572,744 字节。
- **Release 附件**（工具链 7zip/ninja、部分端口的包）仍是抖动区，需要重试 + 预置。

**注意：这**不是**「封禁」，是「抖动」**——同一 URL 曾测到 3 MB/s（50 MB / 17.1 s）
也测到 ~8 KB/s / 502。所以工具链一律按「重试 + 断点续传 + 整轮重跑」设计。

配套脚本与逐条证据：`.workbuddy/qt5-port/vcpkg-mirror/README.md`
（`fetch-asset.py` / `hash-rename.py` / `env-vcpkg-mirror.{sh,ps1}` / `install-deps.sh`）。

---

*本文档所有命中数、文件数均由当前 `main` 快照 grep 统计得出，可复现。*
