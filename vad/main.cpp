// 文件：example_create.cpp

#include <cstdio>
#include <cstdlib>

// 包含 WebRTC APM 头
#include "audio_processing.h"

// 包含 Standalone VAD 头（位于 install/include/vad/ 下面）
#include "webrtc_vad.h"

using namespace webrtc;

int main() {
    // ----------- 1. 创建并配置 AudioProcessing（APM） -----------
    // 注意：要链接时，已在编译命令里加了 -I./include/webrtc-audio-processing/api/audio 等
    rtc::scoped_refptr<AudioProcessing> apm = AudioProcessingBuilder().Create();
    if (!apm) {
        std::fprintf(stderr, "AudioProcessing 创建失败\n");
        return 1;
    }

    // 默认配置：开启 HPF、NS、AEC、AGC
    AudioProcessing::Config config;
    config.high_pass_filter.enabled      = true;
    config.noise_suppression.enabled     = true;
    config.noise_suppression.level       = AudioProcessing::Config::NoiseSuppression::kHigh;
    config.echo_canceller.enabled        = true;
    config.gain_controller1.enabled      = true;
    config.gain_controller1.mode         = AudioProcessing::Config::GainController1::kAdaptiveAnalog;
    apm->ApplyConfig(config);

    // 打印一句，表示 APM 已创建并配置
    std::printf("APM 初始化并配置完成。\n");

    // ----------- 2. 创建并初始化 Standalone VAD --------------
    // 头文件 webrtc_vad.h 中定义了 VadInst, Vad_Create, Vad_Init, Vad_set_mode 等
    VadInst* vad = Vad_Create();
    if (!vad) {
        std::fprintf(stderr, "VAD_Create 失败\n");
        return 1;
    }
    // 初始化 VAD
    if (Vad_Init(vad) != 0) {
        std::fprintf(stderr, "Vad_Init 失败\n");
        Vad_Free(vad);
        return 1;
    }
    // 设置 VAD 模式：0~3，数字越大对噪声越敏感，越容易判定为“有语音”
    if (Vad_set_mode(vad, 3) != 0) {
        std::fprintf(stderr, "Vad_set_mode 失败\n");
        Vad_Free(vad);
        return 1;
    }

    std::printf("VAD 初始化并设置模式完成。\n");

    // ----------- 3. 简单调用示例（仅演示 API 接口） --------------
    // 下面不是真正的音频流，这里演示“对一段全零 PCM”做一下 VAD 检测：
    const int kSampleRate    = 16000;
    const int kFrameMs       = 20;
    const int kFrameSize     = kSampleRate * kFrameMs / 1000; // 320 个 int16
    int16_t fake_pcm[kFrameSize];
    std::memset(fake_pcm, 0, sizeof(fake_pcm)); // 全零：相当于“静音/纯噪声”

    // 用 VAD 检测这 20ms PCM：应该返回 0（表示“无语音”）
    int vad_ret = Vad_Process(vad, kSampleRate, fake_pcm, kFrameSize);
    if (vad_ret < 0) {
        std::fprintf(stderr, "Vad_Process 出错: %d\n", vad_ret);
    } else if (vad_ret == 0) {
        std::printf("VAD 检测结果：无语音 (vad_ret=0)\n");
    } else {
        std::printf("VAD 检测结果：有语音 (vad_ret=1)\n");
    }

    // 用 AudioProcessing 做一次“空帧”处理，仅演示调用，不做输出：
    StreamConfig stream_conf(kSampleRate, 1); // 16kHz 单声道
    int16_t in_pcm[kFrameSize];
    int16_t out_pcm[kFrameSize];
    std::memset(in_pcm, 0, sizeof(in_pcm)); // 全零输入
    apm->ProcessStream(in_pcm, stream_conf, stream_conf, out_pcm);
    std::printf("APM ProcessStream() 调用完成 (输出仍是全零)。\n");

    // ----------- 4. 清理资源并退出 --------------
    Vad_Free(vad);
    // apm 是 scoped_refptr，会自动释放
    std::printf("示例运行完毕，资源已释放。\n");
    return 0;
}

