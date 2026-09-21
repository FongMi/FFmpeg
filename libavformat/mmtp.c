/*
 * MPEG Media Transport Protocol (MMTP) parser, as defined in ISO/IEC 23008-1.
 * Copyright (c) 2025 SuperFashi G.K.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdbool.h>

#include "libavcodec/bytestream.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/packet_internal.h"
#include "libavutil/avassert.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "demux.h"
#include "internal.h"
#include "mmtp.h"
#include "packet_internal.h"

#define MAX_MMT_STREAMS 64
#define MAX_FRAGMENT_ASSEMBLERS 128
#define MAX_FRAGMENT_SIZE (16U * 1024U * 1024U)
#define MAX_FRAGMENT_CAPACITY (32U * 1024U * 1024U)
#define MAX_QUEUED_MFU_PACKETS (UINT16_MAX / (2 + 4) + 1)
#define MAX_QUEUED_PACKET_DATA MAX_FRAGMENT_SIZE

struct MMTPContext;

static int parse_mmt_general_location_info(GetByteContext *gbc,
                                           int *packet_id)
{
    unsigned int payload_size;
    uint8_t location_type;
    bool has_packet_id = false;

    if (packet_id)
        *packet_id = -1;

    if (bytestream2_get_bytes_left(gbc) < 1)
        return AVERROR_INVALIDDATA;
    location_type = bytestream2_get_byteu(gbc);
    switch (location_type) {
    case 0x00:
        if (bytestream2_get_bytes_left(gbc) < 2)
            return AVERROR_INVALIDDATA;
        if (packet_id)
            *packet_id = bytestream2_get_be16u(gbc);
        else
            bytestream2_skipu(gbc, 2);
        return 0;
    case 0x01:
        payload_size = (32 + 32 + 16 + 16) / 8;
        has_packet_id = true;
        break;
    case 0x02:
        payload_size = (128 + 128 + 16 + 16) / 8;
        has_packet_id = true;
        break;
    case 0x03:
        payload_size = (16 + 16 + 3 + 13) / 8;
        break;
    case 0x04:
        payload_size = (128 + 128 + 16 + 3 + 13) / 8;
        break;
    case 0x05:
        if (bytestream2_get_bytes_left(gbc) < 1)
            return AVERROR_INVALIDDATA;
        payload_size = bytestream2_get_byteu(gbc);
        break;
    default:
        return AVERROR_INVALIDDATA;
    }

    if ((unsigned int) bytestream2_get_bytes_left(gbc) < payload_size)
        return AVERROR_INVALIDDATA;
    if (has_packet_id) {
        bytestream2_skipu(gbc, payload_size - 2);
        if (packet_id)
            *packet_id = bytestream2_get_be16u(gbc);
        else
            bytestream2_skipu(gbc, 2);
        return 0;
    }
    bytestream2_skipu(gbc, payload_size);
    return 0;
}

struct Streams {
    AVStream *stream;
    AVStream *subtitle_resource_stream;
    struct MMTPContext *owner;

    AVCodecParserContext *parser;

    int num_timestamp_descriptors;
    struct MPUTimestampDescriptor {
        uint32_t seq_num;
        uint64_t presentation_time;
    }   *timestamp_descriptor;

    int num_ext_timestamp_descriptors;
    struct MPUExtendedTimestampDescriptor {
        uint32_t seq_num;
        uint16_t decoding_time_offset;
        uint8_t  num_of_au;
        uint8_t  pts_offset_type;
        struct {
            uint16_t dts_pts_offset;
            uint16_t pts_offset;
        }        au[0x100];
    }   *ext_timestamp_descriptor;

    uint32_t last_sequence_number;
    bool     has_last_sequence_number;
    uint16_t au_count;
    int64_t  offset;
    int      flags;

    uint8_t  subtitle_tmd;
    uint64_t subtitle_reference_time;
    bool     subtitle_has_reference_time;
    bool     audio_specific_config_descriptor_seen;

    // VPS/SPS/PPS captured from the MFUs (raw NALs), used to build extradata;
    // the parameter sets are otherwise in-band only. [0]=VPS [1]=SPS [2]=PPS.
    struct {
        uint8_t *data;
        int      size;
    } hevc_ps[3];

    struct Streams *next;
};

enum {
    HEVC_NAL_VPS = 32,
    HEVC_NAL_SPS = 33,
    HEVC_NAL_PPS = 34,
};

struct MMTPContext {
    struct FragmentAssembler *assembler;
    struct Streams           *streams;
    AVProgram                *program;
    AVStream                 *unknown_payload_stream;
    unsigned int             nb_assemblers;
    unsigned int             nb_streams;
    size_t                   fragment_capacity;
    PacketList               packet_queue;
    unsigned int             queued_packets;
    size_t                   queued_packet_data;
    uint64_t                 ntp_anchor;

    // below are temporary fields available for the scope of a single packet
    AVFormatContext *s;
    AVPacket        *pkt;
    int64_t         current_packet_pos;
    uint16_t        current_pid;
    uint32_t        current_timestamp;
    uint32_t        current_item_id;
    uint32_t        current_movie_fragment_sequence_number;
    uint32_t        current_sample_number;
    uint32_t        current_offset;
    uint8_t         current_priority;
    uint8_t         current_dependency_counter;
    bool            current_mfu_timed;
    bool            allow_packet_queue;
    uint8_t         is_rap;
};

static struct Streams *find_current_stream(struct MMTPContext *ctx)
{
    struct Streams *streams;
    for (streams = ctx->streams; streams != NULL; streams = streams->next)
        if (streams->stream->id == ctx->current_pid)
            return streams;
    return NULL;
}

static int find_or_allocate_stream(struct MMTPContext *ctx, uint16_t pid,
                                   struct Streams **result)
{
    AVStream       *stream;
    struct Streams *streams;
    for (streams = ctx->streams; streams != NULL; streams = streams->next) {
        if (streams->stream->id == pid) {
            *result = streams;
            return 0;
        }
    }

    if (ctx->nb_streams >= MAX_MMT_STREAMS)
        return AVERROR_INVALIDDATA;

    streams = av_mallocz(sizeof(*streams));
    if (streams == NULL)
        return AVERROR(ENOMEM);

    stream = avformat_new_stream(ctx->s, NULL);
    if (stream == NULL) {
        av_free(streams);
        return AVERROR(ENOMEM);
    }
    stream->id = pid;
    av_program_add_stream_index(ctx->s, ctx->program->id, stream->index);

    streams->stream = stream;
    streams->owner  = ctx;
    streams->next   = ctx->streams;
    streams->offset = -1;
    ctx->streams    = streams;
    ctx->nb_streams++;
    *result = streams;
    return 0;
}

enum {
    MMT_PACKAGE_TABLE_ID  = 0x20,
    PACKAGE_LIST_TABLE_ID = 0x80,
    MH_EIT_TABLE_ID       = 0x8B,
};

enum {
    MPU_TIMESTAMP_DESCRIPTOR          = 0x0001,
    MH_MPEG4_AUDIO_EXT_DESCRIPTOR     = 0x8009,
    VIDEO_COMPONENT_DESCRIPTOR        = 0x8010,
    MH_STREAM_IDENTIFIER_DESCRIPTOR   = 0x8011,
    MH_AUDIO_COMPONENT_DESCRIPTOR     = 0x8014,
    MH_DATA_COMPONENT_DESCRIPTOR      = 0x8020,
    MPU_EXTENDED_TIMESTAMP_DESCRIPTOR = 0x8026,
    MH_SHORT_EVENT_DESCRIPTOR         = 0xF001,
};

enum {
    AUDIO_STREAM_CONTENT_AAC = 0x03,
    AUDIO_STREAM_CONTENT_ALS = 0x04,
    AUDIO_STREAM_TYPE_LATM   = 0x11,
    AUDIO_STREAM_TYPE_RAW    = 0x1C,
};

static AVRational video_frame_rate(uint8_t code)
{
    static const AVRational frame_rates[] = {
        { 0, 1 },
        { 15, 1 },
        { 24000, 1001 },
        { 24, 1 },
        { 25, 1 },
        { 30000, 1001 },
        { 30, 1 },
        { 50, 1 },
        { 60000, 1001 },
        { 60, 1 },
        { 100, 1 },
        { 120000, 1001 },
        { 120, 1 },
    };

    return code < FF_ARRAY_ELEMS(frame_rates)
               ? frame_rates[code] : (AVRational) { 0, 1 };
}

static int
parse_video_component_descriptor(struct Streams *streams,
                                 GetByteContext *gbc)
{
    AVStream *stream = streams ? streams->stream : NULL;
    AVRational frame_rate;
    uint8_t descriptor_length;
    uint8_t frame_rate_code;
    uint8_t language_code[4];

    if (bytestream2_get_bytes_left(gbc) < (16 + 8) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_be16u(gbc) != VIDEO_COMPONENT_DESCRIPTOR)
        return AVERROR_INVALIDDATA;
    descriptor_length = bytestream2_get_byteu(gbc);

    if ((unsigned int) bytestream2_get_bytes_left(gbc) < descriptor_length)
        return AVERROR_INVALIDDATA;
    {
        GetByteContext ngbc;
        bytestream2_init(&ngbc, gbc->buffer, descriptor_length);
        if (bytestream2_get_bytes_left(&ngbc) < 8)
            return AVERROR_INVALIDDATA;

        bytestream2_skipu(&ngbc, 1);
        frame_rate_code = bytestream2_get_byteu(&ngbc) & 0x1f;
        /*
         * skip:
         * - component_tag
         * - video_transfer_characteristics
         * - reserved
         */
        bytestream2_skipu(&ngbc, (16 + 4 + 4) / 8);
        bytestream2_get_bufferu(&ngbc, language_code, 3);
        language_code[3] = '\0';
    }
    bytestream2_skipu(gbc, descriptor_length);

    if (stream == NULL)
        return 0;
    frame_rate = video_frame_rate(frame_rate_code);
    stream->avg_frame_rate = frame_rate;
    return av_dict_set(&stream->metadata, "language",
                       (const char *) language_code, 0);
}

static int audio_sample_rate(uint8_t code)
{
    static const int sample_rates[] = {
        0, 16000, 22050, 24000, 0, 32000, 44100, 48000,
    };

    return sample_rates[code & 0x07];
}

static int
parse_mh_audio_component_descriptor(struct Streams *streams,
                                    GetByteContext *gbc)
{
    AVStream *stream = streams ? streams->stream : NULL;
    enum AVCodecID previous_codec_id;
    int previous_sample_rate;
    int ret;
    uint8_t descriptor_length;
    uint8_t sample_rate_code;
    uint8_t stream_content;
    uint8_t stream_type;
    uint8_t language_code[4];

    if (bytestream2_get_bytes_left(gbc) < (16 + 8) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_be16u(gbc) != MH_AUDIO_COMPONENT_DESCRIPTOR)
        return AVERROR_INVALIDDATA;
    descriptor_length = bytestream2_get_byteu(gbc);

    if (bytestream2_get_bytes_left(gbc) < descriptor_length)
        return AVERROR_INVALIDDATA;
    {
        uint8_t byte;
        bool ES_multi_lingual_flag;

        GetByteContext ngbc;
        bytestream2_init(&ngbc, gbc->buffer, descriptor_length);

        if (bytestream2_get_bytes_left(&ngbc) <
            (4 + 4 + 8 + 16 + 8 + 8 + 1 + 1 + 2 + 3 + 1 + 24) / 8)
            return AVERROR_INVALIDDATA;

        byte           = bytestream2_get_byteu(&ngbc);
        stream_content = byte & 0x0f;

        /*
         * skip:
         * - component_type
         * - component_tag
         */
        bytestream2_skipu(&ngbc, 3);
        stream_type = bytestream2_get_byteu(&ngbc);

        // skip: simulcast_group_tag
        bytestream2_skipu(&ngbc, 1);

        byte                  = bytestream2_get_byteu(&ngbc);
        ES_multi_lingual_flag = byte >> 7;
        sample_rate_code      = byte >> 1 & 0x07;

        bytestream2_get_bufferu(&ngbc, language_code, 3);
        language_code[3] = '\0';

        if (ES_multi_lingual_flag) {
            if (bytestream2_get_bytes_left(&ngbc) < 3)
                return AVERROR_INVALIDDATA;
            bytestream2_skipu(&ngbc, 3);
        }
    }
    bytestream2_skipu(gbc, descriptor_length);

    if (stream == NULL)
        return 0;

    previous_codec_id                 = stream->codecpar->codec_id;
    previous_sample_rate              = stream->codecpar->sample_rate;
    stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    stream->codecpar->codec_id   = AV_CODEC_ID_BIN_DATA;
    stream->discard              = AVDISCARD_DEFAULT;
    avpriv_set_pts_info(stream, 64, 1, 1000);
    if (stream_content == AUDIO_STREAM_CONTENT_AAC &&
        stream_type == AUDIO_STREAM_TYPE_LATM) {
        stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        stream->codecpar->codec_id   = AV_CODEC_ID_AAC_LATM;
    } else if (stream_content == AUDIO_STREAM_CONTENT_AAC &&
        stream_type == AUDIO_STREAM_TYPE_RAW) {
        stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        stream->codecpar->codec_id   = AV_CODEC_ID_AAC;
    } else if (stream_content == AUDIO_STREAM_CONTENT_ALS &&
        stream_type == AUDIO_STREAM_TYPE_RAW) {
        stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        stream->codecpar->codec_id   = AV_CODEC_ID_MP4ALS;
    }
    stream->codecpar->sample_rate = audio_sample_rate(sample_rate_code);
    if (!streams->audio_specific_config_descriptor_seen &&
        (stream->codecpar->codec_id != previous_codec_id ||
         stream->codecpar->sample_rate != previous_sample_rate)) {
        av_freep(&stream->codecpar->extradata);
        stream->codecpar->extradata_size = 0;
    }

    ret = av_dict_set_int(&stream->metadata, "mmt.audio.stream_content",
                          stream_content, 0);
    if (ret >= 0)
        ret = av_dict_set_int(&stream->metadata, "mmt.audio.stream_type",
                              stream_type, 0);
    if (ret >= 0)
        ret = av_dict_set(&stream->metadata, "language",
                          (const char *) language_code, 0);
    return ret;
}

static int
parse_mh_mpeg4_audio_extension_descriptor(struct Streams *streams,
                                          GetByteContext *gbc)
{
    AVStream *stream = streams ? streams->stream : NULL;
    const uint8_t *audio_specific_config = NULL;
    int ret;
    uint8_t audio_specific_config_size = 0;
    uint8_t descriptor_length;
    bool audio_specific_config_present;

    if (bytestream2_get_bytes_left(gbc) < (16 + 8) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_be16u(gbc) != MH_MPEG4_AUDIO_EXT_DESCRIPTOR)
        return AVERROR_INVALIDDATA;
    descriptor_length = bytestream2_get_byteu(gbc);
    if ((unsigned int) bytestream2_get_bytes_left(gbc) < descriptor_length)
        return AVERROR_INVALIDDATA;
    {
        GetByteContext ngbc;
        uint8_t flags;
        uint8_t profile_level_count;

        bytestream2_init(&ngbc, gbc->buffer, descriptor_length);
        if (bytestream2_get_bytes_left(&ngbc) < 1)
            return AVERROR_INVALIDDATA;
        flags = bytestream2_get_byteu(&ngbc);
        audio_specific_config_present = flags >> 7;
        profile_level_count = flags & 0x0f;
        if (bytestream2_get_bytes_left(&ngbc) < profile_level_count)
            return AVERROR_INVALIDDATA;
        bytestream2_skipu(&ngbc, profile_level_count);
        if (audio_specific_config_present) {
            if (bytestream2_get_bytes_left(&ngbc) < 1)
                return AVERROR_INVALIDDATA;
            audio_specific_config_size = bytestream2_get_byteu(&ngbc);
            if (bytestream2_get_bytes_left(&ngbc) <
                audio_specific_config_size)
                return AVERROR_INVALIDDATA;
            audio_specific_config = ngbc.buffer;
        }
    }
    bytestream2_skipu(gbc, descriptor_length);

    if (stream == NULL)
        return 0;
    streams->audio_specific_config_descriptor_seen = true;
    if (!audio_specific_config_present || !audio_specific_config_size) {
        av_freep(&stream->codecpar->extradata);
        stream->codecpar->extradata_size = 0;
        return 0;
    }
    if ((ret = ff_alloc_extradata(stream->codecpar,
                                  audio_specific_config_size)) < 0)
        return ret;
    memcpy(stream->codecpar->extradata, audio_specific_config,
           audio_specific_config_size);
    return 0;
}

#define MAX_NUM_TIMESTAMP_DESCRIPTOR 32

static int sequence_is_before(uint32_t value, uint32_t reference)
{
    return av_compare_mod(value, reference, UINT64_C(1) << 32) < 0;
}

static uint32_t sequence_distance(uint32_t first, uint32_t second)
{
    return FFMIN(first - second, second - first);
}

static int get_timestamp_descriptor(struct Streams *streams, uint32_t seq_num,
                                    struct MPUTimestampDescriptor **result)
{
    struct MPUTimestampDescriptor *desc = NULL;
    uint32_t max_distance = 0;
    int i;

    *result = NULL;
    if (streams->has_last_sequence_number &&
        sequence_is_before(seq_num, streams->last_sequence_number))
        return 0;

    for (i = 0; i < streams->num_timestamp_descriptors; i++) {
        desc = &streams->timestamp_descriptor[i];
        if (desc->seq_num == seq_num) {
            *result = desc;
            return 0;
        }
    }

    desc = NULL;
    if (streams->has_last_sequence_number) {
        for (i = 0; i < streams->num_timestamp_descriptors; i++) {
            desc = &streams->timestamp_descriptor[i];
            if (sequence_is_before(desc->seq_num,
                                   streams->last_sequence_number))
                break;
            desc = NULL;
        }
    }

    if (!desc &&
        streams->num_timestamp_descriptors >= MAX_NUM_TIMESTAMP_DESCRIPTOR) {
        for (i = 0; i < streams->num_timestamp_descriptors; i++) {
            uint32_t distance = sequence_distance(
                streams->timestamp_descriptor[i].seq_num, seq_num);

            if (distance > max_distance) {
                desc = &streams->timestamp_descriptor[i];
                max_distance = distance;
            }
        }
        av_assert1(desc != NULL);
    } else if (!desc) {
        desc = av_dynarray2_add((void **) &streams->timestamp_descriptor,
                                &streams->num_timestamp_descriptors,
                                sizeof(*desc), NULL);
        if (desc == NULL)
            return AVERROR(ENOMEM);
    }

    desc->seq_num = seq_num;
    *result = desc;
    return 0;
}

static int get_extended_timestamp_descriptor(
    struct Streams *streams, uint32_t seq_num,
    struct MPUExtendedTimestampDescriptor **result)
{
    struct MPUExtendedTimestampDescriptor *desc = NULL;
    uint32_t max_distance = 0;
    int i;

    *result = NULL;
    if (streams->has_last_sequence_number &&
        sequence_is_before(seq_num, streams->last_sequence_number))
        return 0;

    for (i = 0; i < streams->num_ext_timestamp_descriptors; i++) {
        desc = &streams->ext_timestamp_descriptor[i];
        if (desc->seq_num == seq_num) {
            *result = desc;
            return 0;
        }
    }

    desc = NULL;
    if (streams->has_last_sequence_number) {
        for (i = 0; i < streams->num_ext_timestamp_descriptors; i++) {
            desc = &streams->ext_timestamp_descriptor[i];
            if (sequence_is_before(desc->seq_num,
                                   streams->last_sequence_number))
                break;
            desc = NULL;
        }
    }

    if (!desc && streams->num_ext_timestamp_descriptors >=
        MAX_NUM_TIMESTAMP_DESCRIPTOR) {
        for (i = 0; i < streams->num_ext_timestamp_descriptors; i++) {
            uint32_t distance = sequence_distance(
                streams->ext_timestamp_descriptor[i].seq_num, seq_num);

            if (distance > max_distance) {
                desc = &streams->ext_timestamp_descriptor[i];
                max_distance = distance;
            }
        }
        av_assert1(desc != NULL);
    } else if (!desc) {
        desc = av_dynarray2_add(
            (void **) &streams->ext_timestamp_descriptor,
            &streams->num_ext_timestamp_descriptors,
            sizeof(*desc), NULL);
        if (desc == NULL)
            return AVERROR(ENOMEM);
    }

    desc->seq_num = seq_num;
    *result = desc;
    return 0;
}

static int
parse_mpu_timestamp_descriptor(struct Streams *streams, GetByteContext *gbc)
{
    uint8_t descriptor_length;

    if (bytestream2_get_bytes_left(gbc) < (16 + 8) / 8)
        return AVERROR_INVALIDDATA;

    if (bytestream2_get_be16u(gbc) != MPU_TIMESTAMP_DESCRIPTOR)
        return AVERROR_INVALIDDATA;
    descriptor_length = bytestream2_get_byteu(gbc);

    if (bytestream2_get_bytes_left(gbc) < descriptor_length)
        return AVERROR_INVALIDDATA;
    {
        GetByteContext ngbc;
        bytestream2_init(&ngbc, gbc->buffer, descriptor_length);

        while (bytestream2_get_bytes_left(&ngbc) > 0) {
            uint32_t mpu_seq_num;
            uint64_t mpu_presentation_time;
            struct MPUTimestampDescriptor *desc = NULL;
            int ret;

            if (bytestream2_get_bytes_left(&ngbc) < (32 + 64) / 8)
                return AVERROR_INVALIDDATA;
            mpu_seq_num = bytestream2_get_be32u(&ngbc);
            mpu_presentation_time = bytestream2_get_be64u(&ngbc);
            streams->owner->ntp_anchor = mpu_presentation_time;

            ret = get_timestamp_descriptor(streams, mpu_seq_num, &desc);
            if (ret < 0)
                return ret;
            if (desc)
                desc->presentation_time = mpu_presentation_time;
        }
    }
    bytestream2_skipu(gbc, descriptor_length);

    return 0;
}

static int parse_mpu_extended_timestamp_descriptor(
    struct Streams *streams, GetByteContext *gbc)
{
    uint8_t descriptor_length;

    AVStream *stream = streams->stream;

    if (bytestream2_get_bytes_left(gbc) < (16 + 8) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_be16u(gbc) != MPU_EXTENDED_TIMESTAMP_DESCRIPTOR)
        return AVERROR_INVALIDDATA;
    descriptor_length = bytestream2_get_byteu(gbc);

    if (bytestream2_get_bytes_left(gbc) < descriptor_length)
        return AVERROR_INVALIDDATA;
    {
        uint8_t  byte;
        uint8_t  pts_offset_type;
        bool timescale_flag;
        uint16_t default_pts_offset = 0;

        GetByteContext ngbc;
        bytestream2_init(&ngbc, gbc->buffer, descriptor_length);

        if (bytestream2_get_bytes_left(&ngbc) < (5 + 2 + 1) / 8)
            return AVERROR_INVALIDDATA;
        byte            = bytestream2_get_byte(&ngbc);
        pts_offset_type = (byte >> 1) & 0x03;
        timescale_flag  = byte & 1;

        if (timescale_flag) {
            uint32_t timescale;

            if (bytestream2_get_bytes_left(&ngbc) < 4)
                return AVERROR_INVALIDDATA;
            timescale = bytestream2_get_be32u(&ngbc);
            if (!timescale || timescale > INT_MAX)
                return AVERROR_INVALIDDATA;
            stream->time_base.num = 1;
            stream->time_base.den = timescale;
        }

        if (pts_offset_type == 3)
            return AVERROR_INVALIDDATA;
        if (pts_offset_type == 1) {
            if (bytestream2_get_bytes_left(&ngbc) < 2)
                return AVERROR_INVALIDDATA;
            default_pts_offset = bytestream2_get_be16u(&ngbc);
        }

        while (bytestream2_get_bytes_left(&ngbc) > 0) {
            int      i;
            int      ret;
            uint8_t  num_of_au;
            uint16_t decoding_time_offset;
            uint32_t mpu_seq_num;

            struct MPUExtendedTimestampDescriptor *desc = NULL;

            if (bytestream2_get_bytes_left(&ngbc) < (32 + 2 + 6 + 16 + 8) / 8)
                return AVERROR_INVALIDDATA;
            mpu_seq_num = bytestream2_get_be32u(&ngbc);
            // skip: leap_indicator
            bytestream2_skip(&ngbc, (2 + 6) / 8);
            decoding_time_offset = bytestream2_get_be16u(&ngbc);
            num_of_au            = bytestream2_get_byteu(&ngbc);

            ret = get_extended_timestamp_descriptor(streams, mpu_seq_num,
                                                     &desc);
            if (ret < 0)
                return ret;
            if (desc) {
                desc->decoding_time_offset = decoding_time_offset;
                desc->num_of_au            = num_of_au;
                desc->pts_offset_type       = pts_offset_type;
            }

            for (i = 0; i < num_of_au; ++i) {
                if (bytestream2_get_bytes_left(&ngbc) < 2)
                    return AVERROR_INVALIDDATA;
                if (desc != NULL)
                    desc->au[i].dts_pts_offset = bytestream2_get_be16u(&ngbc);
                else
                    bytestream2_skipu(&ngbc, 2);

                if (pts_offset_type == 2) {
                    if (bytestream2_get_bytes_left(&ngbc) < 2)
                        return AVERROR_INVALIDDATA;
                    if (desc != NULL)
                        desc->au[i].pts_offset = bytestream2_get_be16u(&ngbc);
                    else
                        bytestream2_skipu(&ngbc, 2);
                } else if (desc != NULL) {
                    desc->au[i].pts_offset = default_pts_offset;
                }
            }
        }
    }
    bytestream2_skipu(gbc, descriptor_length);

    return 0;
}

static int parse_additional_arib_subtitle_info(struct Streams *streams,
                                               GetByteContext *gbc)
{
    AVStream *stream = streams->stream;
    int ret;
    bool    start_mpu_sequence_number_flag;
    uint8_t language_code[4];
    uint8_t subtitle_type;
    uint8_t subtitle_format;
    uint8_t subtitle_resolution;
    uint8_t subtitle_compression;
    uint8_t byte;

    if (bytestream2_get_bytes_left(gbc) <
        (8 + 4 + 1 + 3 + 24 + 2 + 4 + 2 + 4 + 4 + 4 + 4) / 8)
        return AVERROR_INVALIDDATA;
    // skip: subtitle_tag
    bytestream2_skipu(gbc, 1);
    byte = bytestream2_get_byteu(gbc);
    start_mpu_sequence_number_flag = (byte >> 3) & 1;
    bytestream2_get_bufferu(gbc, language_code, 3);
    language_code[3] = '\0';
    byte = bytestream2_get_byteu(gbc);
    subtitle_type = byte >> 6;
    subtitle_format = (byte >> 2) & 0x0f;
    byte = bytestream2_get_byteu(gbc);
    streams->subtitle_tmd = byte >> 4;
    // skip: DMF
    byte = bytestream2_get_byteu(gbc);
    subtitle_resolution = byte >> 4;
    subtitle_compression = byte & 0x0f;
    streams->subtitle_has_reference_time = false;

    if (start_mpu_sequence_number_flag) {
        if (bytestream2_get_bytes_left(gbc) < 4)
            return AVERROR_INVALIDDATA;
        bytestream2_skipu(gbc, 4);
    }

    if (streams->subtitle_tmd == 2) {
        if (bytestream2_get_bytes_left(gbc) < 9)
            return AVERROR_INVALIDDATA;
        streams->subtitle_reference_time = bytestream2_get_be64u(gbc);
        // skip: leap_indicator and reserved
        bytestream2_skipu(gbc, 1);
        streams->subtitle_has_reference_time = true;
        streams->owner->ntp_anchor = streams->subtitle_reference_time;
    }

    stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    stream->codecpar->codec_id   = AV_CODEC_ID_BIN_DATA;
    stream->codecpar->width      = 0;
    stream->codecpar->height     = 0;
    stream->discard              = AVDISCARD_DEFAULT;
    stream->disposition         &= ~AV_DISPOSITION_CAPTIONS;
    avpriv_set_pts_info(stream, 64, 1, 1000);
    ret = av_dict_set(&stream->metadata, "title", NULL, 0);
    if (ret < 0)
        return ret;

    if (subtitle_format == 0 && !subtitle_compression &&
        subtitle_resolution <= 2) {
        stream->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        stream->codecpar->codec_id   = AV_CODEC_ID_TTML;
        stream->codecpar->width      = 1920 << subtitle_resolution;
        stream->codecpar->height     = 1080 << subtitle_resolution;
        stream->disposition         |= AV_DISPOSITION_CAPTIONS;
        ret = av_dict_set(&stream->metadata, "title",
                          subtitle_type == 1 ? "Superimpose" : "Caption",
                          0);
        if (ret < 0)
            return ret;
    }

    return av_dict_set(&stream->metadata, "language",
                       (const char *) language_code, 0);
}

static int parse_mh_data_component_descriptor(struct Streams *streams,
                                              GetByteContext *gbc)
{
    uint8_t descriptor_length;

    if (bytestream2_get_bytes_left(gbc) < (16 + 8) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_be16u(gbc) != MH_DATA_COMPONENT_DESCRIPTOR)
        return AVERROR_INVALIDDATA;
    descriptor_length = bytestream2_get_byteu(gbc);

    if (bytestream2_get_bytes_left(gbc) < descriptor_length)
        return AVERROR_INVALIDDATA;
    {
        GetByteContext ngbc;
        bytestream2_init(&ngbc, gbc->buffer, descriptor_length);
        bytestream2_skipu(gbc, descriptor_length);

        if (bytestream2_get_bytes_left(&ngbc) < 16 / 8)
            return AVERROR_INVALIDDATA;
        switch (bytestream2_get_be16u(&ngbc)) {
        case 0x0020: // additional ARIB subtitle info (Table 7-74, ARIB STD-B60, Version 1.14-E1)
            return parse_additional_arib_subtitle_info(streams, &ngbc);
        }
    }

    return 0;
}

static int parse_stream_identifier_descriptor(GetByteContext *gbc)
{
    uint8_t descriptor_length;

    if (bytestream2_get_bytes_left(gbc) < (16 + 8) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_be16u(gbc) != MH_STREAM_IDENTIFIER_DESCRIPTOR)
        return AVERROR_INVALIDDATA;
    descriptor_length = bytestream2_get_byteu(gbc);

    if ((unsigned int) bytestream2_get_bytes_left(gbc) < descriptor_length)
        return AVERROR_INVALIDDATA;
    {
        // no need for now
    }
    bytestream2_skipu(gbc, descriptor_length);

    return 0;
}

static int skip_unknown_descriptor(void *log, GetByteContext *gbc)
{
    // assumes at least 3 bytes left to read in gbc
    const uint16_t descriptor_tag = bytestream2_get_be16u(gbc);
    unsigned int   descriptor_length;

    av_log(log, AV_LOG_VERBOSE, "Unknown descriptor: 0x%04x\n", descriptor_tag);

    if (descriptor_tag <= 0x3FFF) {        // 8-bit length descriptor
        descriptor_length = bytestream2_get_byteu(gbc);
    } else if (descriptor_tag <= 0x6FFF) { // 16-bit length descriptor
        if (bytestream2_get_bytes_left(gbc) < 2)
            return AVERROR_INVALIDDATA;
        descriptor_length = bytestream2_get_be16u(gbc);
    } else if (descriptor_tag <= 0x7FFF) { // 32-bit length descriptor
        if (bytestream2_get_bytes_left(gbc) < 4)
            return AVERROR_INVALIDDATA;
        descriptor_length = bytestream2_get_be32u(gbc);
    } else if (descriptor_tag <= 0xEFFF) { // 8-bit length descriptor
        descriptor_length = bytestream2_get_byteu(gbc);
    } else {                               // 16-bit length descriptor
        if (bytestream2_get_bytes_left(gbc) < 2)
            return AVERROR_INVALIDDATA;
        descriptor_length = bytestream2_get_be16u(gbc);
    }

    if ((unsigned int) bytestream2_get_bytes_left(gbc) < descriptor_length)
        return AVERROR_INVALIDDATA;
    bytestream2_skipu(gbc, descriptor_length);
    return 0;
}

static int parse_mpt_descriptor(struct Streams *streams, GetByteContext *gbc)
{
    if (bytestream2_get_bytes_left(gbc) < 3)
        return AVERROR_INVALIDDATA;
    switch (bytestream2_peek_be16u(gbc)) {
    case MPU_TIMESTAMP_DESCRIPTOR:
        return parse_mpu_timestamp_descriptor(streams, gbc);
    case MH_MPEG4_AUDIO_EXT_DESCRIPTOR:
        return parse_mh_mpeg4_audio_extension_descriptor(streams, gbc);
    case VIDEO_COMPONENT_DESCRIPTOR:
        return parse_video_component_descriptor(streams, gbc);
    case MH_STREAM_IDENTIFIER_DESCRIPTOR:
        return parse_stream_identifier_descriptor(gbc);
    case MH_AUDIO_COMPONENT_DESCRIPTOR:
        return parse_mh_audio_component_descriptor(streams, gbc);
    case MH_DATA_COMPONENT_DESCRIPTOR:
        return parse_mh_data_component_descriptor(streams, gbc);
    case MPU_EXTENDED_TIMESTAMP_DESCRIPTOR:
        return parse_mpu_extended_timestamp_descriptor(streams, gbc);
    default:
        return skip_unknown_descriptor(streams->stream, gbc);
    }
}

static int parse_mh_short_event_descriptor(
    MMTPContext *ctx, GetByteContext *gbc)
{
    int ret;
    uint16_t descriptor_length;

    if (bytestream2_get_bytes_left(gbc) < (16 + 16) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_be16u(gbc) != MH_SHORT_EVENT_DESCRIPTOR)
        return AVERROR_INVALIDDATA;
    descriptor_length = bytestream2_get_be16u(gbc);

    if (bytestream2_get_bytes_left(gbc) < descriptor_length)
        return AVERROR_INVALIDDATA;
    {
        uint8_t  language_code[4];
        uint8_t  event_name_length;
        uint16_t text_length;
        char     *event_name, *text;

        GetByteContext ngbc;
        bytestream2_init(&ngbc, gbc->buffer, descriptor_length);

        if (bytestream2_get_bytes_left(&ngbc) < 4)
            return AVERROR_INVALIDDATA;
        bytestream2_get_bufferu(&ngbc, language_code, 3);
        language_code[3] = '\0';

        event_name_length = bytestream2_get_byteu(&ngbc);
        if (bytestream2_get_bytes_left(&ngbc) < event_name_length + 2)
            return AVERROR_INVALIDDATA;
        event_name = av_strndup((const char *) ngbc.buffer,
                                event_name_length);
        if (!event_name)
            return AVERROR(ENOMEM);
        bytestream2_skipu(&ngbc, event_name_length);

        text_length = bytestream2_get_be16u(&ngbc);
        if (bytestream2_get_bytes_left(&ngbc) < text_length) {
            av_free(event_name);
            return AVERROR_INVALIDDATA;
        }
        text = av_strndup((const char *) ngbc.buffer, text_length);
        if (!text) {
            av_free(event_name);
            return AVERROR(ENOMEM);
        }

        ret = av_dict_set(&ctx->program->metadata, "language",
                          (const char *) language_code, 0);
        if (ret >= 0)
            ret = av_dict_set(&ctx->program->metadata, "title", event_name, 0);
        if (ret >= 0)
            ret = av_dict_set(&ctx->program->metadata, "description", text, 0);
        av_free(event_name);
        av_free(text);
        if (ret < 0)
            return ret;
    }
    bytestream2_skipu(gbc, descriptor_length);

    return 0;
}

static int parse_mh_eit_descriptor(MMTPContext *ctx, GetByteContext *gbc)
{
    if (bytestream2_get_bytes_left(gbc) < 3)
        return AVERROR_INVALIDDATA;
    switch (bytestream2_peek_be16u(gbc)) {
    case VIDEO_COMPONENT_DESCRIPTOR:
        return parse_video_component_descriptor(NULL, gbc);
    case MH_AUDIO_COMPONENT_DESCRIPTOR:
        return parse_mh_audio_component_descriptor(NULL, gbc);
    case MH_SHORT_EVENT_DESCRIPTOR:
        return parse_mh_short_event_descriptor(ctx, gbc);
    default:
        return skip_unknown_descriptor(ctx->s, gbc);
    }
}

static int parse_mmt_package_table(MMTPContext *ctx, GetByteContext *gbc)
{
    uint16_t length;

    if (bytestream2_get_bytes_left(gbc) < (8 + 8 + 16) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_byteu(gbc) != MMT_PACKAGE_TABLE_ID)
        return AVERROR_INVALIDDATA;
    // skip: version
    bytestream2_skipu(gbc, 1);
    length = bytestream2_get_be16u(gbc);

    if (bytestream2_get_bytes_left(gbc) < length)
        return AVERROR_INVALIDDATA;
    {
        size_t   i, j;
        uint8_t  package_id_length;
        uint16_t descriptors_length;
        uint8_t  number_of_assets;

        GetByteContext ngbc;
        bytestream2_init(&ngbc, gbc->buffer, length);

        if (bytestream2_get_bytes_left(&ngbc) < (6 + 2 + 8) / 8)
            return AVERROR_INVALIDDATA;

        // skip: MPT_mode
        bytestream2_skipu(&ngbc, 1);
        package_id_length = bytestream2_get_byteu(&ngbc);

        if (bytestream2_get_bytes_left(&ngbc) < package_id_length + 2)
            return AVERROR_INVALIDDATA;
        bytestream2_skipu(&ngbc, package_id_length);

        descriptors_length = bytestream2_get_be16u(&ngbc);
        if (bytestream2_get_bytes_left(&ngbc) < descriptors_length)
            return AVERROR_INVALIDDATA;
        bytestream2_skipu(&ngbc, descriptors_length);

        if (bytestream2_get_bytes_left(&ngbc) < 1)
            return AVERROR_INVALIDDATA;
        number_of_assets = bytestream2_get_byteu(&ngbc);

        for (i = 0; i < number_of_assets; ++i) {
            int err;
            bool has_packet_id = false;

            uint8_t  asset_id_length;
            uint8_t  location_count;
            uint16_t asset_descriptors_length;
            uint16_t packet_id = 0;
            uint32_t asset_type;

            struct Streams *stream = NULL;

            if (bytestream2_get_bytes_left(&ngbc) < (8 + 32 + 8) / 8)
                return AVERROR_INVALIDDATA;
            /*
             * skip:
             * - identifier_type
             * - asset_id_scheme
            */
            bytestream2_skipu(&ngbc, (8 + 32) / 8);
            asset_id_length = bytestream2_get_byteu(&ngbc);

            if (bytestream2_get_bytes_left(&ngbc) < asset_id_length + 6)
                return AVERROR_INVALIDDATA;
            bytestream2_skipu(&ngbc, asset_id_length);

            asset_type = bytestream2_get_le32u(&ngbc);

            // skip: asset_clock_relation_flag
            bytestream2_skipu(&ngbc, 1);

            if (bytestream2_get_bytes_left(&ngbc) < 1)
                return AVERROR_INVALIDDATA;
            location_count = bytestream2_get_byteu(&ngbc);
            if (location_count == 0)
                return AVERROR_INVALIDDATA;

            for (j = 0; j < location_count; ++j) {
                int location_packet_id;

                if ((err = parse_mmt_general_location_info(
                         &ngbc, &location_packet_id)) < 0)
                    return err;
                if (!has_packet_id && location_packet_id >= 0) {
                    packet_id = location_packet_id;
                    has_packet_id = true;
                }
            }

            switch (asset_type) {
            case MKTAG('h', 'e', 'v', '1'):
            case MKTAG('h', 'v', 'c', '1'):
                if (!has_packet_id)
                    break;
                if ((err = find_or_allocate_stream(
                         ctx, packet_id, &stream)) < 0)
                    return err;
                stream->stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                stream->stream->codecpar->codec_id   = AV_CODEC_ID_HEVC;
                stream->stream->codecpar->codec_tag  = asset_type;
                break;
            case MKTAG('a', 'v', 'c', '1'):
            case MKTAG('a', 'v', 'c', '3'):
                if (!has_packet_id)
                    break;
                if ((err = find_or_allocate_stream(
                         ctx, packet_id, &stream)) < 0)
                    return err;
                stream->stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                stream->stream->codecpar->codec_id   = AV_CODEC_ID_H264;
                stream->stream->codecpar->codec_tag  = asset_type;
                break;
            case MKTAG('m', 'p', '4', 'a'):
                if (!has_packet_id)
                    break;
                if ((err = find_or_allocate_stream(
                         ctx, packet_id, &stream)) < 0)
                    return err;
                if (stream->stream->codecpar->codec_tag != asset_type) {
                    stream->stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
                    stream->stream->codecpar->codec_id   = AV_CODEC_ID_BIN_DATA;
                    av_freep(&stream->stream->codecpar->extradata);
                    stream->stream->codecpar->extradata_size = 0;
                }
                stream->stream->codecpar->codec_tag  = asset_type;
                stream->stream->discard              = AVDISCARD_DEFAULT;
                avpriv_set_pts_info(stream->stream, 64, 1, 1000);
                break;
            case MKTAG('s', 't', 'p', 'p'):
                if (has_packet_id) {
                    if ((err = find_or_allocate_stream(
                             ctx, packet_id, &stream)) < 0)
                        return err;
                    if (stream->stream->codecpar->codec_tag != asset_type) {
                        stream->stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
                        stream->stream->codecpar->codec_id   = AV_CODEC_ID_BIN_DATA;
                    }
                    stream->stream->codecpar->codec_tag  = asset_type;
                    stream->stream->discard              = AVDISCARD_DEFAULT;
                    avpriv_set_pts_info(stream->stream, 64, 1, 1000);
                }
                break;
            case MKTAG('a', 'a', 'p', 'p'):
            case MKTAG('a', 's', 'g', 'd'):
            case MKTAG('a', 'a', 'g', 'd'):
            default:
                if (!has_packet_id)
                    break;
                if ((err = find_or_allocate_stream(
                         ctx, packet_id, &stream)) < 0)
                    return err;
                stream->stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
                stream->stream->codecpar->codec_id   = AV_CODEC_ID_BIN_DATA;
                stream->stream->codecpar->codec_tag  = asset_type;
                avpriv_set_pts_info(stream->stream, 64, 1, 1000);
                break;
            }

            if (bytestream2_get_bytes_left(&ngbc) < 2)
                return AVERROR_INVALIDDATA;
            asset_descriptors_length = bytestream2_get_be16u(&ngbc);
            if (bytestream2_get_bytes_left(&ngbc) < asset_descriptors_length)
                return AVERROR_INVALIDDATA;
            if (stream != NULL) {
                GetByteContext nngbc;
                stream->audio_specific_config_descriptor_seen = false;
                bytestream2_init(&nngbc, ngbc.buffer, asset_descriptors_length);

                while (bytestream2_get_bytes_left(&nngbc) > 0)
                    if ((err = parse_mpt_descriptor(stream, &nngbc)) < 0)
                        return err;
            }
            bytestream2_skipu(&ngbc, asset_descriptors_length);
        }
    }
    bytestream2_skipu(gbc, length);

    return 0;
}

static int parse_package_list_table(GetByteContext *gbc)
{
    size_t   i;
    uint16_t length;

    if (bytestream2_get_bytes_left(gbc) < (8 + 8 + 16) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_byteu(gbc) != PACKAGE_LIST_TABLE_ID)
        return AVERROR_INVALIDDATA;
    // skip: version
    bytestream2_skipu(gbc, 1);
    length = bytestream2_get_be16u(gbc);

    if (bytestream2_get_bytes_left(gbc) < length)
        return AVERROR_INVALIDDATA;
    {
        int     err;
        uint8_t num_of_package;

        GetByteContext ngbc;
        bytestream2_init(&ngbc, gbc->buffer, length);

        if (bytestream2_get_bytes_left(&ngbc) < 1)
            return AVERROR_INVALIDDATA;
        num_of_package = bytestream2_get_byteu(&ngbc);

        for (i = 0; i < num_of_package; ++i) {
            uint8_t package_id_length;

            if (bytestream2_get_bytes_left(&ngbc) < 1)
                return AVERROR_INVALIDDATA;
            package_id_length = bytestream2_get_byteu(&ngbc);
            if (bytestream2_get_bytes_left(&ngbc) < package_id_length)
                return AVERROR_INVALIDDATA;
            bytestream2_skipu(&ngbc, package_id_length);

            if ((err = parse_mmt_general_location_info(&ngbc, NULL)) < 0)
                return err;
        }

        if (bytestream2_get_bytes_left(&ngbc) < 1)
            return AVERROR_INVALIDDATA;
        /* IP delivery entries are not needed for the current MMTP flow. The
         * enclosing table length still lets us skip them without losing the
         * following signalling message. */
        bytestream2_skipu(&ngbc, 1);
    }
    bytestream2_skipu(gbc, length);

    return 0;
}

static int parse_mh_eit_table(MMTPContext *ctx, GetByteContext *gbc)
{
    uint16_t section_length;

    if (bytestream2_get_bytes_left(gbc) < (8 + 1 + 1 + 2 + 12) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_byteu(gbc) != MH_EIT_TABLE_ID)
        return AVERROR_INVALIDDATA;
    section_length = bytestream2_get_be16u(gbc) & 0x0fff;

    if (bytestream2_get_bytes_left(gbc) < section_length || section_length < 4)
        return AVERROR_INVALIDDATA;
    {
        GetByteContext ngbc;
        bytestream2_init(&ngbc, gbc->buffer, section_length - 4);

        if (bytestream2_get_bytes_left(&ngbc) <
            (16 + 2 + 5 + 1 + 8 + 8 + 16 + 16 + 8 + 8) / 8)
            return AVERROR_INVALIDDATA;
        bytestream2_skipu(&ngbc,
                          (16 + 2 + 5 + 1 + 8 + 8 + 16 + 16 + 8 + 8) / 8);

        while (bytestream2_get_bytes_left(&ngbc) > 0) {
            uint16_t descriptors_loop_length;

            if (bytestream2_get_bytes_left(&ngbc) < (16 + 40 + 24 + 16) / 8)
                return AVERROR_INVALIDDATA;
            bytestream2_skipu(&ngbc, (16 + 40 + 24) / 8);
            descriptors_loop_length =
                bytestream2_get_be16u(&ngbc) & 0x0fff;

            if (bytestream2_get_bytes_left(&ngbc) < descriptors_loop_length)
                return AVERROR_INVALIDDATA;
            {
                int            err;
                GetByteContext nngbc;
                bytestream2_init(&nngbc, ngbc.buffer, descriptors_loop_length);

                while (bytestream2_get_bytes_left(&nngbc) > 0)
                    if ((err = parse_mh_eit_descriptor(ctx, &nngbc)) < 0)
                        return err;
            }
            bytestream2_skipu(&ngbc, descriptors_loop_length);
        }
    }
    bytestream2_skipu(gbc, section_length);

    return 0;
}

static int parse_table(MMTPContext *ctx, GetByteContext *gbc)
{
    if (bytestream2_get_bytes_left(gbc) < 2)
        return AVERROR_INVALIDDATA;
    switch (bytestream2_peek_byteu(gbc)) {
    case MMT_PACKAGE_TABLE_ID:
        return parse_mmt_package_table(ctx, gbc);
    case PACKAGE_LIST_TABLE_ID:
        return parse_package_list_table(gbc);
    case MH_EIT_TABLE_ID:
        return parse_mh_eit_table(ctx, gbc);
    }
    /* Unknown tables are bounded by the enclosing signalling message. */
    bytestream2_skipu(gbc, bytestream2_get_bytes_left(gbc));
    return 0;
}

enum {
    PA_MESSAGE_ID      = 0x0000,
    M2_SECTION_MESSAGE = 0x8000,
};

static int parse_pa_message(MMTPContext *ctx, GetByteContext *gbc)
{
    uint32_t length;

    if (bytestream2_get_bytes_left(gbc) < (16 + 8 + 32) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_be16u(gbc) != PA_MESSAGE_ID)
        return AVERROR_INVALIDDATA;
    // skip: version
    bytestream2_skipu(gbc, 1);
    length = bytestream2_get_be32u(gbc);

    if ((uint32_t) bytestream2_get_bytes_left(gbc) < length)
        return AVERROR_INVALIDDATA;
    {
        int     err;
        size_t  i;
        uint8_t num_of_tables;
        uint16_t table_lengths[UINT8_MAX];

        GetByteContext ngbc;
        bytestream2_init(&ngbc, gbc->buffer, length);

        if (bytestream2_get_bytes_left(&ngbc) < 1)
            return AVERROR_INVALIDDATA;
        num_of_tables = bytestream2_get_byteu(&ngbc);

        if (bytestream2_get_bytes_left(&ngbc) < num_of_tables * 4)
            return AVERROR_INVALIDDATA;
        for (i = 0; i < num_of_tables; ++i) {
            // skip: table_id and table_version
            bytestream2_skipu(&ngbc, 2);
            table_lengths[i] = bytestream2_get_be16u(&ngbc);
        }

        for (i = 0; i < num_of_tables; ++i) {
            const unsigned int table_size = table_lengths[i] + 4;
            GetByteContext table;

            if ((unsigned int) bytestream2_get_bytes_left(&ngbc) < table_size)
                return AVERROR_INVALIDDATA;
            bytestream2_init(&table, ngbc.buffer, table_size);
            err = parse_table(ctx, &table);
            if (err < 0)
                return err;
            bytestream2_skipu(&ngbc, table_size);
        }

        /*
         * ARIB streams have also been observed with number_of_tables=0 and
         * complete tables placed directly after the empty index.
         */
        while (bytestream2_get_bytes_left(&ngbc) > 0) {
            unsigned int table_size;
            GetByteContext table;

            if (bytestream2_get_bytes_left(&ngbc) < 4)
                return AVERROR_INVALIDDATA;
            table_size = AV_RB16(ngbc.buffer + 2) + 4;
            if ((unsigned int) bytestream2_get_bytes_left(&ngbc) < table_size)
                return AVERROR_INVALIDDATA;
            bytestream2_init(&table, ngbc.buffer, table_size);
            if ((err = parse_table(ctx, &table)) < 0)
                return err;
            bytestream2_skipu(&ngbc, table_size);
        }
    }
    bytestream2_skipu(gbc, length);

    return 0;
}

static int parse_m2_section_message(MMTPContext *ctx, GetByteContext *gbc)
{
    int      err;
    uint16_t length;

    if (bytestream2_get_bytes_left(gbc) < (16 + 8 + 16) / 8)
        return AVERROR_INVALIDDATA;
    if (bytestream2_get_be16u(gbc) != M2_SECTION_MESSAGE)
        return AVERROR_INVALIDDATA;
    // skip: version
    bytestream2_skipu(gbc, 1);
    length = bytestream2_get_be16u(gbc);

    if (bytestream2_get_bytes_left(gbc) < length)
        return AVERROR_INVALIDDATA;
    {
        GetByteContext ngbc;
        bytestream2_init(&ngbc, gbc->buffer, length);
        err = parse_table(ctx, &ngbc);
    }
    bytestream2_skipu(gbc, length);

    return err;
}

static int parse_signalling_message(MMTPContext *ctx, GetByteContext *gbc)
{
    if (bytestream2_get_bytes_left(gbc) < 4)
        return AVERROR_INVALIDDATA;
    switch (bytestream2_peek_be16u(gbc)) {
    case PA_MESSAGE_ID:
        return parse_pa_message(ctx, gbc);
    case M2_SECTION_MESSAGE:
        return parse_m2_section_message(ctx, gbc);
    }
    return 0;
}

enum FragmentationIndicator {
    NOT_FRAGMENTED,
    FIRST_FRAGMENT,
    MIDDLE_FRAGMENT,
    LAST_FRAGMENT,
};

struct FragmentAssembler {
    uint16_t                 pid;
    struct FragmentAssembler *next;

    uint8_t *data;
    size_t  size, cap;

    uint32_t last_seq;
    uint32_t item_id;
    uint32_t movie_fragment_sequence_number;
    uint32_t sample_number;
    uint32_t offset;
    uint8_t  priority;
    uint8_t  dependency_counter;

    enum {
        INIT = 0,
        NOT_STARTED,
        IN_FRAGMENT,
        SKIP,
    }        state;
};

static int append_data(struct FragmentAssembler *ctx, MMTPContext *mmtp,
                       const uint8_t *data, uint32_t size)
{
    size_t required;

    if (ctx->size > MAX_FRAGMENT_SIZE ||
        size > MAX_FRAGMENT_SIZE - ctx->size)
        return AVERROR_INVALIDDATA;
    required = ctx->size + size;

    if (ctx->cap < required) {
        void   *new_data;
        size_t new_cap = ctx->cap == 0 ? 1024 : ctx->cap * 2;

        while (new_cap < required) {
            if (new_cap > MAX_FRAGMENT_SIZE / 2) {
                new_cap = MAX_FRAGMENT_SIZE;
                break;
            }
            new_cap *= 2;
        }

        if (new_cap - ctx->cap >
            MAX_FRAGMENT_CAPACITY - mmtp->fragment_capacity)
            return AVERROR_INVALIDDATA;

        new_data = av_realloc(ctx->data, new_cap);
        if (new_data == NULL)
            return AVERROR(ENOMEM);
        mmtp->fragment_capacity += new_cap - ctx->cap;
        ctx->data = new_data;
        ctx->cap  = new_cap;
    }
    memcpy(ctx->data + ctx->size, data, size);
    ctx->size += size;
    return 0;
}

static void discard_fragment(MMTPContext *ctx,
                             struct FragmentAssembler *assembler)
{
    struct Streams *streams = find_current_stream(ctx);

    assembler->size  = 0;
    assembler->state = SKIP;
    if (streams) {
        streams->flags  = 0;
        streams->offset = -1;
    }
}

static bool
check_state(MMTPContext *ctx, struct FragmentAssembler *ass, uint32_t seq_num)
{
    if (ass->state == INIT) {
        ass->state = SKIP;
    } else if (seq_num == ass->last_seq) {
        av_log(ctx->s, AV_LOG_VERBOSE,
               "Duplicate packet sequence number %u; ignoring packet\n",
               seq_num);
        return false;
    } else if (sequence_is_before(seq_num, ass->last_seq)) {
        av_log(ctx->s, AV_LOG_VERBOSE,
               "Out-of-order packet sequence number %u after %u; ignoring packet\n",
               seq_num, ass->last_seq);
        return false;
    } else if (seq_num != ass->last_seq + 1) {
        if (ass->state == IN_FRAGMENT) {
            av_log(ctx->s, AV_LOG_WARNING,
                    "Packet sequence number jump: %u + 1 != %u, drop %zu bytes\n",
                    ass->last_seq, seq_num, ass->size);
            discard_fragment(ctx, ass);
        } else {
            av_log(ctx->s, AV_LOG_WARNING,
                    "Packet sequence number jump: %u + 1 != %u\n",
                    ass->last_seq, seq_num);
            ass->state = SKIP;
        }
    }
    ass->last_seq = seq_num;
    return true;
}

static int assemble_fragment(
    struct FragmentAssembler *ctx, uint32_t seq_num,
    enum FragmentationIndicator indicator,
    const uint8_t *data, uint32_t size,
    int (*parser)(MMTPContext *, GetByteContext *),
    MMTPContext *opaque)
{
    GetByteContext gbc;
    int            err;

    switch (indicator) {
    case NOT_FRAGMENTED:
        ctx->size  = 0;
        ctx->state = NOT_STARTED;
        bytestream2_init(&gbc, data, size);
        return parser(opaque, &gbc);
    case FIRST_FRAGMENT:
        ctx->size  = 0;
        ctx->state = IN_FRAGMENT;
        return append_data(ctx, opaque, data, size);
    case MIDDLE_FRAGMENT:
        if (ctx->state == SKIP) {
            av_log(opaque->s, AV_LOG_VERBOSE, "Drop packet %u\n", seq_num);
            return 0;
        }
        if (ctx->state != IN_FRAGMENT)
            return AVERROR_INVALIDDATA;
        return append_data(ctx, opaque, data, size);
    case LAST_FRAGMENT:
        if (ctx->state == SKIP) {
            av_log(opaque->s, AV_LOG_VERBOSE, "Drop packet %u\n", seq_num);
            return 0;
        }
        if (ctx->state != IN_FRAGMENT)
            return AVERROR_INVALIDDATA;
        if ((err = append_data(ctx, opaque, data, size)) < 0)
            return err;

        bytestream2_init(&gbc, ctx->data, ctx->size);
        err = parser(opaque, &gbc);

        ctx->size  = 0;
        ctx->state = NOT_STARTED;
        return err;
    default:
        return AVERROR_INVALIDDATA;
    }
}

static int find_or_allocate_assembler(MMTPContext *ctx, uint16_t pid,
                                      struct FragmentAssembler **result)
{
    struct FragmentAssembler *ass;
    for (ass = ctx->assembler; ass != NULL; ass = ass->next) {
        if (ass->pid == pid) {
            *result = ass;
            return 0;
        }
    }

    if (ctx->nb_assemblers >= MAX_FRAGMENT_ASSEMBLERS)
        return AVERROR_INVALIDDATA;

    ass = av_mallocz(sizeof(*ass));
    if (ass == NULL)
        return AVERROR(ENOMEM);
    ass->pid       = pid;
    ass->next      = ctx->assembler;
    ctx->assembler = ass;
    ctx->nb_assemblers++;
    *result = ass;
    return 0;
}

static int parse_signalling_messages(
    MMTPContext *ctx, uint32_t seq_num, GetByteContext *gbc)
{
    int                         err;
    uint8_t                     byte;
    enum FragmentationIndicator fragmentation_indicator;
    bool                        length_extension_flag;
    bool                        aggregation_flag;

    struct FragmentAssembler *assembler;

    if ((err = find_or_allocate_assembler(
             ctx, ctx->current_pid, &assembler)) < 0)
        return err;

    if (bytestream2_get_bytes_left(gbc) < (2 + 4 + 1 + 1 + 8) / 8)
        return AVERROR_INVALIDDATA;
    byte                    = bytestream2_get_byteu(gbc);
    fragmentation_indicator = byte >> 6;
    length_extension_flag   = (byte >> 1) & 1;
    aggregation_flag        = byte & 1;

    bytestream2_skipu(gbc, 1);

    if (!check_state(ctx, assembler, seq_num))
        return 0;

    if (!aggregation_flag)
        return assemble_fragment(
            assembler, seq_num, fragmentation_indicator,
            gbc->buffer, bytestream2_get_bytes_left(gbc),
            parse_signalling_message, ctx);

    if (fragmentation_indicator != NOT_FRAGMENTED)
        return AVERROR_INVALIDDATA; // cannot be both fragmented and aggregated

    while (bytestream2_get_bytes_left(gbc) > 0) {
        uint32_t length;

        if (length_extension_flag) {
            if (bytestream2_get_bytes_left(gbc) < 4)
                return AVERROR_INVALIDDATA;
            length = bytestream2_get_be32u(gbc);
        } else {
            if (bytestream2_get_bytes_left(gbc) < 2)
                return AVERROR_INVALIDDATA;
            length = bytestream2_get_be16u(gbc);
        }

        if ((uint32_t) bytestream2_get_bytes_left(gbc) < length)
            return AVERROR_INVALIDDATA;
        if ((err = assemble_fragment(
            assembler, seq_num, NOT_FRAGMENTED,
            gbc->buffer, length, parse_signalling_message, ctx)) < 0)
            return err;
        bytestream2_skipu(gbc, length);
    }

    return 0;
}

static int64_t short_ntp_to_milliseconds(const MMTPContext *ctx,
                                         uint32_t short_ntp);

static int64_t fixed_pts_offset(const struct Streams *s)
{
    const AVStream *stream = s->stream;
    int64_t duration;

    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        duration = av_rescale_q(1, av_inv_q(stream->avg_frame_rate),
                                stream->time_base);
        return duration > 0 ? duration : AV_NOPTS_VALUE;
    }
    if ((stream->codecpar->codec_id == AV_CODEC_ID_AAC_LATM ||
         stream->codecpar->codec_id == AV_CODEC_ID_AAC) &&
        stream->codecpar->sample_rate > 0) {
        AVRational sample_time_base = {
            1, stream->codecpar->sample_rate
        };

        duration = av_rescale_q(1024, sample_time_base, stream->time_base);
        return duration > 0 ? duration : AV_NOPTS_VALUE;
    }
    return AV_NOPTS_VALUE;
}

static void get_pts_dts(struct Streams *s, int64_t *pts, int64_t *dts)
{
    struct MPUTimestampDescriptor         *desc     = NULL;
    struct MPUExtendedTimestampDescriptor *ext_desc = NULL;

    int64_t au_duration = AV_NOPTS_VALUE;
    int64_t fallback;
    int64_t ptime;
    int64_t seconds;
    uint32_t fraction;
    int     i;
    size_t  j;

    *pts = AV_NOPTS_VALUE;
    *dts = AV_NOPTS_VALUE;

    for (i = 0; i < s->num_timestamp_descriptors; ++i) {
        if (s->timestamp_descriptor[i].seq_num ==
            s->last_sequence_number) {
            desc = s->timestamp_descriptor + i;
            break;
        }
    }

    for (i = 0; i < s->num_ext_timestamp_descriptors; ++i) {
        if (s->ext_timestamp_descriptor[i].seq_num ==
            s->last_sequence_number) {
            ext_desc = s->ext_timestamp_descriptor + i;
            break;
        }
    }

    if (ext_desc && ext_desc->pts_offset_type == 0)
        au_duration = fixed_pts_offset(s);
    if (!desc || !ext_desc ||
        s->au_count >= ext_desc->num_of_au ||
        (ext_desc->pts_offset_type == 0 &&
         au_duration == AV_NOPTS_VALUE) ||
        s->stream->time_base.num <= 0 || s->stream->time_base.den <= 0) {
        fallback = short_ntp_to_milliseconds(s->owner,
                                             s->owner->current_timestamp);
        if (fallback != AV_NOPTS_VALUE &&
            s->stream->time_base.num > 0 && s->stream->time_base.den > 0) {
            *pts = av_rescale_q(fallback, (AVRational) { 1, 1000 },
                                s->stream->time_base);
            *dts = *pts;
        }
        if (s->au_count != UINT16_MAX)
            s->au_count++;
        return;
    }

    seconds = (int64_t) (desc->presentation_time >> 32) -
              (int64_t) NTP_OFFSET;
    fraction = desc->presentation_time & UINT32_MAX;
    ptime = av_rescale(seconds, s->stream->time_base.den,
                       s->stream->time_base.num);
    ptime += av_rescale(fraction, s->stream->time_base.den,
                        (1LL << 32) * s->stream->time_base.num);

    *dts = ptime - ext_desc->decoding_time_offset;

    for (j = 0; j < s->au_count; ++j)
        *dts += ext_desc->pts_offset_type == 0
                    ? au_duration : ext_desc->au[j].pts_offset;

    *pts = *dts + ext_desc->au[s->au_count].dts_pts_offset;

    ++s->au_count;
}

static void fill_pts_dts(struct Streams *s)
{
    get_pts_dts(s, &s->parser->pts, &s->parser->dts);
}

static int64_t ntp64_to_milliseconds(uint64_t ntp)
{
    const int64_t seconds = (int64_t) (ntp >> 32) - NTP_OFFSET;
    const uint32_t fraction = ntp & UINT32_MAX;

    return seconds * 1000 + av_rescale(fraction, 1000, 1LL << 32);
}

static int64_t short_ntp_to_milliseconds(const MMTPContext *ctx,
                                         uint32_t short_ntp)
{
    int64_t seconds;
    int64_t anchor_seconds;
    uint64_t ntp;

    if (!ctx->ntp_anchor)
        return AV_NOPTS_VALUE;

    anchor_seconds = ctx->ntp_anchor >> 32;
    seconds = (anchor_seconds & ~INT64_C(0xffff)) | (short_ntp >> 16);
    if (seconds - anchor_seconds > 0x8000)
        seconds -= 0x10000;
    else if (anchor_seconds - seconds > 0x8000)
        seconds += 0x10000;

    ntp = (uint64_t) seconds << 32 |
          (uint64_t) (short_ntp & 0xffff) << 16;
    return ntp64_to_milliseconds(ntp);
}

static int emit_subtitle_resource_mfu(MMTPContext *ctx, struct Streams *st,
                                      GetByteContext *gbc, uint32_t data_size,
                                      uint8_t data_type,
                                      uint8_t subsample_number,
                                      uint8_t last_subsample_number)
{
    AVDictionary *metadata = NULL;
    uint8_t *packed_metadata;
    size_t metadata_size;
    int err;

    if (!st->subtitle_resource_stream) {
        AVStream *stream = avformat_new_stream(ctx->s, NULL);

        if (!stream)
            return AVERROR(ENOMEM);
        stream->id                    = st->stream->id | 0x10000;
        stream->codecpar->codec_type  = AVMEDIA_TYPE_DATA;
        stream->codecpar->codec_id    = AV_CODEC_ID_BIN_DATA;
        stream->codecpar->codec_tag   = MKTAG('s', 't', 'r', 's');
        stream->disposition          |= AV_DISPOSITION_DEPENDENT;
        avpriv_set_pts_info(stream, 64, 1, 1000);
        av_program_add_stream_index(ctx->s, ctx->program->id, stream->index);
        if ((err = av_dict_set(&stream->metadata, "title",
                               "ARIB-TTML resources", 0)) < 0)
            return err;
        st->subtitle_resource_stream = stream;
    }

    if ((err = av_new_packet(ctx->pkt, data_size)) < 0)
        return err;
    bytestream2_get_bufferu(gbc, ctx->pkt->data, data_size);
    if ((err = av_dict_set_int(&metadata, "mmt.subtitle.data_type",
                               data_type, 0)) < 0 ||
        (err = av_dict_set_int(&metadata, "mmt.subtitle.subsample_number",
                               subsample_number, 0)) < 0 ||
        (err = av_dict_set_int(&metadata,
                               "mmt.subtitle.last_subsample_number",
                               last_subsample_number, 0)) < 0) {
        av_dict_free(&metadata);
        goto fail;
    }
    packed_metadata = av_packet_pack_dictionary(metadata, &metadata_size);
    av_dict_free(&metadata);
    if (!packed_metadata) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    if ((err = av_packet_add_side_data(ctx->pkt, AV_PKT_DATA_STRINGS_METADATA,
                                       packed_metadata, metadata_size)) < 0) {
        av_free(packed_metadata);
        goto fail;
    }
    ctx->pkt->stream_index = st->subtitle_resource_stream->index;
    ctx->pkt->pts = short_ntp_to_milliseconds(ctx, ctx->current_timestamp);
    if (st->subtitle_tmd == 2 && st->subtitle_has_reference_time)
        ctx->pkt->pts = ntp64_to_milliseconds(st->subtitle_reference_time);
    ctx->pkt->dts   = ctx->pkt->pts;
    ctx->pkt->flags = st->flags | AV_PKT_FLAG_KEY;
    ctx->pkt->pos   = st->offset;
    ctx->pkt        = NULL;

    st->flags  = 0;
    st->offset = -1;
    return 0;

fail:
    av_packet_unref(ctx->pkt);
    return err;
}

static int emit_closed_caption_mfu(MMTPContext *ctx, struct Streams *st,
                                   GetByteContext *gbc)
{
    uint8_t  data_type, subsample_number, last_subsample_number, byte;
    uint32_t data_size;
    size_t   i;
    int      err;
    bool     length_ext_flag, subsample_info_list_flag;

    av_assert0(ctx->pkt != NULL);

    if (bytestream2_get_bytes_left(gbc) < (8 + 8 + 8 + 8 + 4 + 1 + 1 + 2) / 8)
        return AVERROR_INVALIDDATA;

    /*
     * skip:
     * - subtitle_tag
     * - subtitle_sequence_number
     */
    bytestream2_skipu(gbc, (8 + 8) / 8);

    subsample_number      = bytestream2_get_byteu(gbc);
    last_subsample_number = bytestream2_get_byteu(gbc);

    byte                     = bytestream2_get_byteu(gbc);
    data_type                = byte >> 4;
    length_ext_flag          = (byte >> 3) & 1;
    subsample_info_list_flag = (byte >> 2) & 1;

    if (length_ext_flag) {
        if (bytestream2_get_bytes_left(gbc) < 4)
            return AVERROR_INVALIDDATA;
        data_size = bytestream2_get_be32u(gbc);
    } else {
        if (bytestream2_get_bytes_left(gbc) < 2)
            return AVERROR_INVALIDDATA;
        data_size = bytestream2_get_be16u(gbc);
    }

    if (subsample_number == 0 && last_subsample_number > 0 &&
        subsample_info_list_flag) {
        for (i = 0; i < last_subsample_number; ++i) {
            const int entry_size = 1 + (length_ext_flag ? 4 : 2);

            if (bytestream2_get_bytes_left(gbc) < entry_size)
                return AVERROR_INVALIDDATA;
            // skip: subsample_i_data_type
            bytestream2_skipu(gbc, (4 + 4) / 8);
            // skip: subsample_i_data_size
            if (length_ext_flag) {
                bytestream2_skipu(gbc, 32 / 8);
            } else {
                bytestream2_skipu(gbc, 16 / 8);
            }
        }
    }

    if ((uint32_t) bytestream2_get_bytes_left(gbc) < data_size)
        return AVERROR_INVALIDDATA;
    if (data_type)
        return emit_subtitle_resource_mfu(ctx, st, gbc, data_size, data_type,
                                          subsample_number,
                                          last_subsample_number);
    if ((err = av_new_packet(ctx->pkt, data_size)) < 0)
        return err;
    bytestream2_get_bufferu(gbc, ctx->pkt->data, data_size);

    ctx->pkt->stream_index = st->stream->index;
    ctx->pkt->flags        = st->flags;
    ctx->pkt->pos          = st->offset;
    if (st->subtitle_tmd == 2 && st->subtitle_has_reference_time) {
        ctx->pkt->pts = ntp64_to_milliseconds(st->subtitle_reference_time);
        ctx->pkt->dts = short_ntp_to_milliseconds(ctx,
                                                  ctx->current_timestamp);
        if (ctx->pkt->dts == AV_NOPTS_VALUE)
            ctx->pkt->dts = ctx->pkt->pts;
    } else {
        ctx->pkt->pts = short_ntp_to_milliseconds(ctx,
                                                  ctx->current_timestamp);
        ctx->pkt->dts = ctx->pkt->pts;
    }
    ctx->pkt               = NULL;

    st->flags  = 0;
    st->offset = -1;
    return 0;
}

// Build extradata as an Annex-B VPS+SPS+PPS blob, once all three are known.
static int hevc_rebuild_extradata(struct Streams *st)
{
    static const uint8_t start_code[4] = { 0, 0, 0, 1 };
    AVCodecParameters *par = st->stream->codecpar;
    int      i, total = 0;
    uint8_t *ed, *p;

    for (i = 0; i < 3; i++) {
        if (st->hevc_ps[i].data == NULL)
            return 0; // wait until VPS + SPS + PPS have all been seen
        total += sizeof(start_code) + st->hevc_ps[i].size;
    }

    ed = av_malloc(total + AV_INPUT_BUFFER_PADDING_SIZE);
    if (ed == NULL)
        return AVERROR(ENOMEM);
    for (i = 0, p = ed; i < 3; i++) {
        memcpy(p, start_code, sizeof(start_code));
        p += sizeof(start_code);
        memcpy(p, st->hevc_ps[i].data, st->hevc_ps[i].size);
        p += st->hevc_ps[i].size;
    }
    memset(p, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    av_freep(&par->extradata);
    par->extradata      = ed;
    par->extradata_size = total;
    return 0;
}

// Keep a copy of each parameter-set NAL, rebuilding extradata when one changes.
static int hevc_capture_parameter_set(struct Streams *st,
                                      const uint8_t *nal, int nal_size)
{
    uint8_t *copy;
    int      idx;

    if (nal_size < 2 || nal_size > 1024 * 1024)
        return 0;
    switch ((nal[0] >> 1) & 0x3f) {
    case HEVC_NAL_VPS: idx = 0; break;
    case HEVC_NAL_SPS: idx = 1; break;
    case HEVC_NAL_PPS: idx = 2; break;
    default:           return 0;
    }

    if (st->hevc_ps[idx].data != NULL &&
        st->hevc_ps[idx].size == nal_size &&
        memcmp(st->hevc_ps[idx].data, nal, nal_size) == 0)
        return 0; // already captured, unchanged

    copy = av_malloc(nal_size);
    if (copy == NULL)
        return AVERROR(ENOMEM);
    memcpy(copy, nal, nal_size);
    av_free(st->hevc_ps[idx].data);
    st->hevc_ps[idx].data = copy;
    st->hevc_ps[idx].size = nal_size;

    return hevc_rebuild_extradata(st);
}

static int
emit_packet(MMTPContext *ctx, struct Streams *st, uint8_t *data, int size)
{
    int      err;
    int      consumed;
    int      zero_consumed = 0;
    uint8_t *out_data = NULL;
    int      out_size = 0;

    if (st->parser == NULL) {
        st->parser = av_parser_init(st->stream->codecpar->codec_id);
        if (st->parser == NULL)
            return AVERROR(ENOMEM);
        st->parser->last_pos = 0;
    }

    while (size > 0) {
        if (st->parser->fetch_timestamp) {
            fill_pts_dts(st);
            st->parser->fetch_timestamp = false;
            st->parser->pos             = st->offset;
            // use last_pos to store flags
            st->parser->last_pos        = st->flags;
        }
        st->offset = -1;
        st->flags  = 0;

        consumed = av_parser_parse2(
            st->parser, ffstream(st->stream)->avctx,
            &out_data, &out_size,
            data, size,
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0
        );
        if (consumed < 0 || consumed > size ||
            (consumed == 0 && out_size == 0))
            return AVERROR_INVALIDDATA;
        if (consumed == 0 && zero_consumed) {
            av_log(ctx->s, AV_LOG_WARNING,
                   "pid 0x%04x: parser repeatedly emitted frames without "
                   "consuming the MFU; dropping the unconsumed input\n",
                   ctx->current_pid);
            break;
        }
        zero_consumed = consumed == 0;
        data += consumed;
        size -= consumed;

        if (out_size == 0) {
            continue;
        }
        st->parser->fetch_timestamp = true;

        // An MFU carries a single access unit, so the parser should yield one
        // frame per read. Keep the first from a non-conformant MFU and warn.
        if (ctx->pkt->data != NULL) {
            av_log(ctx->s, AV_LOG_WARNING,
                   "pid 0x%04x: parser produced more than one frame for a single MFU; "
                   "dropping the extra frame\n", ctx->current_pid);
            break;
        }
        ctx->pkt->data = (uint8_t *) out_data;
        ctx->pkt->size = out_size;

        if ((err = av_packet_make_refcounted(ctx->pkt)) < 0)
            return err;

        ctx->pkt->pos          = st->parser->pos;
        ctx->pkt->pts          = st->parser->pts;
        ctx->pkt->dts          = st->parser->dts;
        ctx->pkt->stream_index = st->stream->index;
        ctx->pkt->flags        = st->parser->last_pos;
        if (st->parser->key_frame == 1)
            ctx->pkt->flags |= AV_PKT_FLAG_KEY;
    }
    if (!AVPACKET_IS_EMPTY(ctx->pkt))
        ctx->pkt = NULL;
    return 0;
}

static int convert_video_mfu_to_annexb(struct Streams *st,
                                       GetByteContext *gbc,
                                       uint8_t **result, int *result_size)
{
    static const uint8_t start_code[4] = { 0, 0, 0, 1 };
    const int size = bytestream2_get_bytes_left(gbc);
    uint8_t *buf;
    uint8_t *dst;
    int err;

    *result = NULL;
    *result_size = 0;
    if (size < 5)
        return AVERROR_INVALIDDATA;
    buf = av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!buf)
        return AVERROR(ENOMEM);
    dst = buf;

    while (bytestream2_get_bytes_left(gbc) > 0) {
        const uint8_t *nal;
        uint32_t nal_size;

        if (bytestream2_get_bytes_left(gbc) < 4) {
            err = AVERROR_INVALIDDATA;
            goto end;
        }
        nal_size = bytestream2_get_be32u(gbc);
        if (!nal_size ||
            nal_size > (uint32_t) bytestream2_get_bytes_left(gbc)) {
            err = AVERROR_INVALIDDATA;
            goto end;
        }
        nal = gbc->buffer;
        if (st->stream->codecpar->codec_id == AV_CODEC_ID_HEVC &&
            (err = hevc_capture_parameter_set(st, nal, nal_size)) < 0)
            goto end;
        memcpy(dst, start_code, sizeof(start_code));
        bytestream2_get_bufferu(gbc, dst + sizeof(start_code), nal_size);
        dst += sizeof(start_code) + nal_size;
    }

    memset(dst, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    *result = buf;
    *result_size = size;
    return 0;

end:
    av_free(buf);
    return err;
}

static int emit_video_mfu(MMTPContext *ctx, struct Streams *st,
                          GetByteContext *gbc)
{
    uint8_t *buf;
    int size;
    int err;

    if ((err = convert_video_mfu_to_annexb(st, gbc, &buf, &size)) < 0)
        return err;
    err = emit_packet(ctx, st, buf, size);
    av_free(buf);
    return err;
}

static int emit_raw_audio_mfu(MMTPContext *ctx, struct Streams *st,
                              GetByteContext *gbc)
{
    const int size = bytestream2_get_bytes_left(gbc);
    int err;

    av_assert0(ctx->pkt);
    if ((err = av_new_packet(ctx->pkt, size)) < 0)
        return err;
    bytestream2_get_bufferu(gbc, ctx->pkt->data, size);
    get_pts_dts(st, &ctx->pkt->pts, &ctx->pkt->dts);
    ctx->pkt->stream_index = st->stream->index;
    ctx->pkt->flags        = st->flags;
    ctx->pkt->pos          = st->offset;
    ctx->pkt               = NULL;

    st->flags  = 0;
    st->offset = -1;
    return 0;
}

static int emit_data_mfu(MMTPContext *ctx, struct Streams *st,
                         GetByteContext *gbc)
{
    AVDictionary *metadata = NULL;
    uint8_t *packed_metadata;
    size_t metadata_size;
    const int size = bytestream2_get_bytes_left(gbc);
    int err;

    av_assert0(ctx->pkt);

    if ((err = av_new_packet(ctx->pkt, size)) < 0)
        return err;
    bytestream2_get_bufferu(gbc, ctx->pkt->data, size);
    if ((err = av_dict_set_int(&metadata, "mmt.packet_id",
                               ctx->current_pid, 0)) < 0 ||
        (err = av_dict_set_int(&metadata, "mmt.mpu_sequence_number",
                               st->last_sequence_number, 0)) < 0 ||
        (err = av_dict_set_int(&metadata, "mmt.timed",
                               ctx->current_mfu_timed, 0)) < 0 ||
        (ctx->current_mfu_timed &&
         ((err = av_dict_set_int(
               &metadata, "mmt.movie_fragment_sequence_number",
               ctx->current_movie_fragment_sequence_number, 0)) < 0 ||
          (err = av_dict_set_int(&metadata, "mmt.sample_number",
                                 ctx->current_sample_number, 0)) < 0 ||
          (err = av_dict_set_int(&metadata, "mmt.offset",
                                 ctx->current_offset, 0)) < 0 ||
          (err = av_dict_set_int(&metadata, "mmt.priority",
                                 ctx->current_priority, 0)) < 0 ||
          (err = av_dict_set_int(&metadata, "mmt.dependency_counter",
                                 ctx->current_dependency_counter, 0)) < 0)) ||
        (!ctx->current_mfu_timed &&
         (err = av_dict_set_int(&metadata, "mmt.item_id",
                                ctx->current_item_id, 0)) < 0)) {
        av_dict_free(&metadata);
        goto fail;
    }
    packed_metadata = av_packet_pack_dictionary(metadata, &metadata_size);
    av_dict_free(&metadata);
    if (!packed_metadata) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    if ((err = av_packet_add_side_data(ctx->pkt, AV_PKT_DATA_STRINGS_METADATA,
                                       packed_metadata, metadata_size)) < 0) {
        av_free(packed_metadata);
        goto fail;
    }
    ctx->pkt->stream_index = st->stream->index;
    ctx->pkt->pts = short_ntp_to_milliseconds(ctx, ctx->current_timestamp);
    if (ctx->current_mfu_timed) {
        int i;

        for (i = 0; i < st->num_timestamp_descriptors; i++) {
            if (st->timestamp_descriptor[i].seq_num ==
                st->last_sequence_number) {
                ctx->pkt->pts = ntp64_to_milliseconds(
                    st->timestamp_descriptor[i].presentation_time);
                break;
            }
        }
    }
    ctx->pkt->dts = ctx->pkt->pts;
    ctx->pkt->flags = st->flags | AV_PKT_FLAG_KEY;
    ctx->pkt->pos = st->offset;
    ctx->pkt = NULL;

    st->flags = 0;
    st->offset = -1;
    return 0;

fail:
    av_packet_unref(ctx->pkt);
    return err;
}

static int queue_packet(MMTPContext *ctx, AVPacket *pkt)
{
    const int packet_size = pkt->size;
    int err;

    if (ctx->queued_packets >= MAX_QUEUED_MFU_PACKETS ||
        packet_size < 0 ||
        ctx->queued_packet_data > MAX_QUEUED_PACKET_DATA ||
        (size_t) packet_size >
            MAX_QUEUED_PACKET_DATA - ctx->queued_packet_data)
        return AVERROR_INVALIDDATA;

    err = ff_packet_list_put(&ctx->packet_queue, pkt, NULL, 0);
    if (err >= 0) {
        ctx->queued_packets++;
        ctx->queued_packet_data += packet_size;
    }
    return err;
}

static int consume_mfu(MMTPContext *ctx, GetByteContext *gbc)
{
    AVPacket       queued_packet = { 0 };
    bool           queue_output;
    int            err;
    uint8_t        *buf;
    unsigned int   size;
    struct Streams *st = find_current_stream(ctx);
    av_assert0(st != NULL);

    if (!ctx->pkt && !ctx->allow_packet_queue) {
        av_log(ctx->s, AV_LOG_WARNING,
               "pid 0x%04x: dropping an additional MFU from one TLV packet\n",
               ctx->current_pid);
        return 0;
    }

    queue_output = !ctx->pkt;
    if (queue_output)
        ctx->pkt = &queued_packet;

    switch (st->stream->codecpar->codec_id) {
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_H264:
        err = emit_video_mfu(ctx, st, gbc);
        break;
    case AV_CODEC_ID_AAC_LATM:
        size = bytestream2_get_bytes_left(gbc);
        if (size >> 13) {
            err = AVERROR(EOVERFLOW);
            break;
        }
        if (!(buf = av_malloc(size + 3 + AV_INPUT_BUFFER_PADDING_SIZE))) {
            err = AVERROR(ENOMEM);
            break;
        }
        buf[0] = 0x56;
        buf[1] = 0xe0 | (size >> 8);
        buf[2] = size & 0xff;
        bytestream2_get_bufferu(gbc, buf + 3, size);
        memset(buf + 3 + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        err = emit_packet(ctx, st, buf, size + 3);
        av_free(buf);
        break;
    case AV_CODEC_ID_MP4ALS:
    case AV_CODEC_ID_AAC:
        err = emit_raw_audio_mfu(ctx, st, gbc);
        break;
    case AV_CODEC_ID_TTML:
        err = emit_closed_caption_mfu(ctx, st, gbc);
        break;
    case AV_CODEC_ID_BIN_DATA:
        err = emit_data_mfu(ctx, st, gbc);
        break;
    default:
        err = emit_data_mfu(ctx, st, gbc);
        break;
    }

    if (!queue_output)
        return err;

    if (ctx->pkt == &queued_packet)
        ctx->pkt = NULL;
    if (err >= 0 && !AVPACKET_IS_EMPTY(&queued_packet))
        err = queue_packet(ctx, &queued_packet);
    av_packet_unref(&queued_packet);
    return err;
}

static int parse_mfu_timed_data(
    MMTPContext *ctx, struct FragmentAssembler *assembler,
    uint32_t seq_num, enum FragmentationIndicator indicator,
    GetByteContext *gbc)
{
    uint32_t movie_fragment_sequence_number, sample_number, offset;
    uint8_t priority, dependency_counter;

    ctx->current_mfu_timed = true;
    if (bytestream2_get_bytes_left(gbc) < (32 + 32 + 32 + 8 + 8) / 8)
        return AVERROR_INVALIDDATA;
    movie_fragment_sequence_number = bytestream2_get_be32u(gbc);
    sample_number                  = bytestream2_get_be32u(gbc);
    offset                         = bytestream2_get_be32u(gbc);
    priority                       = bytestream2_get_byteu(gbc);
    dependency_counter             = bytestream2_get_byteu(gbc);
    if (indicator == NOT_FRAGMENTED || indicator == FIRST_FRAGMENT) {
        assembler->movie_fragment_sequence_number =
            movie_fragment_sequence_number;
        assembler->sample_number      = sample_number;
        assembler->offset             = offset;
        assembler->priority           = priority;
        assembler->dependency_counter = dependency_counter;
    } else if (assembler->state == IN_FRAGMENT &&
               (movie_fragment_sequence_number !=
                    assembler->movie_fragment_sequence_number ||
                sample_number != assembler->sample_number ||
                offset != assembler->offset ||
                priority != assembler->priority ||
                dependency_counter != assembler->dependency_counter)) {
        assembler->size = 0;
        assembler->state = SKIP;
        return AVERROR_INVALIDDATA;
    }
    ctx->current_movie_fragment_sequence_number =
        movie_fragment_sequence_number;
    ctx->current_sample_number      = sample_number;
    ctx->current_offset             = offset;
    ctx->current_priority           = priority;
    ctx->current_dependency_counter = dependency_counter;
    return assemble_fragment(
        assembler, seq_num, indicator,
        gbc->buffer, bytestream2_get_bytes_left(gbc),
        consume_mfu, ctx);
}

static int parse_mfu_non_timed_data(
    MMTPContext *ctx, struct FragmentAssembler *assembler,
    uint32_t seq_num, enum FragmentationIndicator indicator,
    GetByteContext *gbc)
{
    uint32_t item_id;

    ctx->current_mfu_timed = false;
    if (bytestream2_get_bytes_left(gbc) < 32 / 8)
        return AVERROR_INVALIDDATA;
    item_id = bytestream2_get_be32u(gbc);
    if (indicator == NOT_FRAGMENTED || indicator == FIRST_FRAGMENT) {
        assembler->item_id = item_id;
    } else if (assembler->state == IN_FRAGMENT &&
               item_id != assembler->item_id) {
        assembler->size = 0;
        assembler->state = SKIP;
        return AVERROR_INVALIDDATA;
    }
    ctx->current_item_id = item_id;
    return assemble_fragment(
        assembler, seq_num, indicator,
        gbc->buffer, bytestream2_get_bytes_left(gbc),
        consume_mfu, ctx);
}

typedef int (*MFUDataParser)(MMTPContext *ctx,
                             struct FragmentAssembler *assembler,
                             uint32_t seq_num,
                             enum FragmentationIndicator indicator,
                             GetByteContext *gbc);

static int parse_aggregated_mfu_data(
    MMTPContext *ctx, struct FragmentAssembler *assembler, uint32_t seq_num,
    GetByteContext *gbc, MFUDataParser parse_mfu_data)
{
    struct Streams *streams = find_current_stream(ctx);

    while (bytestream2_get_bytes_left(gbc) > 0) {
        GetByteContext item;
        uint16_t length;
        int ret;

        if (bytestream2_get_bytes_left(gbc) < 2)
            return AVERROR_INVALIDDATA;
        length = bytestream2_get_be16u(gbc);
        if (bytestream2_get_bytes_left(gbc) < length)
            return AVERROR_INVALIDDATA;

        if (streams) {
            if (streams->offset == -1)
                streams->offset = ctx->current_packet_pos;
            if (ctx->is_rap)
                streams->flags |= AV_PKT_FLAG_KEY;
        }
        bytestream2_init(&item, gbc->buffer, length);
        ret = parse_mfu_data(ctx, assembler, seq_num, NOT_FRAGMENTED, &item);
        if (ret < 0)
            return ret;
        bytestream2_skipu(gbc, length);
    }

    return 0;
}

static int parse_mpu(MMTPContext *ctx, uint32_t seq_num, GetByteContext *gbc)
{
    int                         err;
    uint8_t                     byte, fragment_type;
    bool                        timed_flag;
    enum FragmentationIndicator fragmentation_indicator;
    bool                        aggregation_flag;
    uint16_t                    length;
    uint32_t                    mpu_sequence_number;
    struct FragmentAssembler    *assembler;
    struct Streams              *streams;
    MFUDataParser               parse_mfu_data;

    if (bytestream2_get_bytes_left(gbc) < (16 + 4 + 1 + 2 + 1 + 8 + 32) / 8)
        return AVERROR_INVALIDDATA;

    length = bytestream2_get_be16u(gbc);
    if (length != bytestream2_get_bytes_left(gbc))
        return AVERROR_INVALIDDATA;

    byte                    = bytestream2_get_byteu(gbc);
    fragment_type           = byte >> 4;
    timed_flag              = (byte >> 3) & 1;
    fragmentation_indicator = (byte >> 1) & 0x03;
    aggregation_flag        = byte & 1;

    // skip: fragment_counter
    bytestream2_skipu(gbc, 1);

    mpu_sequence_number = bytestream2_get_be32u(gbc);

    if (aggregation_flag && fragmentation_indicator != NOT_FRAGMENTED)
        return AVERROR_INVALIDDATA; // cannot be both fragmented and aggregated

    streams = find_current_stream(ctx);
    if (fragment_type != 2) {
        if (streams != NULL &&
            streams->stream->discard < AVDISCARD_ALL) {
            if ((err = find_or_allocate_assembler(
                     ctx, ctx->current_pid, &assembler)) < 0)
                return err;
            check_state(ctx, assembler, seq_num);
        }
        return AVERROR_PATCHWELCOME;
    }

    if (streams == NULL || streams->stream->discard >= AVDISCARD_ALL)
        return 0;
    if ((err = find_or_allocate_assembler(
             ctx, ctx->current_pid, &assembler)) < 0)
        return err;

    if (!streams->has_last_sequence_number && !ctx->is_rap &&
        streams->stream->codecpar->codec_type != AVMEDIA_TYPE_DATA)
        return 0; // wait for the first RAP

    if (!check_state(ctx, assembler, seq_num))
        return 0;

    if (assembler->state == IN_FRAGMENT &&
        (fragmentation_indicator == NOT_FRAGMENTED ||
         fragmentation_indicator == FIRST_FRAGMENT)) {
        av_log(ctx->s, AV_LOG_WARNING,
               "New MFU starts before the previous MFU completed; "
               "drop %zu bytes\n", assembler->size);
        discard_fragment(ctx, assembler);
    }

    if (!streams->has_last_sequence_number) {
        streams->last_sequence_number = mpu_sequence_number;
        streams->has_last_sequence_number = true;
    } else if (mpu_sequence_number == streams->last_sequence_number + 1) {
        streams->last_sequence_number = mpu_sequence_number;
        streams->has_last_sequence_number = true;
        streams->au_count             = 0;
    } else if (mpu_sequence_number != streams->last_sequence_number) {
        av_log(streams->stream, AV_LOG_WARNING,
               "MPU sequence number jump: %u + 1 != %u\n",
               streams->last_sequence_number, mpu_sequence_number);
        streams->last_sequence_number = mpu_sequence_number;
        streams->has_last_sequence_number = true;
        streams->au_count             = 0;
    }

    if (streams->offset == -1)
        streams->offset = ctx->current_packet_pos;

    if (ctx->is_rap)
        streams->flags |= AV_PKT_FLAG_KEY;

    parse_mfu_data = timed_flag ? parse_mfu_timed_data
                                : parse_mfu_non_timed_data;
    if (aggregation_flag)
        return parse_aggregated_mfu_data(ctx, assembler, seq_num, gbc,
                                         parse_mfu_data);
    return parse_mfu_data(ctx, assembler, seq_num, fragmentation_indicator,
                          gbc);
}

static int emit_unknown_payload(MMTPContext *ctx, uint8_t payload_type,
                                uint8_t fec_type,
                                uint32_t packet_sequence_number,
                                GetByteContext *gbc)
{
    AVDictionary *metadata = NULL;
    uint8_t *packed_metadata;
    size_t metadata_size;
    int err;

    if (!ctx->pkt)
        return 0;
    if (!ctx->unknown_payload_stream) {
        AVStream *stream = avformat_new_stream(ctx->s, NULL);

        if (!stream)
            return AVERROR(ENOMEM);
        stream->id                   = 0x20000 | ctx->program->id;
        stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        stream->codecpar->codec_id   = AV_CODEC_ID_BIN_DATA;
        stream->codecpar->codec_tag  = MKTAG('m', 'm', 't', 'p');
        stream->disposition         |= AV_DISPOSITION_METADATA;
        avpriv_set_pts_info(stream, 64, 1, 1000);
        av_program_add_stream_index(ctx->s, ctx->program->id, stream->index);
        if ((err = av_dict_set(&stream->metadata, "title",
                               "Unparsed MMTP payloads", 0)) < 0)
            return err;
        ctx->unknown_payload_stream = stream;
    }
    if ((err = av_new_packet(ctx->pkt,
                             bytestream2_get_bytes_left(gbc))) < 0)
        return err;
    bytestream2_get_bufferu(gbc, ctx->pkt->data, ctx->pkt->size);
    if ((err = av_dict_set_int(&metadata, "mmt.payload_type",
                               payload_type, 0)) < 0 ||
        (err = av_dict_set_int(&metadata, "mmt.fec_type",
                               fec_type, 0)) < 0 ||
        (err = av_dict_set_int(&metadata, "mmt.packet_id",
                               ctx->current_pid, 0)) < 0 ||
        (err = av_dict_set_int(&metadata, "mmt.packet_sequence_number",
                               packet_sequence_number, 0)) < 0) {
        av_dict_free(&metadata);
        goto fail;
    }
    packed_metadata = av_packet_pack_dictionary(metadata, &metadata_size);
    av_dict_free(&metadata);
    if (!packed_metadata) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    if ((err = av_packet_add_side_data(ctx->pkt, AV_PKT_DATA_STRINGS_METADATA,
                                       packed_metadata, metadata_size)) < 0) {
        av_free(packed_metadata);
        goto fail;
    }
    ctx->pkt->stream_index = ctx->unknown_payload_stream->index;
    ctx->pkt->pts = short_ntp_to_milliseconds(ctx, ctx->current_timestamp);
    ctx->pkt->dts = ctx->pkt->pts;
    ctx->pkt->flags |= AV_PKT_FLAG_KEY;
    ctx->pkt = NULL;
    return 0;

fail:
    av_packet_unref(ctx->pkt);
    return err;
}

MMTPContext *ff_mmtp_parse_open(AVProgram *program)
{
    MMTPContext *ctx = av_mallocz(sizeof(MMTPContext));
    if (ctx == NULL)
        return NULL;
    ctx->program = program;
    return ctx;
}

int ff_mmtp_parse_packet(MMTPContext *ctx, AVFormatContext *s, AVPacket *pkt,
                         const uint8_t *buf, uint16_t size)
{
    bool     packet_counter_flag;
    bool     extension_header_flag;
    uint8_t  fec_type;
    uint8_t  payload_type;
    uint32_t packet_sequence_number;
    uint8_t  byte;
    int      err = 0;

    GetByteContext gbc;

    ctx->s   = s;
    ctx->pkt = pkt;
    ctx->current_packet_pos = pkt ? pkt->pos : -1;
    ctx->allow_packet_queue = !!pkt;

    bytestream2_init(&gbc, buf, size);
    if (bytestream2_get_bytes_left(&gbc) <
            (2 + 1 + 2 + 1 + 1 + 1 + 2 + 6 + 16 + 32 + 32) / 8 ||
        buf[0] >> 6)
        return AVERROR_INVALIDDATA;

    byte                  = bytestream2_get_byteu(&gbc);
    packet_counter_flag   = (byte >> 5) & 1;
    fec_type              = (byte >> 3) & 0x03;
    extension_header_flag = (byte >> 1) & 1;
    ctx->is_rap = byte & 1;

    byte         = bytestream2_get_byteu(&gbc);
    payload_type = byte & 0x3f;

    ctx->current_pid = bytestream2_get_be16u(&gbc);

    ctx->current_timestamp = bytestream2_get_be32u(&gbc);

    packet_sequence_number = bytestream2_get_be32u(&gbc);

    if (packet_counter_flag) {
        if (bytestream2_get_bytes_left(&gbc) < 4)
            return AVERROR_INVALIDDATA;
        bytestream2_skipu(&gbc, 4);
    }

    if (extension_header_flag) {
        uint16_t extension_header_length;

        if (bytestream2_get_bytes_left(&gbc) < 4)
            return AVERROR_INVALIDDATA;
        // skip: extension_type
        bytestream2_skipu(&gbc, 2);
        extension_header_length = bytestream2_get_be16u(&gbc);
        if (bytestream2_get_bytes_left(&gbc) < extension_header_length)
            return AVERROR_INVALIDDATA;
        bytestream2_skipu(&gbc, extension_header_length);
    }
    if (fec_type == 1) {
        if (bytestream2_get_bytes_left(&gbc) < 4)
            return AVERROR_INVALIDDATA;
        gbc.buffer_end -= 4;
    }

    if (fec_type >= 2)
        return emit_unknown_payload(ctx, payload_type, fec_type,
                                    packet_sequence_number, &gbc);

    switch (payload_type) {
    case 0x00: // MPU
        if (pkt != NULL) {
            GetByteContext raw_gbc = gbc;

            err = parse_mpu(ctx, packet_sequence_number, &gbc);
            if (err == AVERROR_PATCHWELCOME)
                err = emit_unknown_payload(ctx, payload_type, fec_type,
                                           packet_sequence_number, &raw_gbc);
        }
        break;
    case 0x02: // signalling messages
        err = parse_signalling_messages(ctx, packet_sequence_number, &gbc);
        break;
    default:
        err = emit_unknown_payload(ctx, payload_type, fec_type,
                                   packet_sequence_number, &gbc);
        break;
    }
    if (err < 0)
        return err;
    return (pkt == NULL || pkt->data != NULL) ? 0 : FFERROR_REDO;
}

int ff_mmtp_get_packet(MMTPContext *ctx, AVPacket *pkt)
{
    int err = ff_packet_list_get(&ctx->packet_queue, pkt);

    if (err >= 0) {
        av_assert0(ctx->queued_packets > 0);
        av_assert0(ctx->queued_packet_data >= (size_t) pkt->size);
        ctx->queued_packets--;
        ctx->queued_packet_data -= pkt->size;
    }
    return err;
}

void ff_mmtp_set_ntp_anchor(MMTPContext *ctx, uint64_t ntp_anchor)
{
    ctx->ntp_anchor = ntp_anchor;
}

uint64_t ff_mmtp_get_ntp_anchor(const MMTPContext *ctx)
{
    return ctx->ntp_anchor;
}

void ff_mmtp_reset_state(MMTPContext *ctx)
{
    struct Streams           *streams;
    struct FragmentAssembler *assembler;

    ff_packet_list_free(&ctx->packet_queue);
    ctx->queued_packets = 0;
    ctx->queued_packet_data = 0;
    ctx->ntp_anchor = 0;
    for (assembler = ctx->assembler;
         assembler != NULL; assembler = assembler->next) {
        assembler->state = INIT;
        assembler->size  = 0;
    }
    for (streams = ctx->streams; streams != NULL; streams = streams->next) {
        if (streams->parser != NULL) {
            av_parser_close(streams->parser);
            streams->parser = NULL;
        }
        streams->last_sequence_number = 0;
        streams->has_last_sequence_number = false;
        streams->au_count             = 0;
        streams->num_timestamp_descriptors = 0;
        streams->num_ext_timestamp_descriptors = 0;
        streams->subtitle_has_reference_time = false;
        streams->subtitle_reference_time = 0;
        streams->flags                = 0;
        streams->offset               = -1;
    }
}

void ff_mmtp_parse_close(MMTPContext *ctx)
{
    struct FragmentAssembler *ass;
    struct Streams           *streams;

    ff_packet_list_free(&ctx->packet_queue);
    for (ass = ctx->assembler; ass != NULL;) {
        struct FragmentAssembler *next = ass->next;
        av_free(ass->data);
        av_free(ass);
        ass = next;
    }

    for (streams = ctx->streams; streams != NULL;) {
        struct Streams *next = streams->next;
        if (streams->parser != NULL)
            av_parser_close(streams->parser);
        av_free(streams->timestamp_descriptor);
        av_free(streams->ext_timestamp_descriptor);
        av_free(streams->hevc_ps[0].data);
        av_free(streams->hevc_ps[1].data);
        av_free(streams->hevc_ps[2].data);
        av_free(streams);
        streams = next;
    }

    av_free(ctx);
}
