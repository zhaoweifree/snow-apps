# 脚本模式（cmake -P）：跑 lrelease，并强制执行"目录里不得留下未完成翻译"。
#
# 为什么需要这个文件
# ------------------
# 本工程的翻译门禁依赖 Qt6 lrelease 的 `-fail-on-unfinished`（见 AGENTS.md：
# 不允许留下 type="unfinished"）。Qt 5.15.2 的 lrelease **没有这个开关**，实测：
#
#     $ lrelease -help
#     Options: -help -idbased -compress -nounfinished -removeidentical
#              -markuntranslated <prefix> -project <filename> -silent -version
#
# 更关键的是**它不会因为未完成而失败，也不会在统计行里体现未完成**。同一份
# .ts（1 条已完成 + 1 条 type="unfinished"）在 Qt 5.15.2 下的原始输出是：
#
#     Updating 'mix.qm'...
#         Generated 1 translation(s) (1 finished and 0 unfinished)
#         Ignored 1 untranslated source text(s)
#     $ echo $?
#     0
#
# 即：`(X finished and Y unfinished)` 里的 Y 描述的是**已生成条目**的完成度，
# 未完成条目被归到另一行 `Ignored N untranslated source text(s)`，而且加不加
# `-nounfinished` 都不改变这两行、退出码也始终为 0。
# 所以"照抄统计行"会做出一条**永远通过**的假门禁——比构建失败危险得多。
#
# 因此这里用两个相互独立的信号，任一命中即失败：
#   1. 源 .ts 里是否存在 type="unfinished"（主判据；与 AGENTS.md 的措辞逐字对应，
#      不依赖任何工具的文案，工具改措辞也影响不到它）；
#   2. lrelease 自己是否报了 `Ignored N untranslated source text(s)`（N > 0）。
#      实测两者在同一条件下触发（都只针对 type="unfinished"）：一个空但**没有**
#      type="unfinished" 的 <translation></translation> 两个信号都不报——这与 Qt6
#      的行为一致，故不做额外收紧。信号 2 是交叉校验，防的是"字面量扫描因引号
#      形式变化而漏判"。
# 只丢开关不做等价校验会让门禁静默消失，残缺目录照常进安装包，故不采用。
#
# 调用（由 cmake/SnowQtVersionShims.cmake 生成，勿手工执行）：
#   cmake -DSNOW_LRELEASE=<lrelease> -DSNOW_TS_FILE=<ts> -DSNOW_QM_FILE=<qm>
#         -DSNOW_LRELEASE_OPTIONS=<分号分隔的其余选项> -P SnowQt5LreleaseEnforce.cmake

foreach(_snow_required IN ITEMS SNOW_LRELEASE SNOW_TS_FILE SNOW_QM_FILE)
    if(NOT DEFINED "${_snow_required}" OR "${${_snow_required}}" STREQUAL "")
        message(FATAL_ERROR "Snow Qt5 lrelease: ${_snow_required} is required")
    endif()
endforeach()

if(NOT EXISTS "${SNOW_TS_FILE}")
    message(FATAL_ERROR "Snow Qt5 lrelease: no such catalog file: ${SNOW_TS_FILE}")
endif()

# 信号 1：源目录里的未完成标记。
file(READ "${SNOW_TS_FILE}" _snow_ts_content)
string(REGEX MATCHALL "type=\"unfinished\"" _snow_unfinished "${_snow_ts_content}")
list(LENGTH _snow_unfinished _snow_unfinished_count)
if(_snow_unfinished_count GREATER 0)
    message(FATAL_ERROR
        "Snow Qt5 lrelease: ${SNOW_TS_FILE} contains ${_snow_unfinished_count} "
        "type=\"unfinished\" message(s). The Qt6 route rejects this through "
        "-fail-on-unfinished; Qt 5.15.2's lrelease neither fails nor reports them, "
        "so the check is enforced here. Translate or remove them before building.")
endif()

# 信号 2：lrelease 自己的"忽略未翻译"计数。
execute_process(
    COMMAND "${SNOW_LRELEASE}" ${SNOW_LRELEASE_OPTIONS} "${SNOW_TS_FILE}" -qm "${SNOW_QM_FILE}"
    RESULT_VARIABLE _snow_result
    OUTPUT_VARIABLE _snow_stdout
    ERROR_VARIABLE _snow_stderr)

if(NOT _snow_result EQUAL 0)
    message(FATAL_ERROR
        "Snow Qt5 lrelease failed (exit ${_snow_result}) for ${SNOW_TS_FILE}:\n"
        "${_snow_stdout}${_snow_stderr}")
endif()

if("${_snow_stdout}" MATCHES "Ignored ([1-9][0-9]*) untranslated source text")
    message(FATAL_ERROR
        "Snow Qt5 lrelease reported ${CMAKE_MATCH_1} untranslated source text(s) in "
        "${SNOW_TS_FILE}; the Qt5 route must not ship a partial catalog.\n${_snow_stdout}")
endif()

message(STATUS
    "Snow Qt5 lrelease: ${SNOW_TS_FILE} -> ${SNOW_QM_FILE} (no unfinished translations)")
