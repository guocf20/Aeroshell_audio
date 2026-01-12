#!/bin/bash
set -e

echo "======================================"
echo "[CLEAN] Aeroshell Audio Build Artifacts"
echo "======================================"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
THIRD_PARTY="$ROOT_DIR/3rdparty"

echo "[1/6] 清理主程序构建产物"
rm -f "$ROOT_DIR/aec_process"

echo "[2/6] 清理发布目录"
rm -rf "$ROOT_DIR/aeroshell_audio_dist"
rm -f  "$ROOT_DIR/aeroshell_audio.tar.gz"

echo "[3/6] 清理 WebRTC AudioProcessing 构建缓存"
if [ -d "$THIRD_PARTY/webrtc-audio-processing/build" ]; then
    rm -rf "$THIRD_PARTY/webrtc-audio-processing/build"
    echo "  - removed webrtc-audio-processing/build"
fi

if [ -d "$THIRD_PARTY/webrtc-audio-processing/install" ]; then
    rm -rf "$THIRD_PARTY/webrtc-audio-processing/install"
    echo "  - removed webrtc-audio-processing/install"
fi

echo "[4/6] 清理 WebRTC VAD 构建产物"
if [ -d "$THIRD_PARTY/webrtc_vad" ]; then
    (
        cd "$THIRD_PARTY/webrtc_vad"
        make clean >/dev/null 2>&1 || true
    )
    echo "  - cleaned webrtc_vad"
fi

echo "[5/6] 清理运行时残留（logs / 临时文件）"
rm -rf "$ROOT_DIR/run/logs" 2>/dev/null || true
rm -rf "$ROOT_DIR/logs"      2>/dev/null || true

echo "[6/6] 保留的内容（未删除）："
echo "  ✓ 3rdparty/onnxruntime"
echo "  ✓ 3rdparty/ten_vad"
echo "  ✓ 3rdparty/silero_vad"
echo "  ✓ 3rdparty/spdlog-1.17.0"
echo "  ✓ 所有源码文件"

echo "--------------------------------------"
echo "Clean finished. Environment is ready."
