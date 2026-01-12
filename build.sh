#!/bin/bash
set -e

# ================= 基本路径 =================

ROOT_DIR="$(pwd)"
THIRD_DIR="$ROOT_DIR/3rdparty"

WEBRTC_APM_SRC="$THIRD_DIR/webrtc-audio-processing"
WEBRTC_APM_INSTALL="$WEBRTC_APM_SRC/install"

WEBRTC_VAD_DIR="$THIRD_DIR/webrtc_vad"
TEN_VAD_DIR="$THIRD_DIR/ten_vad"
ONNX_DIR="$THIRD_DIR/onnxruntime"
SILERO_DIR="$THIRD_DIR/silero_vad"

PKG_NAME="aeroshell_audio"
DIST_DIR="$ROOT_DIR/${PKG_NAME}_dist"
OUTPUT_PKG="$ROOT_DIR/${PKG_NAME}.tar.gz"

# ================= [0] 清理 =================

echo ">>> [0/6] 清理旧产物 <<<"
rm -rf "$DIST_DIR" "$OUTPUT_PKG"

# ================= [1] 构建 WebRTC AudioProcessing =================

echo ">>> [1/6] 构建 WebRTC AudioProcessing (第三方目录内) <<<"

cd "$WEBRTC_APM_SRC"

meson . build -Dprefix=$PWD/install

ninja -C build
ninja -C build install

cd "$ROOT_DIR"

# ================= [2] 准备 ONNX Runtime =================

echo ">>> [2/6] 检查 ONNX Runtime <<<"

if [ ! -d "$ONNX_DIR" ]; then
    TGZ="$THIRD_DIR/onnxruntime-linux-x64-1.23.2.tgz"
    if [ ! -f "$TGZ" ]; then
        echo "❌ 缺少 $TGZ"
        exit 1
    fi
    tar -xzf "$TGZ" -C "$THIRD_DIR"
    mv "$THIRD_DIR/onnxruntime-linux-x64-1.23.2" "$ONNX_DIR"
fi


echo ">>> [2.5/6] 修复第三方库 link name <<<"

# ---- ONNX Runtime ----
if [ -e "$ONNX_DIR/lib/libonnxruntime.so.1" ] && [ ! -e "$ONNX_DIR/lib/libonnxruntime.so" ]; then
    ln -s libonnxruntime.so.1 "$ONNX_DIR/lib/libonnxruntime.so"
fi

# ================= [3] 编译 WebRTC VAD =================

echo ">>> [3/6] 编译 WebRTC VAD <<<"

cd "$WEBRTC_VAD_DIR"
make clean
make
cd "$ROOT_DIR"

# ================= [4] 编译主程序 =================

echo ">>> [4/6] 编译主程序 aec_process <<<"

g++ main.cpp -std=c++17 -O2 \
    -I"$ROOT_DIR" \
    -I"$WEBRTC_APM_INSTALL/include" \
    -I"$WEBRTC_APM_INSTALL/include/webrtc-audio-processing-2" \
    -I"$WEBRTC_APM_INSTALL/include/webrtc-audio-processing-2/api/audio" \
    -I"$WEBRTC_APM_INSTALL/include/webrtc-audio-processing-2/modules/audio_processing/include" \
    -I"$WEBRTC_VAD_DIR/include" \
    -I"$TEN_VAD_DIR" \
    -I"$ONNX_DIR/include" \
    -I"$THIRD_DIR/spdlog-1.17.0/include" \
    -L"$WEBRTC_APM_INSTALL/lib/x86_64-linux-gnu" \
    -L"$WEBRTC_VAD_DIR" \
    -L"$TEN_VAD_DIR" \
    -L"$ONNX_DIR/lib" \
    -lten_vad \
    -lwebrtc-audio-processing-2 \
    -lwebrtc_vad \
    -lonnxruntime \
    -lopus \
    -lpthread -lm \
    -Wl,-rpath,'$ORIGIN' \
    -o aec_process

# ================= [5] 打包 =================

echo ">>> [5/6] 组装发布目录 <<<"

mkdir -p "$DIST_DIR"

cp aec_process "$DIST_DIR/"
cp "$WEBRTC_APM_INSTALL/lib/x86_64-linux-gnu/libwebrtc-audio-processing-2.so.1" "$DIST_DIR/"
cp "$ONNX_DIR/lib/libonnxruntime.so.1" "$DIST_DIR/"
cp "$TEN_VAD_DIR/libten_vad.so" "$DIST_DIR/"

if [ -f "$SILERO_DIR/silero_vad.onnx" ]; then
    # 直接拷贝到发布目录顶级，供程序 ./silero_vad.onnx 使用
    cp "$SILERO_DIR/silero_vad.onnx" "$DIST_DIR/"
fi

cat <<EOF > "$DIST_DIR/run.sh"
#!/bin/bash
cd "\$(dirname "\$0")"
export LD_LIBRARY_PATH=.
./aec_process
EOF
chmod +x "$DIST_DIR/run.sh"

# ================= [6] 压缩 =================

echo ">>> [6/6] 打包输出 <<<"
tar -czf "$OUTPUT_PKG" -C "$DIST_DIR" .

echo "---------------------------------------"
echo "✅ 构建完成: $OUTPUT_PKG"
