/*
 * Android MediaCodec public API functions
 *
 * Copyright (c) 2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
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

#include "config.h"

#include "libavutil/error.h"

#include "mediacodec.h"

#if CONFIG_MEDIACODEC

#include <jni.h>

#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"

#include "ffjni.h"
#include "mediacodecdec_common.h"

AVMediaCodecContext *av_mediacodec_alloc_context(void)
{
    return av_mallocz(sizeof(AVMediaCodecContext));
}

int av_mediacodec_default_init(AVCodecContext *avctx, AVMediaCodecContext *ctx, void *surface)
{
    int ret = 0;
    JNIEnv *env = NULL;

    env = ff_jni_get_env(avctx);
    if (!env) {
        return AVERROR_EXTERNAL;
    }

    ctx->surface = (*env)->NewGlobalRef(env, surface);
    if (ctx->surface) {
        avctx->hwaccel_context = ctx;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Could not create new global reference\n");
        ret = AVERROR_EXTERNAL;
    }

    return ret;
}

void av_mediacodec_default_free(AVCodecContext *avctx)
{
    JNIEnv *env = NULL;

    AVMediaCodecContext *ctx = avctx->hwaccel_context;

    if (!ctx) {
        return;
    }

    env = ff_jni_get_env(avctx);
    if (!env) {
        return;
    }

    if (ctx->surface) {
        (*env)->DeleteGlobalRef(env, ctx->surface);
        ctx->surface = NULL;
    }

    av_freep(&avctx->hwaccel_context);
}

int av_mediacodec_get_buffer_timestamp(const AVMediaCodecBuffer *buffer,
                                       int64_t *presentation_time_us)
{
    if (!buffer || !presentation_time_us)
        return AVERROR(EINVAL);

    *presentation_time_us = buffer->pts;
    return 0;
}

int av_mediacodec_release_buffer_status(AVMediaCodecBuffer *buffer, int render)
{
    if (!buffer)
        return AVERROR(EINVAL);

    MediaCodecDecContext *ctx = buffer->ctx;
    int released = atomic_fetch_add(&buffer->released, 1);

    if (!released && (ctx->delay_flush || buffer->serial == atomic_load(&ctx->serial))) {
        atomic_fetch_sub(&ctx->hw_buffer_count, 1);
        av_log(ctx, AV_LOG_DEBUG,
               "Releasing output buffer %zd (%p) ts=%"PRId64" with render=%d [%d pending]\n",
               buffer->index, buffer, buffer->pts, render, atomic_load(&ctx->hw_buffer_count));
        int ret = ff_AMediaCodec_releaseOutputBuffer(ctx->codec, buffer->index, render);
        return ret < 0 ? ret : 1;
    }

    return 0;
}

int av_mediacodec_release_buffer(AVMediaCodecBuffer *buffer, int render)
{
    int ret = av_mediacodec_release_buffer_status(buffer, render);
    return ret < 0 ? ret : 0;
}

int av_mediacodec_render_buffer_at_time(AVMediaCodecBuffer *buffer, int64_t time)
{
    MediaCodecDecContext *ctx = buffer->ctx;
    int released = atomic_fetch_add(&buffer->released, 1);

    if (!released && (ctx->delay_flush || buffer->serial == atomic_load(&ctx->serial))) {
        atomic_fetch_sub(&ctx->hw_buffer_count, 1);
        av_log(ctx, AV_LOG_DEBUG,
               "Rendering output buffer %zd (%p) ts=%"PRId64" with time=%"PRId64" [%d pending]\n",
               buffer->index, buffer, buffer->pts, time, atomic_load(&ctx->hw_buffer_count));
        return ff_AMediaCodec_releaseOutputBufferAtTime(ctx->codec, buffer->index, time);
    }

    return 0;
}

#else

#include <stdlib.h>

AVMediaCodecContext *av_mediacodec_alloc_context(void)
{
    return NULL;
}

int av_mediacodec_default_init(AVCodecContext *avctx, AVMediaCodecContext *ctx, void *surface)
{
    return AVERROR(ENOSYS);
}

void av_mediacodec_default_free(AVCodecContext *avctx)
{
}

int av_mediacodec_get_buffer_timestamp(const AVMediaCodecBuffer *buffer,
                                       int64_t *presentation_time_us)
{
    return AVERROR(ENOSYS);
}

int av_mediacodec_release_buffer_status(AVMediaCodecBuffer *buffer, int render)
{
    return AVERROR(ENOSYS);
}

int av_mediacodec_release_buffer(AVMediaCodecBuffer *buffer, int render)
{
    return AVERROR(ENOSYS);
}

int av_mediacodec_render_buffer_at_time(AVMediaCodecBuffer *buffer, int64_t time)
{
    return AVERROR(ENOSYS);
}

#endif
