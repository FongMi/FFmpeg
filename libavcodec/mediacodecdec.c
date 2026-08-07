/*
 * Android MediaCodec MPEG-2 / H.264 / H.265 / MPEG-4 / VP8 / VP9 decoders
 *
 * Copyright (c) 2015-2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
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

#include "config_components.h"

#include <stdint.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/dovi_meta.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/internal.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "dovi_rpu.h"
#include "h264_parse.h"
#include "h2645_parse.h"
#include "h264_ps.h"
#include "hevc/hevc.h"
#include "hevc/parse.h"
#include "hwconfig.h"
#include "internal.h"
#include "jni.h"
#include "mediacodec_wrapper.h"
#include "mediacodecdec_common.h"

typedef struct MediaCodecH264DecContext {

    AVClass *avclass;

    MediaCodecDecContext *ctx;

    AVPacket buffered_pkt;

    int delay_flush;
    int amlogic_mpeg2_api23_workaround;

    int use_ndk_codec;
    // Ref. MediaFormat KEY_OPERATING_RATE
    int operating_rate;
    int dovi_sink_support;
    int dovi_gpu_mapping_support;

#if CONFIG_HEVC_MEDIACODEC_DECODER
    HEVCParamSets hevc_ps;
    HEVCSEI hevc_sei;
    H2645Packet hevc_metadata_pkt;
    DOVIContext dovi_ctx;
    AVFrame *buffered_frame_props;
#endif
} MediaCodecH264DecContext;

static av_cold int mediacodec_decode_close(AVCodecContext *avctx)
{
    MediaCodecH264DecContext *s = avctx->priv_data;

    ff_mediacodec_dec_close(avctx, s->ctx);
    s->ctx = NULL;

    av_packet_unref(&s->buffered_pkt);

#if CONFIG_HEVC_MEDIACODEC_DECODER
    av_frame_free(&s->buffered_frame_props);
    ff_h2645_packet_uninit(&s->hevc_metadata_pkt);
    ff_hevc_ps_uninit(&s->hevc_ps);
    ff_hevc_reset_sei(&s->hevc_sei);
    ff_dovi_ctx_unref(&s->dovi_ctx);
#endif

    return 0;
}

#if CONFIG_H264_MEDIACODEC_DECODER || CONFIG_HEVC_MEDIACODEC_DECODER
static int h2645_ps_to_nalu(const uint8_t *src, int src_size, uint8_t **out, int *out_size)
{
    int i;
    int ret = 0;
    uint8_t *p = NULL;
    static const uint8_t nalu_header[] = { 0x00, 0x00, 0x00, 0x01 };

    if (!out || !out_size) {
        return AVERROR(EINVAL);
    }

    p = av_malloc(sizeof(nalu_header) + src_size);
    if (!p) {
        return AVERROR(ENOMEM);
    }

    *out = p;
    *out_size = sizeof(nalu_header) + src_size;

    memcpy(p, nalu_header, sizeof(nalu_header));
    memcpy(p + sizeof(nalu_header), src, src_size);

    /* Escape 0x00, 0x00, 0x0{0-3} pattern */
    for (i = 4; i < *out_size; i++) {
        if (i < *out_size - 3 &&
            p[i + 0] == 0 &&
            p[i + 1] == 0 &&
            p[i + 2] <= 3) {
            uint8_t *new;

            *out_size += 1;
            new = av_realloc(*out, *out_size);
            if (!new) {
                ret = AVERROR(ENOMEM);
                goto done;
            }
            *out = p = new;

            i = i + 2;
            memmove(p + i + 1, p + i, *out_size - (i + 1));
            p[i] = 0x03;
        }
    }
done:
    if (ret < 0) {
        av_freep(out);
        *out_size = 0;
    }

    return ret;
}
#endif

#if CONFIG_H264_MEDIACODEC_DECODER
static int h264_set_extradata(AVCodecContext *avctx, FFAMediaFormat *format)
{
    int i;
    int ret;

    H264ParamSets ps = {0};
    const PPS *pps = NULL;
    const SPS *sps = NULL;
    int is_avc = 0;
    int nal_length_size = 0;

    ret = ff_h264_decode_extradata(avctx->extradata, avctx->extradata_size,
                                   &ps, &is_avc, &nal_length_size, 0, avctx);
    if (ret < 0) {
        goto done;
    }

    for (i = 0; i < MAX_PPS_COUNT; i++) {
        if (ps.pps_list[i]) {
            pps = ps.pps_list[i];
            break;
        }
    }

    if (pps) {
        if (ps.sps_list[pps->sps_id]) {
            sps = ps.sps_list[pps->sps_id];
        }
    }

    if (pps && sps) {
        uint8_t *data = NULL;
        int data_size = 0;

        avctx->profile = ff_h264_get_profile(sps);
        avctx->level = sps->level_idc;

        if ((ret = h2645_ps_to_nalu(sps->data, sps->data_size, &data, &data_size)) < 0) {
            goto done;
        }
        ff_AMediaFormat_setBuffer(format, "csd-0", (void*)data, data_size);
        av_freep(&data);

        if ((ret = h2645_ps_to_nalu(pps->data, pps->data_size, &data, &data_size)) < 0) {
            goto done;
        }
        ff_AMediaFormat_setBuffer(format, "csd-1", (void*)data, data_size);
        av_freep(&data);
    } else {
        const int warn = is_avc && (avctx->codec_tag == MKTAG('a','v','c','1') ||
                                    avctx->codec_tag == MKTAG('a','v','c','2'));
        av_log(avctx, warn ? AV_LOG_WARNING : AV_LOG_DEBUG,
               "Could not extract PPS/SPS from extradata\n");
        ret = 0;
    }

done:
    ff_h264_ps_uninit(&ps);

    return ret;
}
#endif

#if CONFIG_HEVC_MEDIACODEC_DECODER
static int hevc_set_extradata(AVCodecContext *avctx, FFAMediaFormat *format)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    HEVCParamSets *ps = &s->hevc_ps;
    HEVCSEI *sei = &s->hevc_sei;
    int i;
    int ret;

    const HEVCVPS *vps = NULL;
    const HEVCPPS *pps = NULL;
    const HEVCSPS *sps = NULL;
    int is_nalff = 0;
    int nal_length_size = 0;

    uint8_t *vps_data = NULL;
    uint8_t *sps_data = NULL;
    uint8_t *pps_data = NULL;
    int vps_data_size = 0;
    int sps_data_size = 0;
    int pps_data_size = 0;

    ret = ff_hevc_decode_extradata(avctx->extradata, avctx->extradata_size,
                                   ps, sei, &is_nalff, &nal_length_size, 0, 1, avctx);
    if (ret < 0)
        goto done;

    for (i = 0; i < HEVC_MAX_VPS_COUNT; i++) {
        if (ps->vps_list[i]) {
            vps = ps->vps_list[i];
            break;
        }
    }

    for (i = 0; i < HEVC_MAX_PPS_COUNT; i++) {
        if (ps->pps_list[i]) {
            pps = ps->pps_list[i];
            break;
        }
    }

    if (pps) {
        if (ps->sps_list[pps->sps_id]) {
            sps = ps->sps_list[pps->sps_id];
        }
    }

    if (vps && pps && sps) {
        uint8_t *data;
        int data_size;

        avctx->profile = sps->ptl.general_ptl.profile_idc;
        avctx->level   = sps->ptl.general_ptl.level_idc;

        if ((ret = h2645_ps_to_nalu(vps->data, vps->data_size, &vps_data, &vps_data_size)) < 0 ||
            (ret = h2645_ps_to_nalu(sps->data, sps->data_size, &sps_data, &sps_data_size)) < 0 ||
            (ret = h2645_ps_to_nalu(pps->data, pps->data_size, &pps_data, &pps_data_size)) < 0) {
            goto done;
        }

        data_size = vps_data_size + sps_data_size + pps_data_size;
        data = av_mallocz(data_size);
        if (!data) {
            ret = AVERROR(ENOMEM);
            goto done;
        }

        memcpy(data                                , vps_data, vps_data_size);
        memcpy(data + vps_data_size                , sps_data, sps_data_size);
        memcpy(data + vps_data_size + sps_data_size, pps_data, pps_data_size);

        ff_AMediaFormat_setBuffer(format, "csd-0", data, data_size);

        av_freep(&data);
    } else {
        const int warn = is_nalff && avctx->codec_tag == MKTAG('h','v','c','1');
        av_log(avctx, warn ? AV_LOG_WARNING : AV_LOG_DEBUG,
               "Could not extract VPS/PPS/SPS from extradata\n");
        ret = 0;
    }

done:
    av_freep(&vps_data);
    av_freep(&sps_data);
    av_freep(&pps_data);

    return ret;
}

static void mediacodec_reset_unexported_hevc_sei(HEVCSEI *sei)
{
    H2645SEIMasteringDisplay mastering_display =
        sei->common.mastering_display;
    H2645SEIContentLight content_light = sei->common.content_light;
    AVBufferRef *dynamic_hdr_plus = sei->common.itut_t35.hdr_plus;

    /* Preserve the HDR state that applies until replaced while releasing SEI
     * payloads this decoder does not export. */
    sei->common.itut_t35.hdr_plus = NULL;
    ff_hevc_reset_sei(sei);
    sei->common.mastering_display = mastering_display;
    sei->common.content_light = content_light;
    sei->common.itut_t35.hdr_plus = dynamic_hdr_plus;
}

static int mediacodec_extract_hevc_metadata(AVCodecContext *avctx,
                                            MediaCodecH264DecContext *s,
                                            const AVPacket *pkt,
                                            AVFrame *frame)
{
    H2645NAL *rpu_nal = NULL;
    const uint8_t *side_data;
    size_t side_data_size;
    bool require_dovi_mapping;
    int ret;

    av_frame_unref(frame);

    /* Native Dolby Vision decoders consume the untouched RPU in the coded
     * stream. Parsing it again is unnecessary and would add CPU work to the
     * direct-output path. */
    if (s->ctx->native_dovi)
        return 0;

    side_data = av_packet_get_side_data(pkt, AV_PKT_DATA_DOVI_CONF,
                                        &side_data_size);
    if (side_data && side_data_size >= sizeof(s->dovi_ctx.cfg))
        memcpy(&s->dovi_ctx.cfg, side_data, sizeof(s->dovi_ctx.cfg));

    if (s->dovi_ctx.cfg.dv_profile == 5) {
        if (s->dovi_gpu_mapping_support != 1) {
            av_log(avctx, AV_LOG_ERROR,
                   "Dolby Vision profile 5 discovered after decoder "
                   "initialization, but GPU mapping is unavailable\n");
            return AVERROR(ENOSYS);
        }
        s->ctx->require_dovi_mapping = true;
    }
    require_dovi_mapping = s->ctx->require_dovi_mapping;

    /* Well-described SDR and HLG streams do not carry the PQ/Dolby metadata
     * exported here. Avoid scanning their compressed access units. */
    if (!s->dovi_ctx.cfg.dv_profile &&
        avctx->color_trc != AVCOL_TRC_SMPTE2084 &&
        avctx->color_trc != AVCOL_TRC_UNSPECIFIED)
        return 0;

    ret = ff_h2645_packet_split(&s->hevc_metadata_pkt,
                                pkt->data, pkt->size, avctx, 0,
                                AV_CODEC_ID_HEVC,
                                H2645_FLAG_SMALL_PADDING |
                                H2645_FLAG_HEVC_METADATA_ONLY);
    if (ret < 0) {
        if (ret == AVERROR(ENOMEM))
            return ret;
        if (require_dovi_mapping) {
            av_log(avctx, AV_LOG_ERROR,
                   "Could not inspect HEVC metadata required for Dolby Vision "
                   "GPU mapping: %s\n", av_err2str(ret));
            return ret;
        }
        av_log(avctx, AV_LOG_DEBUG,
               "Could not inspect HEVC metadata NAL units: %s\n",
               av_err2str(ret));
        return 0;
    }

    for (int i = 0; i < s->hevc_metadata_pkt.nb_nals; i++) {
        H2645NAL *nal = &s->hevc_metadata_pkt.nals[i];

        switch (nal->type) {
        case HEVC_NAL_VPS:
            ret = ff_hevc_decode_nal_vps(&nal->gb, avctx, &s->hevc_ps);
            break;
        case HEVC_NAL_SPS:
            ret = ff_hevc_decode_nal_sps(&nal->gb, avctx, &s->hevc_ps,
                                         nal->nuh_layer_id, 1);
            break;
        case HEVC_NAL_PPS:
            ret = ff_hevc_decode_nal_pps(&nal->gb, avctx, &s->hevc_ps);
            break;
        case HEVC_NAL_SEI_PREFIX:
        case HEVC_NAL_SEI_SUFFIX:
            ret = ff_hevc_decode_nal_sei(&nal->gb, avctx, &s->hevc_sei,
                                         &s->hevc_ps, nal->type);
            break;
        case HEVC_NAL_UNSPEC62:
            ret = 0;
            if (nal->size > 2 && nal->raw_size > 2 &&
                !nal->nuh_layer_id && !nal->temporal_id)
                rpu_nal = nal;
            break;
        default:
            av_assert0(0);
        }

        if (ret == AVERROR(ENOMEM))
            return ret;
        if (ret < 0)
            av_log(avctx, AV_LOG_WARNING,
                   "Ignoring invalid HEVC metadata NAL unit type %d: %s\n",
                   nal->type, av_err2str(ret));
    }

    ret = ff_h2645_sei_hdr_to_frame(frame, &s->hevc_sei.common, avctx);
    if (ret < 0)
        return ret;

    if (require_dovi_mapping && !rpu_nal) {
        av_log(avctx, AV_LOG_ERROR,
               "Dolby Vision profile 5 frame has no RPU for required GPU mapping\n");
        return AVERROR_INVALIDDATA;
    }

    if (s->hevc_sei.common.itut_t35.hdr_plus) {
        AVBufferRef *info =
            av_buffer_ref(s->hevc_sei.common.itut_t35.hdr_plus);

        if (!info)
            return AVERROR(ENOMEM);
        if (!av_frame_new_side_data_from_buf(frame,
                                             AV_FRAME_DATA_DYNAMIC_HDR_PLUS,
                                             info)) {
            av_buffer_unref(&info);
            return AVERROR(ENOMEM);
        }
    }

    if (s->hevc_sei.common.alternative_transfer.present) {
        int trc = s->hevc_sei.common.alternative_transfer
                                     .preferred_transfer_characteristics;

        if (trc != AVCOL_TRC_UNSPECIFIED && av_color_transfer_name(trc))
            frame->color_trc = trc;
    }

    if (rpu_nal) {
        AVBufferRef *rpu = av_buffer_alloc(rpu_nal->raw_size - 2);

        if (!rpu)
            return AVERROR(ENOMEM);
        memcpy(rpu->data, rpu_nal->raw_data + 2, rpu_nal->raw_size - 2);

        ret = ff_dovi_rpu_parse(&s->dovi_ctx,
                                rpu_nal->data + 2, rpu_nal->size - 2,
                                avctx->err_recognition);
        if (ret < 0) {
            av_buffer_unref(&rpu);
            if (ret == AVERROR(ENOMEM))
                return ret;
            if (require_dovi_mapping) {
                av_log(avctx, AV_LOG_ERROR,
                       "Invalid Dolby Vision RPU required for GPU mapping: %s\n",
                       av_err2str(ret));
                return ret;
            }
            av_log(avctx, AV_LOG_WARNING,
                   "Ignoring invalid Dolby Vision RPU: %s\n",
                   av_err2str(ret));
        } else {
            if (!av_frame_new_side_data_from_buf(frame,
                                                 AV_FRAME_DATA_DOVI_RPU_BUFFER,
                                                 rpu)) {
                av_buffer_unref(&rpu);
                return AVERROR(ENOMEM);
            }
            ret = ff_dovi_attach_side_data(&s->dovi_ctx, frame);
            if (ret < 0)
                return ret;
        }
    }

    mediacodec_reset_unexported_hevc_sei(&s->hevc_sei);
    return 0;
}
#endif

#if CONFIG_MPEG2_MEDIACODEC_DECODER || \
    CONFIG_MPEG4_MEDIACODEC_DECODER || \
    CONFIG_VP8_MEDIACODEC_DECODER   || \
    CONFIG_VP9_MEDIACODEC_DECODER   || \
    CONFIG_AV1_MEDIACODEC_DECODER   || \
    CONFIG_AAC_MEDIACODEC_DECODER   || \
    CONFIG_AMRNB_MEDIACODEC_DECODER || \
    CONFIG_AMRWB_MEDIACODEC_DECODER || \
    CONFIG_MP3_MEDIACODEC_DECODER
static int common_set_extradata(AVCodecContext *avctx, FFAMediaFormat *format)
{
    int ret = 0;

    if (avctx->extradata) {
        ff_AMediaFormat_setBuffer(format, "csd-0", avctx->extradata, avctx->extradata_size);
    }

    return ret;
}
#endif

static const AVDOVIDecoderConfigurationRecord *
mediacodec_get_dovi_config(AVCodecContext *avctx)
{
    const AVPacketSideData *sd =
        ff_get_coded_side_data(avctx, AV_PKT_DATA_DOVI_CONF);

    if (!sd || sd->size < sizeof(AVDOVIDecoderConfigurationRecord))
        return NULL;

    return (const AVDOVIDecoderConfigurationRecord *)sd->data;
}

static int mediacodec_dovi_has_compatible_base_layer(
    AVCodecContext *avctx,
    const AVDOVIDecoderConfigurationRecord *dovi)
{
    /* Only use explicitly present, independently decodable base layers. */
    if (!dovi->bl_present_flag)
        return 0;

    switch (dovi->dv_profile) {
    case 0:
        return avctx->codec_id == AV_CODEC_ID_H264;
    case 2:
    case 4:
    case 6:
    case 7:
        return avctx->codec_id == AV_CODEC_ID_HEVC;
    case 8:
        return avctx->codec_id == AV_CODEC_ID_HEVC &&
               (dovi->dv_bl_signal_compatibility_id == 1 ||
                dovi->dv_bl_signal_compatibility_id == 2 ||
                dovi->dv_bl_signal_compatibility_id == 4);
    case 9:
        return avctx->codec_id == AV_CODEC_ID_H264;
    case 10:
        return avctx->codec_id == AV_CODEC_ID_AV1 &&
               (dovi->dv_bl_signal_compatibility_id == 1 ||
                dovi->dv_bl_signal_compatibility_id == 2 ||
                dovi->dv_bl_signal_compatibility_id == 4);
    default:
        return 0;
    }
}

static int mediacodec_dovi_can_gpu_map(
    AVCodecContext *avctx,
    const AVDOVIDecoderConfigurationRecord *dovi)
{
    return avctx->codec_id == AV_CODEC_ID_HEVC && dovi->dv_profile == 5;
}

static av_cold int mediacodec_init_decoder(AVCodecContext *avctx,
                                           MediaCodecH264DecContext *s,
                                           const AVDOVIDecoderConfigurationRecord *dovi)
{
    FFAMediaFormat *format = NULL;
    const char *codec_mime = NULL;
    int format_profile = -1;
    bool native_dovi = false;
    int ret;

    format = ff_AMediaFormat_new(s->use_ndk_codec);
    if (!format) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create media format\n");
        return AVERROR_EXTERNAL;
    }

    switch (avctx->codec_id) {
#if CONFIG_AV1_MEDIACODEC_DECODER
    case AV_CODEC_ID_AV1:
        codec_mime = "video/av01";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_H264_MEDIACODEC_DECODER
    case AV_CODEC_ID_H264:
        codec_mime = "video/avc";

        ret = h264_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_HEVC_MEDIACODEC_DECODER
    case AV_CODEC_ID_HEVC:
        codec_mime = "video/hevc";

        ret = hevc_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_MPEG2_MEDIACODEC_DECODER
    case AV_CODEC_ID_MPEG2VIDEO:
        codec_mime = "video/mpeg2";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_MPEG4_MEDIACODEC_DECODER
    case AV_CODEC_ID_MPEG4:
        codec_mime = "video/mp4v-es";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_VP8_MEDIACODEC_DECODER
    case AV_CODEC_ID_VP8:
        codec_mime = "video/x-vnd.on2.vp8";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_VP9_MEDIACODEC_DECODER
    case AV_CODEC_ID_VP9:
        codec_mime = "video/x-vnd.on2.vp9";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_AAC_MEDIACODEC_DECODER
    case AV_CODEC_ID_AAC:
        codec_mime = "audio/mp4a-latm";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_AMRNB_MEDIACODEC_DECODER
    case AV_CODEC_ID_AMR_NB:
        codec_mime = "audio/3gpp";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_AMRWB_MEDIACODEC_DECODER
    case AV_CODEC_ID_AMR_WB:
        codec_mime = "audio/amr-wb";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
#if CONFIG_MP3_MEDIACODEC_DECODER
    case AV_CODEC_ID_MP3:
        codec_mime = "audio/mpeg";

        ret = common_set_extradata(avctx, format);
        if (ret < 0)
            goto done;
        break;
#endif
    default:
        av_assert0(0);
    }

    if (dovi && dovi->dv_profile <= 10) {
        codec_mime = FF_MEDIACODEC_MIME_DOLBY_VISION;
        format_profile = 1 << dovi->dv_profile;
        native_dovi = true;
        av_log(avctx, AV_LOG_INFO,
               "Dolby Vision profile %u detected, requesting a "
               "Dolby Vision MediaCodec decoder\n",
               dovi->dv_profile);
    } else if (dovi) {
        av_log(avctx, AV_LOG_WARNING,
               "Unsupported Dolby Vision profile %u, using the base-layer decoder\n",
               dovi->dv_profile);
    }

    ff_AMediaFormat_setString(format, "mime", codec_mime);
    if (format_profile >= 0)
        ff_AMediaFormat_setInt32(format, "profile", format_profile);

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ff_AMediaFormat_setInt32(format, "width", avctx->width);
        ff_AMediaFormat_setInt32(format, "height", avctx->height);
        ff_mediacodec_dec_set_input_color(avctx, format);
    } else {
        ff_AMediaFormat_setInt32(format, "channel-count", avctx->ch_layout.nb_channels);
        ff_AMediaFormat_setInt32(format, "sample-rate", avctx->sample_rate);
    }
    if (s->operating_rate > 0)
        ff_AMediaFormat_setInt32(format, "operating-rate", s->operating_rate);

    s->ctx = av_mallocz(sizeof(*s->ctx));
    if (!s->ctx) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate MediaCodecDecContext\n");
        ret = AVERROR(ENOMEM);
        goto done;
    }

    s->ctx->delay_flush = s->delay_flush;
    s->ctx->use_ndk_codec = s->use_ndk_codec;
    s->ctx->native_dovi = native_dovi;

    if ((ret = ff_mediacodec_dec_init(avctx, s->ctx, codec_mime, format)) < 0) {
        s->ctx = NULL;
        goto done;
    }

done:
    ff_AMediaFormat_delete(format);
    return ret;
}

static av_cold int mediacodec_decode_init(AVCodecContext *avctx)
{
    const AVDOVIDecoderConfigurationRecord *dovi;
    const AVDOVIDecoderConfigurationRecord *decoder_dovi;
    MediaCodecH264DecContext *s = avctx->priv_data;
    int sdk_int;
    int ret;

    if (s->use_ndk_codec < 0)
        s->use_ndk_codec = !av_jni_get_java_vm(avctx);

    dovi = avctx->codec_type == AVMEDIA_TYPE_VIDEO
           ? mediacodec_get_dovi_config(avctx)
           : NULL;
    decoder_dovi = dovi;

    /* Native Dolby Vision is safe only when the caller has positively
     * identified a compatible output sink. Raw profile 5 may instead use the
     * regular HEVC decoder only when the caller can preserve its samples and
     * apply the parsed RPU metadata in a GPU renderer. */
    if (dovi && s->dovi_sink_support != 1) {
        if (!mediacodec_dovi_has_compatible_base_layer(avctx, dovi)) {
            if (s->dovi_gpu_mapping_support != 1 ||
                !mediacodec_dovi_can_gpu_map(avctx, dovi)) {
                av_log(avctx, AV_LOG_WARNING,
                       "Native Dolby Vision output is unavailable and profile %u "
                       "cannot use the active GPU mapping path; refusing MediaCodec\n",
                       dovi->dv_profile);
                return AVERROR(ENOSYS);
            }
            av_log(avctx, AV_LOG_INFO,
                   "Native Dolby Vision output is unavailable, decoding raw "
                   "profile 5 for GPU mapping\n");
        } else {
            av_log(avctx, AV_LOG_INFO,
                   "Native Dolby Vision output is unavailable, using the "
                   "base-layer decoder for profile %u\n",
                   dovi->dv_profile);
        }
        decoder_dovi = NULL;
    }

    ret = mediacodec_init_decoder(avctx, s, decoder_dovi);
    if (ret < 0)
        goto fail;

    s->ctx->require_dovi_mapping =
        !s->ctx->native_dovi && s->dovi_gpu_mapping_support == 1 && dovi &&
        mediacodec_dovi_can_gpu_map(avctx, dovi);

#if CONFIG_HEVC_MEDIACODEC_DECODER
    if (avctx->codec_id == AV_CODEC_ID_HEVC && !s->ctx->native_dovi) {
        ret = ff_h2645_sei_to_context(avctx, &s->hevc_sei.common);
        if (ret < 0)
            goto fail;

        s->buffered_frame_props = av_frame_alloc();
        if (!s->buffered_frame_props) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        s->dovi_ctx.logctx = avctx;
        if (dovi)
            s->dovi_ctx.cfg = *dovi;
    }
#endif

    av_log(avctx, AV_LOG_INFO,
           "MediaCodec started successfully: codec = %s, ret = %d\n",
           s->ctx->codec_name, ret);

    sdk_int = ff_Build_SDK_INT(avctx);
    /* ff_Build_SDK_INT can fail when target API < 24 and JVM isn't available.
     * If we don't check sdk_int > 0, the workaround might be enabled by
     * mistake.
     * JVM is required to make the workaround works reliably. On the other hand,
     * missing a workaround should not be a serious issue, we do as best we can.
     */
    if (sdk_int > 0 && sdk_int <= 23 &&
        strcmp(s->ctx->codec_name, "OMX.amlogic.mpeg2.decoder.awesome") == 0) {
        av_log(avctx, AV_LOG_INFO, "Enabling workaround for %s on API=%d\n",
               s->ctx->codec_name, sdk_int);
        s->amlogic_mpeg2_api23_workaround = 1;
    }

    return ret;

fail:
    mediacodec_decode_close(avctx);
    return ret;
}

static int mediacodec_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    const AVFrame *frame_props = NULL;
    int ret;
    ssize_t index;

#if CONFIG_HEVC_MEDIACODEC_DECODER
    if (avctx->codec_id == AV_CODEC_ID_HEVC)
        frame_props = s->buffered_frame_props;
#endif

    /* In delay_flush mode, wait until the user has released or rendered
       all retained frames. */
    if (s->delay_flush && ff_mediacodec_dec_is_flushing(avctx, s->ctx)) {
        if (!ff_mediacodec_dec_flush(avctx, s->ctx)) {
            return AVERROR(EAGAIN);
        }
    }

    /* poll for new frame */
    ret = ff_mediacodec_dec_receive(avctx, s->ctx, frame, false);
    if (ret != AVERROR(EAGAIN))
        return ret;

    /* feed decoder */
    while (1) {
        if (s->ctx->current_input_buffer < 0 && !s->ctx->draining) {
            /* poll for input space */
            index = ff_AMediaCodec_dequeueInputBuffer(s->ctx->codec, 0);
            if (index < 0) {
                /* no space, block for an output frame to appear */
                ret = ff_mediacodec_dec_receive(avctx, s->ctx, frame, true);
                /* Try again if both input port and output port return EAGAIN.
                 * If no data is consumed and no frame in output, it can make
                 * both avcodec_send_packet() and avcodec_receive_frame()
                 * return EAGAIN, which violate the design.
                 */
                if (ff_AMediaCodec_infoTryAgainLater(s->ctx->codec, index) &&
                    ret == AVERROR(EAGAIN))
                    continue;
                return ret;
            }
            s->ctx->current_input_buffer = index;
        }

        /* try to flush any buffered packet data */
        if (s->buffered_pkt.size > 0) {
            ret = ff_mediacodec_dec_send(avctx, s->ctx, &s->buffered_pkt,
                                         frame_props, false);
            if (ret >= 0) {
                s->buffered_pkt.size -= ret;
                s->buffered_pkt.data += ret;
                if (s->buffered_pkt.size <= 0) {
                    av_packet_unref(&s->buffered_pkt);
#if CONFIG_HEVC_MEDIACODEC_DECODER
                    if (frame_props)
                        av_frame_unref(s->buffered_frame_props);
#endif
                } else {
                    av_log(avctx, AV_LOG_WARNING,
                           "could not send entire packet in single input buffer (%d < %d)\n",
                           ret, s->buffered_pkt.size+ret);
                }
            } else if (ret < 0 && ret != AVERROR(EAGAIN)) {
                return ret;
            }

            if (s->amlogic_mpeg2_api23_workaround && s->buffered_pkt.size <= 0) {
                /* fallthrough to fetch next packet regardless of input buffer space */
            } else {
                /* poll for space again */
                continue;
            }
        }

        /* fetch new packet or eof */
        ret = ff_decode_get_packet(avctx, &s->buffered_pkt);
        if (ret == AVERROR_EOF) {
            AVPacket null_pkt = { 0 };
            ret = ff_mediacodec_dec_send(avctx, s->ctx, &null_pkt, NULL, true);
            if (ret < 0)
                return ret;
            return ff_mediacodec_dec_receive(avctx, s->ctx, frame, true);
        } else if (ret == AVERROR(EAGAIN) && s->ctx->current_input_buffer < 0) {
            return ff_mediacodec_dec_receive(avctx, s->ctx, frame, true);
        } else if (ret < 0) {
            return ret;
        }

#if CONFIG_HEVC_MEDIACODEC_DECODER
        if (frame_props) {
            ret = mediacodec_extract_hevc_metadata(avctx, s,
                                                   &s->buffered_pkt,
                                                   s->buffered_frame_props);
            if (ret < 0) {
                av_packet_unref(&s->buffered_pkt);
                av_frame_unref(s->buffered_frame_props);
                return ret;
            }
        }
#endif
    }

    return AVERROR(EAGAIN);
}

static void mediacodec_decode_flush(AVCodecContext *avctx)
{
    MediaCodecH264DecContext *s = avctx->priv_data;

    av_packet_unref(&s->buffered_pkt);

#if CONFIG_HEVC_MEDIACODEC_DECODER
    if (avctx->codec_id == AV_CODEC_ID_HEVC) {
        if (s->buffered_frame_props)
            av_frame_unref(s->buffered_frame_props);
        ff_hevc_reset_sei(&s->hevc_sei);
        ff_dovi_ctx_flush(&s->dovi_ctx);
    }
#endif

    ff_mediacodec_dec_flush(avctx, s->ctx);
}

static const AVCodecHWConfigInternal *const mediacodec_hw_configs[] = {
    &(const AVCodecHWConfigInternal) {
        .public          = {
            .pix_fmt     = AV_PIX_FMT_MEDIACODEC,
            .methods     = AV_CODEC_HW_CONFIG_METHOD_AD_HOC |
                           AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX,
            .device_type = AV_HWDEVICE_TYPE_MEDIACODEC,
        },
        .hwaccel         = NULL,
    },
    NULL
};

#define OFFSET(x) offsetof(MediaCodecH264DecContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption ff_mediacodec_vdec_options[] = {
    { "delay_flush", "Delay flush until hw output buffers are returned to the decoder",
                     OFFSET(delay_flush), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VD },
    { "ndk_codec", "Use MediaCodec from NDK",
                   OFFSET(use_ndk_codec), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, VD },
    { "operating_rate", "The desired operating rate that the codec will need to operate at, zero for unspecified",
            OFFSET(operating_rate), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, VD },
    { "dovi_sink_support", "Whether the output sink positively supports native Dolby Vision "
                           "(-1 unspecified, 0 no, 1 yes); only 1 enables native output",
            OFFSET(dovi_sink_support), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, VD },
    { "dovi_gpu_mapping_support", "Whether the output path can preserve raw Dolby Vision "
                                  "samples and apply parsed metadata on the GPU",
            OFFSET(dovi_gpu_mapping_support), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, VD },
    { NULL }
};

#define DECLARE_MEDIACODEC_VCLASS(short_name)                   \
static const AVClass ff_##short_name##_mediacodec_dec_class = { \
    .class_name = #short_name "_mediacodec",                    \
    .item_name  = av_default_item_name,                         \
    .option     = ff_mediacodec_vdec_options,                   \
    .version    = LIBAVUTIL_VERSION_INT,                        \
};

#define DECLARE_MEDIACODEC_VDEC(short_name, full_name, codec_id, bsf)                          \
DECLARE_MEDIACODEC_VCLASS(short_name)                                                          \
const FFCodec ff_ ## short_name ## _mediacodec_decoder = {                                     \
    .p.name         = #short_name "_mediacodec",                                               \
    CODEC_LONG_NAME(full_name " Android MediaCodec decoder"),                                  \
    .p.type         = AVMEDIA_TYPE_VIDEO,                                                      \
    .p.id           = codec_id,                                                                \
    .p.priv_class   = &ff_##short_name##_mediacodec_dec_class,                                 \
    .priv_data_size = sizeof(MediaCodecH264DecContext),                                        \
    .init           = mediacodec_decode_init,                                                  \
    FF_CODEC_RECEIVE_FRAME_CB(mediacodec_receive_frame),                                       \
    .flush          = mediacodec_decode_flush,                                                 \
    .close          = mediacodec_decode_close,                                                 \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_SETS_FRAME_PROPS,        \
    .bsfs           = bsf,                                                                     \
    .hw_configs     = mediacodec_hw_configs,                                                   \
    .p.wrapper_name = "mediacodec",                                                            \
};                                                                                             \

#if CONFIG_H264_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(h264, "H.264", AV_CODEC_ID_H264, "h264_mp4toannexb")
#endif

#if CONFIG_HEVC_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(hevc, "H.265", AV_CODEC_ID_HEVC, "hevc_mp4toannexb")
#endif

#if CONFIG_MPEG2_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(mpeg2, "MPEG-2", AV_CODEC_ID_MPEG2VIDEO, NULL)
#endif

#if CONFIG_MPEG4_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(mpeg4, "MPEG-4", AV_CODEC_ID_MPEG4, NULL)
#endif

#if CONFIG_VP8_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(vp8, "VP8", AV_CODEC_ID_VP8, NULL)
#endif

#if CONFIG_VP9_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(vp9, "VP9", AV_CODEC_ID_VP9, NULL)
#endif

#if CONFIG_AV1_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_VDEC(av1, "AV1", AV_CODEC_ID_AV1, NULL)
#endif

#define AD AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption ff_mediacodec_adec_options[] = {
    { "ndk_codec", "Use MediaCodec from NDK",
                   OFFSET(use_ndk_codec), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, AD },
    { "operating_rate", "The desired operating rate that the codec will need to operate at, zero for unspecified",
            OFFSET(operating_rate), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, AD },
    { NULL }
};

#define DECLARE_MEDIACODEC_ACLASS(short_name)                   \
static const AVClass ff_##short_name##_mediacodec_dec_class = { \
    .class_name = #short_name "_mediacodec",                    \
    .item_name  = av_default_item_name,                         \
    .option     = ff_mediacodec_adec_options,                   \
    .version    = LIBAVUTIL_VERSION_INT,                        \
};

#define DECLARE_MEDIACODEC_ADEC(short_name, full_name, codec_id, bsf)                          \
DECLARE_MEDIACODEC_ACLASS(short_name)                                                          \
const FFCodec ff_ ## short_name ## _mediacodec_decoder = {                                     \
    .p.name         = #short_name "_mediacodec",                                               \
    CODEC_LONG_NAME(full_name " Android MediaCodec decoder"),                                  \
    .p.type         = AVMEDIA_TYPE_AUDIO,                                                      \
    .p.id           = codec_id,                                                                \
    .p.priv_class   = &ff_##short_name##_mediacodec_dec_class,                                 \
    .priv_data_size = sizeof(MediaCodecH264DecContext),                                        \
    .init           = mediacodec_decode_init,                                                  \
    FF_CODEC_RECEIVE_FRAME_CB(mediacodec_receive_frame),                                       \
    .flush          = mediacodec_decode_flush,                                                 \
    .close          = mediacodec_decode_close,                                                 \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE,                              \
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_SETS_FRAME_PROPS,        \
    .bsfs           = bsf,                                                                     \
    .p.wrapper_name = "mediacodec",                                                            \
};                                                                                             \

#if CONFIG_AAC_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(aac, "AAC", AV_CODEC_ID_AAC, "aac_adtstoasc")
#endif

#if CONFIG_AMRNB_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(amrnb, "AMR-NB", AV_CODEC_ID_AMR_NB, NULL)
#endif

#if CONFIG_AMRWB_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(amrwb, "AMR-WB", AV_CODEC_ID_AMR_WB, NULL)
#endif

#if CONFIG_MP3_MEDIACODEC_DECODER
DECLARE_MEDIACODEC_ADEC(mp3, "MP3", AV_CODEC_ID_MP3, NULL)
#endif
