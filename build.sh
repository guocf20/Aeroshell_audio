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

g++ audio_udp_packet.cpp main.cpp -std=c++17 -O2 \
    -I"$ROOT_DIR" \
    -I"$THIRD_DIR" \
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

# ================= [4.5] 编译 Go Provider =================

# ================= [4.5] 编译 Go Provider =================

echo ">>> [4.5/6] 编译 Go Provider <<<"

PROVIDER_DIR="$ROOT_DIR/provider"
PROVIDER_BIN="asr_provider"

if [ ! -d "$PROVIDER_DIR" ]; then
    echo "❌ 缺少 provider 目录: $PROVIDER_DIR"
    exit 1
fi

cd "$PROVIDER_DIR"

go build -o "$ROOT_DIR/$PROVIDER_BIN" main.go

cd "$ROOT_DIR"


# ================= [5] 打包 =================

echo ">>> [5/6] 组装发布目录 <<<"

mkdir -p "$DIST_DIR"

cp aec_process "$DIST_DIR/"
cp "$ROOT_DIR/$PROVIDER_BIN" "$DIST_DIR/"
cp "$WEBRTC_APM_INSTALL/lib/x86_64-linux-gnu/libwebrtc-audio-processing-2.so.1" "$DIST_DIR/"
cp "$ONNX_DIR/lib/libonnxruntime.so.1" "$DIST_DIR/"
cp "$TEN_VAD_DIR/libten_vad.so" "$DIST_DIR/"

if [ -f "$SILERO_DIR/silero_vad.onnx" ]; then
    # 直接拷贝到发布目录顶级，供程序 ./silero_vad.onnx 使用
    cp "$SILERO_DIR/silero_vad.onnx" "$DIST_DIR/"
fi


cat <<EOF > "$DIST_DIR/run.sh"
#!/bin/bash
set -e

BASE_DIR="\$(cd "\$(dirname "\$0")" && pwd)"
cd "\$BASE_DIR"

export LD_LIBRARY_PATH="\$BASE_DIR:\${LD_LIBRARY_PATH:-}"

CONFIG_FILE="/etc/aero_audio/config.json"

PROVIDER_BIN="$PROVIDER_BIN"
GATEWAY_BIN="aec_process"

PID_DIR="\$BASE_DIR/pids"
LOG_DIR="\$BASE_DIR/logs"

PROVIDER_PID_FILE="\$PID_DIR/provider.pid"
GATEWAY_PID_FILE="\$PID_DIR/gateway.pid"

mkdir -p "\$PID_DIR" "\$LOG_DIR"

is_running() {
    local pid_file="\$1"

    if [ ! -f "\$pid_file" ]; then
        return 1
    fi

    local pid
    pid="\$(cat "\$pid_file")"

    if [ -z "\$pid" ]; then
        return 1
    fi

    kill -0 "\$pid" 2>/dev/null
}

start_provider() {
    if is_running "\$PROVIDER_PID_FILE"; then
        echo "ASR Provider 已在运行，PID=\$(cat "\$PROVIDER_PID_FILE")"
        return
    fi

    echo ">>> 启动 ASR Provider <<<"
    "./\$PROVIDER_BIN" -c "\$CONFIG_FILE" > "\$LOG_DIR/provider.log" 2>&1 &
    echo \$! > "\$PROVIDER_PID_FILE"
    echo "ASR Provider 启动成功，PID=\$(cat "\$PROVIDER_PID_FILE")"
}

start_gateway() {
    if is_running "\$GATEWAY_PID_FILE"; then
        echo "Audio Gateway 已在运行，PID=\$(cat "\$GATEWAY_PID_FILE")"
        return
    fi

    echo ">>> 启动 Audio Gateway <<<"
    "./\$GATEWAY_BIN" -c "\$CONFIG_FILE" > "\$LOG_DIR/gateway.log" 2>&1 &
    echo \$! > "\$GATEWAY_PID_FILE"
    echo "Audio Gateway 启动成功，PID=\$(cat "\$GATEWAY_PID_FILE")"
}

stop_one() {
    local name="\$1"
    local pid_file="\$2"

    if ! is_running "\$pid_file"; then
        echo "\$name 未运行"
        rm -f "\$pid_file"
        return
    fi

    local pid
    pid="\$(cat "\$pid_file")"

    echo "正在停止 \$name，PID=\$pid"
    kill "\$pid" 2>/dev/null || true

    for i in \$(seq 1 10); do
        if ! kill -0 "\$pid" 2>/dev/null; then
            rm -f "\$pid_file"
            echo "\$name 已停止"
            return
        fi
        sleep 1
    done

    echo "\$name 未正常退出，强制停止"
    kill -9 "\$pid" 2>/dev/null || true
    rm -f "\$pid_file"
}

start_all() {
    start_provider
    sleep 1
    start_gateway
}

stop_all() {
    stop_one "Audio Gateway" "\$GATEWAY_PID_FILE"
    stop_one "ASR Provider" "\$PROVIDER_PID_FILE"
}

status_one() {
    local name="\$1"
    local pid_file="\$2"

    if is_running "\$pid_file"; then
        echo "\$name: running, PID=\$(cat "\$pid_file")"
    else
        echo "\$name: stopped"
    fi
}

status_all() {
    status_one "ASR Provider" "\$PROVIDER_PID_FILE"
    status_one "Audio Gateway" "\$GATEWAY_PID_FILE"
}

case "\${1:-start}" in
    start)
        start_all
        ;;

    stop)
        stop_all
        ;;

    restart)
        stop_all
        sleep 1
        start_all
        ;;

    status)
        status_all
        ;;

    logs)
        echo "Provider log: \$LOG_DIR/provider.log"
        echo "Gateway log:  \$LOG_DIR/gateway.log"
        ;;

    *)
        echo "用法: \$0 {start|stop|restart|status|logs}"
        exit 1
        ;;
esac
EOF

chmod +x "$DIST_DIR/run.sh"


# ================= [6] 压缩 =================

echo ">>> [6/6] 打包输出 <<<"
tar -czf "$OUTPUT_PKG" -C "$DIST_DIR" .

echo "---------------------------------------"
echo "✅ 构建完成: $OUTPUT_PKG"
