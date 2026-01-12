#!/bin/bash

# 项目名称
PKG_NAME="aeroshell_audio"
# 最终打包的临时目录
DIST_DIR="./${PKG_NAME}_dist"
# 最终压缩包名称
OUTPUT_PKG="${PKG_NAME}.tar.gz"

echo ">>> [0/6] 初始化环境与构建 WebRTC <<<"
# 清理旧的构建和打包目录
rm -rf  $DIST_DIR $OUTPUT_PKG
# 编译并安装 WebRTC 到本地 install 目录
meson . build -Dprefix=$PWD/install
ninja -C build install

echo ">>> [1/6] 准备第三方依赖 (ONNX Runtime) <<<"
if [ ! -d "onnxruntime" ]; then
    if [ -f "onnxruntime-linux-x64-1.23.2.tgz" ]; then
        echo "正在解压 ONNX Runtime..."
        tar -xzf onnxruntime-linux-x64-1.23.2.tgz
        mv onnxruntime-linux-x64-1.23.2 onnxruntime
    else
        echo "错误: 缺少 onnxruntime 压缩包"
        exit 1
    fi
fi

echo ">>> [2/6] 编译 WebRTC VAD 静态库 <<<"
if [ -d "vad" ]; then
    # 保持 minimal changes 原则，调用你已有的 Makefile
    cd vad && make clean && make && cd ..
else
    echo "错误: 找不到 vad 目录"
    exit 1
fi

echo ">>> [3/6] 编译主程序 aec_process <<<"
# 这里的编译参数已经包含了之前调试通的所有 -I 和 -L
g++ main.cpp -std=c++17 -O2 \
    -I. \
    -I./install/include \
    -I./install/include/webrtc-audio-processing-2 \
    -I./install/include/webrtc-audio-processing-2/api/audio \
    -I./install/include/webrtc-audio-processing-2/modules/audio_processing/include \
    -I./vad/include \
    -I./3rdparty/ten_vad \
    -I./3rdparty/spdlog-1.17.0/include\
    -I./onnxruntime/include \
     -L./3rdparty/ten_vad \
    -L./install/lib/x86_64-linux-gnu \
    -L./vad \
    -L./onnxruntime/lib \
    -lten_vad \
    -lwebrtc-audio-processing-2 \
    -lwebrtc_vad \
    -lonnxruntime \
    -lopus \
    -lpthread -lm \
    -Wl,-rpath,'$ORIGIN' \
    -o aec_process

if [ $? -ne 0 ]; then echo "编译失败！"; exit 1; fi

echo ">>> [4/6] 组装发布目录结构 <<<"
mkdir -p $DIST_DIR

# 拷贝核心文件 (利用 $ORIGIN，所有库和程序放在同一级)
cp aec_process $DIST_DIR/
[ -f "silero_vad.onnx" ] && cp silero_vad.onnx $DIST_DIR/
cp ./install/lib/x86_64-linux-gnu/libwebrtc-audio-processing-2.so.1 $DIST_DIR/
cp ./onnxruntime/lib/libonnxruntime.so.1 $DIST_DIR/
cp ./3rdparty/ten_vad/libten_vad.so $DIST_DIR/

# (可选) 增加一个启动脚本，防止环境中有奇葩的 LD_LIBRARY_PATH 干扰
cat <<EOF > $DIST_DIR/run.sh
#!/bin/bash
cd "\$(dirname "\$0")"
export LD_LIBRARY_PATH=.
./aec_process
EOF
chmod +x $DIST_DIR/run.sh

echo ">>> [5/6] 制作最终压缩包: $OUTPUT_PKG <<<"
# 进入目录打包，这样解压后不会带长串的路径
tar -czf $OUTPUT_PKG -C $DIST_DIR .

echo ">>> [6/6] 验证压缩包内容 <<<"
tar -tvf $OUTPUT_PKG

echo "---------------------------------------"
echo "构建成功！"
echo "部署方式: 将 $OUTPUT_PKG 拷贝到目标机器，解压后执行 ./run.sh 或 ./aec_process"
