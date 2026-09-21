/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef AVS3_HOA_RENDERER_H
#define AVS3_HOA_RENDERER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Avs3HoaRenderer Avs3HoaRenderer;

/* AVS3 HOA PCM uses ACN channel order and N3D normalization. Render it to
 * stereo speakers in place; the buffer must hold samples * input_channels. */
int avs3_hoa_renderer_create(Avs3HoaRenderer **renderer, int input_channels);
int avs3_hoa_renderer_process(Avs3HoaRenderer *renderer, int16_t *pcm, int samples,
                              int input_channels, char *error, size_t error_size);
void avs3_hoa_renderer_destroy(Avs3HoaRenderer *renderer);

#ifdef __cplusplus
}
#endif
#endif
