#!/bin/bash
# Harness Guard — 铁律自动检查脚本
# 用法: bash scripts/lint_rules.sh [--fix]
# Codex 每次任务完成后自动运行此脚本

set -euo pipefail
cd "$(dirname "$0")/.."

ERRORS=0
WARNINGS=0

echo "╔══════════════════════════════════════════════════╗"
echo "║        Harness Guard — 铁律自动检查              ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""

# ─── C++ / FFmpeg 铁律 ───

echo "── [1/7] FFmpeg 裸指针检查 ──"
if find plasma-hawking/src -name "*.h" -o -name "*.cpp" | \
   xargs grep -n 'AVFrame\s*\*\|AVPacket\s*\*\|AVCodecContext\s*\*\|AVFormatContext\s*\*' 2>/dev/null | \
   grep -v 'unique_ptr\|shared_ptr\|Deleter\|av_frame_free\|av_packet_free\|avcodec_free\|//\|/\*\|#include\|decltype\|using\s' ; then
    echo "❌ 违反铁律 #1: FFmpeg 裸指针！必须使用 FFmpegUtils.h 中的 RAII Wrapper"
    ERRORS=$((ERRORS+1))
else
    echo "✅ 通过"
fi

echo ""
echo "── [2/7] 旧版 channel_layout 检查 ──"
if find plasma-hawking/src -name "*.cpp" -o -name "*.h" | \
   xargs grep -n '->channel_layout\|->channels\s*=' 2>/dev/null | \
   grep -v '//\|/\*\|ch_layout' ; then
    echo "❌ 违反铁律 #2: 禁止旧版 channel_layout，使用 AVChannelLayout"
    ERRORS=$((ERRORS+1))
else
    echo "✅ 通过"
fi

echo ""
echo "── [3/7] OpenGL Shader 版本检查 ──"
SHADER_ERR=0
for f in plasma-hawking/shaders/*.vert plasma-hawking/shaders/*.frag; do
    [ -f "$f" ] || continue
    if ! head -1 "$f" | grep -q "#version 460 core"; then
        echo "❌ $f 首行必须是 #version 460 core"
        SHADER_ERR=1
    fi
done
if [ $SHADER_ERR -eq 1 ]; then
    ERRORS=$((ERRORS+1))
else
    echo "✅ 通过"
fi

# ─── QML 铁律 ───

echo ""
echo "── [4/7] QML 旧版 import 检查 ──"
if find plasma-hawking/qml -name "*.qml" | \
   xargs grep -n 'import QtQuick [0-9]\|import QtQuick\.Controls [0-9]' 2>/dev/null ; then
    echo "❌ 违反铁律 #5: 禁止 import QtQuick 2.x，使用 import QtQuick"
    ERRORS=$((ERRORS+1))
else
    echo "✅ 通过"
fi

echo ""
echo "── [5/7] Main.qml 行数检查 ──"
if [ -f "plasma-hawking/qml/Main.qml" ]; then
    LINES=$(wc -l < "plasma-hawking/qml/Main.qml")
    if [ "$LINES" -gt 120 ]; then
        echo "⚠️  警告: Main.qml 有 ${LINES} 行，建议拆分子组件 (目标 ≤80 行)"
        WARNINGS=$((WARNINGS+1))
    else
        echo "✅ 通过 (${LINES} 行)"
    fi
fi

# ─── Go 铁律 ───

echo ""
echo "── [6/7] Go Session 直接写检查 ──"
if [ -f "meeting-server/signaling/server/session.go" ]; then
    if grep -n 'conn\.Write\|\.conn\.Write' meeting-server/signaling/server/session.go | \
       grep -v 'writeLoop\|//\|/\*' ; then
        echo "⚠️  警告: session.go 中发现非 writeLoop 的 conn.Write 调用"
        WARNINGS=$((WARNINGS+1))
    else
        echo "✅ 通过"
    fi
fi

echo ""
echo "── [7/7] Go Handler 测试覆盖检查 ──"
HANDLER_ERR=0
for f in meeting-server/signaling/handler/*_handler.go; do
    [ -f "$f" ] || continue
    base=$(basename "$f" .go)
    test_file="meeting-server/signaling/handler/${base}_test.go"
    if [ ! -f "$test_file" ]; then
        echo "❌ 缺少测试: $f → 需要 ${test_file}"
        HANDLER_ERR=1
    fi
done
if [ $HANDLER_ERR -eq 1 ]; then
    ERRORS=$((ERRORS+1))
else
    echo "✅ 通过"
fi

# ─── 总结 ───

echo ""
echo "══════════════════════════════════════════════════"
echo "  结果: ${ERRORS} 个违规 | ${WARNINGS} 个警告"
echo "══════════════════════════════════════════════════"

if [ $ERRORS -gt 0 ]; then
    echo "🚫 铁律检查未通过，请修复后重新提交"
    exit 1
else
    echo "✅ 铁律检查全部通过"
    exit 0
fi
