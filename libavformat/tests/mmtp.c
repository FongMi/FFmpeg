/*
 * Copyright (c) 2026 FongMi
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

#include <stdio.h>

#include "libavformat/mmtp.c"

extern const FFInputFormat ff_mmttlv_demuxer;

static int check_mmt_data_metadata(
    const AVPacket *pkt, const char *packet_id,
    const char *mpu_sequence_number, const char *timed, const char *item_id,
    const char *movie_fragment_sequence_number,
    const char *sample_number, const char *offset,
    const char *priority, const char *dependency_counter)
{
    const uint8_t *side_data;
    size_t side_data_size;
    AVDictionary *metadata = NULL;
    const AVDictionaryEntry *entry;
    int ret = 1;

    side_data = av_packet_get_side_data(pkt, AV_PKT_DATA_STRINGS_METADATA,
                                        &side_data_size);
    if (!side_data ||
        av_packet_unpack_dictionary(side_data, side_data_size, &metadata) < 0)
        goto end;
    entry = av_dict_get(metadata, "mmt.packet_id", NULL, 0);
    if (!entry || strcmp(entry->value, packet_id))
        goto end;
    entry = av_dict_get(metadata, "mmt.mpu_sequence_number", NULL, 0);
    if (!entry || strcmp(entry->value, mpu_sequence_number))
        goto end;
    entry = av_dict_get(metadata, "mmt.timed", NULL, 0);
    if (!entry || strcmp(entry->value, timed))
        goto end;
    entry = av_dict_get(metadata, "mmt.item_id", NULL, 0);
    if ((item_id && (!entry || strcmp(entry->value, item_id))) ||
        (!item_id && entry))
        goto end;
    entry = av_dict_get(metadata, "mmt.movie_fragment_sequence_number",
                        NULL, 0);
    if ((movie_fragment_sequence_number &&
         (!entry || strcmp(entry->value, movie_fragment_sequence_number))) ||
        (!movie_fragment_sequence_number && entry))
        goto end;
    entry = av_dict_get(metadata, "mmt.sample_number", NULL, 0);
    if ((sample_number && (!entry || strcmp(entry->value, sample_number))) ||
        (!sample_number && entry))
        goto end;
    entry = av_dict_get(metadata, "mmt.offset", NULL, 0);
    if ((offset && (!entry || strcmp(entry->value, offset))) ||
        (!offset && entry))
        goto end;
    entry = av_dict_get(metadata, "mmt.priority", NULL, 0);
    if ((priority && (!entry || strcmp(entry->value, priority))) ||
        (!priority && entry))
        goto end;
    entry = av_dict_get(metadata, "mmt.dependency_counter", NULL, 0);
    if ((dependency_counter &&
         (!entry || strcmp(entry->value, dependency_counter))) ||
        (!dependency_counter && entry))
        goto end;
    ret = 0;

end:
    av_dict_free(&metadata);
    return ret;
}

static int test_multiple_locations(void)
{
    static const uint8_t table[] = {
        MMT_PACKAGE_TABLE_ID, 0x00, 0x00, 0x1a,
        0x00,                         /* MPT mode */
        0x01, 0x01,                   /* package id */
        0x00, 0x00,                   /* package descriptors */
        0x01,                         /* number of assets */
        0x00, 0x00, 0x00, 0x00, 0x00, /* identifier type and scheme */
        0x00,                         /* asset id */
        'h', 'e', 'v', '1',
        0x00,                         /* asset clock relation */
        0x02,                         /* location count */
        0x00, 0x12, 0x34,             /* same MMTP flow */
        0x05, 0x01, 'x',              /* URL fallback */
        0x00, 0x00,                   /* asset descriptors */
    };
    AVFormatContext *format = avformat_alloc_context();
    AVProgram *program;
    MMTPContext *mmtp;
    GetByteContext gbc;
    int ret = 1;

    if (!format)
        return 1;
    program = av_new_program(format, 1);
    mmtp = program ? ff_mmtp_parse_open(program) : NULL;
    if (!mmtp)
        goto end;
    mmtp->s = format;
    bytestream2_init(&gbc, table, sizeof(table));

    if (parse_mmt_package_table(mmtp, &gbc) < 0 ||
        bytestream2_get_bytes_left(&gbc) ||
        format->nb_streams != 1 ||
        format->streams[0]->id != 0x1234 ||
        format->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
        format->streams[0]->codecpar->codec_id != AV_CODEC_ID_HEVC) {
        fprintf(stderr, "MPT did not retain the same-flow MMTP location\n");
        goto close;
    }
    ret = 0;

close:
    ff_mmtp_parse_close(mmtp);
end:
    avformat_free_context(format);
    return ret;
}

static int test_truncated_url(void)
{
    static const uint8_t missing_length[] = { 0x05 };
    static const uint8_t truncated_url[] = { 0x05, 0x02, 'x' };
    GetByteContext gbc;

    bytestream2_init(&gbc, missing_length, sizeof(missing_length));
    if (parse_mmt_general_location_info(&gbc, NULL) != AVERROR_INVALIDDATA) {
        fprintf(stderr, "URL location without a length was accepted\n");
        return 1;
    }

    bytestream2_init(&gbc, truncated_url, sizeof(truncated_url));
    if (parse_mmt_general_location_info(&gbc, NULL) != AVERROR_INVALIDDATA) {
        fprintf(stderr, "truncated URL location was accepted\n");
        return 1;
    }
    return 0;
}

static int test_ip_locations(void)
{
    static const uint8_t ipv4[] = {
        0x01,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x12, 0x34,
    };
    static const uint8_t ipv6[] = {
        0x02,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x56, 0x78,
    };
    GetByteContext gbc;
    int packet_id;

    bytestream2_init(&gbc, ipv4, sizeof(ipv4));
    if (parse_mmt_general_location_info(&gbc, &packet_id) < 0 ||
        packet_id != 0x1234 || bytestream2_get_bytes_left(&gbc)) {
        fprintf(stderr, "IPv4 MMT location packet ID was not retained\n");
        return 1;
    }

    bytestream2_init(&gbc, ipv6, sizeof(ipv6));
    if (parse_mmt_general_location_info(&gbc, &packet_id) < 0 ||
        packet_id != 0x5678 || bytestream2_get_bytes_left(&gbc)) {
        fprintf(stderr, "IPv6 MMT location packet ID was not retained\n");
        return 1;
    }
    return 0;
}

static int test_video_asset_types(void)
{
    static const uint8_t table[] = {
        MMT_PACKAGE_TABLE_ID, 0x00, 0x00, 0x4a,
        0x00,                         /* MPT mode */
        0x01, 0x01,                   /* package id */
        0x00, 0x00,                   /* package descriptors */
        0x04,                         /* number of assets */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'h', 'v', 'c', '1', 0x00, 0x01, 0x00, 0x10, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'a', 'v', 'c', '1', 0x00, 0x01, 0x00, 0x10, 0x02, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'a', 'v', 'c', '3', 0x00, 0x01, 0x00, 0x10, 0x03, 0x00, 0x00,
    };
    AVFormatContext *format = avformat_alloc_context();
    AVProgram *program;
    MMTPContext *mmtp;
    GetByteContext gbc;
    int ret = 1;

    if (!format)
        return 1;
    program = av_new_program(format, 1);
    mmtp = program ? ff_mmtp_parse_open(program) : NULL;
    if (!mmtp)
        goto end;
    mmtp->s = format;
    bytestream2_init(&gbc, table, sizeof(table));

    if (parse_mmt_package_table(mmtp, &gbc) < 0 ||
        bytestream2_get_bytes_left(&gbc) ||
        format->nb_streams != 3 ||
        format->streams[0]->codecpar->codec_id != AV_CODEC_ID_HEVC ||
        format->streams[0]->codecpar->codec_tag != MKTAG('h', 'v', 'c', '1') ||
        format->streams[1]->codecpar->codec_id != AV_CODEC_ID_H264 ||
        format->streams[1]->codecpar->codec_tag != MKTAG('a', 'v', 'c', '1') ||
        format->streams[2]->codecpar->codec_id != AV_CODEC_ID_H264 ||
        format->streams[2]->codecpar->codec_tag != MKTAG('a', 'v', 'c', '3')) {
        fprintf(stderr, "MMT video asset types were not mapped correctly\n");
        goto close;
    }
    ret = 0;

close:
    ff_mmtp_parse_close(mmtp);
end:
    avformat_free_context(format);
    return ret;
}

static int test_audio_codec_support(void)
{
    static const uint8_t latm_descriptor[] = {
        0x80, 0x14, 0x0a,
        0x03, 0x00, 0x00, 0x00, 0x11, 0x00, 0x0e, 'j', 'p', 'n',
    };
    static const uint8_t raw_aac_descriptor[] = {
        0x80, 0x14, 0x0a,
        0x03, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x0e, 'j', 'p', 'n',
    };
    static const uint8_t raw_aac_config_descriptor[] = {
        0x80, 0x09, 0x05, 0x81, 0x2a, 0x02, 0x11, 0x90,
    };
    static const uint8_t truncated_config_descriptor[] = {
        0x80, 0x09, 0x04, 0x80, 0x03, 0x11, 0x90,
    };
    static const uint8_t als_descriptor[] = {
        0x80, 0x14, 0x0a,
        0x04, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x0e, 'j', 'p', 'n',
    };
    static const uint8_t als_config_descriptor[] = {
        0x80, 0x09, 0x04, 0x80, 0x02, 0xf8, 0x80,
    };
    static const uint8_t no_config_descriptor[] = {
        0x80, 0x09, 0x01, 0x00,
    };
    static const uint8_t unsupported_descriptor[] = {
        0x80, 0x14, 0x0a,
        0x04, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 'j', 'p', 'n',
    };
    AVCodecParameters codecpar = { 0 };
    AVStream stream = {
        .codecpar = &codecpar,
        .discard  = AVDISCARD_ALL,
    };
    struct Streams streams = { .stream = &stream };
    GetByteContext gbc;
    int ret = 1;

    bytestream2_init(&gbc, latm_descriptor, sizeof(latm_descriptor));
    if (parse_mh_audio_component_descriptor(&streams, &gbc) < 0 ||
        codecpar.codec_id != AV_CODEC_ID_AAC_LATM ||
        codecpar.sample_rate != 48000 ||
        stream.discard != AVDISCARD_DEFAULT) {
        fprintf(stderr, "AAC-LATM audio descriptor was not enabled\n");
        goto end;
    }

    bytestream2_init(&gbc, raw_aac_descriptor,
                     sizeof(raw_aac_descriptor));
    if (parse_mh_audio_component_descriptor(&streams, &gbc) < 0 ||
        codecpar.codec_type != AVMEDIA_TYPE_AUDIO ||
        codecpar.codec_id != AV_CODEC_ID_AAC ||
        codecpar.sample_rate != 48000 ||
        stream.discard != AVDISCARD_DEFAULT) {
        fprintf(stderr, "raw AAC audio descriptor was not enabled\n");
        goto end;
    }
    bytestream2_init(&gbc, raw_aac_config_descriptor,
                     sizeof(raw_aac_config_descriptor));
    if (parse_mpt_descriptor(&streams, &gbc) < 0 ||
        codecpar.extradata_size != 2 ||
        memcmp(codecpar.extradata, "\x11\x90", 2)) {
        fprintf(stderr, "raw AAC AudioSpecificConfig was not retained\n");
        goto end;
    }
    bytestream2_init(&gbc, truncated_config_descriptor,
                     sizeof(truncated_config_descriptor));
    if (parse_mpt_descriptor(&streams, &gbc) >= 0 ||
        codecpar.extradata_size != 2 ||
        memcmp(codecpar.extradata, "\x11\x90", 2)) {
        fprintf(stderr, "truncated AudioSpecificConfig was accepted\n");
        goto end;
    }

    streams.audio_specific_config_descriptor_seen = false;
    bytestream2_init(&gbc, als_config_descriptor,
                     sizeof(als_config_descriptor));
    if (parse_mpt_descriptor(&streams, &gbc) < 0)
        goto end;
    bytestream2_init(&gbc, als_descriptor, sizeof(als_descriptor));
    if (parse_mh_audio_component_descriptor(&streams, &gbc) < 0 ||
        codecpar.codec_type != AVMEDIA_TYPE_AUDIO ||
        codecpar.codec_id != AV_CODEC_ID_MP4ALS ||
        codecpar.sample_rate != 48000 ||
        codecpar.extradata_size != 2 ||
        memcmp(codecpar.extradata, "\xf8\x80", 2) ||
        stream.discard != AVDISCARD_DEFAULT) {
        fprintf(stderr, "MPEG-4 ALS audio descriptor was not enabled\n");
        goto end;
    }

    streams.audio_specific_config_descriptor_seen = false;
    bytestream2_init(&gbc, unsupported_descriptor,
                     sizeof(unsupported_descriptor));
    if (parse_mh_audio_component_descriptor(&streams, &gbc) < 0 ||
        codecpar.codec_type != AVMEDIA_TYPE_DATA ||
        codecpar.codec_id != AV_CODEC_ID_BIN_DATA ||
        codecpar.extradata || codecpar.extradata_size ||
        stream.discard != AVDISCARD_DEFAULT) {
        fprintf(stderr, "unsupported MMT audio was not preserved as data\n");
        goto end;
    }
    bytestream2_init(&gbc, als_config_descriptor,
                     sizeof(als_config_descriptor));
    if (parse_mpt_descriptor(&streams, &gbc) < 0 ||
        codecpar.extradata_size != 2)
        goto end;
    bytestream2_init(&gbc, no_config_descriptor,
                     sizeof(no_config_descriptor));
    if (parse_mpt_descriptor(&streams, &gbc) < 0 ||
        codecpar.extradata || codecpar.extradata_size) {
        fprintf(stderr, "absent AudioSpecificConfig did not clear extradata\n");
        goto end;
    }
    ret = 0;

end:
    av_freep(&codecpar.extradata);
    av_dict_free(&stream.metadata);
    return ret;
}

static int test_subtitle_codec_support(void)
{
    static const uint8_t unsupported[] = {
        0x30, 0x00, 'j', 'p', 'n', 0x04, 0x00, 0x00,
    };
    static const uint8_t supported[] = {
        0x30, 0x00, 'j', 'p', 'n', 0x00, 0x00, 0x00,
    };
    AVCodecParameters codecpar = { 0 };
    AVStream stream = { .codecpar = &codecpar };
    struct Streams streams = { .stream = &stream };
    GetByteContext gbc;
    int ret = 1;

    bytestream2_init(&gbc, unsupported, sizeof(unsupported));
    if (parse_additional_arib_subtitle_info(&streams, &gbc) < 0 ||
        codecpar.codec_type != AVMEDIA_TYPE_DATA ||
        codecpar.codec_id != AV_CODEC_ID_BIN_DATA ||
        stream.disposition & AV_DISPOSITION_CAPTIONS) {
        fprintf(stderr, "unsupported ARIB-TTML was not preserved as data\n");
        goto end;
    }

    bytestream2_init(&gbc, supported, sizeof(supported));
    if (parse_additional_arib_subtitle_info(&streams, &gbc) < 0 ||
        codecpar.codec_type != AVMEDIA_TYPE_SUBTITLE ||
        codecpar.codec_id != AV_CODEC_ID_TTML ||
        codecpar.width != 1920 || codecpar.height != 1080 ||
        !(stream.disposition & AV_DISPOSITION_CAPTIONS)) {
        fprintf(stderr, "uncompressed ARIB-TTML was not enabled\n");
        goto end;
    }
    ret = 0;

end:
    av_dict_free(&stream.metadata);
    return ret;
}

static int test_repeated_mpt_descriptor_omission(void)
{
    static const uint8_t initial_table[] = {
        MMT_PACKAGE_TABLE_ID, 0x00, 0x00, 0x4a,
        0x00, 0x01, 0x01, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'm', 'p', '4', 'a', 0x00, 0x01, 0x00, 0x10, 0x01, 0x00, 0x15,
        0x80, 0x14, 0x0a,
        0x03, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x0e, 'j', 'p', 'n',
        0x80, 0x09, 0x05, 0x81, 0x2a, 0x02, 0x11, 0x90,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        's', 't', 'p', 'p', 0x00, 0x01, 0x00, 0x10, 0x02, 0x00, 0x0d,
        0x80, 0x20, 0x0a, 0x00, 0x20,
        0x30, 0x00, 'j', 'p', 'n', 0x00, 0x00, 0x00,
    };
    static const uint8_t repeated_table[] = {
        MMT_PACKAGE_TABLE_ID, 0x01, 0x00, 0x28,
        0x00, 0x01, 0x01, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'm', 'p', '4', 'a', 0x00, 0x01, 0x00, 0x10, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        's', 't', 'p', 'p', 0x00, 0x01, 0x00, 0x10, 0x02, 0x00, 0x00,
    };
    AVFormatContext *format = avformat_alloc_context();
    AVProgram *program;
    MMTPContext *mmtp;
    GetByteContext gbc;
    int ret = 1;

    if (!format)
        return 1;
    program = av_new_program(format, 1);
    mmtp = program ? ff_mmtp_parse_open(program) : NULL;
    if (!mmtp)
        goto end;
    mmtp->s = format;
    bytestream2_init(&gbc, initial_table, sizeof(initial_table));
    if (parse_mmt_package_table(mmtp, &gbc) < 0 ||
        format->nb_streams != 2 ||
        format->streams[0]->codecpar->codec_id != AV_CODEC_ID_AAC ||
        format->streams[0]->codecpar->extradata_size != 2 ||
        format->streams[1]->codecpar->codec_id != AV_CODEC_ID_TTML) {
        fprintf(stderr, "initial MPT descriptors were not applied\n");
        goto close;
    }

    bytestream2_init(&gbc, repeated_table, sizeof(repeated_table));
    if (parse_mmt_package_table(mmtp, &gbc) < 0 ||
        format->streams[0]->codecpar->codec_id != AV_CODEC_ID_AAC ||
        format->streams[0]->codecpar->extradata_size != 2 ||
        memcmp(format->streams[0]->codecpar->extradata, "\x11\x90", 2) ||
        format->streams[1]->codecpar->codec_id != AV_CODEC_ID_TTML) {
        fprintf(stderr, "repeated MPT omitted retained descriptors\n");
        goto close;
    }
    ret = 0;

close:
    ff_mmtp_parse_close(mmtp);
end:
    avformat_free_context(format);
    return ret;
}

static int test_application_asset_types(void)
{
    static const uint8_t table[] = {
        MMT_PACKAGE_TABLE_ID, 0x00, 0x00, 0x39,
        0x00,                         /* MPT mode */
        0x01, 0x01,                   /* package id */
        0x00, 0x00,                   /* package descriptors */
        0x03,                         /* number of assets */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'a', 'a', 'p', 'p', 0x00, 0x01, 0x00, 0x10, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'a', 's', 'g', 'd', 0x00, 0x01, 0x00, 0x10, 0x02, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'a', 'a', 'g', 'd', 0x00, 0x01, 0x00, 0x10, 0x03, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'z', 'z', 'z', 'z', 0x00, 0x01, 0x00, 0x10, 0x04, 0x00, 0x00,
    };
    AVFormatContext *format = avformat_alloc_context();
    AVProgram *program;
    MMTPContext *mmtp;
    GetByteContext gbc;
    int i;
    int ret = 1;

    if (!format)
        return 1;
    program = av_new_program(format, 1);
    mmtp = program ? ff_mmtp_parse_open(program) : NULL;
    if (!mmtp)
        goto end;
    mmtp->s = format;
    bytestream2_init(&gbc, table, sizeof(table));

    if (parse_mmt_package_table(mmtp, &gbc) < 0 ||
        bytestream2_get_bytes_left(&gbc) ||
        format->nb_streams != 4) {
        fprintf(stderr, "MMT application asset types were not discovered\n");
        goto close;
    }
    for (i = 0; i < 4; i++) {
        AVStream *stream = format->streams[i];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_DATA ||
            stream->codecpar->codec_id != AV_CODEC_ID_BIN_DATA ||
            stream->time_base.num != 1 || stream->time_base.den != 1000) {
            fprintf(stderr, "MMT application asset stream %d was not mapped correctly\n", i);
            goto close;
        }
    }
    if (format->streams[0]->codecpar->codec_tag != MKTAG('a', 'a', 'p', 'p') ||
        format->streams[1]->codecpar->codec_tag != MKTAG('a', 's', 'g', 'd') ||
        format->streams[2]->codecpar->codec_tag != MKTAG('a', 'a', 'g', 'd') ||
        format->streams[3]->codecpar->codec_tag != MKTAG('z', 'z', 'z', 'z')) {
        fprintf(stderr, "MMT application asset FourCCs were not retained\n");
        goto close;
    }
    ret = 0;

close:
    ff_mmtp_parse_close(mmtp);
end:
    avformat_free_context(format);
    return ret;
}

static int test_ip_delivery_is_ignorable(void)
{
    static const uint8_t table[] = {
        PACKAGE_LIST_TABLE_ID, 0x00, 0x00, 0x02,
        0x00, /* packages */
        0x01, /* unsupported IP delivery entries */
    };
    GetByteContext gbc;

    bytestream2_init(&gbc, table, sizeof(table));
    if (parse_package_list_table(&gbc) < 0 ||
        bytestream2_get_bytes_left(&gbc)) {
        fprintf(stderr, "ignorable IP delivery data rejected the package list\n");
        return 1;
    }
    return 0;
}

static int test_pa_table_boundaries(void)
{
    static const uint8_t message[] = {
        PA_MESSAGE_ID >> 8, PA_MESSAGE_ID & 0xff, 0x00,
        0x00, 0x00, 0x00, 0x2b,       /* message payload length */
        0x02,                         /* number of tables */
        0x99, 0x00, 0x00, 0x00,       /* unknown table */
        MMT_PACKAGE_TABLE_ID, 0x00, 0x00, 0x1a,
        0x99, 0x00, 0x00, 0x00,       /* unknown table data */
        MMT_PACKAGE_TABLE_ID, 0x00, 0x00, 0x1a,
        0x00,                         /* MPT mode */
        0x01, 0x01,                   /* package id */
        0x00, 0x00,                   /* package descriptors */
        0x01,                         /* number of assets */
        0x00, 0x00, 0x00, 0x00, 0x00, /* identifier type and scheme */
        0x00,                         /* asset id */
        'h', 'e', 'v', '1',
        0x00,                         /* asset clock relation */
        0x02,                         /* location count */
        0x00, 0x12, 0x34,             /* same MMTP flow */
        0x05, 0x01, 'x',              /* URL fallback */
        0x00, 0x00,                   /* asset descriptors */
    };
    static const uint8_t direct_message[] = {
        PA_MESSAGE_ID >> 8, PA_MESSAGE_ID & 0xff, 0x00,
        0x00, 0x00, 0x00, 0x1f,       /* message payload length */
        0x00,                         /* empty table index */
        MMT_PACKAGE_TABLE_ID, 0x00, 0x00, 0x1a,
        0x00,                         /* MPT mode */
        0x01, 0x01,                   /* package id */
        0x00, 0x00,                   /* package descriptors */
        0x01,                         /* number of assets */
        0x00, 0x00, 0x00, 0x00, 0x00, /* identifier type and scheme */
        0x00,                         /* asset id */
        'h', 'e', 'v', '1',
        0x00,                         /* asset clock relation */
        0x02,                         /* location count */
        0x00, 0x56, 0x78,             /* same MMTP flow */
        0x05, 0x01, 'x',              /* URL fallback */
        0x00, 0x00,                   /* asset descriptors */
    };
    AVFormatContext *format = avformat_alloc_context();
    AVProgram *program;
    MMTPContext *mmtp;
    GetByteContext gbc;
    int ret = 1;

    if (!format)
        return 1;
    program = av_new_program(format, 1);
    mmtp = program ? ff_mmtp_parse_open(program) : NULL;
    if (!mmtp)
        goto end;
    mmtp->s = format;
    bytestream2_init(&gbc, message, sizeof(message));

    if (parse_pa_message(mmtp, &gbc) < 0 ||
        bytestream2_get_bytes_left(&gbc) ||
        format->nb_streams != 1 || format->streams[0]->id != 0x1234) {
        fprintf(stderr, "unknown PA table consumed the following MPT\n");
        goto close;
    }

    bytestream2_init(&gbc, direct_message, sizeof(direct_message));
    if (parse_pa_message(mmtp, &gbc) < 0 ||
        bytestream2_get_bytes_left(&gbc) ||
        format->nb_streams != 2 || format->streams[1]->id != 0x5678) {
        fprintf(stderr, "directly embedded PA table was not parsed\n");
        goto close;
    }
    ret = 0;

close:
    ff_mmtp_parse_close(mmtp);
end:
    avformat_free_context(format);
    return ret;
}

static int test_sequence_wrap(void)
{
    if (sequence_is_before(0, UINT32_MAX) ||
        !sequence_is_before(UINT32_MAX, 0) ||
        sequence_distance(0, UINT32_MAX) != 1) {
        fprintf(stderr, "MMTP sequence comparison is not wrap-aware\n");
        return 1;
    }
    return 0;
}

static int test_timestamp_descriptor_reuse(void)
{
    struct Streams streams = { 0 };
    struct MPUTimestampDescriptor *timestamp = NULL;
    struct MPUTimestampDescriptor *original_timestamp;
    struct MPUExtendedTimestampDescriptor *extended = NULL;
    struct MPUExtendedTimestampDescriptor *original_extended;
    int ret = 1;

    if (get_timestamp_descriptor(&streams, UINT32_MAX, &timestamp) < 0 ||
        get_extended_timestamp_descriptor(&streams, UINT32_MAX, &extended) < 0 ||
        !timestamp || !extended)
        goto end;
    original_timestamp = timestamp;
    original_extended  = extended;

    streams.last_sequence_number = 0;
    streams.has_last_sequence_number = true;
    if (get_timestamp_descriptor(&streams, UINT32_MAX, &timestamp) < 0 ||
        get_extended_timestamp_descriptor(&streams, UINT32_MAX, &extended) < 0 ||
        timestamp || extended)
        goto end;
    if (get_timestamp_descriptor(&streams, 0, &timestamp) < 0 ||
        get_extended_timestamp_descriptor(&streams, 0, &extended) < 0 ||
        timestamp != original_timestamp || extended != original_extended ||
        timestamp->seq_num != 0 || extended->seq_num != 0)
        goto end;

    ret = 0;
end:
    if (ret)
        fprintf(stderr, "timestamp descriptor cache was not reused safely\n");
    av_freep(&streams.timestamp_descriptor);
    av_freep(&streams.ext_timestamp_descriptor);
    return ret;
}

static int test_missing_timestamp_descriptor_fallback(void)
{
    AVCodecParserContext parser = { 0 };
    AVStream stream = {
        .time_base = { 1, 90000 },
    };
    MMTPContext mmtp = {
        .current_timestamp = 0x7e910000,
        .ntp_anchor        = UINT64_C(0x83aa7e9000000000),
    };
    struct Streams streams = {
        .stream = &stream,
        .owner  = &mmtp,
        .parser = &parser,
    };

    fill_pts_dts(&streams);
    if (parser.pts != 1530000 || parser.dts != parser.pts ||
        streams.au_count != 1) {
        fprintf(stderr, "short NTP timestamp fallback was not applied\n");
        return 1;
    }

    mmtp.ntp_anchor = 0;
    fill_pts_dts(&streams);
    if (parser.pts != AV_NOPTS_VALUE || parser.dts != AV_NOPTS_VALUE ||
        streams.au_count != 2) {
        fprintf(stderr, "missing timestamp anchor caused a packet drop\n");
        return 1;
    }
    return 0;
}

static int test_fixed_interval_timestamp_descriptor(void)
{
    AVCodecParserContext parser = { 0 };
    AVCodecParameters codecpar = {
        .codec_type = AVMEDIA_TYPE_VIDEO,
    };
    AVStream stream = {
        .codecpar       = &codecpar,
        .time_base      = { 1, 90000 },
        .avg_frame_rate = { 30000, 1001 },
    };
    MMTPContext mmtp = { 0 };
    struct MPUTimestampDescriptor timestamp = {
        .seq_num = 7,
        .presentation_time = (UINT64_C(2208988800) + 100) << 32,
    };
    struct MPUExtendedTimestampDescriptor extended = {
        .seq_num        = 7,
        .num_of_au      = 2,
        .pts_offset_type = 0,
    };
    struct Streams streams = {
        .stream                        = &stream,
        .owner                         = &mmtp,
        .parser                        = &parser,
        .num_timestamp_descriptors     = 1,
        .timestamp_descriptor          = &timestamp,
        .num_ext_timestamp_descriptors = 1,
        .ext_timestamp_descriptor      = &extended,
        .last_sequence_number          = 7,
    };

    fill_pts_dts(&streams);
    if (parser.pts != 9000000 || parser.dts != parser.pts)
        goto fail;
    fill_pts_dts(&streams);
    if (parser.pts != 9003003 || parser.dts != parser.pts ||
        streams.au_count != 2)
        goto fail;
    return 0;

fail:
    fprintf(stderr, "fixed-interval timestamp descriptor was not applied\n");
    return 1;
}

static int test_fragmented_timed_mfu(void)
{
    static const uint8_t document[] = "<tt><body/></tt>";
    uint8_t caption[7 + sizeof(document) - 1] = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, sizeof(document) - 1,
    };
    uint8_t first[14 + 5] = { 0 };
    uint8_t last[14 + sizeof(caption) - 5] = { 0 };
    uint8_t mpu_metadata[8] = { 0 };
    uint8_t aggregated[10 + 14 + sizeof(caption)] = { 0 };
    AVFormatContext *format = avformat_alloc_context();
    struct FragmentAssembler *assembler;
    struct Streams *stream;
    AVProgram *program;
    MMTPContext *mmtp;
    GetByteContext gbc;
    AVPacket pkt = { 0 };
    int ret = 1;

    memcpy(caption + 7, document, sizeof(document) - 1);
    AV_WB32(first, 0x12345678);
    AV_WB32(first + 4, 0x87654321);
    AV_WB32(first + 8, 0x01020304);
    first[12] = 0xfe;
    first[13] = 0x7f;
    memcpy(first + 14, caption, 5);
    memcpy(last, first, 14);
    memcpy(last + 14, caption + 5, sizeof(caption) - 5);
    AV_WB16(mpu_metadata, sizeof(mpu_metadata) - 2);
    mpu_metadata[2] = 0x10; /* movie fragment metadata, not MFU */
    AV_WB32(mpu_metadata + 4, 1);
    AV_WB16(aggregated, sizeof(aggregated) - 2);
    aggregated[2] = 0x29; /* timed, not fragmented, aggregated MFU */
    AV_WB32(aggregated + 4, 3);
    AV_WB16(aggregated + 8, sizeof(aggregated) - 10);
    memcpy(aggregated + 24, caption, sizeof(caption));

    if (!format)
        return 1;
    program = av_new_program(format, 1);
    mmtp = program ? ff_mmtp_parse_open(program) : NULL;
    if (!mmtp)
        goto end;
    mmtp->s = format;
    mmtp->pkt = &pkt;
    mmtp->current_pid = 0x1234;
    pkt.pos = 42;
    if (find_or_allocate_stream(mmtp, mmtp->current_pid, &stream) < 0 ||
        find_or_allocate_assembler(mmtp, mmtp->current_pid, &assembler) < 0)
        goto close;
    stream->stream->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    stream->stream->codecpar->codec_id = AV_CODEC_ID_TTML;
    avpriv_set_pts_info(stream->stream, 64, 1, 1000);

    if (!check_state(mmtp, assembler, 1))
        goto close;
    bytestream2_init(&gbc, first, sizeof(first));
    if (parse_mfu_timed_data(mmtp, assembler, 1, FIRST_FRAGMENT, &gbc) < 0)
        goto close;
    if (check_state(mmtp, assembler, 1)) {
        fprintf(stderr, "duplicate MMTP packet was not ignored\n");
        goto close;
    }
    bytestream2_init(&gbc, mpu_metadata, sizeof(mpu_metadata));
    if (parse_mpu(mmtp, 2, &gbc) != AVERROR_PATCHWELCOME)
        goto close;
    if (!check_state(mmtp, assembler, 3))
        goto close;
    bytestream2_init(&gbc, last, sizeof(last));
    if (parse_mfu_timed_data(mmtp, assembler, 3, LAST_FRAGMENT, &gbc) < 0 ||
        pkt.size != sizeof(document) - 1 ||
        memcmp(pkt.data, document, sizeof(document) - 1)) {
        fprintf(stderr, "fragmented 14-byte timed MFU was not reassembled\n");
        goto close;
    }

    av_packet_unref(&pkt);
    ff_mmtp_reset_state(mmtp);
    mmtp->pkt = &pkt;
    if (!check_state(mmtp, assembler, 4))
        goto close;
    bytestream2_init(&gbc, first, sizeof(first));
    if (parse_mfu_timed_data(mmtp, assembler, 4, FIRST_FRAGMENT, &gbc) < 0)
        goto close;
    AV_WB32(last + 4, 0x87654322);
    if (!check_state(mmtp, assembler, 5))
        goto close;
    bytestream2_init(&gbc, last, sizeof(last));
    if (parse_mfu_timed_data(
            mmtp, assembler, 5, LAST_FRAGMENT, &gbc) != AVERROR_INVALIDDATA ||
        pkt.data) {
        fprintf(stderr, "timed MFU fragments with different samples were joined\n");
        goto close;
    }

    ff_mmtp_reset_state(mmtp);
    mmtp->pkt = &pkt;
    mmtp->is_rap = 1;
    pkt.pos = 43;
    bytestream2_init(&gbc, aggregated, sizeof(aggregated));
    if (parse_mpu(mmtp, 6, &gbc) < 0 ||
        pkt.size != sizeof(document) - 1 ||
        memcmp(pkt.data, document, sizeof(document) - 1)) {
        fprintf(stderr, "aggregated timed MFU was not parsed\n");
        goto close;
    }
    ret = 0;

close:
    av_packet_unref(&pkt);
    ff_mmtp_parse_close(mmtp);
end:
    avformat_free_context(format);
    return ret;
}

static int test_new_mfu_resets_fragment_packet_state(void)
{
    static const uint8_t document[] = "<tt><body/></tt>";
    uint8_t caption[7 + sizeof(document) - 1] = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, sizeof(document) - 1,
    };
    uint8_t first[8 + 14 + 5] = { 0 };
    uint8_t complete[8 + 14 + sizeof(caption)] = { 0 };
    AVFormatContext *format = avformat_alloc_context();
    struct Streams *stream;
    AVProgram *program;
    MMTPContext *mmtp;
    GetByteContext gbc;
    AVPacket pkt = { 0 };
    int ret = 1;

    memcpy(caption + 7, document, sizeof(document) - 1);
    AV_WB16(first, sizeof(first) - 2);
    first[2] = 0x2a; /* timed, first MFU fragment */
    AV_WB32(first + 4, 1);
    AV_WB32(first + 8, 2);
    AV_WB32(first + 12, 3);
    memcpy(first + 22, caption, 5);

    AV_WB16(complete, sizeof(complete) - 2);
    complete[2] = 0x28; /* timed, complete MFU */
    AV_WB32(complete + 4, 2);
    AV_WB32(complete + 8, 4);
    AV_WB32(complete + 12, 5);
    memcpy(complete + 22, caption, sizeof(caption));

    if (!format)
        return 1;
    program = av_new_program(format, 1);
    mmtp = program ? ff_mmtp_parse_open(program) : NULL;
    if (!mmtp)
        goto end;
    mmtp->s = format;
    mmtp->pkt = &pkt;
    mmtp->current_pid = 0x1234;
    if (find_or_allocate_stream(mmtp, mmtp->current_pid, &stream) < 0)
        goto close;
    stream->stream->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    stream->stream->codecpar->codec_id = AV_CODEC_ID_TTML;
    avpriv_set_pts_info(stream->stream, 64, 1, 1000);

    mmtp->is_rap = 1;
    mmtp->current_packet_pos = 10;
    bytestream2_init(&gbc, first, sizeof(first));
    if (parse_mpu(mmtp, 1, &gbc) < 0)
        goto close;

    mmtp->is_rap = 0;
    mmtp->current_packet_pos = 20;
    bytestream2_init(&gbc, complete, sizeof(complete));
    if (parse_mpu(mmtp, 2, &gbc) < 0 ||
        pkt.size != sizeof(document) - 1 ||
        pkt.pos != 20 || (pkt.flags & AV_PKT_FLAG_KEY)) {
        fprintf(stderr, "replacement MFU inherited stale packet state\n");
        goto close;
    }
    ret = 0;

close:
    av_packet_unref(&pkt);
    ff_mmtp_parse_close(mmtp);
end:
    avformat_free_context(format);
    return ret;
}

static int test_non_rap_aggregated_application_mfus(void)
{
    static const uint8_t first_data[] = { 0x78, 0x9c, 0x01, 0x02 };
    static const uint8_t second_data[] = { 0x03, 0x04, 0x05 };
    uint8_t mpu[8 + 2 + 4 + sizeof(first_data) +
                2 + 4 + sizeof(second_data)] = { 0 };
    AVFormatContext *format = avformat_alloc_context();
    struct Streams *stream;
    AVProgram *program;
    MMTPContext *mmtp;
    GetByteContext gbc;
    AVPacket pkt = { 0 };
    int pos = 8;
    int ret = 1;

    AV_WB16(mpu, sizeof(mpu) - 2);
    mpu[2] = 0x21; /* non-timed, complete, aggregated MFUs */
    AV_WB32(mpu + 4, UINT32_MAX - 1);
    AV_WB16(mpu + pos, 4 + sizeof(first_data));
    pos += 2;
    AV_WB32(mpu + pos, 0x12345678);
    pos += 4;
    memcpy(mpu + pos, first_data, sizeof(first_data));
    pos += sizeof(first_data);
    AV_WB16(mpu + pos, 4 + sizeof(second_data));
    pos += 2;
    AV_WB32(mpu + pos, 0x87654321);
    pos += 4;
    memcpy(mpu + pos, second_data, sizeof(second_data));
    av_assert0(pos + sizeof(second_data) == sizeof(mpu));

    if (!format)
        return 1;
    program = av_new_program(format, 1);
    mmtp = program ? ff_mmtp_parse_open(program) : NULL;
    if (!mmtp)
        goto end;
    mmtp->s = format;
    mmtp->pkt = &pkt;
    mmtp->allow_packet_queue = true;
    mmtp->current_pid = 0x1234;
    mmtp->is_rap = 0;
    pkt.pos = 42;
    mmtp->current_packet_pos = pkt.pos;
    if (find_or_allocate_stream(mmtp, mmtp->current_pid, &stream) < 0)
        goto close;
    stream->stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    stream->stream->codecpar->codec_id = AV_CODEC_ID_BIN_DATA;
    avpriv_set_pts_info(stream->stream, 64, 1, 1000);

    bytestream2_init(&gbc, mpu, sizeof(mpu));
    if (parse_mpu(mmtp, 1, &gbc) < 0 ||
        pkt.size != sizeof(first_data) ||
        memcmp(pkt.data, first_data, sizeof(first_data)) ||
        !(pkt.flags & AV_PKT_FLAG_KEY)) {
        fprintf(stderr, "first aggregated MMT application MFU was not emitted\n");
        goto close;
    }
    if (check_mmt_data_metadata(
            &pkt, "4660", "4294967294", "0", "305419896", NULL, NULL, NULL,
            NULL, NULL)) {
        fprintf(stderr, "first MMT application identifiers were not preserved\n");
        goto close;
    }
    av_packet_unref(&pkt);

    if (ff_mmtp_get_packet(mmtp, &pkt) < 0 ||
        pkt.size != sizeof(second_data) ||
        memcmp(pkt.data, second_data, sizeof(second_data)) ||
        pkt.pos != 42 ||
        check_mmt_data_metadata(
            &pkt, "4660", "4294967294", "0", "2271560481",
            NULL, NULL, NULL, NULL, NULL)) {
        fprintf(stderr, "second aggregated MMT application MFU was not queued\n");
        goto close;
    }
    av_packet_unref(&pkt);
    if (ff_mmtp_get_packet(mmtp, &pkt) != AVERROR(EAGAIN)) {
        fprintf(stderr, "MMT application packet queue did not drain\n");
        goto close;
    }
    ret = 0;

close:
    av_packet_unref(&pkt);
    ff_mmtp_parse_close(mmtp);
end:
    avformat_free_context(format);
    return ret;
}

static int test_timed_general_data_mfu(void)
{
    static const uint8_t general_data[] = { 0x01, 0x02, 0x03, 0x04 };
    uint8_t mpu[22 + sizeof(general_data)] = { 0 };
    AVFormatContext *format = avformat_alloc_context();
    struct Streams *stream;
    AVProgram *program;
    MMTPContext *mmtp;
    GetByteContext gbc;
    AVPacket pkt = { 0 };
    int ret = 1;

    AV_WB16(mpu, sizeof(mpu) - 2);
    mpu[2] = 0x28; /* timed, complete MFU */
    AV_WB32(mpu + 4, 7);
    AV_WB32(mpu + 8, 0x12345678);
    AV_WB32(mpu + 12, 0x87654321);
    AV_WB32(mpu + 16, 0x01020304);
    mpu[20] = 0xfe;
    mpu[21] = 0x7f;
    memcpy(mpu + 22, general_data, sizeof(general_data));

    if (!format)
        return 1;
    program = av_new_program(format, 1);
    mmtp = program ? ff_mmtp_parse_open(program) : NULL;
    if (!mmtp)
        goto end;
    mmtp->s = format;
    mmtp->pkt = &pkt;
    mmtp->current_pid = 0x1234;
    mmtp->current_timestamp = 0x7e910000;
    mmtp->ntp_anchor = UINT64_C(0x83aa7e9000000000);
    pkt.pos = 42;
    if (find_or_allocate_stream(mmtp, mmtp->current_pid, &stream) < 0)
        goto close;
    stream->stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    stream->stream->codecpar->codec_id = AV_CODEC_ID_BIN_DATA;
    avpriv_set_pts_info(stream->stream, 64, 1, 1000);
    stream->timestamp_descriptor =
        av_mallocz(sizeof(*stream->timestamp_descriptor));
    if (!stream->timestamp_descriptor)
        goto close;
    stream->num_timestamp_descriptors = 1;
    stream->timestamp_descriptor[0].seq_num = 7;
    stream->timestamp_descriptor[0].presentation_time =
        UINT64_C(0x83aa7e9000000000);

    bytestream2_init(&gbc, mpu, sizeof(mpu));
    if (parse_mpu(mmtp, 1, &gbc) < 0 ||
        pkt.size != sizeof(general_data) ||
        memcmp(pkt.data, general_data, sizeof(general_data)) ||
        pkt.pts != ntp64_to_milliseconds(
                       stream->timestamp_descriptor[0].presentation_time)) {
        fprintf(stderr, "timed MMT general-data MFU was not emitted intact\n");
        goto close;
    }
    if (check_mmt_data_metadata(
            &pkt, "4660", "7", "1", NULL, "305419896", "2271560481",
            "16909060", "254", "127")) {
        fprintf(stderr, "timed MMT general-data identifiers were not preserved\n");
        goto close;
    }
    ret = 0;

close:
    av_packet_unref(&pkt);
    ff_mmtp_parse_close(mmtp);
end:
    avformat_free_context(format);
    return ret;
}

static int test_multiple_video_nals(void)
{
    static const uint8_t length_prefixed[] = {
        0x00, 0x00, 0x00, 0x02, 0x67, 0x01,
        0x00, 0x00, 0x00, 0x03, 0x68, 0x02, 0x03,
    };
    static const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x68, 0x02, 0x03,
    };
    static const uint8_t truncated[] = {
        0x00, 0x00, 0x00, 0x02, 0x67,
    };
    AVCodecParameters codecpar = { .codec_id = AV_CODEC_ID_H264 };
    AVStream avstream = { .codecpar = &codecpar };
    struct Streams stream = { .stream = &avstream };
    GetByteContext gbc;
    uint8_t *annexb = NULL;
    int annexb_size = 0;
    int ret = 1;

    bytestream2_init(&gbc, length_prefixed, sizeof(length_prefixed));
    if (convert_video_mfu_to_annexb(&stream, &gbc, &annexb,
                                    &annexb_size) < 0 ||
        annexb_size != sizeof(expected) ||
        memcmp(annexb, expected, sizeof(expected))) {
        fprintf(stderr, "multiple length-prefixed video NALs were not converted\n");
        goto end;
    }
    av_freep(&annexb);
    bytestream2_init(&gbc, truncated, sizeof(truncated));
    if (convert_video_mfu_to_annexb(&stream, &gbc, &annexb,
                                    &annexb_size) != AVERROR_INVALIDDATA ||
        annexb) {
        fprintf(stderr, "truncated length-prefixed video NAL was accepted\n");
        goto end;
    }
    ret = 0;

end:
    av_free(annexb);
    return ret;
}

static int test_unparsed_payload_handling(void)
{
    static const uint8_t arib_reserved_packet[] = {
        0x04, 0xc4, 0x12, 0x34,
        0x7e, 0x91, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0xaa, 0xbb,
    };
    static const uint8_t source_packet[] = {
        0x08, 0x04, 0x12, 0x34,
        0x7e, 0x91, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01,
        0xaa, 0xbb,
        0x01, 0x02, 0x03, 0x04,
    };
    static const uint8_t repair_packet[] = {
        0x18, 0x03, 0x12, 0x34,
        0x7e, 0x91, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x02,
        0xcc, 0xdd, 0xee,
    };
    static const uint8_t mpu_metadata_packet[] = {
        0x00, 0x00, 0x12, 0x34,
        0x7e, 0x91, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x03,
        0x00, 0x06, 0x10, 0x00,
        0x00, 0x00, 0x00, 0x01,
    };
    AVFormatContext *format = avformat_alloc_context();
    AVProgram *program;
    MMTPContext *mmtp;
    AVPacket pkt = { 0 };
    int ret = 1;

    if (!format)
        return 1;
    program = av_new_program(format, 1);
    mmtp = program ? ff_mmtp_parse_open(program) : NULL;
    if (!mmtp)
        goto end;

    if (ff_mmtp_parse_packet(mmtp, format, &pkt, arib_reserved_packet,
                             sizeof(arib_reserved_packet)) < 0 ||
        pkt.size != 2 || memcmp(pkt.data, arib_reserved_packet + 12, 2)) {
        fprintf(stderr, "ARIB MMTP reserved bits were rejected\n");
        goto close;
    }
    av_packet_unref(&pkt);

    if (ff_mmtp_parse_packet(mmtp, format, &pkt, source_packet,
                             sizeof(source_packet)) < 0 ||
        pkt.size != 2 || memcmp(pkt.data, source_packet + 12, 2)) {
        fprintf(stderr, "source FEC payload ID was not removed\n");
        goto close;
    }
    av_packet_unref(&pkt);

    if (ff_mmtp_parse_packet(mmtp, format, &pkt, repair_packet,
                             sizeof(repair_packet)) < 0 ||
        pkt.size != 3 || memcmp(pkt.data, repair_packet + 12, 3)) {
        fprintf(stderr, "FEC repair payload was not preserved\n");
        goto close;
    }
    av_packet_unref(&pkt);

    if (ff_mmtp_parse_packet(mmtp, format, &pkt, mpu_metadata_packet,
                             sizeof(mpu_metadata_packet)) < 0 ||
        pkt.size != 8 || memcmp(pkt.data, mpu_metadata_packet + 12, 8)) {
        fprintf(stderr, "MPU metadata payload was not preserved\n");
        goto close;
    }
    ret = 0;

close:
    av_packet_unref(&pkt);
    ff_mmtp_parse_close(mmtp);
end:
    avformat_free_context(format);
    return ret;
}

static int test_tlv_probe_requires_complete_packets(void)
{
    static uint8_t complete[] = {
        0x7f, 0x03, 0x00, 0x03, 0x00, 0x00, 0x61,
        0x7f, 0x03, 0x00, 0x03, 0x00, 0x00, 0x61,
    };
    static uint8_t truncated[] = {
        0x7f, 0x03, 0x00, 0x03, 0x00, 0x00, 0x61,
        0x7f, 0x03, 0x00, 0x04, 0x00, 0x00, 0x61,
    };
    static uint8_t nested[] = {
        0x7f, 0x02, 0x00, 0x0e,
        0x7f, 0x03, 0x00, 0x03, 0x00, 0x00, 0x61,
        0x7f, 0x03, 0x00, 0x03, 0x00, 0x00, 0x61,
    };
    AVProbeData probe = { 0 };

    probe.buf = complete;
    probe.buf_size = sizeof(complete);
    if (ff_mmttlv_demuxer.read_probe(&probe) != AVPROBE_SCORE_MAX) {
        fprintf(stderr, "complete TLV packet chain was not recognized\n");
        return 1;
    }

    probe.buf = truncated;
    probe.buf_size = sizeof(truncated);
    if (ff_mmttlv_demuxer.read_probe(&probe) == AVPROBE_SCORE_MAX) {
        fprintf(stderr, "truncated TLV packet chain received maximum score\n");
        return 1;
    }

    probe.buf = nested;
    probe.buf_size = sizeof(nested);
    if (ff_mmttlv_demuxer.read_probe(&probe)) {
        fprintf(stderr, "nested signatures inside an IPv6 packet were counted\n");
        return 1;
    }
    return 0;
}

static int test_tlv_probe_raw_ip_packets(void)
{
    static uint8_t packets[] = {
        0x7f, 0x01, 0x00, 0x1d,
        0x45, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00, 0x00,
        0x40, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x02, 0x00, 0x09, 0x00, 0x00, 0x00,
        0x7f, 0x02, 0x00, 0x31,
        0x60, 0x00, 0x00, 0x00, 0x00, 0x09, 0x11, 0x40,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x02, 0x00, 0x09, 0x00, 0x00, 0x00,
    };
    AVProbeData probe = { .buf = packets, .buf_size = sizeof(packets) };

    if (ff_mmttlv_demuxer.read_probe(&probe) != AVPROBE_SCORE_MAX) {
        fprintf(stderr, "raw IPv4/IPv6 TLV packet chain was not recognized\n");
        return 1;
    }
    return 0;
}

static int test_tlv_probe_compressed_ipv4_headers(void)
{
    static uint8_t full_headers[] = {
        0x7f, 0x03, 0x00, 0x17, 0x00, 0x00, 0x20,
        0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x11,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x02,
        0x7f, 0x03, 0x00, 0x17, 0x00, 0x10, 0x20,
        0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x11,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x02,
    };
    static uint8_t compressed_headers[] = {
        0x7f, 0x03, 0x00, 0x05, 0x00, 0x00, 0x21, 0x00, 0x01,
        0x7f, 0x03, 0x00, 0x05, 0x00, 0x10, 0x21, 0x00, 0x02,
    };
    static uint8_t truncated_full_header[] = {
        0x7f, 0x03, 0x00, 0x16, 0x00, 0x00, 0x20,
        0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x11,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00,
    };
    AVProbeData probe = { 0 };

    probe.buf = full_headers;
    probe.buf_size = sizeof(full_headers);
    if (ff_mmttlv_demuxer.read_probe(&probe) != AVPROBE_SCORE_MAX) {
        fprintf(stderr, "full compressed IPv4 TLV headers were not recognized\n");
        return 1;
    }

    probe.buf = compressed_headers;
    probe.buf_size = sizeof(compressed_headers);
    if (ff_mmttlv_demuxer.read_probe(&probe) != AVPROBE_SCORE_MAX) {
        fprintf(stderr, "compressed IPv4 TLV headers were not recognized\n");
        return 1;
    }

    probe.buf = truncated_full_header;
    probe.buf_size = sizeof(truncated_full_header);
    if (ff_mmttlv_demuxer.read_probe(&probe)) {
        fprintf(stderr, "truncated compressed IPv4 TLV header was recognized\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    return test_multiple_locations() ||
           test_truncated_url() ||
           test_ip_locations() ||
           test_video_asset_types() ||
           test_audio_codec_support() ||
           test_subtitle_codec_support() ||
           test_repeated_mpt_descriptor_omission() ||
           test_application_asset_types() ||
           test_ip_delivery_is_ignorable() ||
           test_pa_table_boundaries() ||
           test_sequence_wrap() ||
           test_timestamp_descriptor_reuse() ||
           test_missing_timestamp_descriptor_fallback() ||
           test_fixed_interval_timestamp_descriptor() ||
           test_fragmented_timed_mfu() ||
           test_new_mfu_resets_fragment_packet_state() ||
           test_non_rap_aggregated_application_mfus() ||
           test_timed_general_data_mfu() ||
           test_multiple_video_nals() ||
           test_unparsed_payload_handling() ||
           test_tlv_probe_requires_complete_packets() ||
           test_tlv_probe_raw_ip_packets() ||
           test_tlv_probe_compressed_ipv4_headers();
}
