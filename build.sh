#!/bin/bash

# 项目根目录: /home/audio/test/Aeroshell_audio
TARGET_DIR="/home/test"

echo ">>> [1/5] 检查并解压依赖资源 <<<"
# 如果 onnxruntime 目录不存在但压缩包存在，则解压
if [ ! -d "onnxruntime" ]; then
    if [ -f "onnxruntime-linux-x64-1.23.2.tgz" ]; then
        echo "解压 ONNX Runtime..."
        tar -xzf onnxruntime-linux-x64-1.23.2.tgz
        mv onnxruntime-linux-x64-1.23.2 onnxruntime
    else
        echo "错误: 找不到 onnxruntime 目录或压缩包"
        exit 1
    fi
fi

echo ">>> [2/5] 编译 WebRTC VAD 静态库 <<<"
if [ -d "vad" ]; then
    cd vad && make clean && make
    cd ..
else
    echo "错误: 找不到 vad 源码目录"
    exit 1
fi

echo ">>> [3/5] 执行本地 C++ 构建 <<<"
# 这里的 -I 路径完全匹配你刚才 find 出来的 WebRTC 2.1 结构
g++ main.cpp -std=c++17 -O2 \
    -I. \
    -I./install/include \
    -I./install/include/webrtc-audio-processing-2 \
    -I./install/include/webrtc-audio-processing-2/api/audio \
    -I./install/include/webrtc-audio-processing-2/modules/audio_processing/include \
    -I./vad/include \
    -I./onnxruntime/include \
    -L./install/lib/x86_64-linux-gnu \
    -L./vad \
    -L./onnxruntime/lib \
    -lwebrtc-audio-processing-2 \
    -lwebrtc_vad \
    -lonnxruntime \
    -lopus \
    -lpthread -lm \
    -Wl,-rpath,'$ORIGIN' \
    -o aec_process

if [ $? -eq 0 ]; then
    echo ">>> 编译成功: aec_process 已生成。"
else
    echo ">>> 编译失败。"
    exit 1
fi

echo ">>> [4/5] 自动提取组件到 $TARGET_DIR <<<"
mkdir -p $TARGET_DIR
# 拷贝程序和模型
cp aec_process $TARGET_DIR/
[ -f "silero_vad.onnx" ] && cp silero_vad.onnx $TARGET_DIR/

# 拷贝运行时必需的动态库 (自动处理版本号)
cp ./install/lib/x86_64-linux-gnu/libwebrtc-audio-processing-2.so.1 $TARGET_DIR/
cp ./onnxruntime/lib/libonnxruntime.so.1 $TARGET_DIR/

echo ">>> [5/5] 验证运行环境 <<<"
cd $TARGET_DIR
ldd ./aec_process | grep -E "webrtc|onnx"

echo "---------------------------------------"
echo "构建完成！测试程序位置: $TARGET_DIR/aec_process"