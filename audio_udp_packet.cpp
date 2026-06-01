#include "audio_udp_packet.h"

#include <spdlog/spdlog.h>

static constexpr uint32_t kAudioMagic = 0x41554450; // "AUDP"
static constexpr ssize_t kAudioBaseHeaderSize = 10;

// 读取网络字节序 uint16
static uint16_t read_u16_be(const uint8_t* p)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) |
         static_cast<uint16_t>(p[1])
    );
}

// 读取网络字节序 uint32
static uint32_t read_u32_be(const uint8_t* p)
{
    return
        (static_cast<uint32_t>(p[0]) << 24) |
        (static_cast<uint32_t>(p[1]) << 16) |
        (static_cast<uint32_t>(p[2]) << 8)  |
         static_cast<uint32_t>(p[3]);
}

/*
 * 包格式：
 *
 * [magic 4][seq 4][near_len 2][near_opus][ref_len 2][ref_opus]
 *
 * 注意：
 * 这个函数只解析 UDP buffer 里的位置。
 * 不拷贝 near_opus/ref_opus。
 * 不解码 Opus。
 */
bool parse_audio_packet(
    const uint8_t* buffer,
    ssize_t n,
    AudioPacketView& pkt
) {
    if (buffer == nullptr || n < kAudioBaseHeaderSize) {
        return false;
    }

    const uint32_t magic = read_u32_be(buffer);
    if (magic != kAudioMagic) {
        spdlog::warn("invalid audio packet magic: {}", magic);
        return false;
    }

    pkt.seq = read_u32_be(buffer + 4);
    pkt.near_len = read_u16_be(buffer + 8);

    ssize_t offset = kAudioBaseHeaderSize;

    if (
        pkt.near_len == 0 ||
        n < offset + pkt.near_len + 2
    ) {
        spdlog::warn(
            "invalid near_len={}, recv={}",
            pkt.near_len,
            n
        );
        return false;
    }

    // near_opus 起始位置
    pkt.near_payload = buffer + offset;
    offset += pkt.near_len;

    // ref_len 位于 near_opus 后面 2 字节
    pkt.ref_len = read_u16_be(buffer + offset);
    offset += 2;


    if (n < offset + pkt.ref_len) {
        spdlog::warn(
            "invalid ref_len={}, recv={}",
            pkt.ref_len,
            n
        );
        return false;
    }
    
    // ref_len 为 0 时，表示没有参考音频
    pkt.ref_payload = pkt.ref_len > 0 ? buffer + offset : nullptr;

    return true;
}