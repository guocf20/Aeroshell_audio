项目背景

## 项目背景

**Aeroshell Audio Gateway** 是 [Aeroshell](https://www.termdev.com) 智能终端生态下的高性能语音处理子项目。

作为 Aeroshell 跨平台能力在语音通信领域的延伸，本项目提供了一个 **面向极端复杂网络环境的高性能语音网关**。

在高丢包、高抖动、乱序、延迟不可控的网络条件下（如弱网、跨境网络、蜂窝网络、卫星链路、边缘设备等），传统实时语音系统往往会出现音频断裂、VAD 误判以及 STT（语音转文字）准确率下降等问题。

本网关针对上述痛点进行了系统级优化，是 Aeroshell 构建全平台、全场景智能连接能力的重要组成部分。

> **关于 Aeroshell**
> Aeroshell 是一款跨平台的智能终端软件，致力于提供极致的远程连接与 AI 集成体验。
> 官方网站：[www.termdev.com](https://www.termdev.com)

---

## 核心定位

该网关旨在解决以下技术挑战：
* **音频包重组**：在 UDP 等不可靠传输下，重组支离破碎的语音包。
* **质量恢复**：通过音频预处理（AEC / NS / AGC）在边缘端恢复可用语音质量。
* **稳定切分**：集成多种 VAD 实现，保证在噪声环境下的语音段切分稳定性。
* **精准对接**：为 STT 系统提供连续、纯净的语音流，大幅提升识别准确率。

## 适用场景
* **Aeroshell 生态集成**：作为 Aeroshell 移动端或桌面端的语音中继服务。
* **实时语音转文字（STT）系统**
* **语音交互设备（嵌入式 / IoT / 边缘计算）**
* **AI 语音代理与远程会议**


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

