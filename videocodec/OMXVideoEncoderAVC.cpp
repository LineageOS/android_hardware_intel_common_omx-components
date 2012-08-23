/*
* Copyright (c) 2009-2011 Intel Corporation.  All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/


//#define LOG_NDEBUG 0
#define LOG_TAG "OMXVideoEncoderAVC"
#include <utils/Log.h>
#include "OMXVideoEncoderAVC.h"

static const char *AVC_MIME_TYPE = "video/h264";

OMXVideoEncoderAVC::OMXVideoEncoderAVC() {
    BuildHandlerList();
    mVideoEncoder = createVideoEncoder(AVC_MIME_TYPE);
    if (!mVideoEncoder) LOGE("OMX_ErrorInsufficientResources");

    mAVCParams = new VideoParamsAVC();
    if (!mAVCParams) LOGE("OMX_ErrorInsufficientResources");
}

OMXVideoEncoderAVC::~OMXVideoEncoderAVC() {
    if(mAVCParams) {
        delete mAVCParams;
        mAVCParams = NULL;
    }
}

OMX_ERRORTYPE OMXVideoEncoderAVC::InitOutputPortFormatSpecific(OMX_PARAM_PORTDEFINITIONTYPE *paramPortDefinitionOutput) {
    // OMX_VIDEO_PARAM_AVCTYPE
    memset(&mParamAvc, 0, sizeof(mParamAvc));
    SetTypeHeader(&mParamAvc, sizeof(mParamAvc));
    mParamAvc.nPortIndex = OUTPORT_INDEX;
    mParamAvc.eProfile = OMX_VIDEO_AVCProfileBaseline;
    mParamAvc.eLevel = OMX_VIDEO_AVCLevel41;

    // OMX_NALSTREAMFORMATTYPE
    memset(&mNalStreamFormat, 0, sizeof(mNalStreamFormat));
    SetTypeHeader(&mNalStreamFormat, sizeof(mNalStreamFormat));
    mNalStreamFormat.nPortIndex = OUTPORT_INDEX;
    // TODO: check if this is desired Nalu Format
    mNalStreamFormat.eNaluFormat = OMX_NaluFormatStartCodesSeparateFirstHeader;
    //mNalStreamFormat.eNaluFormat = OMX_NaluFormatLengthPrefixedSeparateFirstHeader;
    // OMX_VIDEO_CONFIG_AVCINTRAPERIOD
    memset(&mConfigAvcIntraPeriod, 0, sizeof(mConfigAvcIntraPeriod));
    SetTypeHeader(&mConfigAvcIntraPeriod, sizeof(mConfigAvcIntraPeriod));
    mConfigAvcIntraPeriod.nPortIndex = OUTPORT_INDEX;
    // TODO: need to be populated from Video Encoder
    mConfigAvcIntraPeriod.nIDRPeriod = 1;
    mConfigAvcIntraPeriod.nPFrames = 0;

    // OMX_VIDEO_CONFIG_NALSIZE
    memset(&mConfigNalSize, 0, sizeof(mConfigNalSize));
    SetTypeHeader(&mConfigNalSize, sizeof(mConfigNalSize));
    mConfigNalSize.nPortIndex = OUTPORT_INDEX;
    mConfigNalSize.nNaluBytes = 0;

    // OMX_VIDEO_PARAM_INTEL_AVCVUI
    memset(&mParamIntelAvcVui, 0, sizeof(mParamIntelAvcVui));
    SetTypeHeader(&mParamIntelAvcVui, sizeof(mParamIntelAvcVui));
    mParamIntelAvcVui.nPortIndex = OUTPORT_INDEX;
    mParamIntelAvcVui.bVuiGeneration = OMX_FALSE;

    // OMX_VIDEO_CONFIG_INTEL_SLICE_NUMBERS
    memset(&mConfigIntelSliceNumbers, 0, sizeof(mConfigIntelSliceNumbers));
    SetTypeHeader(&mConfigIntelSliceNumbers, sizeof(mConfigIntelSliceNumbers));
    mConfigIntelSliceNumbers.nPortIndex = OUTPORT_INDEX;
    mConfigIntelSliceNumbers.nISliceNumber = 2;
    mConfigIntelSliceNumbers.nPSliceNumber = 2;

    // Override OMX_PARAM_PORTDEFINITIONTYPE
    paramPortDefinitionOutput->nBufferCountActual = OUTPORT_ACTUAL_BUFFER_COUNT;
    paramPortDefinitionOutput->nBufferCountMin = OUTPORT_MIN_BUFFER_COUNT;
    paramPortDefinitionOutput->nBufferSize = OUTPORT_BUFFER_SIZE;
    paramPortDefinitionOutput->format.video.cMIMEType = (OMX_STRING)AVC_MIME_TYPE;
    paramPortDefinitionOutput->format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;

    // Override OMX_VIDEO_PARAM_PROFILELEVELTYPE
    // TODO: check if profile/level supported is correct
    mParamProfileLevel.eProfile = mParamAvc.eProfile;
    mParamProfileLevel.eLevel = mParamAvc.eLevel;

    // Override OMX_VIDEO_PARAM_BITRATETYPE
    mParamBitrate.nTargetBitrate = 192000;

    // Override OMX_VIDEO_CONFIG_INTEL_BITRATETYPE
    mConfigIntelBitrate.nInitialQP = 0;  // Initial QP for I frames

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::SetVideoEncoderParam(void) {

    Encode_Status ret = ENCODE_SUCCESS;
    LOGV("OMXVideoEncoderAVC::SetVideoEncoderParam");

    if (!mEncoderParams) {
        LOGE("NULL pointer: mEncoderParams");
        return OMX_ErrorBadParameter;
    }

    mVideoEncoder->getParameters(mEncoderParams);
    mEncoderParams->profile = (VAProfile)VAProfileH264Baseline;
    // 0 - all luma and chroma block edges of the slice are filtered
    // 1 - deblocking is disabled for all block edges of the slice
    // 2 - all luma and chroma block edges of the slice are filtered
    // with exception of the block edges that coincide with slice boundaries
    mEncoderParams->disableDeblocking = 0;
    OMXVideoEncoderBase::SetVideoEncoderParam();

    mVideoEncoder->getParameters(mAVCParams);
    if(mParamIntelAvcVui.bVuiGeneration == OMX_TRUE) {
        mAVCParams->VUIFlag = 1;
    }
    // For resolution below VGA, single core can hit the performance target and provide VQ gain
    if (mEncoderParams->resolution.width <= 640 && mEncoderParams->resolution.height <= 480) {
        mConfigIntelSliceNumbers.nISliceNumber = 1;
        mConfigIntelSliceNumbers.nPSliceNumber = 1;
    }
    mAVCParams->sliceNum.iSliceNum = mConfigIntelSliceNumbers.nISliceNumber;
    mAVCParams->sliceNum.pSliceNum = mConfigIntelSliceNumbers.nPSliceNumber;
    mAVCParams->idrInterval = mConfigAvcIntraPeriod.nIDRPeriod;
    mAVCParams->maxSliceSize = mConfigNalSize.nNaluBytes * 8;
    ret = mVideoEncoder ->setParameters(mAVCParams);
    CHECK_ENCODE_STATUS("setParameters");

    VideoConfigAVCIntraPeriod avcIntraPreriod;
    avcIntraPreriod.idrInterval = mConfigAvcIntraPeriod.nIDRPeriod;
    // hardcode intra period for AVC encoder, get value from OMX_VIDEO_PARAM_AVCTYPE.nPFrames or
    // OMX_VIDEO_CONFIG_AVCINTRAPERIOD.nPFrames is a more flexible method
    if (mParamAvc.nPFrames == 0) {
        avcIntraPreriod.intraPeriod = 0;
    } else {
        avcIntraPreriod.intraPeriod = 30;
    }
    ret = mVideoEncoder->setConfig(&avcIntraPreriod);
    CHECK_ENCODE_STATUS("setConfig");

    LOGV("VUIFlag = %d\n", mAVCParams->VUIFlag);
    LOGV("sliceNum.iSliceNum = %d\n", mAVCParams->sliceNum.iSliceNum);
    LOGV("sliceNum.pSliceNum = %d\n", mAVCParams->sliceNum.pSliceNum);
    LOGV("idrInterval = %d\n ", mAVCParams->idrInterval);
    LOGV("maxSliceSize = %d\n ", mAVCParams->maxSliceSize);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::ProcessorInit(void) {
    mFirstFrame = OMX_TRUE;
    return  OMXVideoEncoderBase::ProcessorInit();
}

OMX_ERRORTYPE OMXVideoEncoderAVC::ProcessorDeinit(void) {
    return OMXVideoEncoderBase::ProcessorDeinit();
}

OMX_ERRORTYPE OMXVideoEncoderAVC::ProcessorProcess(
    OMX_BUFFERHEADERTYPE **buffers,
    buffer_retain_t *retains,
    OMX_U32 numberBuffers) {

    OMX_U32 outfilledlen = 0;
    OMX_S64 outtimestamp = 0;
    OMX_U32 outflags = 0;

    OMX_ERRORTYPE oret = OMX_ErrorNone;
    Encode_Status ret = ENCODE_SUCCESS;

    VideoEncOutputBuffer outBuf;
    VideoEncRawBuffer inBuf;

    OMX_NALUFORMATSTYPE NaluFormat = mNalStreamFormat.eNaluFormat;

    LOGV_IF(buffers[INPORT_INDEX]->nFlags & OMX_BUFFERFLAG_EOS,
            "%s(),%d: got OMX_BUFFERFLAG_EOS\n", __func__, __LINE__);

    if (!buffers[INPORT_INDEX]->nFilledLen) {
        LOGE("%s(),%d: input buffer's nFilledLen is zero\n",  __func__, __LINE__);
        goto out;
    }

    inBuf.data = buffers[INPORT_INDEX]->pBuffer + buffers[INPORT_INDEX]->nOffset;
    inBuf.size = buffers[INPORT_INDEX]->nFilledLen;

    LOGV("inBuf.data=%x, size=%d",(unsigned)inBuf.data, inBuf.size);

    outBuf.data = buffers[OUTPORT_INDEX]->pBuffer + buffers[OUTPORT_INDEX]->nOffset;
    outBuf.dataSize = 0;
    outBuf.bufferSize = buffers[OUTPORT_INDEX]->nAllocLen - buffers[OUTPORT_INDEX]->nOffset;

    if(inBuf.size<=0) {
        LOGE("The Input buf size is 0\n");
        return OMX_ErrorBadParameter;
    }

    LOGV("in buffer = 0x%x ts = %lld",
         buffers[INPORT_INDEX]->pBuffer + buffers[INPORT_INDEX]->nOffset,
         buffers[INPORT_INDEX]->nTimeStamp);

    if(inBuf.data == NULL) {
        LOGE("The Input buf is NULL\n");
        return OMX_ErrorBadParameter;
    }

    if(mFrameRetrieved) {
        // encode and setConfig need to be thread safe
        pthread_mutex_lock(&mSerializationLock);
        ret = mVideoEncoder->encode(&inBuf);
        pthread_mutex_unlock(&mSerializationLock);
        CHECK_ENCODE_STATUS("encode");
        mFrameRetrieved = OMX_FALSE;

        // This is for buffer contention, we won't release current buffer
        // but the last input buffer
        ports[INPORT_INDEX]->ReturnAllRetainedBuffers();
    }

    if (mStoreMetaDataInBuffers)
        NaluFormat = OMX_NaluFormatLengthPrefixedSeparateFirstHeader;

    switch (NaluFormat) {
        case OMX_NaluFormatStartCodes:

            outBuf.format = OUTPUT_EVERYTHING;
            ret = mVideoEncoder->getOutput(&outBuf);
            CHECK_ENCODE_STATUS("encode");

            LOGV("output data size = %d", outBuf.dataSize);
            outfilledlen = outBuf.dataSize;
            outtimestamp = buffers[INPORT_INDEX]->nTimeStamp;


            if (outBuf.flag & ENCODE_BUFFERFLAG_SYNCFRAME) {
                outflags |= OMX_BUFFERFLAG_SYNCFRAME;
            }

            if(outBuf.flag & ENCODE_BUFFERFLAG_ENDOFFRAME) {
                outflags |= OMX_BUFFERFLAG_ENDOFFRAME;
                mFrameRetrieved = OMX_TRUE;
                retains[INPORT_INDEX] = BUFFER_RETAIN_ACCUMULATE;

            } else {
                retains[INPORT_INDEX] = BUFFER_RETAIN_GETAGAIN;  //get again

            }

            if (outfilledlen > 0) {
                retains[OUTPORT_INDEX] = BUFFER_RETAIN_NOT_RETAIN;
            } else {
                retains[OUTPORT_INDEX] = BUFFER_RETAIN_GETAGAIN;
            }

            break;
        case OMX_NaluFormatOneNaluPerBuffer:

            outBuf.format = OUTPUT_ONE_NAL;
            ret = mVideoEncoder->getOutput(&outBuf);
            CHECK_ENCODE_STATUS("getOutput");
            // Return code could not be ENCODE_BUFFER_TOO_SMALL
            // If we don't return error, we will have dead lock issue
            if (ret == ENCODE_BUFFER_TOO_SMALL) {
                return OMX_ErrorUndefined;
            }

            LOGV("output codec data size = %d", outBuf.dataSize);

            outfilledlen = outBuf.dataSize;
            outtimestamp = buffers[INPORT_INDEX]->nTimeStamp;

            if (outBuf.flag & ENCODE_BUFFERFLAG_SYNCFRAME) {
                outflags |= OMX_BUFFERFLAG_SYNCFRAME;
            }

            if(outBuf.flag & ENCODE_BUFFERFLAG_ENDOFFRAME) {
                outflags |= OMX_BUFFERFLAG_ENDOFFRAME;
                mFrameRetrieved = OMX_TRUE;
                retains[INPORT_INDEX] = BUFFER_RETAIN_ACCUMULATE;

            } else {
                retains[INPORT_INDEX] = BUFFER_RETAIN_GETAGAIN;  //get again
            }

            if (outfilledlen > 0) {
                retains[OUTPORT_INDEX] = BUFFER_RETAIN_NOT_RETAIN;
            } else {
                retains[OUTPORT_INDEX] = BUFFER_RETAIN_GETAGAIN;
            }

            break;
        case OMX_NaluFormatStartCodesSeparateFirstHeader:

            if(mFirstFrame) {
                LOGV("mFirstFrame\n");
                outBuf.format = OUTPUT_CODEC_DATA;
                ret = mVideoEncoder->getOutput(&outBuf);
                CHECK_ENCODE_STATUS("getOutput");

                // Return code could not be ENCODE_BUFFER_TOO_SMALL
                // If we don't return error, we will have dead lock issue
                if (ret == ENCODE_BUFFER_TOO_SMALL) {
                    return OMX_ErrorUndefined;
                }

                LOGV("output codec data size = %d", outBuf.dataSize);

                outflags |= OMX_BUFFERFLAG_CODECCONFIG;
                outflags |= OMX_BUFFERFLAG_ENDOFFRAME;
                outflags |= OMX_BUFFERFLAG_SYNCFRAME;

                // This input buffer need to be gotten again
                retains[INPORT_INDEX] = BUFFER_RETAIN_GETAGAIN;
                outfilledlen = outBuf.dataSize;
                mFirstFrame = OMX_FALSE;
            } else {
                outBuf.format = OUTPUT_EVERYTHING;
                ret = mVideoEncoder->getOutput(&outBuf);
                CHECK_ENCODE_STATUS("getOutput");

                LOGV("output data size = %d", outBuf.dataSize);

                outfilledlen = outBuf.dataSize;
                outtimestamp = buffers[INPORT_INDEX]->nTimeStamp;

                if (outBuf.flag & ENCODE_BUFFERFLAG_SYNCFRAME) {
                    outflags |= OMX_BUFFERFLAG_SYNCFRAME;
                }
                if(outBuf.flag & ENCODE_BUFFERFLAG_ENDOFFRAME) {
                    LOGV("Get buffer done\n");
                    outflags |= OMX_BUFFERFLAG_ENDOFFRAME;
                    mFrameRetrieved = OMX_TRUE;
                    retains[INPORT_INDEX] = BUFFER_RETAIN_ACCUMULATE;

                } else {
                    retains[INPORT_INDEX] = BUFFER_RETAIN_GETAGAIN;  //get again

                }
            }

            if (outfilledlen > 0) {
                retains[OUTPORT_INDEX] = BUFFER_RETAIN_NOT_RETAIN;
            } else {
                retains[OUTPORT_INDEX] = BUFFER_RETAIN_GETAGAIN;
            }
            break;
	case OMX_NaluFormatLengthPrefixedSeparateFirstHeader:

            if(mFirstFrame) {
                LOGV("mFirstFrame\n");
                outBuf.format = OUTPUT_CODEC_DATA;
                ret = mVideoEncoder->getOutput(&outBuf);
                CHECK_ENCODE_STATUS("getOutput");
                // Return code could not be ENCODE_BUFFER_TOO_SMALL
                // If we don't return error, we will have dead lock issue
                if (ret == ENCODE_BUFFER_TOO_SMALL) {
                    return OMX_ErrorUndefined;
                }

                LOGV("output codec data size = %d", outBuf.dataSize);

                outflags |= OMX_BUFFERFLAG_CODECCONFIG;
                outflags |= OMX_BUFFERFLAG_ENDOFFRAME;
                outflags |= OMX_BUFFERFLAG_SYNCFRAME;

                // This input buffer need to be gotten again
                retains[INPORT_INDEX] = BUFFER_RETAIN_GETAGAIN;
                outfilledlen = outBuf.dataSize;
                mFirstFrame = OMX_FALSE;
            } else {
                outBuf.format = OUTPUT_LENGTH_PREFIXED;
                ret = mVideoEncoder->getOutput(&outBuf);
                CHECK_ENCODE_STATUS("getOutput");

                LOGV("output data size = %d", outBuf.dataSize);

                outfilledlen = outBuf.dataSize;
                outtimestamp = buffers[INPORT_INDEX]->nTimeStamp;

                if (outBuf.flag & ENCODE_BUFFERFLAG_SYNCFRAME) {
                    outflags |= OMX_BUFFERFLAG_SYNCFRAME;
                }
                if(outBuf.flag & ENCODE_BUFFERFLAG_ENDOFFRAME) {
                    LOGV("Get buffer done\n");
                    outflags |= OMX_BUFFERFLAG_ENDOFFRAME;
                    mFrameRetrieved = OMX_TRUE;
                    retains[INPORT_INDEX] = BUFFER_RETAIN_ACCUMULATE;

                } else {
                    retains[INPORT_INDEX] = BUFFER_RETAIN_GETAGAIN;  //get again
                }
            }

            if (outfilledlen > 0) {
                retains[OUTPORT_INDEX] = BUFFER_RETAIN_NOT_RETAIN;
            } else {
                retains[OUTPORT_INDEX] = BUFFER_RETAIN_GETAGAIN;
            }
	    break;
    }

out:
    LOGV("output buffers = %p:%d, flag = %x", buffers[OUTPORT_INDEX]->pBuffer, outfilledlen, outflags);

    if(retains[OUTPORT_INDEX] != BUFFER_RETAIN_GETAGAIN) {
        buffers[OUTPORT_INDEX]->nFilledLen = outfilledlen;
        buffers[OUTPORT_INDEX]->nTimeStamp = outtimestamp;
        buffers[OUTPORT_INDEX]->nFlags = outflags;
    }

    if (retains[INPORT_INDEX] == BUFFER_RETAIN_NOT_RETAIN ||
            retains[INPORT_INDEX] == BUFFER_RETAIN_ACCUMULATE ) {
        mFrameInputCount ++;
    }

    if (retains[OUTPORT_INDEX] == BUFFER_RETAIN_NOT_RETAIN) mFrameOutputCount  ++;

#if 0
    if (avcEncParamIntelBitrateType.eControlRate != OMX_Video_Intel_ControlRateVideoConferencingMode) {
        if (oret == (OMX_ERRORTYPE) OMX_ErrorIntelExtSliceSizeOverflow) {
            oret = OMX_ErrorNone;
        }
    }
#endif
    LOGV_IF(oret == OMX_ErrorNone, "%s(),%d: exit, encode is done\n", __func__, __LINE__);

    return oret;

}

OMX_ERRORTYPE OMXVideoEncoderAVC::BuildHandlerList(void) {
    OMXVideoEncoderBase::BuildHandlerList();
    AddHandler(OMX_IndexParamVideoAvc, GetParamVideoAvc, SetParamVideoAvc);
    AddHandler((OMX_INDEXTYPE)OMX_IndexParamNalStreamFormat, GetParamNalStreamFormat, SetParamNalStreamFormat);
    AddHandler((OMX_INDEXTYPE)OMX_IndexParamNalStreamFormatSupported, GetParamNalStreamFormatSupported, SetParamNalStreamFormatSupported);
    AddHandler((OMX_INDEXTYPE)OMX_IndexParamNalStreamFormatSelect, GetParamNalStreamFormatSelect, SetParamNalStreamFormatSelect);
    AddHandler(OMX_IndexConfigVideoAVCIntraPeriod, GetConfigVideoAVCIntraPeriod, SetConfigVideoAVCIntraPeriod);
    AddHandler(OMX_IndexConfigVideoNalSize, GetConfigVideoNalSize, SetConfigVideoNalSize);
    AddHandler((OMX_INDEXTYPE)OMX_IndexConfigIntelSliceNumbers, GetConfigIntelSliceNumbers, SetConfigIntelSliceNumbers);
    AddHandler((OMX_INDEXTYPE)OMX_IndexParamIntelAVCVUI, GetParamIntelAVCVUI, SetParamIntelAVCVUI);
    AddHandler((OMX_INDEXTYPE)OMX_IndexParamVideoBytestream, GetParamVideoBytestream, SetParamVideoBytestream);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::GetParamVideoAvc(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_AVCTYPE *p = (OMX_VIDEO_PARAM_AVCTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);

    memcpy(p, &mParamAvc, sizeof(*p));
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::SetParamVideoAvc(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_AVCTYPE *p = (OMX_VIDEO_PARAM_AVCTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);
    CHECK_SET_PARAM_STATE();

    // TODO: do we need to check if port is enabled?
    // TODO: see SetPortAvcParam implementation - Can we make simple copy????
    memcpy(&mParamAvc, p, sizeof(mParamAvc));
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::GetParamNalStreamFormat(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_NALSTREAMFORMATTYPE *p = (OMX_NALSTREAMFORMATTYPE *)pStructure;

    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);
    // TODO: check if this is desired format
    p->eNaluFormat = mNalStreamFormat.eNaluFormat; //OMX_NaluFormatStartCodes;
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::SetParamNalStreamFormat(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_NALSTREAMFORMATTYPE *p = (OMX_NALSTREAMFORMATTYPE *)pStructure;

    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);
    LOGV("p->eNaluFormat =%d\n",p->eNaluFormat);
    if(p->eNaluFormat != OMX_NaluFormatStartCodes &&
            p->eNaluFormat != OMX_NaluFormatStartCodesSeparateFirstHeader &&
            p->eNaluFormat != OMX_NaluFormatOneNaluPerBuffer &&
            p->eNaluFormat != OMX_NaluFormatLengthPrefixedSeparateFirstHeader) {
        LOGE("Format not support\n");
        return OMX_ErrorUnsupportedSetting;
    }
    mNalStreamFormat.eNaluFormat = p->eNaluFormat;
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::GetParamNalStreamFormatSupported(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_NALSTREAMFORMATTYPE *p = (OMX_NALSTREAMFORMATTYPE *)pStructure;

    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);
    p->eNaluFormat = (OMX_NALUFORMATSTYPE)
                     (OMX_NaluFormatStartCodes |
                      OMX_NaluFormatStartCodesSeparateFirstHeader |
                      OMX_NaluFormatOneNaluPerBuffer|
                      OMX_NaluFormatLengthPrefixedSeparateFirstHeader);

    // TODO: check if this is desired format
    // OMX_NaluFormatFourByteInterleaveLength |
    // OMX_NaluFormatZeroByteInterleaveLength);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::SetParamNalStreamFormatSupported(OMX_PTR pStructure) {
    LOGW("SetParamNalStreamFormatSupported is not supported.");
    return OMX_ErrorUnsupportedSetting;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::GetParamNalStreamFormatSelect(OMX_PTR pStructure) {
    LOGW("GetParamNalStreamFormatSelect is not supported.");
    return OMX_ErrorUnsupportedSetting;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::SetParamNalStreamFormatSelect(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_NALSTREAMFORMATTYPE *p = (OMX_NALSTREAMFORMATTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);

    // return OMX_ErrorIncorrectStateOperation if not in Loaded state
    CHECK_SET_PARAM_STATE();

    if (p->eNaluFormat != OMX_NaluFormatStartCodes &&
            p->eNaluFormat != OMX_NaluFormatStartCodesSeparateFirstHeader &&
            p->eNaluFormat != OMX_NaluFormatOneNaluPerBuffer&&
            p->eNaluFormat != OMX_NaluFormatLengthPrefixedSeparateFirstHeader) {
        //p->eNaluFormat != OMX_NaluFormatFourByteInterleaveLength &&
        //p->eNaluFormat != OMX_NaluFormatZeroByteInterleaveLength) {
        // TODO: check if this is desried
        return OMX_ErrorBadParameter;
    }

    mNalStreamFormat = *p;
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::GetConfigVideoAVCIntraPeriod(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_CONFIG_AVCINTRAPERIOD *p = (OMX_VIDEO_CONFIG_AVCINTRAPERIOD *)pStructure;

    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);
    // TODO: populate mConfigAvcIntraPeriod from VideoEncoder
    // return OMX_ErrorNotReady if VideoEncoder is not created.
    memcpy(p, &mConfigAvcIntraPeriod, sizeof(*p));
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::SetConfigVideoAVCIntraPeriod(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    Encode_Status retStatus = ENCODE_SUCCESS;
    OMX_VIDEO_CONFIG_AVCINTRAPERIOD *p = (OMX_VIDEO_CONFIG_AVCINTRAPERIOD *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);

    // set in either Loaded state (ComponentSetParam) or Executing state (ComponentSetConfig)
    mConfigAvcIntraPeriod = *p;

    // return OMX_ErrorNone if not in Executing state
    // TODO:  return OMX_ErrorIncorrectStateOperation?
    CHECK_SET_CONFIG_STATE();

    // TODO: apply AVC Intra Period configuration in Executing state
    VideoConfigAVCIntraPeriod avcIntraPreriod;
    avcIntraPreriod.idrInterval = mConfigAvcIntraPeriod.nIDRPeriod;
    avcIntraPreriod.intraPeriod = mConfigAvcIntraPeriod.nPFrames;
    retStatus = mVideoEncoder->setConfig(&avcIntraPreriod);
    if(retStatus !=  ENCODE_SUCCESS) {
        LOGW("set avc intra prerod config failed");
    }
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::GetConfigVideoNalSize(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_CONFIG_NALSIZE *p = (OMX_VIDEO_CONFIG_NALSIZE *)pStructure;

    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);
    memcpy(p, &mConfigNalSize, sizeof(*p));
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::SetConfigVideoNalSize(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    Encode_Status retStatus = ENCODE_SUCCESS;
    if (mParamIntelBitrate.eControlRate == OMX_Video_Intel_ControlRateMax) {
        LOGE("SetConfigVideoNalSize failed. Feature is disabled.");
        return OMX_ErrorUnsupportedIndex;
    }
    OMX_VIDEO_CONFIG_NALSIZE *p = (OMX_VIDEO_CONFIG_NALSIZE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);

    // set in either Loaded  state (ComponentSetParam) or Executing state (ComponentSetConfig)
    mConfigNalSize = *p;

    // return OMX_ErrorNone if not in Executing state
    // TODO: return OMX_ErrorIncorrectStateOperation?
    CHECK_SET_CONFIG_STATE();

    if (mParamIntelBitrate.eControlRate != OMX_Video_Intel_ControlRateVideoConferencingMode) {
        LOGE("SetConfigVideoNalSize failed. Feature is supported only in VCM.");
        return OMX_ErrorUnsupportedSetting;
    }
    VideoConfigNALSize configNalSize;
    configNalSize.maxSliceSize = mConfigNalSize.nNaluBytes * 8;
    retStatus = mVideoEncoder->setConfig(&configNalSize);
    if(retStatus != ENCODE_SUCCESS) {
        LOGW("set NAL size config failed");
    }
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::GetConfigIntelSliceNumbers(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_CONFIG_INTEL_SLICE_NUMBERS *p = (OMX_VIDEO_CONFIG_INTEL_SLICE_NUMBERS *)pStructure;

    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);
    memcpy(p, &mConfigIntelSliceNumbers, sizeof(*p));
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::SetConfigIntelSliceNumbers(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    Encode_Status retStatus = ENCODE_SUCCESS;
    if (mParamIntelBitrate.eControlRate == OMX_Video_Intel_ControlRateMax) {
        LOGE("SetConfigIntelSliceNumbers failed. Feature is disabled.");
        return OMX_ErrorUnsupportedIndex;
    }
    OMX_VIDEO_CONFIG_INTEL_SLICE_NUMBERS *p = (OMX_VIDEO_CONFIG_INTEL_SLICE_NUMBERS *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);

    // set in either Loaded  state (ComponentSetParam) or Executing state (ComponentSetConfig)
    mConfigIntelSliceNumbers = *p;

    // return OMX_ErrorNone if not in Executing state
    // TODO: return OMX_ErrorIncorrectStateOperation?
    CHECK_SET_CONFIG_STATE();

    if (mParamIntelBitrate.eControlRate != OMX_Video_Intel_ControlRateVideoConferencingMode) {
        LOGE("SetConfigIntelSliceNumbers failed. Feature is supported only in VCM.");
        return OMX_ErrorUnsupportedSetting;
    }
    VideoConfigSliceNum sliceNum;
    sliceNum.sliceNum.iSliceNum = mConfigIntelSliceNumbers.nISliceNumber;
    sliceNum.sliceNum.pSliceNum = mConfigIntelSliceNumbers.nPSliceNumber;
    retStatus = mVideoEncoder->setConfig(&sliceNum);
    if(retStatus != ENCODE_SUCCESS) {
        LOGW("set silce num config failed!\n");
    }
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::GetParamIntelAVCVUI(OMX_PTR pStructure) {

    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_INTEL_AVCVUI *p = (OMX_VIDEO_PARAM_INTEL_AVCVUI *)pStructure;

    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);
    memcpy(p, &mParamIntelAvcVui, sizeof(*p));

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::SetParamIntelAVCVUI(OMX_PTR pStructure) {

    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_INTEL_AVCVUI *p = (OMX_VIDEO_PARAM_INTEL_AVCVUI *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);

    // set only in Loaded state (ComponentSetParam)
    CHECK_SET_PARAM_STATE();

    mParamIntelAvcVui = *p;
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::GetParamVideoBytestream(OMX_PTR pStructure) {
    return OMX_ErrorUnsupportedSetting;
}

OMX_ERRORTYPE OMXVideoEncoderAVC::SetParamVideoBytestream(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_BYTESTREAMTYPE *p = (OMX_VIDEO_PARAM_BYTESTREAMTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, OUTPORT_INDEX);

    // set only in Loaded state (ComponentSetParam)
    CHECK_SET_PARAM_STATE();

    if (p->bBytestream == OMX_TRUE) {
        mNalStreamFormat.eNaluFormat = OMX_NaluFormatStartCodes;
    } else {
        // TODO: do we need to override the Nalu format?
        mNalStreamFormat.eNaluFormat = OMX_NaluFormatZeroByteInterleaveLength;
    }

    return OMX_ErrorNone;
}


DECLARE_OMX_COMPONENT("OMX.Intel.VideoEncoder.AVC", "video_encoder.avc", OMXVideoEncoderAVC);
