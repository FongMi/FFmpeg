/*
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

#include "libavcodec/avcodec.h"
#include "libavcodec/h2645_parse.h"
#include "libavcodec/hevc/hevc.h"
#include "libavcodec/hevc/parse.h"

static int check_extradata(const uint8_t *data, int size,
                           int expect_nalff, int expect_length_size)
{
    HEVCParamSets ps = { 0 };
    HEVCSEI sei = { 0 };
    int is_nalff = !expect_nalff, nal_length_size = 0;
    int ret;

    ret = ff_hevc_decode_extradata(data, size, &ps, &sei, &is_nalff,
                                  &nal_length_size, AV_EF_EXPLODE, 1, NULL);
    if (ret < 0 || is_nalff != expect_nalff ||
        nal_length_size != expect_length_size) {
        fprintf(stderr, "unexpected framing for HEVC extradata of size %d\n", size);
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

static int check_packet(const uint8_t *data, int size, int nal_length_size,
                        int flags)
{
    H2645Packet pkt = { 0 };
    int ret;

    ret = ff_h2645_packet_split(&pkt, data, size, NULL, nal_length_size,
                                AV_CODEC_ID_HEVC,
                                flags | H2645_FLAG_SMALL_PADDING |
                                H2645_FLAG_HEVC_METADATA_ONLY);
    if (ret < 0) {
        fprintf(stderr, "packet split failed: %d\n", ret);
        goto done;
    }

    if (pkt.nb_nals != 2 ||
        pkt.nals[0].type != HEVC_NAL_SEI_PREFIX ||
        pkt.nals[1].type != HEVC_NAL_UNSPEC62) {
        fprintf(stderr, "unexpected metadata NAL sequence:");
        for (int i = 0; i < pkt.nb_nals; i++)
            fprintf(stderr, " %d", pkt.nals[i].type);
        fprintf(stderr, "\n");
        ret = AVERROR_INVALIDDATA;
    }

done:
    ff_h2645_packet_uninit(&pkt);
    return ret;
}

int main(void)
{
    static const uint8_t hvcc[23] = { 1, [21] = 0xff };
    static const uint8_t annexb[] = {
        0x00, 0x00, 0x00, 0x01,
        0x02, 0x01, 0x11, 0x00, 0x00, 0x03, 0x01, 0x80,
        0x00, 0x00, 0x01,
        0x4e, 0x01, 0x80,
        0x00, 0x00, 0x01,
        0x7c, 0x01, 0x80,
        0x00, 0x00, 0x01,
        0x02, 0x01, 0x22, 0x80,
    };
    static const uint8_t nalff[] = {
        0x00, 0x00, 0x00, 0x08,
        0x02, 0x01, 0x11, 0x00, 0x00, 0x03, 0x01, 0x80,
        0x00, 0x00, 0x00, 0x03,
        0x4e, 0x01, 0x80,
        0x00, 0x00, 0x00, 0x03,
        0x7c, 0x01, 0x80,
        0x00, 0x00, 0x00, 0x04,
        0x02, 0x01, 0x22, 0x80,
    };

    if (check_extradata(NULL, 0, 0, 0) < 0 ||
        check_extradata(annexb, 0, 0, 0) < 0 ||
        check_extradata(hvcc, sizeof(hvcc), 1, 4) < 0)
        return 1;
    if (check_packet(annexb, sizeof(annexb), 0, 0) < 0)
        return 1;
    if (check_packet(nalff, sizeof(nalff), 4, H2645_FLAG_IS_NALFF) < 0)
        return 1;

    return 0;
}
