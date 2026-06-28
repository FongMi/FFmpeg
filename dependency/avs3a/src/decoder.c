#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "avs3_cnst_com.h"
#include "avs3_prot_dec.h"
#include "avs3_rom_com.h"
#include "decoder.h"
#include "avs3_prot_com.h"

AVS3DecoderHandle avs3_create_decoder()
{
    AVS3DecoderHandle hAvs3Dec = NULL;
    if ((hAvs3Dec = (AVS3DecoderHandle)calloc(1, sizeof(AVS3Decoder))) == NULL)
    {
        return NULL;
    }
    LOGD("avs3_create_decoder %p\n", hAvs3Dec);
    return hAvs3Dec;
}

void avs3_destroy_decoder(AVS3DecoderHandle hAvs3Dec)
{
    LOGD("%p, avs3_destroy_decoder in\n", hAvs3Dec);
    if (!hAvs3Dec)
        return;

    if (hAvs3Dec->fModel != NULL)
    {
        fclose(hAvs3Dec->fModel);
    }

    if (hAvs3Dec->pLog)
        fclose(hAvs3Dec->pLog);

    Avs3DecoderDestroy(hAvs3Dec);
    LOGD("avs3_destroy_decoder out\n");
}

/*
	return value:
		0: failed
		1: success
		2: data not enough
*/
int parse_header(AVS3DecoderHandle hAvs3Dec, unsigned char* pData, int nLenIn, int isInitFrame, int *pnLenConsumed, unsigned short *crc)
{
    uint8_t headerBs[MAX_NBYTES_FRAME_HEADER];
    uint32_t nextBitPos = 0;
    int pos = 0;

    if (pnLenConsumed)
        *pnLenConsumed = 0;

    if (hAvs3Dec == NULL)
        return AVS3_FALSE;

    if (nLenIn < MAX_NBYTES_FRAME_HEADER)
    {
        return AVS3_DATA_NOT_ENOUGH;
    }

    // Read max header length into bs buffer
    memcpy(headerBs, pData, MAX_NBYTES_FRAME_HEADER);

    do {
        // Sync word, 12 bits
        uint16_t syncWord;
        syncWord = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_SYNC_WORD);
        // Check sync word
        if (syncWord != SYNC_WORD_COMPAT) {
            pos++;
            if (nLenIn - pos < MAX_NBYTES_FRAME_HEADER)
            {
                if (pnLenConsumed)
                    *pnLenConsumed = pos;
                return AVS3_DATA_NOT_ENOUGH;
            }
            memcpy(headerBs, pData + pos, MAX_NBYTES_FRAME_HEADER);
            nextBitPos = 0;
            continue;
        }
        if (pnLenConsumed)
            *pnLenConsumed = pos + MAX_NBYTES_FRAME_HEADER;
        break;
    } while (1);

    // audio codec id, 4 bits
    uint16_t audioCodecId;
    audioCodecId = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_AUDIO_CODEC_ID);
    // Check audio codec id, should be 2 for HW branch
    if (audioCodecId != 2) {
        LOGD("%p, audioCodecId != 2", hAvs3Dec);
        return AVS3_FALSE;
    }

    // anc data index, fixed to 0 in HW branch, 1 bit
    uint16_t ancDataIndex;
    ancDataIndex = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_ANC_DATA_INDEX);
    // Check anc data index
    if (ancDataIndex == 1) {
        LOGD("%p, ancDataIndex == 1", hAvs3Dec);
        return AVS3_FALSE;
    }

    // NN type, 3 bit
    // 0 for default main, 1 for default lc
    uint16_t nnTypeConfig;
    nnTypeConfig = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_NN_TYPE);

    // coding profile, 3 bit
    // 0 for mono/stereo/mc, 1 for channel + obj mix, 2 for hoa
    uint16_t codingProfile;
    codingProfile = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_CODING_PROFILE);

    // sampling rate index, 4 bit
    uint16_t samplingRateIdx;
    samplingRateIdx = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_SAMPLING_RATE_INDEX);

    // CRC first part
    uint16_t crcTmp;
    crcTmp = (uint16_t)GetNextIndice(headerBs, &nextBitPos, AVS3_BS_BYTE_SIZE);
    crcTmp = crcTmp << AVS3_BS_BYTE_SIZE;

    uint16_t channelNumIdx;
    uint16_t numObjs;
    uint16_t hoaOrder;
    uint16_t soundBedType;
    uint16_t bitrateIdxPerObj;
    uint16_t bitrateIdxBedMc;
    if (codingProfile == 0) {
        // channel number index
        // for mono/stereo/mc, 7 bits
        channelNumIdx = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_CHANNEL_NUMBER_INDEX);
    }
    else if (codingProfile == 1) {
        // sound bed type, 2bits
        soundBedType = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_SOUNDBED_TYPE);

        if (soundBedType == 0) {
            // for only objs
            // object number, 7 bits
            numObjs = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_NUM_OBJS);
            numObjs += 1;
            // bitrate index for each obj, 4 bits
            bitrateIdxPerObj = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_BITRATE_INDEX);
        }
        else if (soundBedType == 1) {
            // for MC+objs
            // channel number index, 7 bits
            channelNumIdx = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_CHANNEL_NUMBER_INDEX);
            // bitrate index for sound bed, 4 bits
            bitrateIdxBedMc = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_BITRATE_INDEX);

            // object number, 7 bits
            numObjs = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_NUM_OBJS);
            numObjs += 1;
            // bitrate index for each obj, 4 bits
            bitrateIdxPerObj = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_BITRATE_INDEX);
        }
    }
    else if (codingProfile == 2) {
        // for HOA, 4 bits
        hoaOrder = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_HOA_ORDER);
        hoaOrder += 1;
    }

    // resolution, i.e. bitDepth, 2 bits
    uint16_t resolution;
    resolution = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_RESOLUTION);

    // bitrate index, 4 bits
    uint16_t bitrateIdx;
    if (codingProfile != 1) {
        bitrateIdx = (uint16_t)GetNextIndice(headerBs, &nextBitPos, NBITS_BITRATE_INDEX);
    }

    // second part of CRC, 8 bits
    crcTmp += (uint16_t)GetNextIndice(headerBs, &nextBitPos, AVS3_BS_BYTE_SIZE);

    // 	// rewind bs file if needed
    // 	if (isInitFrame == 1) {
    // 		// first frame, seek to file begin
    // 		fseek(fBitstream, 0, SEEK_SET);
    // 	}
    // 	else {
    // 		// for mono/stereo/mc/hoa, header size 7 bytes, need rewind by 1 byte
    // 		// for mix, no need to rewind
    uint32_t headerBsBytes = (uint32_t)(ceil((float)nextBitPos / 8));
    if (headerBsBytes < MAX_NBYTES_FRAME_HEADER) {
        if (pnLenConsumed)
            *pnLenConsumed -= MAX_NBYTES_FRAME_HEADER - headerBsBytes;

        // #ifndef MIX_EXT
        // 			fseek(fBitstream, -1, SEEK_CUR);
        // #else
        // 			fseek(fBitstream, headerBsBytes - MAX_NBYTES_FRAME_HEADER, SEEK_CUR);
        // #endif
    }
    // 	}

    // Config decoder
    // sampling frequency
    hAvs3Dec->outputFs = avs3SamplingRateTable[samplingRateIdx];

    // frame length
    hAvs3Dec->frameLength = GetFrameLength(hAvs3Dec->outputFs);

    // bitdepth
    if (resolution == 0) {
        hAvs3Dec->bitDepth = 8;
    }
    else if (resolution == 1) {
        hAvs3Dec->bitDepth = 16;
    }
    else if (resolution == 2) {
        hAvs3Dec->bitDepth = 24;
    }

    // NN type config
    hAvs3Dec->nnTypeConfig = (NnTypeConfig)nnTypeConfig;

    // Codec format and bitrate
    if (codingProfile == 0) {
        // mono/stereo/mc
        hAvs3Dec->isMixedContent = 0;
        hAvs3Dec->channelNumConfig = (ChannelNumConfig)channelNumIdx;
        if (hAvs3Dec->channelNumConfig == CHANNEL_CONFIG_MONO) {
            // mono
            hAvs3Dec->avs3CodecFormat = AVS3_MONO_FORMAT;
            hAvs3Dec->avs3CodecCore = AVS3_MDCT_CORE;
            hAvs3Dec->numChansOutput = 1;
        }
        else if (hAvs3Dec->channelNumConfig == CHANNEL_CONFIG_STEREO) {
            // stereo
            hAvs3Dec->avs3CodecFormat = AVS3_STEREO_FORMAT;
            hAvs3Dec->avs3CodecCore = AVS3_MDCT_CORE;
            hAvs3Dec->numChansOutput = 2;
        }
        else if (hAvs3Dec->channelNumConfig <= CHANNEL_CONFIG_MC_7_1_4) {
            // mc
            hAvs3Dec->avs3CodecFormat = AVS3_MC_FORMAT;
            hAvs3Dec->avs3CodecCore = AVS3_MDCT_CORE;
            for (int16_t i = 0; i < AVS3_SIZE_MC_CONFIG_TABLE; i++) {
                if (hAvs3Dec->channelNumConfig == mcChannelConfigTable[i].channelNumConfig) {
                    hAvs3Dec->numChansOutput = mcChannelConfigTable[i].numChannels;
                }
            }
            hAvs3Dec->hasLfe = 1;
            if (hAvs3Dec->channelNumConfig == CHANNEL_CONFIG_MC_4_0) {
                hAvs3Dec->hasLfe = 0;
            }
        }
        else {
            LOGD("%p, hAvs3Dec->channelNumConfig not valid:%d", hAvs3Dec, hAvs3Dec->channelNumConfig);
            return AVS3_FALSE;
        }
    }
    else if (codingProfile == 1) {
        // mix
        hAvs3Dec->isMixedContent = 1;

        // sound bed type
        hAvs3Dec->soundBedType = soundBedType;

        if (hAvs3Dec->soundBedType == 0) {
            // object number
            hAvs3Dec->numObjsOutput = numObjs;
            hAvs3Dec->numChansOutput = numObjs;

            if (numObjs == 1) {
                hAvs3Dec->avs3CodecFormat = AVS3_MONO_FORMAT;
                hAvs3Dec->avs3CodecCore = AVS3_MDCT_CORE;
            }
            else if (numObjs == 2) {
                hAvs3Dec->avs3CodecFormat = AVS3_STEREO_FORMAT;
                hAvs3Dec->avs3CodecCore = AVS3_MDCT_CORE;
            }
            else if (numObjs >= 3) {
                hAvs3Dec->avs3CodecFormat = AVS3_MC_FORMAT;
                hAvs3Dec->avs3CodecCore = AVS3_MDCT_CORE;
            }

            // channelNumConfig not used for pure objs
            hAvs3Dec->channelNumConfig = CHANNEL_CONFIG_UNKNOWN;

            // bitrate per obj
            hAvs3Dec->bitratePerObj = codecBitrateConfigTable[CHANNEL_CONFIG_MONO].bitrateTable[bitrateIdxPerObj];

            // total bitrate, only objs
            hAvs3Dec->totalBitrate = hAvs3Dec->numObjsOutput * hAvs3Dec->bitratePerObj;

            // for pure objs, lfe not exist
            hAvs3Dec->hasLfe = 0;
        }
        else if (hAvs3Dec->soundBedType == 1) {
            hAvs3Dec->avs3CodecFormat = AVS3_MC_FORMAT;
            hAvs3Dec->avs3CodecCore = AVS3_MDCT_CORE;

            // channelNumIdx for sound bed
            hAvs3Dec->channelNumConfig = (ChannelNumConfig)channelNumIdx;

            // sound bed bitrate
            hAvs3Dec->bitrateBedMc = codecBitrateConfigTable[hAvs3Dec->channelNumConfig].bitrateTable[bitrateIdxBedMc];

            // numChannels for sound bed
            for (int16_t i = 0; i < AVS3_SIZE_MC_CONFIG_TABLE; i++) {
                if (hAvs3Dec->channelNumConfig == mcChannelConfigTable[i].channelNumConfig) {
                    hAvs3Dec->numChansOutput = mcChannelConfigTable[i].numChannels;
                }
            }

            // object number
            hAvs3Dec->numObjsOutput = numObjs;

            // bitrate per obj
            hAvs3Dec->bitratePerObj = codecBitrateConfigTable[CHANNEL_CONFIG_MONO].bitrateTable[bitrateIdxPerObj];

            // add num chans and num objs to get total chans
            hAvs3Dec->numChansOutput += hAvs3Dec->numObjsOutput;

            // total bitrate, sound bed + objs
            hAvs3Dec->totalBitrate = hAvs3Dec->bitrateBedMc + hAvs3Dec->numObjsOutput * hAvs3Dec->bitratePerObj;

            // for sound bed + obj mix
            // if sound bed is stereo/MC4.0, no LFE, if sound bed is other mc configs, with lfe
            if (hAvs3Dec->channelNumConfig == CHANNEL_CONFIG_STEREO ||
                hAvs3Dec->channelNumConfig == CHANNEL_CONFIG_MC_4_0) {
                hAvs3Dec->hasLfe = 0;
            }
            else {
                hAvs3Dec->hasLfe = 1;
            }
        }
    }
    else if (codingProfile == 2) {
        // hoa
        hAvs3Dec->channelNumConfig = CHANNEL_CONFIG_UNKNOWN;
        if (hoaOrder == 1) {
            hAvs3Dec->channelNumConfig = CHANNEL_CONFIG_HOA_ORDER1;
        }
        else if (hoaOrder == 2) {
            hAvs3Dec->channelNumConfig = CHANNEL_CONFIG_HOA_ORDER2;
        }
        else if (hoaOrder == 3) {
            hAvs3Dec->channelNumConfig = CHANNEL_CONFIG_HOA_ORDER3;
        }

        hAvs3Dec->avs3CodecFormat = AVS3_HOA_FORMAT;
        hAvs3Dec->avs3CodecCore = AVS3_MDCT_CORE;
        hAvs3Dec->numChansOutput = (hoaOrder + 1) * (hoaOrder + 1);

        hAvs3Dec->isMixedContent = 0;
    }

    if (hAvs3Dec->channelNumConfig >= CHANNEL_CONFIG_UNKNOWN || hAvs3Dec->channelNumConfig < CHANNEL_CONFIG_MONO)
    {
        LOGD("%p, channelNumConfig(%d) invalid.", hAvs3Dec, hAvs3Dec->channelNumConfig);
        return AVS3_FALSE;
    }

    // total bitrate
    if (hAvs3Dec->isMixedContent == 0) {
        hAvs3Dec->totalBitrate = codecBitrateConfigTable[hAvs3Dec->channelNumConfig].bitrateTable[bitrateIdx];
    }

    // if not first frame
//	if (isInitFrame == 0)
    {
        // 		// copy crc bs
        if (crc)
            *crc = crcTmp;

        // update bitrate
        hAvs3Dec->lastTotalBrate = hAvs3Dec->totalBitrate;

        hAvs3Dec->bitsPerFrame = (int32_t)(((float)hAvs3Dec->totalBitrate / (float)hAvs3Dec->outputFs) * hAvs3Dec->frameLength);

        // subtract frame bs header bits
        if (hAvs3Dec->isMixedContent == 0) {
            if (hAvs3Dec->avs3CodecFormat == AVS3_MONO_FORMAT) {
                hAvs3Dec->bitsPerFrame -= NBITS_FRAME_HEADER_MONO;
                hAvs3Dec->bitsHeader = NBITS_FRAME_HEADER_MONO;
            }
            else if (hAvs3Dec->avs3CodecFormat == AVS3_STEREO_FORMAT) {
                hAvs3Dec->bitsPerFrame -= NBITS_FRAME_HEADER_STEREO;
                hAvs3Dec->bitsHeader = NBITS_FRAME_HEADER_STEREO;
            }
            else if (hAvs3Dec->avs3CodecFormat == AVS3_MC_FORMAT) {
                hAvs3Dec->bitsPerFrame -= NBITS_FRAME_HEADER_MC;
                hAvs3Dec->bitsHeader = NBITS_FRAME_HEADER_MC;
            }
            else if (hAvs3Dec->avs3CodecFormat == AVS3_HOA_FORMAT) {
                hAvs3Dec->bitsPerFrame -= NBITS_FRAME_HEADER_HOA;
                hAvs3Dec->bitsHeader = NBITS_FRAME_HEADER_HOA;
            }
        }
        else {
            if (hAvs3Dec->soundBedType == 0) {
                hAvs3Dec->bitsPerFrame -= NBITS_FRAME_HEADER_MIX_SBT0;
                hAvs3Dec->bitsHeader = NBITS_FRAME_HEADER_MIX_SBT0;
            }
            else if (hAvs3Dec->soundBedType == 1) {
                hAvs3Dec->bitsPerFrame -= NBITS_FRAME_HEADER_MIX_SBT1;
                hAvs3Dec->bitsHeader = NBITS_FRAME_HEADER_MIX_SBT1;
            }
        }
    }

    if (hAvs3Dec->bitsPerFrame <= 0)
    {
        LOGD("%p, hAvs3Dec->bitsPerFrame(%d) <= 0.", hAvs3Dec, hAvs3Dec->bitsPerFrame);
        return AVS3_FALSE;
    }

    uint32_t bytesFrame = (uint32_t)(ceil((float)hAvs3Dec->bitsPerFrame / 8));
    uint32_t bytesHeader = (uint32_t)(ceil((float)hAvs3Dec->bitsHeader / 8));
#if 0
    if (pos + bytesFrame + bytesHeader > nLenIn)
	{
		if (pnLenConsumed)
		{
			if (*pnLenConsumed >= bytesHeader)
				*pnLenConsumed -= bytesHeader;
			else
				*pnLenConsumed = 0;
		}
		LOGD("parse_header pos(%d) + bytesFrame(%d) + bytesHeader(%d) > nLenIn(%d)", pos, bytesFrame, bytesHeader, nLenIn);
		return AVS3_DATA_NOT_ENOUGH;
	}

	uint16_t crcResult = 0;
	crcResult = Crc16(pData + pos + bytesHeader, bytesFrame);
	if (crcResult != crcTmp) {
		LOGD("%p, crc error.", hAvs3Dec);
		return AVS3_FALSE;
	}
#endif

    return AVS3_TRUE;
}
int avs3_decode(AVS3DecoderHandle hAvs3Dec, unsigned char* pDataIN, int nLenIn, unsigned char* pDataOut, int *pnLenOut, int *pnLenConsumed)
{
    short bytesPerFrame = 0;
    short data[MAX_CHANNELS * FRAME_LEN];

#ifdef CRC_CHECK
    uint16_t crcBs, crcResult;          // crc info from BS and calculated at decoder
#endif

    if (!hAvs3Dec || !pDataIN || !pDataOut || !pnLenOut || !pnLenConsumed)
        return AVS3_FALSE;

    *pnLenOut = 0;
    *pnLenConsumed = 0;

    if (!hAvs3Dec->bInited)
    {
        Avs3InitDecoder(hAvs3Dec, &hAvs3Dec->fModel, "model.bin");
        hAvs3Dec->bInited = 1;
    }

    uint8_t* bitstream = hAvs3Dec->hBitstream->bitstream;

    bytesPerFrame = (uint32_t)(ceil((float)hAvs3Dec->bitsPerFrame / 8));

    if (nLenIn < bytesPerFrame)
        return AVS3_DATA_NOT_ENOUGH;

    /* frame payload */
    memcpy(bitstream, pDataIN, bytesPerFrame);

    Avs3Decode(hAvs3Dec, data);

    ResetBitstream(hAvs3Dec->hBitstream);

    memcpy(pDataOut, data, hAvs3Dec->numChansOutput*hAvs3Dec->frameLength * 2);
    *pnLenOut = hAvs3Dec->numChansOutput*hAvs3Dec->frameLength * 2;
    *pnLenConsumed = bytesPerFrame;

    // #ifdef CRC_CHECK
    // 	/* CRC check */
    // 	crcResult = Crc16(bitstream, bytesPerFrame);
    // 	if (crcResult != crcBs) {
    // 		return AVS3_FALSE;
    // 	}
    // #endif

    return AVS3_TRUE;
}
