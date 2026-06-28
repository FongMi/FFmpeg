#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <decoder.h>

#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "internal.h"

#define AV3A_MAX_OUTPUT_SIZE (4096 * 64)
#define AV3A_MAX_OBJECTS     6
#define AV3A_MAX_ERRORS      50

typedef struct LibARCDAV3AOutput {
    uint8_t *data;
    int size;
    int channels;
    int sample_rate;
    int channel_config;
} LibARCDAV3AOutput;

typedef struct LibARCDAV3AContext {
    LibARCDAV3AOutput output;
    AVS3DecoderHandle decoder;
    uint8_t *buffer;
    int buffer_size;
    int buffered_size;
    int error_count;
    int last_channel_config;
    int last_object_count;
    int last_mix_type;
    int last_total_bitrate;
    bool first_frame;
} LibARCDAV3AContext;

static void libarcdav3a_reset_state(LibARCDAV3AContext *s)
{
    s->first_frame         = true;
    s->buffered_size       = 0;
    s->error_count         = 0;
    s->last_channel_config = -1;
    s->last_object_count   = -1;
    s->last_mix_type       = -1;
    s->last_total_bitrate  = -1;
}

static void libarcdav3a_set_channel_layout(AVChannelLayout *layout,
                                           int channels, int config)
{
    av_channel_layout_uninit(layout);

    switch ((ChannelNumConfig)config) {
    case CHANNEL_CONFIG_MC_5_1_4:
        if (channels == 10) {
            *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1POINT4_BACK;
            return;
        }
        break;
    case CHANNEL_CONFIG_MC_7_1_2:
        if (channels == 10) {
            *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1POINT2;
            return;
        }
        break;
    case CHANNEL_CONFIG_MC_7_1_4:
        if (channels == 12) {
            *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1POINT4_BACK;
            return;
        }
        break;
    case CHANNEL_CONFIG_HOA_ORDER3:
        if (channels == 16) {
            *layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_HEXADECAGONAL;
            return;
        }
        break;
    default:
        break;
    }

    av_channel_layout_default(layout, channels);
}

static int libarcdav3a_append_input(LibARCDAV3AContext *s,
                                    const uint8_t *input, int input_size)
{
    int min_size;

    if (input_size > INT_MAX - s->buffered_size)
        return AVERROR_INVALIDDATA;

    min_size = s->buffered_size + input_size;
    if (min_size > s->buffer_size) {
        int new_size = min_size;
        uint8_t *tmp;

        if (input_size <= (INT_MAX - s->buffered_size) / 2)
            new_size = s->buffered_size + input_size * 2;

        tmp = av_realloc(s->buffer, new_size);
        if (!tmp) {
            av_freep(&s->buffer);
            s->buffer_size = 0;
            return AVERROR(ENOMEM);
        }
        s->buffer      = tmp;
        s->buffer_size = new_size;
    }

    memcpy(s->buffer + s->buffered_size, input, input_size);
    s->buffered_size = min_size;

    return 0;
}

static av_cold int libarcdav3a_decode_init(AVCodecContext *avctx)
{
    LibARCDAV3AContext *s = avctx->priv_data;

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;

    s->decoder = avs3_create_decoder();
    if (!s->decoder)
        return AVERROR(ENOMEM);

    libarcdav3a_reset_state(s);

    s->output.data = av_mallocz(AV3A_MAX_OUTPUT_SIZE);
    if (!s->output.data)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold void libarcdav3a_flush(AVCodecContext *avctx)
{
    LibARCDAV3AContext *s = avctx->priv_data;

    if (s->decoder)
        avs3_destroy_decoder(s->decoder);
    s->decoder = avs3_create_decoder();

    if (!s->decoder)
        av_log(avctx, AV_LOG_ERROR, "Failed to recreate AV3A decoder\n");

    libarcdav3a_reset_state(s);
}

static int libarcdav3a_decode_buffer(AVCodecContext *avctx, const uint8_t *input,
                                     int input_size, int *input_consumed,
                                     LibARCDAV3AOutput *output)
{
    LibARCDAV3AContext *s = avctx->priv_data;
    int ret, pos = 0, out_index = 0;

    if (!s->decoder)
        return AVERROR(ENOMEM);

    ret = libarcdav3a_append_input(s, input, input_size);
    if (ret < 0)
        return ret;

    *input_consumed = input_size;
    output->size = 0;

    do {
        int consumed = 0;

        while ((ret = parse_header(s->decoder, s->buffer + pos,
                                   s->buffered_size - pos, s->first_frame,
                                   &consumed, NULL)) != AVS3_TRUE) {
            if (consumed < 0)
                return AVERROR_INVALIDDATA;

            if (ret == AVS3_DATA_NOT_ENOUGH) {
                if (pos + consumed < s->buffered_size)
                    pos += consumed;
                else
                    pos = s->buffered_size;
                break;
            }

            if (pos + consumed < s->buffered_size) {
                pos += consumed;
            } else {
                pos = s->buffered_size;
                ret = AVS3_DATA_NOT_ENOUGH;
                break;
            }
        }

        if (ret != AVS3_TRUE)
            break;
        if (consumed < 0)
            return AVERROR_INVALIDDATA;

        if (s->decoder->numObjsOutput > AV3A_MAX_OBJECTS) {
            libarcdav3a_flush(avctx);
            out_index = 0;
            break;
        }

        if ((s->last_channel_config != -1 &&
             s->last_channel_config != s->decoder->channelNumConfig) ||
            (s->last_object_count != -1 &&
             s->last_object_count != s->decoder->numObjsOutput) ||
            (s->last_mix_type != -1 &&
             s->last_mix_type != s->decoder->isMixedContent) ||
            (s->last_total_bitrate != -1 &&
             s->last_total_bitrate != s->decoder->totalBitrate)) {
            libarcdav3a_flush(avctx);
            out_index = 0;
            break;
        }

        if (pos + consumed < s->buffered_size) {
            pos += consumed;
        } else {
            pos = s->buffered_size;
            ret = AVS3_DATA_NOT_ENOUGH;
            break;
        }

        s->first_frame = false;

        if (s->decoder->numChansOutput <= 0 || s->decoder->frameLength <= 0)
            return AVERROR_INVALIDDATA;

        {
            int out_size;
            int out_len = 0;

            if (s->decoder->numChansOutput > INT_MAX / s->decoder->frameLength / 2)
                return AVERROR_BUFFER_TOO_SMALL;
            out_size = s->decoder->numChansOutput * s->decoder->frameLength * 2;

            if (out_index > AV3A_MAX_OUTPUT_SIZE ||
                out_size > AV3A_MAX_OUTPUT_SIZE - out_index)
                return AVERROR_BUFFER_TOO_SMALL;

            ret = avs3_decode(s->decoder, s->buffer + pos,
                              s->buffered_size - pos, output->data + out_index,
                              &out_len, &consumed);
            if (consumed < 0 || consumed > s->buffered_size - pos)
                return AVERROR_INVALIDDATA;
            pos += consumed;

            if (ret != AVS3_TRUE || out_len <= 0) {
                s->error_count++;
                if (s->error_count > AV3A_MAX_ERRORS) {
                    s->error_count = 0;
                    libarcdav3a_flush(avctx);
                    out_index = 0;
                }
            } else {
                s->error_count = 0;
            }

            if (ret != AVS3_TRUE)
                break;

            s->last_channel_config = s->decoder->channelNumConfig;
            s->last_object_count   = s->decoder->numObjsOutput;
            s->last_mix_type       = s->decoder->isMixedContent;
            s->last_total_bitrate  = s->decoder->totalBitrate;

            if (out_len > 0)
                out_index += out_len;
            else
                break;
        }
    } while (s->buffered_size > pos);

    if (out_index) {
        output->size           = out_index;
        output->channels       = s->decoder->numChansOutput;
        output->sample_rate    = s->decoder->outputFs;
        output->channel_config = s->decoder->channelNumConfig;
        s->decoder->numObjsOutput = 0;
    }

    if (pos > 0 && pos < s->buffered_size) {
        memmove(s->buffer, s->buffer + pos, s->buffered_size - pos);
        s->buffered_size -= pos;
    } else if (pos >= s->buffered_size) {
        s->buffered_size = 0;
    }

    return ret;
}

static int libarcdav3a_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                                    int *got_frame_ptr, AVPacket *avpkt)
{
    LibARCDAV3AContext *s = avctx->priv_data;
    uint8_t *input = avpkt->data;
    int input_size = avpkt->size;
    int ret;

    *got_frame_ptr = 0;

    while (input_size > 0) {
        int consumed = 0;

        ret = libarcdav3a_decode_buffer(avctx, input, input_size,
                                        &consumed, &s->output);
        if (ret < 0)
            return ret;
        if (consumed < 0 || consumed > input_size)
            return AVERROR_INVALIDDATA;
        if (!consumed)
            break;

        input      += consumed;
        input_size -= consumed;

        if (!s->output.size)
            continue;

        if (s->output.channels <= 0 ||
            s->output.size % (s->output.channels * (int)sizeof(int16_t)))
            return AVERROR_INVALIDDATA;

        frame->nb_samples = s->output.size /
                            (s->output.channels * (int)sizeof(int16_t));
        frame->sample_rate = s->output.sample_rate;
        frame->format      = AV_SAMPLE_FMT_S16;

        if (s->output.channel_config == CHANNEL_CONFIG_UNKNOWN)
            av_log(avctx, AV_LOG_WARNING,
                   "Unknown AV3A channel configuration\n");

        libarcdav3a_set_channel_layout(&frame->ch_layout, s->output.channels,
                                       s->output.channel_config);
        ret = av_channel_layout_copy(&avctx->ch_layout, &frame->ch_layout);
        if (ret < 0)
            return ret;

        avctx->sample_rate = frame->sample_rate;
        avctx->sample_fmt  = AV_SAMPLE_FMT_S16;

        ret = ff_get_buffer(avctx, frame, 0);
        if (ret < 0)
            return ret;

        memcpy(frame->data[0], s->output.data, s->output.size);
        *got_frame_ptr = 1;

        return avpkt->size;
    }

    return avpkt->size;
}

static av_cold int libarcdav3a_decode_close(AVCodecContext *avctx)
{
    LibARCDAV3AContext *s = avctx->priv_data;

    if (s->decoder)
        avs3_destroy_decoder(s->decoder);
    s->decoder = NULL;

    av_freep(&s->buffer);
    av_freep(&s->output.data);

    return 0;
}

const FFCodec ff_libarcdav3a_decoder = {
    .p.name         = "libarcdav3a",
    CODEC_LONG_NAME("libarcdav3a AV3A"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_AV3A,
    .p.capabilities = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
    .p.wrapper_name = "libarcdav3a",
    .priv_data_size = sizeof(LibARCDAV3AContext),
    .init           = libarcdav3a_decode_init,
    .close          = libarcdav3a_decode_close,
    FF_CODEC_DECODE_CB(libarcdav3a_decode_frame),
    .flush          = libarcdav3a_flush,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_S16),
};
