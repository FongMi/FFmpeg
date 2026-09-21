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
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "libavcodec/bsf.h"
#include "libavcodec/dovi_rpu.h"
#include "libavcodec/h2645_parse.h"
#include "libavcodec/hevc/hevc.h"

// Unregistered user data with a complete payload but no rbsp_trailing_bits.
static const uint8_t opaque_sei[] = {
    0x00, 0x00, 0x00, 0x01, 0x4e, 0x01, 0x05, 0x10,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static int init_filter(AVBSFContext **bsf)
{
    const AVBitStreamFilter *filter = av_bsf_get_by_name("dovi_rpu");
    AVDOVIDecoderConfigurationRecord *cfg;
    AVPacketSideData *sd;
    int ret;

    if (!filter)
        return AVERROR_BSF_NOT_FOUND;
    ret = av_bsf_alloc(filter, bsf);
    if (ret < 0)
        return ret;
    (*bsf)->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
    (*bsf)->par_in->codec_id = AV_CODEC_ID_HEVC;
    sd = av_packet_side_data_new(&(*bsf)->par_in->coded_side_data,
                                &(*bsf)->par_in->nb_coded_side_data,
                                AV_PKT_DATA_DOVI_CONF, sizeof(*cfg), 0);
    if (!sd)
        return AVERROR(ENOMEM);
    cfg = (AVDOVIDecoderConfigurationRecord *) sd->data;
    memset(cfg, 0, sizeof(*cfg));
    cfg->dv_version_major = 1;
    cfg->dv_profile = 7;
    cfg->dv_level = 6;
    cfg->rpu_present_flag = cfg->el_present_flag = cfg->bl_present_flag = 1;
    ret = av_opt_set((*bsf)->priv_data, "convert", "p81", 0);
    if (ret < 0)
        return ret;
    ret = av_opt_set((*bsf)->priv_data, "compression", "none", 0);
    return ret < 0 ? ret : av_bsf_init(*bsf);
}

static int generate_rpu(uint8_t **rpu, int *size)
{
    DOVIContext enc = { .enable = 1, .cfg = { .dv_profile = 7 } };
    AVDOVIMetadata *metadata = av_dovi_metadata_alloc(NULL);
    AVDOVIRpuDataHeader *hdr;
    AVDOVIDataMapping *mapping;
    int ret;

    if (!metadata)
        return AVERROR(ENOMEM);
    hdr = av_dovi_get_header(metadata);
    hdr->rpu_type = 2;
    hdr->rpu_format = 18;
    hdr->vdr_rpu_profile = 1;
    hdr->coef_log2_denom = 23;
    hdr->vdr_rpu_normalized_idc = 1;
    hdr->bl_bit_depth = hdr->el_bit_depth = 10;
    hdr->vdr_bit_depth = 12;
    hdr->el_spatial_resampling_filter_flag = 1;
    mapping = av_dovi_get_mapping(metadata);
    mapping->num_x_partitions = mapping->num_y_partitions = 1;
    mapping->nlq_method_idc = AV_DOVI_NLQ_LINEAR_DZ;
    for (int c = 0; c < 3; c++) {
        AVDOVIReshapingCurve *curve = &mapping->curves[c];

        curve->num_pivots = 2;
        curve->pivots[1] = 1023;
        curve->poly_order[0] = 1;
        curve->poly_coef[0][1] = UINT64_C(1) << hdr->coef_log2_denom;
        mapping->nlq[c].vdr_in_max = UINT64_C(1) << hdr->coef_log2_denom;
    }
    *av_dovi_get_color(metadata) = ff_dovi_color_default;
    ret = ff_dovi_rpu_generate(&enc, metadata, FF_DOVI_WRAP_NAL, rpu, size);
    ff_dovi_ctx_unref(&enc);
    av_free(metadata);
    return ret;
}

static int check_packet(const uint8_t *rpu, int rpu_size, int expect_error)
{
    static const uint8_t rpu_start[] = { 0, 0, 0, 1, 0x7c, 1 };
    static const uint8_t el[] = { 0, 0, 0, 1, 0x7e, 1, 0x80 };
    DOVIContext dec = { .cfg = { .dv_profile = 8 } };
    H2645Packet nals = { 0 };
    AVBSFContext *bsf = NULL;
    AVPacket *pkt = av_packet_alloc();
    int size = sizeof(opaque_sei) + sizeof(el);
    int ret, offset;

    if (!pkt)
        return AVERROR(ENOMEM);
    if (rpu_size)
        size += sizeof(rpu_start) + rpu_size;
    ret = init_filter(&bsf);
    if (ret < 0)
        goto done;
    ret = av_new_packet(pkt, size);
    if (ret < 0)
        goto done;
    memcpy(pkt->data, opaque_sei, sizeof(opaque_sei));
    offset = sizeof(opaque_sei);
    if (rpu_size) {
        memcpy(pkt->data + offset, rpu_start, sizeof(rpu_start));
        offset += sizeof(rpu_start);
        memcpy(pkt->data + offset, rpu, rpu_size);
        offset += rpu_size;
    }
    memcpy(pkt->data + offset, el, sizeof(el));
    pkt->pts = 1234;
    pkt->dts = 1200;
    pkt->duration = 41;
    pkt->flags = AV_PKT_FLAG_KEY;
    ret = av_bsf_send_packet(bsf, pkt);
    if (ret < 0)
        goto done;
    ret = av_bsf_receive_packet(bsf, pkt);
    if (expect_error) {
        if (ret != AVERROR_INVALIDDATA) {
            fprintf(stderr, "malformed RPU returned %d\n", ret);
            ret = AVERROR_INVALIDDATA;
        } else {
            ret = 0;
        }
        goto done;
    }
    if (ret < 0)
        goto done;
    if (pkt->pts != 1234 || pkt->dts != 1200 || pkt->duration != 41 ||
        pkt->flags != AV_PKT_FLAG_KEY) {
        fprintf(stderr, "packet properties changed\n");
        ret = AVERROR_INVALIDDATA;
        goto done;
    }
    ret = ff_h2645_packet_split(&nals, pkt->data, pkt->size, NULL, 0,
                                AV_CODEC_ID_HEVC, H2645_FLAG_SMALL_PADDING);
    if (ret < 0)
        goto done;
    if (nals.nb_nals != 1 + !!rpu_size ||
        nals.nals[0].type != HEVC_NAL_SEI_PREFIX ||
        nals.nals[0].size != sizeof(opaque_sei) - 4 ||
        memcmp(nals.nals[0].data, opaque_sei + 4, sizeof(opaque_sei) - 4)) {
        fprintf(stderr, "opaque SEI changed or enhancement layer retained\n");
        ret = AVERROR_INVALIDDATA;
        goto done;
    }
    if (rpu_size) {
        if (nals.nals[1].type != HEVC_NAL_UNSPEC62) {
            ret = AVERROR_INVALIDDATA;
            goto done;
        }
        ret = ff_dovi_rpu_parse(&dec, nals.nals[1].data + 2,
                                 nals.nals[1].size - 2, 0);
        if (ret < 0)
            goto done;
        if (!dec.header.disable_residual_flag ||
            dec.header.el_spatial_resampling_filter_flag ||
            !dec.mapping || dec.mapping->nlq_method_idc != AV_DOVI_NLQ_NONE) {
            fprintf(stderr, "RPU was not converted to profile 8.1\n");
            ret = AVERROR_INVALIDDATA;
            goto done;
        }
    }

done:
    ff_dovi_ctx_unref(&dec);
    ff_h2645_packet_uninit(&nals);
    av_packet_free(&pkt);
    av_bsf_free(&bsf);
    return ret;
}

int main(void)
{
    static const uint8_t invalid_rpu[] = { 25, 0xff, 0xff, 0xff };
    uint8_t *rpu = NULL;
    int size, ret;

    av_log_set_level(AV_LOG_QUIET);
    ret = generate_rpu(&rpu, &size);
    if (ret >= 0)
        ret = check_packet(NULL, 0, 0);
    if (ret >= 0)
        ret = check_packet(rpu, size, 0);
    if (ret >= 0)
        ret = check_packet(invalid_rpu, sizeof(invalid_rpu), 1);
    av_free(rpu);
    if (ret < 0)
        fprintf(stderr, "dovi_rpu regression failed: %d\n", ret);
    return ret < 0;
}
