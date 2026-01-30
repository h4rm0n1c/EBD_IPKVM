#pragma once

#include <stdint.h>

#define STREAM_MAGIC0 0xEB
#define STREAM_MAGIC1 0xD1

#define STREAM_HEADER_BYTES 8
#define STREAM_FLAG_RLE 0x8000u
#define STREAM_LEN_MASK 0x7FFFu

typedef struct __attribute__((packed)) stream_packet_header {
    uint8_t magic[2];
    uint16_t frame_id_le;
    uint16_t line_id_le;
    uint16_t length_flags_le;
} stream_packet_header_t;

static inline void stream_write_header(uint8_t *dst,
                                       uint16_t frame_id,
                                       uint16_t line_id,
                                       uint16_t length_flags) {
    dst[0] = STREAM_MAGIC0;
    dst[1] = STREAM_MAGIC1;
    dst[2] = (uint8_t)(frame_id & 0xFFu);
    dst[3] = (uint8_t)((frame_id >> 8) & 0xFFu);
    dst[4] = (uint8_t)(line_id & 0xFFu);
    dst[5] = (uint8_t)((line_id >> 8) & 0xFFu);
    dst[6] = (uint8_t)(length_flags & 0xFFu);
    dst[7] = (uint8_t)((length_flags >> 8) & 0xFFu);
}
