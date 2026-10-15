#!/usr/bin/env python3
"""Qt6 -> Qt5 双版本改造：机械改写规则执行器

设计原则
--------
1. 只做「确定性」改写：能用语法规则判定的才自动改，判断不了的一律输出到人工清单。
2. 默认 --dry-run：只出报告不动文件；--apply 才写盘。
3. 每条规则可单独启用（--rule R4），便于一条一个 commit。

规则
----
R1  CMake: Qt6::<模块> -> Qt::<模块>（官方 versionless，Qt5.15 / Qt6 双向生效）
R1b CMake: Qt6::<插件|Private|qtpaths|其它> -> ${SNOW_QT_NS}::<名>
R3  C++  : #include <Qt6 专有头> -> #include "snow_shot/compat/qt_compat.h"
R4  C++  : QEnterEvent -> SnowEnterEvent（签名统一，两种版本都能 override 基类）
R5  C++  : <recv>.removeIf(pred) -> snow::compat::removeIf(<recv>, pred)
R7  C++  : QMetaType::fromType<T>() -> snowMetaType<T>()
R8  C++  : <recv>.metaType() -> snowMetaTypeOf(<recv>)
R24-R27 C++: Qt::ColorScheme / QTimeZone::fromSecondsAheadOfUtc / QPromise / QChronoTimer
R29 C++  : addData(QByteArrayView(a, b)) -> addData(a, static_cast<int>(b))
R32 C++  : <recv>.sliced(pos[, n]) -> <recv>.mid(pos[, n])（Qt 6.0 成员，Qt5 无）
R33 C++  : QLibraryInfo::path(...) -> snowLibraryInfoPath(...)（Qt 6.0 把 location() 改名）

人工清单（不自动改，见 manual-*.txt）
R9  setShortcut(QKeyCombination) -> 需要 snowKeySequence() 包裹
R4b enterEvent 体内的 event->position() -> snowEnterPosition(event)
R30 QNativeInterface::Q*Screen（Windows 已收敛为 nativeMonitorHandle()，macOS 延后）
R31 QPointingDevice（已收敛为 snowNativeGestureEvent() 工厂）
R34 QFont::Tag（已收敛为 SNOW_FONT_SET_VARIABLE_AXIS / SNOW_FONT_SET_FEATURE 宏）
RM  Qt6 专有目标在 Qt5 无对应（如 QSchannelBackendPlugin）需版本分支
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# 需要扫描的模块根（不含第三方与构建产物）
SOURCE_ROOTS = [
    "snow_shot/src",
    "snow_shot/include",
    "snow_shot/tests",
    "ant_design_qt/packages",
    "ant_design_qt/tests",
    "snow_draw_engine_qt/src",
    "snow_draw_engine_qt/include",
    "snow_draw_engine_qt/tests",
    "snow_image/src",
    "snow_image/include",
    "snow_image_viewer/src",
    "snow_image_viewer/tests",
]

CMAKE_ROOTS = [
    "CMakeLists.txt",
    "cmake",
    # 模块根目录（递归覆盖其下所有 CMakeLists.txt / *.cmake）。
    # 只列顶层 CMakeLists 会漏掉嵌套的测试与 demo，例如
    # snow_image_viewer/tests/CMakeLists.txt、snow_image/tests/CMakeLists.txt。
    "snow_shot",
    "ant_design_qt",
    "snow_draw_engine_qt",
    "snow_image",
    "snow_image_viewer",
    "test-support",
]

CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".mm"}

# 版本分派机制自身的文件：它们必须保留显式的 Qt6:: / Qt5:: 分支
# （cmake/SnowQt.cmake 里的 Qt6 分支就是靠 Qt6::qtpaths 等真实目标工作的），
# 机械改写会把这份分派逻辑"扁平化"，因此整体排除。
PROTECTED_CMAKE_FILES = {
    "SnowQt.cmake",
    "SnowQtVersionShims.cmake",
}

# Qt 官方 versionless 覆盖的模块目标（Qt5.15 与 Qt6 均提供 Qt::<模块>）
VERSIONLESS_MODULES = {
    "Core", "Gui", "Widgets", "Network", "Svg", "SvgWidgets", "Concurrent", "Test",
    "DBus", "PrintSupport", "OpenGL", "OpenGLWidgets", "Sql", "Xml", "Qml", "Quick",
    "QuickControls2", "LinguistTools", "Core5Compat", "Multimedia", "MultimediaWidgets",
    "WebEngineWidgets", "WebSockets", "SerialPort", "StateMachine", "UiTools",
}

# Qt 5.15 中不存在的 Qt6 专有头（含则必须替换为兼容头）
QT6_ONLY_HEADERS = [
    "QKeyCombination",
    "QStringDecoder",
    "QStringEncoder",
    "QLatin1StringView",
    "QEnterEvent",
    "QPromise",
    "QChronoTimer",  # Qt 6.8；Qt5 用兼容头的 SnowChronoTimer（QTimer 子类）
    "QRhi",
]

QT6_HEADER_INCLUDE_RE = re.compile(
    r"^[ \t]*#\s*include\s*<(" + "|".join(QT6_ONLY_HEADERS) + r")>\s*$",
    re.MULTILINE,
)

COMPAT_INCLUDE = '#include "snow_shot/compat/qt_compat.h"'


@dataclass
class Finding:
    rule: str
    path: str
    line: int
    before: str
    after: str
    note: str = ""


@dataclass
class Report:
    findings: list[Finding] = field(default_factory=list)
    manual: list[Finding] = field(default_factory=list)
    suppressed: list[Finding] = field(default_factory=list)
    unknown_targets: dict[str, int] = field(default_factory=lambda: defaultdict(int))

    def add(self, f: Finding) -> None:
        self.findings.append(f)

    def add_manual(self, f: Finding) -> None:
        self.manual.append(f)

    def add_suppressed(self, f: Finding) -> None:
        self.suppressed.append(f)


def iter_cpp_files() -> list[Path]:
    out: list[Path] = []
    for root in SOURCE_ROOTS:
        base = REPO / root
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if p.suffix in CPP_SUFFIXES and p.is_file():
                out.append(p)
    return sorted(out)


def iter_cmake_files() -> list[Path]:
    out: list[Path] = []
    for root in CMAKE_ROOTS:
        base = REPO / root
        if base.is_file():
            out.append(base)
        elif base.is_dir():
            for p in base.rglob("*"):
                if not p.is_file():
                    continue
                if p.name == "CMakeLists.txt" or p.suffix == ".cmake":
                    # 第三方 overlay 端口不属于本工程代码，单独处理
                    if "vcpkg-overlay-ports" in p.as_posix():
                        continue
                    # 版本分派文件本身不参与改写（见 PROTECTED_CMAKE_FILES 说明）
                    if p.name in PROTECTED_CMAKE_FILES:
                        continue
                    out.append(p)
    return sorted(set(out))


# ---------------------------------------------------------------------------
# R1 / R1b —— CMake 目标重命名
# ---------------------------------------------------------------------------
QT6_TARGET_RE = re.compile(r"Qt6::([A-Za-z0-9_]+)")


def rule_cmake_targets(path: Path, text: str, rep: Report) -> str:
    def repl(m: re.Match[str]) -> str:
        name = m.group(1)
        if name in VERSIONLESS_MODULES:
            rep.add(Finding("R1", rel(path), 0, f"Qt6::{name}", f"Qt::{name}",
                            "官方 versionless 目标，Qt5.15/Qt6 双向可用"))
            return f"Qt::{name}"
        # 非模块目标：插件 / Private / 工具，QA5.15 不保证提供 Qt:: 前缀版本
        rep.unknown_targets[name] += 1
        rep.add(Finding("R1b", rel(path), 0, f"Qt6::{name}", f"${{SNOW_QT_NS}}::{name}",
                        "非模块目标，需按 Qt 命名空间变量解析"))
        return f"${{SNOW_QT_NS}}::{name}"

    return QT6_TARGET_RE.sub(repl, text)


# ---------------------------------------------------------------------------
# R3 —— Qt6 专有头替换为兼容头
# ---------------------------------------------------------------------------
def rule_compat_includes(path: Path, text: str, rep: Report) -> str:
    def repl(m: re.Match[str]) -> str:
        header = m.group(1)
        rep.add(Finding("R3", rel(path), 0, f"#include <{header}>", COMPAT_INCLUDE,
                        "Qt5 无此头文件，改由兼容层提供"))
        return COMPAT_INCLUDE

    return QT6_HEADER_INCLUDE_RE.sub(repl, text)


# ---------------------------------------------------------------------------
# R4 —— QEnterEvent 签名统一
#
#   Qt6：QWidget::enterEvent(QEnterEvent*)   —— 虚函数签名里就有 QEnterEvent
#   Qt5：QWidget::enterEvent(QEvent*)        —— QEnterEvent 类存在(5.14+)但不在签名里
#   把形参写成 SnowEnterEvent* 后，一份 override 声明在两个版本都成立。
#
#   注意：必须同时删掉 `class QEnterEvent;` 这类**前向声明**。兼容头是用
#   `using SnowEnterEvent = QEvent;` 提供别名的，若改名后留下
#   `class SnowEnterEvent;`，MSVC 会报
#       error C2371: "SnowEnterEvent": 重定义；不同的基类型
#   并连锁引发 enterEvent 无法 override / C2664 / C2440 等几十条错误。
# ---------------------------------------------------------------------------
ENTER_FORWARD_DECL_RE = re.compile(
    r"^[ \t]*(?:class|struct)[ \t]+(?:QEnterEvent|SnowEnterEvent)[ \t]*;[ \t]*\r?\n",
    re.MULTILINE,
)


def rule_enter_event(path: Path, text: str, rep: Report) -> str:
    if "QEnterEvent" not in text and "SnowEnterEvent" not in text:
        return text
    removed = len(ENTER_FORWARD_DECL_RE.findall(text))
    if removed:
        rep.add(Finding("R4", rel(path), 0, f"前向声明 class QEnterEvent; ×{removed}", "删除",
                        "兼容头用 using 别名提供 SnowEnterEvent，前向声明会与之冲突(C2371)"))
        text = ENTER_FORWARD_DECL_RE.sub("", text)
    if "QEnterEvent" not in text:
        return text
    count = text.count("QEnterEvent")
    rep.add(Finding("R4", rel(path), 0, f"QEnterEvent ×{count}", f"SnowEnterEvent ×{count}",
                    "enterEvent 虚函数签名需与基类一致"))
    return text.replace("QEnterEvent", "SnowEnterEvent")


# ---------------------------------------------------------------------------
# 表达式接收者提取：从 `recv.method(` 的 method 位置向左回溯出 recv
# ---------------------------------------------------------------------------
IDENT_CHARS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")


def extract_receiver(line: str, dot_pos: int) -> tuple[str, int] | None:
    """line[dot_pos] 是 '.'；返回 (接收者文本, 起始下标)；无法确定时返回 None。"""
    i = dot_pos - 1
    while i >= 0 and line[i] == " ":
        i -= 1
    if i < 0:
        return None

    # 先吃掉一个简单成员链/标识符：a、a.b、a->b、this->a、m_x.y
    end = i + 1
    # 允许括号包裹的整体（例如 (ptr).removeIf），以及解引用 (*it).removeIf
    if line[i] == ")":
        depth = 0
        j = i
        while j >= 0:
            if line[j] == ")":
                depth += 1
            elif line[j] == "(":
                depth -= 1
                if depth == 0:
                    # 向左继续吃掉 (* or (Type*)
                    k = j - 1
                    while k >= 0 and line[k] in "*& ":
                        k -= 1
                    if k >= 0 and (line[k] in IDENT_CHARS or line[k] in "]_"):
                        j = k
                    else:
                        j = j
                    return line[j:end], j
            j -= 1
        return None

    if line[i] not in IDENT_CHARS and line[i] != "]":
        return None

    start = i
    while start - 1 >= 0:
        prev = line[start - 1]
        if prev in IDENT_CHARS or prev in "_" or prev == "]":
            start -= 1
            continue
        # 成员访问链
        if prev == "." :
            # 需要左边是标识符
            k = start - 2
            if k >= 0 and (line[k] in IDENT_CHARS or line[k] == "]"):
                start = k
                continue
            break
        if prev == ">" and start - 2 >= 0 and line[start - 2] == "-":
            k = start - 3
            if k >= 0 and (line[k] in IDENT_CHARS or line[k] == "]"):
                start = k
                continue
            break
        break
    return line[start:end], start


# ---------------------------------------------------------------------------
# R5 —— removeIf
# ---------------------------------------------------------------------------
REMOVEIF_RE = re.compile(r"(\.|->)removeIf\s*\(")


def rule_remove_if(path: Path, text: str, rep: Report) -> str:
    if not REMOVEIF_RE.search(text):
        return text
    out_lines: list[str] = []
    for lineno, line in enumerate(text.split("\n"), start=1):
        if not REMOVEIF_RE.search(line):
            out_lines.append(line)
            continue
        replaced = line
        # 从右往左替换，避免下标漂移
        for m in reversed(list(REMOVEIF_RE.finditer(line))):
            operator = m.group(1)
            recv = extract_receiver(line, m.start())
            if recv is None:
                rep.add_manual(Finding("R5", rel(path), lineno, line.strip(), "",
                                       "接收者表达式无法安全提取，需人工改写"))
                continue
            recv_text, recv_start = recv
            # `it->removeIf(...)` / `ptr->removeIf(...)`：`->` 意味着接收者是迭代器
            # 或指针（Qt6 的 QHash::iterator::operator-> 返回 T*），removeIf 作用在
            # 被指对象上。兼容头里的 removeIf 取的是容器引用，故必须显式解引用，
            # 否则会退化成 removeIf(iterator&, pred) 而编译失败。
            argument = f"*({recv_text})" if operator == "->" else recv_text
            new = f"snow::compat::removeIf({argument}, "
            # 记录前后片段
            rep.add(Finding("R5", rel(path), lineno,
                            line[m.start():m.end()].join(["", "…"]),
                            f" {argument}, ", ""))
            # 截断点必须是**接收者的起点** recv_start，不是运算符位置 m.start()。
            # 用 m.start() 会把接收者原样留下，产出
            #     pathssnow::compat::removeIf(paths, ...)
            # 这种既编译不过、又肉眼很难一眼看出的损坏（实测在 22 个文件里攒了 49 处）。
            replaced = replaced[:recv_start] + new + replaced[m.end():]
        out_lines.append(replaced)
    return "\n".join(out_lines)


# ---------------------------------------------------------------------------
# R7 —— QMetaType::fromType
# ---------------------------------------------------------------------------
def rule_meta_type_from(path: Path, text: str, rep: Report) -> str:
    if "QMetaType::fromType" not in text:
        return text
    n = text.count("QMetaType::fromType")
    rep.add(Finding("R7", rel(path), 0, f"QMetaType::fromType ×{n}", f"snowMetaType ×{n}", ""))
    return text.replace("QMetaType::fromType", "snowMetaType")


# ---------------------------------------------------------------------------
# R8 —— QVariant::metaType()
# ---------------------------------------------------------------------------
META_OF_RE = re.compile(r"(\w[\w\.\->\[\]]*)\.metaType\s*\(\s*\)")


def rule_variant_meta_type(path: Path, text: str, rep: Report) -> str:
    if ".metaType()" not in text:
        return text
    # 兼容头自身的实现不能改
    if path.name == "qt_compat.h":
        return text
    out_lines: list[str] = []
    for lineno, line in enumerate(text.split("\n"), start=1):
        if ".metaType()" not in line:
            out_lines.append(line)
            continue
        def repl(m: re.Match[str]) -> str:
            rep.add(Finding("R8", rel(path), lineno, m.group(0),
                            f"snowMetaTypeOf({m.group(1)})", ""))
            return f"snowMetaTypeOf({m.group(1)})"
        out_lines.append(META_OF_RE.sub(repl, line))
    return "\n".join(out_lines)


# ---------------------------------------------------------------------------
# R10 / R11 —— 事件坐标访问器改名
#
#   Qt6 把输入事件的坐标访问器统一改名为 position()/globalPosition()：
#       Qt5.x                         Qt6
#       QMouseEvent::localPos()       -> position()
#       QMouseEvent::globalPos()      -> globalPosition()
#       QHoverEvent::posF()           -> position()
#       QEnterEvent::localPos()       -> position()
#       QEnterEvent::globalPos()      -> globalPosition()
#   QWheelEvent 是例外：Qt 5.15 已把 position()/globalPosition() 回移，无需改写。
#   （全部结论来自读本机 Qt 5.15.2 QtGui/qevent.h，非推测。）
#
#   安全策略：只改「接收者在同一文件里被声明为事件指针」的调用
#   （形如 `QMouseEvent* event` 出现在任意上下文：形参、局部变量、static_cast 目标）。
#   判断不了一律进人工清单 —— position() 在 Qt5 里也存在于 QWindow/QMediaPlayer 等
#   完全无关的类型上，盲改会引入静默错误。
# ---------------------------------------------------------------------------
# 事件类型名。SnowEnterEvent 也要算进来：它是兼容头提供的别名
# （Qt5 下等价 QEvent、Qt6 下等价 QEnterEvent），enterEvent 的形参就是它。
#
# QWheelEvent 必须登记在内：Qt 5.15 **已经**把 position()/globalPosition() 回移，
# 登记进来是为了让规则把它判为「无需改写」，而不是掉进人工清单
# （实测漏登记会在 globalcanvascontroller.cpp / screenshottoolpalette.cpp /
#  screenshotpinnedinteraction.cpp 产生 16 条假阳性）。
#
# QNativeGestureEvent 必须登记在内：Qt6 里它继承 QSinglePointEvent 因而有 position()，
# 而 Qt5 只有 localPos()/windowPos()/screenPos()/globalPos()（QtGui/qevent.h:343-362），
# 是**真实需要改写**的类型。漏登记会让这处真阳性沉进人工清单而被忽略。
EVENT_TYPE_ALT = (
    r"(?:SnowEnterEvent|Q(?:Mouse|Hover|Enter|ContextMenu|Drop|Tablet|Wheel|NativeGesture)Event)"
)
EVENT_POS_DECL_RE = re.compile(r"\b(" + EVENT_TYPE_ALT + r")\s*\*+\s*(\w+)")
# `auto* mouseEvent = static_cast<QMouseEvent*>(event);` —— 类型只出现在 cast 里，
# 上面那条声明正则抓不到（`*` 后面紧跟的是 `>` 而不是标识符）。
EVENT_POS_CAST_DECL_RE = re.compile(
    r"\b(\w+)\s*=\s*(?:static_cast|dynamic_cast|reinterpret_cast|const_cast)\s*<\s*"
    r"(?:const\s+)?(" + EVENT_TYPE_ALT + r")\s*\*?\s*>"
)
# `static_cast<const QMouseEvent*>(event)->position()` —— 接收者本身就是 cast 表达式，
# 但类型在源码里是显式写出来的，因此可以直接判定，不必进人工清单。
# 改写时必须把**整个 cast 表达式**原样带上：早期版本只保留括号里的实参，虽然仍能编译，
# 但会退化成 QEvent* 重载 → 白白多跑一次运行时类型分发，且丢掉编译期类型检查。
EVENT_POS_CAST_EXPR = (
    r"(?:static_cast|dynamic_cast|reinterpret_cast|const_cast)\s*<\s*(?:const\s+)?"
    + EVENT_TYPE_ALT
    + r"\s*\*?\s*>\s*\([^()]*\)"
)
EVENT_POS_CAST_CALL_RE = re.compile(
    r"(" + EVENT_POS_CAST_EXPR + r")\s*->\s*(position|globalPosition)\s*\(\s*\)"
)
EVENT_POS_CAST_TYPE_RE = re.compile(EVENT_TYPE_ALT)
POSITION_CALL_RE = re.compile(r"\b(\w+)\s*->\s*(position|globalPosition)\s*\(\s*\)")


def _event_receiver_types(text: str) -> dict[str, str]:
    """收集「标识符 -> 事件类型」映射（取文件中任意位置的声明）。"""
    mapping: dict[str, str] = {}
    for m in EVENT_POS_DECL_RE.finditer(text):
        mapping[m.group(2)] = m.group(1)
    for m in EVENT_POS_CAST_DECL_RE.finditer(text):
        mapping.setdefault(m.group(1), m.group(2))
    return mapping


def _rewrite_position_call(etype: str, method: str, arg: str) -> str | None:
    """返回改写后的调用；None 表示该 (类型, 方法) 组合在 Qt5 无等价实现。"""
    if etype == "QWheelEvent":
        return None  # Qt 5.15 已同名，保持原样
    if method == "globalPosition" and etype == "QHoverEvent":
        return None  # Qt5 的 QHoverEvent 根本没有全局坐标
    target = "snowPos" if method == "position" else "snowGlobalPos"
    return f"{target}({arg})"


def rule_event_positions(path: Path, text: str, rep: Report) -> str:
    if "->position()" not in text and "->globalPosition()" not in text:
        return text
    recv_types = _event_receiver_types(text)

    out_lines: list[str] = []
    for lineno, line in enumerate(text.split("\n"), start=1):
        if "->position()" not in line and "->globalPosition()" not in line:
            out_lines.append(line)
            continue

        def cast_repl(m: re.Match[str]) -> str:
            cast_expr, method = m.group(1), m.group(2)
            tm = EVENT_POS_CAST_TYPE_RE.search(cast_expr)
            etype = tm.group(0) if tm else ""
            new = _rewrite_position_call(etype, method, cast_expr)
            if new is None:
                if etype == "QHoverEvent":
                    rep.add_manual(Finding("R11", rel(path), lineno, m.group(0), "",
                                           "Qt5 的 QHoverEvent 没有全局坐标，"
                                           "需在调用点用 widget->mapToGlobal() 手工重写"))
                return m.group(0)
            rule = "R10" if method == "position" else "R11"
            rep.add(Finding(rule, rel(path), lineno, m.group(0), new, "cast 接收者"))
            return new

        # 先处理 cast 表达式接收者（类型显式，可直接判定）
        replaced = EVENT_POS_CAST_CALL_RE.sub(cast_repl, line)

        def repl(m: re.Match[str]) -> str:
            recv, method = m.group(1), m.group(2)
            etype = recv_types.get(recv)
            if etype is None:
                rep.add_manual(Finding("R10", rel(path), lineno, m.group(0),
                                       "snowPos(...)",
                                       f"接收者 `{recv}` 未在本文件被声明为事件指针，需人工确认类型"))
                return m.group(0)
            new = _rewrite_position_call(etype, method, recv)
            if new is None:
                if etype == "QHoverEvent":
                    rep.add_manual(Finding("R11", rel(path), lineno, m.group(0), "",
                                           "Qt5 的 QHoverEvent 没有全局坐标，"
                                           "需在调用点用 widget->mapToGlobal() 手工重写"))
                return m.group(0)
            rule = "R10" if method == "position" else "R11"
            rep.add(Finding(rule, rel(path), lineno, m.group(0), new, ""))
            return new

        replaced = POSITION_CALL_RE.sub(repl, replaced)
        # 残留检测：上面两条路径覆盖了「cast 表达式接收者」与「简单标识符接收者」的
        # 全部命中，未改写的也都已记入人工清单。因此这里把这两类统统从探针里抹掉，
        # 剩下的 `->position()` 就只可能是「接收者是调表达式」这一类，例如
        # `block.layout()->position()`（QTextLayout，Qt5/Qt6 同名，无需改写）。
        # 早期版本用一条专门的 WHEEL_CAST_CALL_RE 只屏蔽 QWheelEvent 的 cast 形式，
        # 一旦 QWheelEvent 被登记进 EVENT_TYPE_ALT，那条正则就失效、条目会重新冒出来。
        probe = EVENT_POS_CAST_CALL_RE.sub("", replaced)
        probe = POSITION_CALL_RE.sub("", probe)
        if "->position()" in probe or "->globalPosition()" in probe:
            for m in re.finditer(r"->\s*(?:position|globalPosition)\s*\(\s*\)", probe):
                rep.add_manual(Finding("R10", rel(path), lineno, m.group(0), "",
                                       "接收者表达式不是简单标识符，需人工改写"))
        out_lines.append(replaced)
    return "\n".join(out_lines)


# ---------------------------------------------------------------------------
# R12 —— QWidget::nativeEvent 的第三个参数
#   Qt6: qintptr *result     Qt5: long *result
#   qintptr 在 Qt5 也存在，所以只把 nativeEvent 签名里的那一个改掉。
# ---------------------------------------------------------------------------
NATIVE_EVENT_QINTPTR_RE = re.compile(r"\bqintptr\b")


def rule_native_event_result(path: Path, text: str, rep: Report) -> str:
    if "nativeEvent" not in text or "qintptr" not in text:
        return text
    if path.name == "qt_compat.h":
        return text
    out_lines: list[str] = []
    for lineno, line in enumerate(text.split("\n"), start=1):
        if "nativeEvent" in line and "qintptr" in line:
            rep.add(Finding("R12", rel(path), lineno, "qintptr", "snowNativeEventResult",
                            "Qt5 的 nativeEvent 第三参是 long*（两版本 ABI 不同）"))
            line = NATIVE_EVENT_QINTPTR_RE.sub("snowNativeEventResult", line)
        out_lines.append(line)
    return "\n".join(out_lines)


# ---------------------------------------------------------------------------
# R13-R23 —— 其余 Qt6 专有成员的机械改写
#
#   每条规则的判据都写明了出处（读本机 Qt5 头文件 / 实测编译错误码），不是推测。
#   模板里的 \1 是「接收者表达式」。
# ---------------------------------------------------------------------------
# 成员表达式接收者：a、a.b、a->b、tile.pixmap、a->b.c->d。
# 负向后行断言避免从 `m_x[0].foo()` 里只截出 `0`。
_MEMBER_RECV = r"(?<![\w.\]\)])\w+(?:\s*(?:->|\.)\s*\w+)*"

SIMPLE_MEMBER_RULES: list[tuple[str, re.Pattern[str], str, str]] = [
    ("R13",
     re.compile(r"std::clamp\(\s*(" + _MEMBER_RECV + r")\.alphaF\(\s*\)\s*,\s*0\.0F\s*,\s*1\.0F\s*\)"),
     r"std::clamp(snowAlphaF(\1), 0.0F, 1.0F)",
     "Qt6 QColor::alphaF() 返回 float，Qt5 返回 qreal(double)；std::clamp 要求三参同型"),
    ("R15",
     re.compile(r"(" + _MEMBER_RECV + r")\.deviceIndependentSize\(\s*\)"),
     r"snowDeviceIndependentSize(\1)",
     "QPixmap::deviceIndependentSize() 是 Qt 6.2+ 专有"),
    ("R16",
     re.compile(r"(" + _MEMBER_RECV + r")\.typeId\(\s*\)"),
     r"\1.userType()",
     "QVariant::typeId() 是 Qt6 专有，Qt5 只有 userType()"),
    ("R23",
     re.compile(r"(" + _MEMBER_RECV + r")\.resolveMask\(\s*\)"),
     r"snowFontResolveMask(\1)",
     "Qt5 的等价 API 是无参 QFont::resolve()（public，语义完全相同）"),
]

# 纯文本替换规则（无需提取接收者）
SIMPLE_TEXT_RULES: list[tuple[str, str, str, str]] = [
    ("R14", "qHashMulti(", "snowHashMulti(", "Qt6 专有组合器，Qt5 无等价实现"),
    ("R17", "QEvent::DevicePixelRatioChange", "snowDevicePixelRatioChange",
     "Qt5 的 QEvent 没有 DevicePixelRatioChange"),
    ("R18", "QPalette::Accent", "snowAccentRole",
     "Qt 6.6 新增的角色，Qt5 无等价（降级到 Highlight，语义有损）"),
    ("R22", "QAccessible::AnnouncementPoliteness", "SnowAnnouncementPoliteness",
     "Qt 6.8 新增，Qt5 无等价"),
    ("R22b", "QAccessibleAnnouncementEvent", "SnowAccessibleAnnouncementEvent",
     "Qt 6.8 新增，Qt5 用 QAccessibleEvent(Alert) 近似"),
]

# `setScreen(x)` / `<w>->setScreen(x)`
#
# 风险点：QWindow::setScreen() 在 Qt5 **本来就存在**（QtGui/qwindow.h:258），所以
#   `windowHandle()->setScreen(s)`、`handle->setScreen(s)`、`m_window->setScreen(s)`
# 这类调用**不能改**（改成 QWidget* 形参后编译不过或语义错）。
# 而 Qt5 的 QWidget **完全没有** setScreen（QtWidgets/qwidget.h 里 grep 为 0），
# 所以 `popup->setScreen(s)`、裸调用 `setScreen(s)` 都一定是 Qt6 的
# QWidget::setScreen()，必须改写。
#
# 为什么不能靠 lookbehind 区分：
#   早期版本写成 `(?:((RECV))\s*->\s*)?(?<!->)(?<!\.)setScreen...`，本意是挡住
#   `windowHandle()->setScreen`（接收者带 `()`，_MEMBER_RECV 匹配不到，可选组不
#   参与匹配，于是退化成「裸调用」被误改）。但 lookbehind 的位置正好落在 `->`
#   之后，于是**带接收者的合法调用也被一并挡住** —— 规则整体失效（本轮实测：
#   R19 只命中 4 处裸调用，`popup->setScreen(...)` 全部漏改）。
# 现改为：正则只负责切出「可选接收者 + 实参」，是否受保护由 Python 端用接收者
# 名字 / 前一个字符显式判定。
#
# 实参模式必须允许一层嵌套括号：`setScreen(anchor->screen())`、
# `setAccessibleIdentifier(button->objectName())` 这类实参含 `()`，
# 原来用 `[^()]*` 会直接匹配失败（这是 R20 漏改的第二个原因）。
#
# 点式调用 `first.setScreen(x)`（测试里大量存在的、栈上构造的 QWidget 对象）
# 也要覆盖：接收者以 `&recv` 取址传入。工程内不存在自定义 setScreen（Qt5 的
# QWidget 根本没有该成员），所以 `->` / `.` 两种形式都只可能是 Qt6 的
# QWidget::setScreen()。
_CALL_ARG = r"(?:[^()]|\([^()]*\))*"
_MEMBER_CALL_RECV_GROUP = (
    r"(?:(?P<recv>" + _MEMBER_RECV + r")\s*(?P<op>->|\.)\s*|(?P<bare>))"
)

SET_SCREEN_RE = re.compile(
    _MEMBER_CALL_RECV_GROUP + r"\bsetScreen\s*\((?P<arg>" + _CALL_ARG + r")\)"
)
WINDOW_LIKE_RECEIVER_RE = re.compile(r"(?i)handle|window")
SET_ACCESSIBLE_ID_RE = re.compile(
    _MEMBER_CALL_RECV_GROUP + r"\bsetAccessibleIdentifier\s*\((?P<arg>" + _CALL_ARG + r")\)"
)
# `<a>->associatedObjects()`
ASSOCIATED_OBJECTS_RE = re.compile(r"(" + _MEMBER_RECV + r")\s*->\s*associatedObjects\(\s*\)")


def _call_receiver(m: re.Match[str], line: str) -> str | None:
    """解析这次成员调用应传给兼容函数的「接收者表达式」。

    * `<recv>->setScreen(x)`  -> ``"recv"``（已经是指针）
    * `<recv>.setScreen(x)`   -> ``"&recv"``（对象，需取址）
    * `setScreen(x)`          -> ``"this"``（隐式 this 的成员函数体内）

    返回 ``None`` 表示这次匹配到的其实是被保护的 `->` 尾部，必须原样保留。
    场景：`windowHandle()->setScreen(s)` —— _MEMBER_RECV 里的 ``\\w+`` 吃不掉
    `()`，可选接收者组失配，正则退化成从 `setScreen` 开始的裸匹配；此时前一个
    字符是 `>`，据此判为受保护。
    """
    recv = m.group("recv")
    if recv is not None:
        return recv if m.group("op") == "->" else f"&{recv}"
    start = m.start()
    if start > 0:
        prev = line[start - 1]
        if prev.isalnum() or prev in "._)>":
            return None
    return "this"


def _rule_set_screen(path: Path, text: str, rep: Report) -> str:
    out_lines: list[str] = []
    for lineno, line in enumerate(text.split("\n"), start=1):
        if "setScreen(" not in line:
            out_lines.append(line)
            continue

        def repl(m: re.Match[str]) -> str:
            receiver = _call_receiver(m, line)
            if receiver is None:
                # QWindow::setScreen() 在 Qt5 合法，不能碰
                return m.group(0)
            if not receiver.startswith("&") and receiver != "this" \
                    and WINDOW_LIKE_RECEIVER_RE.search(receiver):
                # `handle->setScreen()` / `m_window->setScreen()`：Qt5 合法的
                # QWindow* 调用。点式形式（receiver 以 `&` 开头）不适用这条：
                # 能对值对象点调用的只可能是 QWidget（QWidget::setScreen 在
                # Qt6 才有），若真是 QWindow 栈对象则会编译报错，不会静默错。
                return m.group(0)
            new = f"snowSetWidgetScreen({receiver}, {m.group('arg').strip()})"
            rep.add(Finding("R19", rel(path), lineno, m.group(0), new,
                            "Qt6 的 QWidget::setScreen()，Qt5 需走 windowHandle()->setScreen()"))
            return new

        out_lines.append(SET_SCREEN_RE.sub(repl, line))
    return "\n".join(out_lines)


def _rule_set_accessible_identifier(path: Path, text: str, rep: Report) -> str:
    out_lines: list[str] = []
    for lineno, line in enumerate(text.split("\n"), start=1):
        if "setAccessibleIdentifier(" not in line:
            out_lines.append(line)
            continue

        def repl(m: re.Match[str]) -> str:
            receiver = _call_receiver(m, line)
            if receiver is None:
                return m.group(0)
            new = f"snowSetAccessibleIdentifier({receiver}, {m.group('arg').strip()})"
            rep.add(Finding("R20", rel(path), lineno, m.group(0), new,
                            "Qt6 的 QWidget::setAccessibleIdentifier()，Qt5 无等价 API"))
            return new

        out_lines.append(SET_ACCESSIBLE_ID_RE.sub(repl, line))
    return "\n".join(out_lines)


def _rule_associated_objects(path: Path, text: str, rep: Report) -> str:
    if "associatedObjects(" not in text:
        return text
    out_lines: list[str] = []
    for lineno, line in enumerate(text.split("\n"), start=1):
        if "associatedObjects(" not in line:
            out_lines.append(line)
            continue

        def repl(m: re.Match[str]) -> str:
            new = f"snowAssociatedObjects({m.group(1)})"
            rep.add(Finding("R21", rel(path), lineno, m.group(0), new,
                            "Qt6 把 associatedWidgets() 改名为 associatedObjects()"))
            return new

        out_lines.append(ASSOCIATED_OBJECTS_RE.sub(repl, line))
    return "\n".join(out_lines)


def rule_qt6_members(path: Path, text: str, rep: Report) -> str:
    """R13-R23：Qt6 专有成员的统一机械改写。"""
    for rule, pattern, template, note in SIMPLE_MEMBER_RULES:
        if not pattern.search(text):
            continue
        out_lines: list[str] = []
        for lineno, line in enumerate(text.split("\n"), start=1):
            def repl(m: re.Match[str], _rule: str = rule, _tpl: str = template,
                     _note: str = note) -> str:
                new = m.expand(_tpl)
                rep.add(Finding(_rule, rel(path), lineno, m.group(0), new, _note))
                return new
            out_lines.append(pattern.sub(repl, line))
        text = "\n".join(out_lines)

    for rule, old, new, note in SIMPLE_TEXT_RULES:
        if old not in text:
            continue
        rep.add(Finding(rule, rel(path), 0, old, new, note))
        text = text.replace(old, new)

    # rule_late_shims 由 process() 统一调用（它需要活动规则集），此处不再重复。
    text = _rule_set_screen(path, text, rep)
    text = _rule_set_accessible_identifier(path, text, rep)
    text = _rule_associated_objects(path, text, rep)
    return text


# 本节各规则的替换目标都是「类名本身」，因此必须避开 `#include <QPromise>` 这类
# 行 —— 把 `#include <QChronoTimer>` 换成 `#include <SnowChronoTimer>` 会直接
# 编译失败（没有这个头文件）。
_LATE_SHIM_TEXT_RULES: list[tuple[str, str, str, str]] = [
    ("R24", "Qt::ColorScheme::Dark", "SnowColorSchemeDark", "Qt 6.5 引入，Qt5 无 Qt::ColorScheme"),
    ("R24", "Qt::ColorScheme::Light", "SnowColorSchemeLight", "同上"),
    ("R24", "Qt::ColorScheme::Unknown", "SnowColorSchemeUnknown", "同上"),
    ("R24", "Qt::ColorScheme", "SnowColorScheme", "同上（枚举本身）"),
    ("R25", "QTimeZone::fromSecondsAheadOfUtc(", "snowTimeZoneFromSecondsAheadOfUtc(",
     "Qt 6.5 引入，Qt5 用显式构造函数 QTimeZone(int offsetSeconds)"),
    ("R26", "QPromise<", "SnowPromise<", "Qt 6.0 引入，Qt5 用 QFutureInterface 垫片"),
    ("R27", "QChronoTimer", "SnowChronoTimer", "Qt 6.8 引入，Qt5 用 QTimer 子类垫片"),
    # R32/R33：这两条本身是「机械可改写」的，但实际落地统一走
    # .workbuddy/qt5-port/apply-manual-qt5-fixes.py（本脚本只报告、从不写盘）。
    # 保留在表里的意义是**回归探测**：将来有人重新引入就能在报告里看到。
    ("R32", ".sliced(", ".mid(",
     "Qt 6.0 引入的成员；Qt5 的 QString/QByteArray/QList/QVector 只有 mid()"),
    ("R33", "QLibraryInfo::path(", "snowLibraryInfoPath(",
     "Qt 6.0 把 location() 改名为 path()；Qt5 只有 location()"),
]
_LATE_SHIM_RULES = {r for r, _, _, _ in _LATE_SHIM_TEXT_RULES} | {"R28"}
# 只把「看起来是 QStyleHints」的接收者判为 Qt6 的 colorScheme()。
# 工程自有 API 的调用（menu->colorScheme() 等）会被挡到人工清单。
_STYLE_HINTS_RECEIVER_RE = re.compile(r"(?i)stylehints|style_hints|hints")


def rule_late_shims(path: Path, text: str, rep: Report, active: set[str]) -> str:
    if not (_LATE_SHIM_RULES & active):
        return text

    out_lines: list[str] = []
    for lineno, line in enumerate(text.split("\n"), start=1):
        stripped = line.lstrip()
        if stripped.startswith("#include"):
            out_lines.append(line)
            continue
        for rule, old, new, note in _LATE_SHIM_TEXT_RULES:
            if rule not in active or old not in line:
                continue
            rep.add(Finding(rule, rel(path), lineno, old, new, note))
            line = line.replace(old, new)
        # R28: `<hints>->colorScheme()` -> snowColorScheme(<hints>)
        #
        # 只对「形如 QStyleHints 的接收者」自动改写。原因：`colorScheme()` 在本工程
        # 里同时是 adqt 自有 API（AdContextMenu::colorScheme()、AdTag::colorScheme()
        # 等），无条件改写会把 `menu->colorScheme() != AdContextMenu::ColorScheme::Inherit`
        # 变成 `snowColorScheme(menu)`，直接编译失败 —— 这是实测踩到的假阳性。
        if "R28" in active and "->colorScheme(" in line:
            def repl(m: re.Match[str]) -> str:
                receiver = m.group(1)
                if not _STYLE_HINTS_RECEIVER_RE.search(receiver):
                    rep.add_manual(Finding("R28", rel(path), lineno, m.group(0),
                                           f"snowColorScheme({receiver})",
                                           "接收者名字不像 QStyleHints；若是工程自有 colorScheme() 则不处理"))
                    return m.group(0)
                new = f"snowColorScheme({receiver})"
                rep.add(Finding("R28", rel(path), lineno, m.group(0), new,
                                "QStyleHints::colorScheme() 是 Qt 6.5 新增；Qt5 退化为按调色板推断"))
                return new
            line = re.sub(r"(" + _MEMBER_RECV + r")\s*->\s*colorScheme\(\s*\)", repl, line)

            # 调用表达式接收者：`QGuiApplication::styleHints()->colorScheme()`。
            # _MEMBER_RECV 只吃「标识符成员链」，接不住 `styleHints()` 这种带括号的
            # 接收者，于是这类调用会被**静默漏掉**（既没改写也没进人工清单）——
            # 实测在 permissionguidewidget.cpp 与 snow_image_viewer 的
            # application_setup.cpp 各有一处，Qt5 下都会编译失败。
            def repl_call(m: re.Match[str]) -> str:
                receiver = m.group(1)
                if not _STYLE_HINTS_RECEIVER_RE.search(receiver):
                    rep.add_manual(Finding("R28", rel(path), lineno, m.group(0),
                                           f"snowColorScheme({receiver})",
                                           "接收者是调表达式且名字不像 styleHints()；"
                                           "若确为 QStyleHints 则需手工改写"))
                    return m.group(0)
                new = f"snowColorScheme({receiver})"
                rep.add(Finding("R28", rel(path), lineno, m.group(0), new,
                                "QStyleHints::colorScheme() 是 Qt 6.5 新增；Qt5 退化为按调色板推断"))
                return new
            line = re.sub(r"(\w+(?:::\w+)*\(\s*\))\s*->\s*colorScheme\(\s*\)", repl_call, line)
        out_lines.append(line)
    return "\n".join(out_lines)


# ---------------------------------------------------------------------------
# 人工清单：R9 / R4b / RM
# ---------------------------------------------------------------------------
SET_SHORTCUT_RE = re.compile(r"setShortcut\s*\(([^)]*)\)")
POSITION_IN_ENTER_RE = re.compile(r"event\s*->\s*position\s*\(\s*\)")


# ---------------------------------------------------------------------------
# R29 —— QByteArrayView 作为 QCryptographicHash::addData() 的临时包装
#
#   Qt6: hash.addData(QByteArrayView(ptr, len));
#   Qt5: hash.addData(ptr, static_cast<int>(len));   // addData(const char*, int)
#
#   Qt 5.15.2 既没有 QByteArrayView 类，也没有转发头（QtCore/QByteArrayView 不存在，
#   qbytearray.h 里 0 次提及）。QCryptographicHash 只有 addData(const char*, int) 与
#   addData(const QByteArray&)（qcryptographichash.h:97-99），没有视图重载。
#
#   只改写「addData( 后面紧跟 QByteArrayView( ... ) 且 addData 的右括号就在视图右括号
#   之后」这一种确凿形态；其余（例如把 QByteArrayView 当类型用）交给兼容头的
#   QByteArrayView 垫片，不在此处猜。
# ---------------------------------------------------------------------------
ADDDATA_CALL_RE = re.compile(r"\baddData\s*\(")


def _match_paren(text: str, open_index: int) -> int:
    """text[open_index] 必须是 '('；返回配对的 ')' 下标，未配对返回 -1。字符串字面量内不计。"""
    depth = 0
    quote = ""
    i = open_index
    while i < len(text):
        ch = text[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _split_top_level(text: str) -> list[str]:
    """按顶层逗号切分实参；括号与字符串字面量内的逗号不切。"""
    parts: list[str] = []
    depth = 0
    last = 0
    quote = ""
    i = 0
    while i < len(text):
        ch = text[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(text[last:i])
            last = i + 1
        i += 1
    parts.append(text[last:])
    return parts


def rule_byte_array_view(path: Path, text: str, rep: Report) -> str:
    if "QByteArrayView" not in text:
        return text
    i = 0
    while True:
        m = ADDDATA_CALL_RE.search(text, i)
        if m is None:
            break
        # 跳过 `addData(` 之后的空白，看是否紧跟 QByteArrayView( |
        j = m.end()
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        if not text.startswith("QByteArrayView", j):
            i = m.end()
            continue
        k = j + len("QByteArrayView")
        while k < len(text) and text[k] in " \t\r\n":
            k += 1
        if k >= len(text) or text[k] != "(":
            i = m.end()
            continue
        view_close = _match_paren(text, k)
        if view_close < 0:
            i = m.end()
            continue
        # addData 的右括号必须紧跟在视图右括号之后（允许空白）。
        p = view_close + 1
        while p < len(text) and text[p] in " \t\r\n":
            p += 1
        if p >= len(text) or text[p] != ")":
            i = m.end()
            continue
        inner = text[k + 1:view_close]
        args = _split_top_level(inner)
        lineno = text.count("\n", 0, m.start()) + 1
        if len(args) != 2:
            rep.add_manual(Finding("R29", rel(path), lineno, text[m.start():p + 1][:90], "",
                                   f"addData(QByteArrayView(...)) 有 {len(args)} 个实参，"
                                   "不是两实参形态，需人工改写"))
            i = p + 1
            continue
        arg0 = args[0].strip()
        arg1 = args[1].strip()
        new_call = f"addData({arg0}, static_cast<int>({arg1}))"
        rep.add(Finding("R29", rel(path), lineno, "addData(QByteArrayView(...))", new_call,
                        "Qt5 无 QByteArrayView 重载，改用 addData(const char*, int)"))
        text = text[:m.start()] + new_call + text[p + 1:]
        i = m.start() + len(new_call)
    return text


def collect_manual(path: Path, text: str, rep: Report) -> None:
    lines = text.split("\n")
    in_enter = False
    for lineno, line in enumerate(lines, start=1):
        if "enterEvent" in line and "{" in line:
            in_enter = True
        if in_enter and POSITION_IN_ENTER_RE.search(line):
            rep.add_manual(Finding("R4b", rel(path), lineno, line.strip(),
                                   "snowEnterPosition(event)",
                                   "enterEvent 体内取位置，Qt5 需走兼容函数"))
        if in_enter and line.strip() == "}":
            in_enter = False

        for m in SET_SHORTCUT_RE.finditer(line):
            arg = m.group(1).strip()
            if arg and not arg.startswith("QKeySequence") and "snowKeySequence" not in arg:
                rep.add_manual(Finding("R9", rel(path), lineno, line.strip(),
                                       f"setShortcut(snowKeySequence({arg}))",
                                       "Qt5 的 setShortcut 只接受 QKeySequence"))

        # 回归哨兵。R5 曾把截断点写成运算符位置（m.start()）而不是接收者起点，
        # 于是接收者被原样留下，产出 `pathssnow::compat::removeIf(paths, ...)`：
        # 语法错误，但混在长行里极难肉眼发现，且在 snow_shot 编译前不会暴露。
        # 标识符紧贴 `snow::compat::` 只可能是这种损坏，因此无条件报出。
        if re.search(r"[A-Za-z0-9_]snow::compat::", line):
            rep.add_manual(Finding("R5-mark", rel(path), lineno, line.strip(), "",
                                   "标识符紧贴 snow::compat::，疑似 R5 前缀重复损坏，必须修复"))

        # Qt6 专有、且无法靠垫片消除的类别：登记进人工清单，不让它悄悄留在代码里。
        # 漏网的代价是 UOS 真机上直接编译失败（本机 Windows 也编译不到，因为
        # snow_shot 还卡在 vcpkg 依赖上），所以必须显式留痕。
        stripped = line.strip()
        is_comment = stripped.startswith("//") or stripped.startswith("*")
        if "QNativeInterface" in line and not is_comment:
            if rel(path) == "snow_shot/src/platform/windows/monitorgeometry.cpp":
                # 这个文件正是 R30 的**收敛点**本身：Qt6 分支保留 QNativeInterface，
                # Qt5 分支走 QPlatformNativeInterface::nativeResourceForScreen("handle", ...)。
                # 由 QT_VERSION 守卫，两侧各自正确，不是待办。
                pass
            elif "QCocoaScreen" in line:
                rep.add_manual(Finding(
                    "R30", rel(path), lineno, stripped, "",
                    "Qt6 的 QNativeInterface::QCocoaScreen（用于取 NSScreen*）。Qt5 没有公开等价物："
                    "cocoa 平台插件在 5.15 里并未实现 nativeResourceForScreen（已读上游 "
                    "qcocoascreen.mm 确认），NSScreen* 只能经插件私有头 QCocoaScreen::nativeScreen() "
                    "取得，而平台插件私有头不随 Qt 安装。macOS 不在本次交付范围"
                    "（Windows 验证 + UOS arm64/amd64），故显式延后。"))
            else:
                rep.add_manual(Finding(
                    "R30", rel(path), lineno, stripped, "",
                    "Qt6 的 QScreen::nativeInterface<T>()；Qt5 无此 API，Windows 侧请改用 "
                    "snow_shot/platform/windows/monitorgeometry.h 的 nativeMonitorHandle()，"
                    "其它平台需按平台另做（见 docs-qt5-downgrade-plan.md §R30）"))
        if "QPointingDevice" in line and not is_comment:
            # 已核实（本机 Qt 5.15.2 头 + 上游源码）：
            #   Qt5 QNativeGestureEvent 有两条构造：
            #     (a) QT_DEPRECATED (type, localPos, windowPos, screenPos, value, delta)
            #     (b) (type, const QTouchDevice *dev, localPos, windowPos, screenPos, value, delta)
            #   设备类型是 QTouchDevice*，且 device() 返回 const QTouchDevice*。
            #   Qt6 改成 (type, const QPointingDevice *device, int fingerCount, localPos,
            #   scenePos, globalPos, value, delta) —— 不只是换类型，还多了 fingerCount
            #   形参、中间多一个 scenePos。所以测试里的
            #   `QPointingDevice::primaryPointingDevice()` 不能机械替换成 nullptr，
            #   必须整条构造按版本分支重写。
            # 收敛点已选：compat 头的 snowNativeGestureEvent() 工厂（4 处调用点
            # 已全部改完，探针第 33 段覆盖 Qt5 分支）。此处保留检测，防止以后
            # 重新把 QPointingDevice 写回调用点。
            rep.add_manual(Finding(
                "R31", rel(path), lineno, stripped, "",
                "Qt6 的 QPointingDevice；Qt5 对应 QTouchDevice 且构造函数形参个数不同，"
                "请改用 compat 头的 snowNativeGestureEvent() 工厂（详见本规则上方注释）"))
        if "QFont::Tag(" in line and not is_comment:
            # R34：Qt 6.7 的字体变体轴（QFont::setVariableAxis(QFont::Tag, float)）与
            # Qt 6.1 的 OpenType 特性（QFont::setFeature(QFont::Tag, quint32)）在
            # Qt 5.15 里**完全没有** —— 已 grep 本机 QtGui/qfont.h，`Tag` /
            # `setVariableAxis` / `setFeature` / `featureTags` 全部 0 命中。
            # 也没有可回落的等价物：Qt5 的 QFont::Weight 是 0..99 旧刻度，承载不了
            # 任意 `wght` 轴值（如 650），Qt5 更没有字体特性 API。
            # 收敛点已选：兼容头第 6 节的 SNOW_FONT_SET_VARIABLE_AXIS /
            # SNOW_FONT_SET_FEATURE 宏（Qt5 展开为 no-op）。此处保留检测，
            # 防止以后重新把 QFont::Tag 写回调用点。
            rep.add_manual(Finding(
                "R34", rel(path), lineno, stripped, "",
                "Qt6 的 QFont::Tag；Qt5 无字体变体轴 / 特性 API，"
                "请改用 compat 头的 SNOW_FONT_SET_VARIABLE_AXIS / SNOW_FONT_SET_FEATURE 宏"))


def rel(p: Path) -> str:
    return p.relative_to(REPO).as_posix()


# ---------------------------------------------------------------------------
# 已人工复核、确认「无需改写」的命中点
#
# 人工清单唯一的价值是「列出来的都还没人看过」。已复核的项如果不剔除，清单会被噪音
# 淹没，真正的漏网点就会被忽略 —— 这不是洁癖，是实测教训：QNativeGestureEvent 那处
# 真阳性正是被 60 多条假阳性埋掉的。
#
# 因此这里逐条登记，每条都写明判定依据与证据位置；删掉任何一条，它都会重新回到
# 人工清单（不会静默消失，报告里单独统计「已复核-无需处理」）。
# key = (规则, 仓库相对路径, 命中文本)
# ---------------------------------------------------------------------------
REVIEWED_SAFE: dict[tuple[str, str, str], str] = {
    # ---- R9：实参本身就是 QKeySequence，再包一层 snowKeySequence() 是多余的 ----
    ("R9", "ant_design_qt/packages/ant_design_qt/src/widgets/context_menu.cpp",
     "action->setShortcut(shortcut);"):
        "形参 shortcut 已是 const QKeySequence&（context_menu.cpp:884 起 "
        "`if (!shortcut.isEmpty())` 即其成员调用），Qt5 的 setShortcut 直接可用",

    # ---- R10：接收者不是 QEvent 派生对象，position() 与 Qt 版本无关 ----
    ("R10", "snow_image/src/codec_registry.cpp", "sink->position()"):
        "snow_image 自有字节流 RasterWriterSink::position()，返回 Result<std::uint64_t>"
        "（codec_registry.cpp:488/497/529/538），与 QEvent::position() 无关",
    ("R10", "snow_shot/src/presentation/pinned/screenshotpinnedwindow.cpp",
     "m_physicalCursor->position()"):
        "snow_shot::platform::PhysicalCursor::position()，工程自有类型"
        "（screenshotpinnedwindow.cpp:1172 构造、8941 调用）",
    ("R10", "snow_shot/src/presentation/services/screenshotcontroller.cpp",
     "m_physicalCursor->position()"):
        "同上，PhysicalCursor::position()（screenshotcontroller.cpp:3440/5649/5719）",
    # 残留检测路径：接收者是调表达式而非简单标识符，before 退化为 `->position()`。
    ("R10", "snow_shot/tests/screenshot_pinned_window_tests.cpp", "->position()"):
        "QTextLayout::position()（block.layout()->position()，测试 13400/13552/13712/13803），"
        "Qt5 与 Qt6 同名 API",
    ("R10", "snow_shot/tests/screenshot_recognition_window_tests.cpp", "->position()"):
        "同上，QTextLayout::position()（测试 2017）",

    # ---- R28：工程自有 colorScheme()，不是 QStyleHints::colorScheme() ----
    # AdContextMenu / AdNavigationMenu / AdTag 各自在类内声明了
    # `enum class ColorScheme` 与 `ColorScheme colorScheme() const`（见
    # context_menu.h:34/85、navigation_menu.h:269、tag.h:169），与 Qt::ColorScheme 无关。
    ("R28", "ant_design_qt/packages/ant_design_qt/src/widgets/context_menu.cpp",
     "menu->colorScheme()"):
        "AdContextMenu::colorScheme()（context_menu.cpp:100/102，返回类内 enum ColorScheme）",
    ("R28", "ant_design_qt/packages/ant_design_qt/src/widgets/context_menu.cpp",
     "parent_->colorScheme()"):
        "AdContextMenu::colorScheme()（context_menu.cpp:926，setColorScheme(parent_->...)）",
    ("R28", "ant_design_qt/packages/ant_design_qt/src/widgets/navigation_menu.cpp",
     "owner_->colorScheme()"):
        "AdNavigationMenu::colorScheme()（navigation_menu.cpp:3420/3454）",
    ("R28", "ant_design_qt/packages/ant_design_qt/tests/context_menu_tests.cpp",
     "submenu->colorScheme()"):
        "AdContextMenu::colorScheme()（测试 97，与 AdContextMenu::ColorScheme::Dark 比较）",
    ("R28", "snow_shot/tests/floating_toolbar_tests.cpp", "menu->colorScheme()"):
        "AdContextMenu::colorScheme()（测试 1334，与 AdContextMenu::ColorScheme::Inherit 比较）",
    ("R28", "snow_shot/tests/mcp_settings_widget_tests.cpp", "status->colorScheme()"):
        "AdTag::colorScheme()（测试 94/150，与 AdTag::ColorScheme::Success 比较）",
    ("R28", "snow_shot/tests/sidebar_widget_tests.cpp", "menu->colorScheme()"):
        "AdNavigationMenu::colorScheme()（测试 86，与 AdNavigationMenu::ColorScheme::Inherit 比较）",
}


def _reviewed_safe_reason(f: Finding) -> str | None:
    """命中文本做一次空白归一化再查表，避免缩进/空格差异导致抑制失效。"""
    key = (f.rule, f.path, " ".join(f.before.split()))
    return REVIEWED_SAFE.get(key)


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def process(path: Path, text: str, rules: set[str], rep: Report) -> str:
    # 兼容头自身既是"改写目标"（它定义 SnowEnterEvent/snowPos/snowMetaType/
    # snow::compat::removeIf），也是"改写依据"。任何规则作用到它上面都会造成
    # 自引用破坏（例如 R3 把它的 #include <QEnterEvent> 换成 include 自己，
    # R5 把 removeIf 的 Qt6 分支改成递归调用自己）。因此整体跳过。
    if path.name == "qt_compat.h":
        return text

    if path.suffix in CPP_SUFFIXES:
        if "R3" in rules:
            text = rule_compat_includes(path, text, rep)
        if "R4" in rules:
            text = rule_enter_event(path, text, rep)
        if "R5" in rules:
            text = rule_remove_if(path, text, rep)
        if "R7" in rules:
            text = rule_meta_type_from(path, text, rep)
        if "R8" in rules:
            text = rule_variant_meta_type(path, text, rep)
        if "R10" in rules or "R11" in rules:
            text = rule_event_positions(path, text, rep)
        if "R12" in rules:
            text = rule_native_event_result(path, text, rep)
        if any(r in rules for r in ("R13", "R14", "R15", "R16", "R17", "R18", "R19", "R20",
                                    "R21", "R22", "R22b", "R23")):
            text = rule_qt6_members(path, text, rep)
        if _LATE_SHIM_RULES & rules:
            text = rule_late_shims(path, text, rep, rules)
        if "R29" in rules:
            text = rule_byte_array_view(path, text, rep)
    else:
        if "R1" in rules:
            text = rule_cmake_targets(path, text, rep)
    collect_manual(path, text, rep)
    return text


ALL_RULES = {
    "R1", "R3", "R4", "R5", "R7", "R8", "R10", "R11", "R12",
    "R13", "R14", "R15", "R16", "R17", "R18", "R19", "R20", "R21", "R22", "R22b", "R23",
    "R24", "R25", "R26", "R27", "R28", "R29",
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="写盘（默认 dry-run）")
    ap.add_argument("--rule", action="append", help="只跑指定规则，可重复；默认全部")
    ap.add_argument("--output", default=None, help="报告输出目录")
    args = ap.parse_args()

    rules = set(args.rule) if args.rule else set(ALL_RULES)
    rep = Report()

    changed = 0
    for path in iter_cpp_files() + iter_cmake_files():
        try:
            original = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError) as exc:
            print(f"skip {rel(path)}: {exc}", file=sys.stderr)
            continue
        updated = process(path, original, rules, rep)
        if updated != original:
            changed += 1
            if args.apply:
                path.write_text(updated, encoding="utf-8", newline="\n")

    by_rule: dict[str, int] = defaultdict(int)
    for f in rep.findings:
        by_rule[f.rule] += 1

    # 拆成「仍需人工确认」与「已复核-无需处理」两拨。已复核的依据见 REVIEWED_SAFE，
    # 单独成文件是为了可审计：任何人都能拿它去核对，而不是凭空相信清单变短了。
    pending: list[Finding] = []
    reviewed: list[tuple[Finding, str]] = []
    for f in rep.manual:
        reason = _reviewed_safe_reason(f)
        if reason is None:
            pending.append(f)
        else:
            reviewed.append((f, reason))

    out_dir = Path(args.output) if args.output else REPO / ".workbuddy" / "qt5-port"
    out_dir.mkdir(parents=True, exist_ok=True)

    mode = "APPLIED" if args.apply else "DRY-RUN"
    lines = [f"# Qt6 -> Qt5 机械改写报告 ({mode})", ""]
    lines.append(f"- 命中改写点: {len(rep.findings)}")
    lines.append(f"- 涉及文件: {changed}")
    lines.append(f"- 待人工确认: {len(pending)}")
    lines.append(f"- 已复核-无需处理: {len(reviewed)}（见 manual-reviewed-safe.txt）")
    lines.append("")
    lines.append("## 按规则统计")
    lines.append("")
    lines.append("| 规则 | 命中 |")
    lines.append("|---|---|")
    for r in sorted(by_rule):
        lines.append(f"| {r} | {by_rule[r]} |")
    lines.append("")
    if rep.unknown_targets:
        lines.append("## Qt6 非模块目标（需 ${SNOW_QT_NS} 解析）")
        lines.append("")
        lines.append("| 目标 | 出现次数 |")
        lines.append("|---|---|")
        for name, cnt in sorted(rep.unknown_targets.items(), key=lambda kv: -kv[1]):
            lines.append(f"| Qt6::{name} | {cnt} |")
        lines.append("")

    (out_dir / "report.md").write_text("\n".join(lines), encoding="utf-8")

    manual_lines = [f"# 需人工确认的改写点 ({len(pending)})", ""]
    cur = None
    for f in sorted(pending, key=lambda x: (x.rule, x.path, x.line)):
        key = (f.rule, f.path)
        if key != cur:
            cur = key
            manual_lines.append("")
            manual_lines.append(f"## [{f.rule}] {f.path}")
        manual_lines.append(f"- L{f.line}: `{f.before}`")
        if f.after:
            manual_lines.append(f"    -> `{f.after}`  ({f.note})")
    if not pending:
        manual_lines.append("(无)")
    manual_lines.append("")
    (out_dir / "manual-review.txt").write_text("\n".join(manual_lines), encoding="utf-8")

    reviewed_lines = [f"# 已复核-无需处理的命中点 ({len(reviewed)})", ""]
    cur = None
    for f, reason in sorted(reviewed, key=lambda x: (x[0].rule, x[0].path, x[0].line)):
        key = (f.rule, f.path)
        if key != cur:
            cur = key
            reviewed_lines.append("")
            reviewed_lines.append(f"## [{f.rule}] {f.path}")
        reviewed_lines.append(f"- L{f.line}: `{f.before}`")
        reviewed_lines.append(f"    -> 判定：{reason}")
    (out_dir / "manual-reviewed-safe.txt").write_text("\n".join(reviewed_lines),
                                                      encoding="utf-8")

    print("\n".join(lines))
    print(f"\n报告: {out_dir / 'report.md'}")
    print(f"人工清单: {out_dir / 'manual-review.txt'}")
    print(f"已复核清单: {out_dir / 'manual-reviewed-safe.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
