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

配置 → 编译 → 逐项手测：

| 项 | 验收要点 |
|---|---|
| 启动 | 无 `This application failed to start because no Qt platform plugin could be initialized` |
| 截图 | 全屏 / 区域 / 窗口三种模式各成一次 |
| 贴图 | 贴图窗口置顶、拖动、缩放、关闭 |
| 快捷键 | **重点**——`QKeyCombination` 改造的正确性直接体现在这里（含 Ctrl+Shift+Tab 区域类型循环） |
| 工具栏悬停 | **重点**——`enterEvent` 签名改造的正确性（悬停高亮、弹出菜单） |
| 设置页 | 各页可切换、无 `removeIf` 相关崩溃 |
| OCR | 中文识别出文字 |
| 录屏 | 能起录、能停、产物可播 |

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

#### (3) 仍未闭合的 Qt5 缺口（本轮只记录，不静默降级）

**stock 对话框目录的合并**：Qt6 的 `MERGE_QT_TRANSLATIONS` 会把 `qtbase_<locale>.qm`
并进 `snow_shot_<locale>.qm`；Qt5 垫片没有对应实现，只打一条 STATUS 说"由打包步骤处理"
（而**目前没有任何打包步骤真的处理它**）。而 `LanguageManager::discoverCatalogs()`
只从 `:/i18n` 装载 `snow_shot_*.qm` 并安装为 `QTranslator`，**没有**第二个翻译器去读
Qt 自带的 stock 目录。因此 Qt5 路线下 `QFileDialog`/`QColorDialog` 等标准对话框的
中文会退回英文——**编译与链接不受影响，属运行时外观回归**。

两条可选的收口方式（择一，尚未实现）：

| 方案 | 做法 | 代价 |
|---|---|---|
| 甲：运行时装载 | Qt5 分支在 `LanguageManager` 里额外安装一个 `QTranslator`，从 `QLibraryInfo::location(TranslationsPath)`（回退应用目录）读 `qt_zh_CN.qm` / `qtbase_zh_TW.qm`；打包时把对应 `.qm` 一并带上 | 改动小，但要动安装包清单 |
| 乙：构建期合并 | 在 Qt5 垫片里实现等价的目录合并（需 `lconvert` 往返 `.qm`），使产物与 Qt6 逐字一致 | 成本高，且 `.qm` 往返会丢元信息 |

顺带记录两处 Qt6 命名的既有假设，进 UOS 前需要一并按版本分流：
`scripts/snow-build-environment.ps1:252` 与 `scripts/validate-static-qt.py:192`
都写死了 `translations/qtbase_$language.qm`，在 Qt5 套件（无 `qtbase_zh_CN.qm`）上会判失败。

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
| **P5** | **Windows Qt5 编译打通 + 3.4 冒烟清单全绿** | 冒烟清单逐项通过，无红色误报弹窗 |
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
