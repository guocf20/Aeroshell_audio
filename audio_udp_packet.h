#ifndef AUDIO_UDP_PACKET_H
#define AUDIO_UDP_PACKET_H

#include <cstdint>
#include <sys/types.h>

struct AudioPacketView {
    uint32_t seq = 0;

    const uint8_t* near_payload = nullptr;
    uint16_t near_len = 0;

    const uint8_t* ref_payload = nullptr;
    uint16_t ref_len = 0;
};

bool parse_audio_packet(
    const uint8_t* buffer,
    ssize_t n,
    AudioPacketView& pkt
);

#endif