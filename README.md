D（语音活动检测）误判频繁

STT 输入质量下降，文本准确率急剧降低

本网关针对上述问题进行了系统级设计与优化，能够：

在 UDP 等不可靠传输下，重组支离破碎的语音包

通过音频预处理（AEC / NS / AGC）恢复可用语音质量

在多种 VAD 实现之间灵活切换，保证语音段切分稳定

将连续、干净的语音流精准送入 STT 系统

在网络极端恶劣时，依然保持较高的语音还原度与识别准确率

该网关适用于：

实时语音转文字（STT）系统

语音交互设备（嵌入式 / IoT / 边缘计算）

远程会议、语音客服、AI 语音代理

弱网环境下的语音采集与分析

核心能力概述

抗弱网能力强
支持乱序、丢包情况下的音频重组与平滑输出。

高质量音频预处理
集成 WebRTC Audio Processing，支持回声消除（AEC）、噪声抑制（NS）、自动增益控制（AGC）。

多 VAD 引擎支持

WebRTC VAD（低延迟、轻量）

Silero VAD（高准确率，神经网络）

TenVAD（轻量神经网络 VAD）

高性能、低延迟
全流程基于 C/C++ 实现，适合长时间稳定运行。

编译依赖说明
系统依赖

在构建本项目之前，需要确保系统已安装以下基础依赖：

1. Opus 编解码库

Opus 用于音频数据的编码与解码，是整个语音链路的核心组件之一。

在 Debian / Ubuntu 系统上可通过以下方式安装：

apt-get update
apt-get install -y libopus-dev
apt install libc++1

2. Meson 构建系统

Meson 用于构建 WebRTC Audio Processing 等第三方组件。

推荐使用系统包管理器安装：

apt-get install -y meson ninja-build


注意：

Meson 需要配合 ninja 使用

不建议使用过旧版本的 Meson，否则可能在构建第三方组件时出现兼容问题

第三方组件说明

项目依赖的第三方库统一放置在 3rdparty/ 目录中，不污染项目根目录，包括但不限于：

WebRTC Audio Processing

WebRTC VAD

TenVAD

ONNX Runtime（用于 Silero VAD）

所有第三方组件均在各自目录内完成构建与安装，主程序仅通过头文件与链接库进行依赖，确保工程结构清晰、可维护。

构建原则

项目根目录仅包含业务代码与构建脚本

第三方源码与构建产物全部隔离在 3rdparty/ 下

构建过程可重复、可清理、可迁移

不依赖系统全局库路径，避免环境污染
