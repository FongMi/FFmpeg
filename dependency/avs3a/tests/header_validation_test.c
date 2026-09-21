#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "decoder.h"
#include "avs3_cnst_com.h"

static void put_bits(uint8_t *header, int *position, unsigned value, int count)
{
    for (int bit = count - 1; bit >= 0; bit--) {
        if ((value >> bit) & 1)
            header[*position / 8] |= 1 << (7 - *position % 8);
        (*position)++;
    }
}

static void make_header(uint8_t *header, unsigned profile, unsigned sampling_rate,
                        unsigned channel_config, unsigned object_count,
                        unsigned bitrate_index)
{
    int position = 0;
    memset(header, 0, MAX_NBYTES_FRAME_HEADER);
    put_bits(header, &position, SYNC_WORD_COMPAT, 12);
    put_bits(header, &position, 2, 4);
    put_bits(header, &position, 0, 1 + 3);
    put_bits(header, &position, profile, 3);
    put_bits(header, &position, sampling_rate, 4);
    put_bits(header, &position, 0, 8);
    if (profile == 1) {
        put_bits(header, &position, 1, 2); /* Sound bed with objects. */
        put_bits(header, &position, channel_config, 7);
        put_bits(header, &position, bitrate_index, 4);
        put_bits(header, &position, object_count - 1, 7);
        put_bits(header, &position, 0, 4); /* Per-object bitrate. */
    } else {
        put_bits(header, &position, channel_config, 7);
    }
    put_bits(header, &position, 1, 2); /* 16-bit resolution. */
    if (profile != 1)
        put_bits(header, &position, bitrate_index, 4);
    put_bits(header, &position, 0, 8);
}

static void assert_parsed_header(uint8_t *header, int expected)
{
    AVS3Decoder decoder = { 0 };
    int consumed;
    assert(parse_header(&decoder, header, MAX_NBYTES_FRAME_HEADER, 1,
                        &consumed, NULL) == expected);
}

static void assert_header(unsigned profile, unsigned sampling_rate,
                          unsigned channel_config, unsigned object_count,
                          unsigned bitrate_index, int expected)
{
    uint8_t header[MAX_NBYTES_FRAME_HEADER];
    make_header(header, profile, sampling_rate, channel_config, object_count,
                bitrate_index);
    assert_parsed_header(header, expected);
}

static void make_pure_object_header(uint8_t *header)
{
    int position = 0;
    memset(header, 0, MAX_NBYTES_FRAME_HEADER);
    put_bits(header, &position, SYNC_WORD_COMPAT, 12);
    put_bits(header, &position, 2, 4);
    put_bits(header, &position, 0, 1 + 3);
    put_bits(header, &position, 1, 3); /* Mixed-content profile. */
    put_bits(header, &position, 2, 4); /* 48 kHz. */
    put_bits(header, &position, 0, 8);
    put_bits(header, &position, 0, 2); /* No sound bed. */
    put_bits(header, &position, 0, 7); /* One object. */
    put_bits(header, &position, 0, 4); /* Per-object bitrate. */
    put_bits(header, &position, 1, 2); /* 16-bit resolution. */
    put_bits(header, &position, 0, 8);
}

int main(void)
{
    uint8_t header[MAX_NBYTES_FRAME_HEADER];
    assert_header(0, 2, CHANNEL_CONFIG_MONO, 0, 0, AVS3_TRUE);
    assert_header(0, AVS3_SIZE_FS_TABLE, CHANNEL_CONFIG_MONO, 0, 0, AVS3_FALSE);
    assert_header(0, 2, CHANNEL_CONFIG_MC_10_2, 0, 0, AVS3_FALSE);
    assert_header(0, 2, CHANNEL_CONFIG_MONO, 0, 15, AVS3_FALSE);
    assert_header(1, 2, CHANNEL_CONFIG_MC_5_1, 6, 0, AVS3_TRUE);
    assert_header(1, 2, CHANNEL_CONFIG_MC_7_1_4, 5, 0, AVS3_FALSE);
    assert_header(1, 2, CHANNEL_CONFIG_MC_5_1, 17, 0, AVS3_FALSE);
    assert_header(1, 2, 127, 1, 0, AVS3_FALSE);
    make_pure_object_header(header);
    assert_parsed_header(header, AVS3_UNSUPPORTED);
    make_header(header, 0, 2, CHANNEL_CONFIG_MONO, 0, 0);
    header[1] = (header[1] & 0xf0) | 1; /* Lossless audio codec ID. */
    assert_parsed_header(header, AVS3_UNSUPPORTED);
    make_header(header, 0, 2, CHANNEL_CONFIG_MONO, 0, 0);
    header[2] |= 0x20; /* Unsupported neural-network type. */
    assert_parsed_header(header, AVS3_FALSE);
    make_header(header, 0, 2, CHANNEL_CONFIG_MONO, 0, 0);
    header[5] |= 0x30; /* Unsupported bit depth. */
    assert_parsed_header(header, AVS3_FALSE);
    return 0;
}
