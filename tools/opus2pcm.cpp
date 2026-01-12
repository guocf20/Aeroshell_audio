// decode_len_prefixed_opus.cpp
//
// 将一个“2B 大端长度前缀 + 裸 Opus 帧”格式的文件解码为 16kHz、16bit、单声道的原始 PCM。
// 编译：
//   g++ decode_len_prefixed_opus.cpp -o decode_len_prefixed_opus -lopus
// 使用：
//   ./decode_len_prefixed_opus input.opus output.pcm
//
// 依赖：需安装 libopus-dev（Debian/Ubuntu: sudo apt install libopus-dev）。

#include <opus/opus.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::fprintf(stderr, "用法: %s <输入 length+Opus 文件> <输出 raw PCM 文件>\n", argv[0]);
        return 1;
    }

    const char* input_opus = argv[1];
    const char* output_pcm = argv[2];

    // PCM 输出参数
    const opus_int32 SAMPLE_RATE = 16000;            // 采样率 16 kHz
    const int CHANNELS = 1;                          // 单声道
    const int FRAME_MS = 10;                         // 20 ms → 320 个样本
    const int SAMPLES_PER_FRAME = SAMPLE_RATE * FRAME_MS / 1000; // = 320

    // 打开输入文件
    FILE* opus_fp = std::fopen(input_opus, "rb");
    if (!opus_fp) {
        std::perror("打开输入 Opus 文件失败");
        return 1;
    }

    // 打开输出 PCM 文件
    FILE* pcm_fp = std::fopen(output_pcm, "wb");
    if (!pcm_fp) {
        std::perror("打开输出 PCM 文件失败");
        std::fclose(opus_fp);
        return 1;
    }

    // 创建 Opus 解码器
    int err = 0;
    OpusDecoder* decoder = opus_decoder_create(
        SAMPLE_RATE,
        CHANNELS,
        &err
    );
    if (err != OPUS_OK || !decoder) {
        std::fprintf(stderr, "创建 Opus 解码器失败: %s\n", opus_strerror(err));
        std::fclose(opus_fp);
        std::fclose(pcm_fp);
        return 1;
    }

    // 解码后 PCM 缓冲区
    std::vector<opus_int16> pcm_out(SAMPLES_PER_FRAME * CHANNELS);

    while (true) {
        // 读取 2 字节大端长度前缀
        uint8_t len_buf[2];
        size_t n = std::fread(len_buf, 1, 2, opus_fp);
        if (n == 0) {
            // 正常到文件末尾
            break;
        }
        if (n < 2) {
            std::fprintf(stderr, "读取长度前缀失败，文件可能损坏\n");
            break;
        }
        int packet_len = (len_buf[0] << 8) | len_buf[1];
        if (packet_len <= 0) {
            std::fprintf(stderr, "无效的 Opus 包长度: %d\n", packet_len);
            break;
        }

        // 分配缓冲区并读取真正的 Opus 数据
        std::vector<uint8_t> opus_packet(packet_len);
        n = std::fread(opus_packet.data(), 1, packet_len, opus_fp);
        if (static_cast<int>(n) < packet_len) {
            std::fprintf(stderr, "读取 Opus 数据失败: 期望 %d 字节, 实际 %zu 字节\n", packet_len, n);
            break;
        }

        // 解码
        int frame_size = opus_decode(
            decoder,
            opus_packet.data(),
            packet_len,
            pcm_out.data(),
            SAMPLES_PER_FRAME,
            0   // 不使用 FEC
        );
        if (frame_size < 0) {
            std::fprintf(stderr, "Opus 解码错误: %s\n", opus_strerror(frame_size));
            break;
        }
        if (frame_size != SAMPLES_PER_FRAME) {
            std::fprintf(stderr, "警告：解码出的样本数 %d ≠ 期望 %d\n", frame_size, SAMPLES_PER_FRAME);
        }

        // 写入 PCM（小端 16-bit）
        size_t written = std::fwrite(pcm_out.data(), sizeof(opus_int16), frame_size * CHANNELS, pcm_fp);
        if (written < static_cast<size_t>(frame_size * CHANNELS)) {
            std::perror("写入 PCM 文件失败");
            break;
        }
    }

    // 清理
    opus_decoder_destroy(decoder);
    std::fclose(opus_fp);
    std::fclose(pcm_fp);

    std::printf("解码完成 → 已生成 PCM 文件：%s\n", output_pcm);
    return 0;
}

