#!/bin/bash

# 脚本位置：/home/audio/test/Aeroshell_audio/build.sh
TARGET_DIR="/home/test"

echo ">>> 正在项目根目录执行本地构建 <<<"

# 编译部分
g++ main.cpp -std=c++17 -O2 \
    -I. \
    -I./install/include \
    -I./install/include/webrtc-audio-processing-2 \
    -I./install/include/webrtc-audio-processing-2/api/audio \
    -I./install/include/webrtc-audio-processing-2/modules/audio_processing/include \
    -I./vad/include \
    -I./onnxruntime/include/ \
    -L./install/lib/x86_64-linux-gnu/ \
    -L./vad \
    -L./onnxruntime/lib \
    -lwebrtc-audio-processing-2 \
    -lwebrtc_vad \
    -lonnxruntime \
    -lopus \
    -lpthread -lm \
    -Wl,-rpath,'$ORIGIN' \
    -o aec_process

# 检查编译结果
if [ $? -eq 0 ]; then
    echo ">>> 编译成功: aec_process 已生成。"
else
    echo ">>> 编译失败。"
    exit 1
fi

# 自动打包部署
echo ">>> 正在同步二进制文件与库到 $TARGET_DIR ..."
mkdir -p $TARGET_DIR
cp aec_process $TARGET_DIR/
[ -f "silero_vad.onnx" ] && cp silero_vad.onnx $TARGET_DIR/
cp ./install/lib/x86_64-linux-gnu/libwebrtc-audio-processing-2.so.1 $TARGET_DIR/
cp ./onnxruntime/lib/libonnxruntime.so.1 $TARGET_DIR/

echo ">>> 构建与部署完成。"