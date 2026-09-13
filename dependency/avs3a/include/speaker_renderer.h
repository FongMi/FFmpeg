/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef AVS3_SPEAKER_RENDERER_H
#define AVS3_SPEAKER_RENDERER_H

#include <stddef.h>
#include <stdint.h>
#include "avs3_stat_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Avs3SpeakerRenderer Avs3SpeakerRenderer;

/* Render mixed-content PCM to its sound-bed layout. content_index selects an
 * audioProgramme content reference, so alternative presentations are not mixed
 * together. Output channels retain the reference decoder's sound-bed order.
 * Unsupported metadata returns a negative errno instead of dropping objects. */
int avs3_speaker_renderer_create(Avs3SpeakerRenderer **renderer, int channel_config, int channels,
                                 int content_index);
int avs3_speaker_renderer_process(Avs3SpeakerRenderer *renderer, const Avs3MetaData *metadata,
                                  int16_t *pcm, int samples, int input_channels, char *error,
                                  size_t error_size);
/* Seek within the same stream: retain programme metadata, clear gain history. */
void avs3_speaker_renderer_reset(Avs3SpeakerRenderer *renderer);
void avs3_speaker_renderer_destroy(Avs3SpeakerRenderer *renderer);

#ifdef __cplusplus
}
#endif
#endif
