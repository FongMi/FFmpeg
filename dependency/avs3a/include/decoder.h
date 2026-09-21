
#ifndef DAVS3_DAV3A_DEC_API_H
#define DAVS3_DAV3A_DEC_API_H

#include "avs3_stat_dec.h"
#ifdef __cplusplus
extern "C" {
#endif

AVS3DecoderHandle avs3_create_decoder();
void avs3_destroy_decoder(AVS3DecoderHandle hAvs3Dec);
int parse_header(AVS3DecoderHandle hAvs3Dec, unsigned char *pData, int nLenIn, int isInitFrame, int *pnLenConsumed, unsigned short *crc);
int avs3_decode(AVS3DecoderHandle hAvs3Dec, unsigned char *pDataIN, int nLenIn, unsigned char *pDataOut, int *pnLenOut, int *pnLenConsumed);

#ifdef __cplusplus
}
#endif

#endif  /// DAVS3_DAVS3_DEC_API_H
