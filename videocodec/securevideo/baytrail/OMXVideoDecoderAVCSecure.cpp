/*
* Copyright (c) 2009-2012 Intel Corporation.  All rights reserved.
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
#define LOG_TAG "OMXVideoDecoder"
#include <utils/Log.h>
#include "OMXVideoDecoderAVCSecure.h"
#include <time.h>
#include <signal.h>
#include <pthread.h>

extern "C" {
#include "widevine.h"
}

// Be sure to have an equal string in VideoDecoderHost.cpp (libmix)
static const char* AVC_MIME_TYPE = "video/avc";
static const char* AVC_SECURE_MIME_TYPE = "video/avc-secure";

#define PASS_FRAME_INFO 1
#define WV_CEILING(a,b) ((a)%(b)==0?(a):((a)/(b)+1)*(b))
#define DMA_BUFFER_SIZE (4 * 1024 * 1024)
#define SEC_INITIAL_OFFSET      0 //1024
#define SEC_BUFFER_SIZE         (4 * 1024 * 1024)
#define KEEP_ALIVE_INTERVAL     5 // seconds
#define DRM_KEEP_ALIVE_TIMER    1000000
#define WV_SESSION_ID           0x00000011
#define NALU_BUFFER_SIZE        8192
#define FLUSH_WAIT_INTERVAL     (30 * 1000) //30 ms

// SEC addressable region
#define SEC_REGION_SIZE                 (0x01000000) // 16 MB
#define SEC_REGION_FRAME_BUFFERS_OFFSET (0)
#define SEC_REGION_FRAME_BUFFERS_SIZE   (0x00F00000) // 15 MB
#define SEC_REGION_NALU_BUFFERS_OFFSET  (SEC_REGION_FRAME_BUFFERS_OFFSET+SEC_REGION_FRAME_BUFFERS_SIZE)
#define SEC_REGION_NALU_BUFFERS_SIZE    (NALU_BUFFER_SIZE*INPORT_ACTUAL_BUFFER_COUNT)
#define SEC_REGION_PAVP_INFO_OFFSET     (SEC_REGION_NALU_BUFFERS_OFFSET+SEC_REGION_NALU_BUFFERS_SIZE)
#define SEC_REGION_PAVP_INFO_SIZE       (sizeof(pavp_info_t)*INPORT_ACTUAL_BUFFER_COUNT)

// TEST ONLY
static uint8_t* g_SECRegionTest_REMOVE_ME;

#pragma pack(push, 1)
#define WV_AES_IV_SIZE 16
#define WV_MAX_PACKETS_IN_FRAME 20 /* 20*64K=1.3M, max frame size */
typedef struct {
    uint16_t packet_byte_size; // number of bytes in this PES packet, same for input and output
    uint16_t packet_is_not_encrypted; // 1 if this PES packet is not encrypted.  0 otherwise
    uint8_t  packet_iv[WV_AES_IV_SIZE]; // IV used for CBC-CTS decryption, if the PES packet is encrypted
} wv_packet_metadata;
// TODO: synchronize SECFrameBuffer with SECDataBuffer in WVCrypto.cpp
// - offset replaced by index.

struct SECFrameBuffer {
    uint32_t index;
    uint32_t size;
    uint8_t  *data;
    uint8_t  clear;  // 0 when SEC offset is valid, 1 when data is valid
    uint8_t num_entries;
    wv_packet_metadata  packet_metadata[WV_MAX_PACKETS_IN_FRAME];
    pavp_lib_session *pLibInstance;
    android::Mutex* pWVPAVPLock;
    struct meimm MeiMm;
    uint32_t VADmaBase;
};
#pragma pack(pop)

uint8_t          outiv[WV_AES_IV_SIZE];
OMXVideoDecoderAVCSecure::OMXVideoDecoderAVCSecure()
    : mSessionPaused(false),
      mVADmaBase(0),
      mpLibInstance(NULL) {
    LOGV("OMXVideoDecoderAVCSecure is constructed.");
    mVideoDecoder = createVideoDecoder(AVC_SECURE_MIME_TYPE);
    if (!mVideoDecoder) {
        LOGE("createVideoDecoder failed for \"%s\"", AVC_SECURE_MIME_TYPE);
    }
    // Override default native buffer count defined in the base class
    mNativeBufferCount = OUTPORT_NATIVE_BUFFER_COUNT;

    BuildHandlerList();
    mSECRegion.initialized = 0;
}

OMXVideoDecoderAVCSecure::~OMXVideoDecoderAVCSecure() {
    LOGV("OMXVideoDecoderAVCSecure is destructed.");

    if(g_SECRegionTest_REMOVE_ME)
    {
        delete[] g_SECRegionTest_REMOVE_ME;
        g_SECRegionTest_REMOVE_ME = NULL;
    }
    LOGV("OMXVideoDecoderAVCSecure is destructed.");
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::InitInputPortFormatSpecific(OMX_PARAM_PORTDEFINITIONTYPE *paramPortDefinitionInput) {
    // OMX_PARAM_PORTDEFINITIONTYPE
    paramPortDefinitionInput->nBufferCountActual = INPORT_ACTUAL_BUFFER_COUNT;
    paramPortDefinitionInput->nBufferCountMin = INPORT_MIN_BUFFER_COUNT;
    paramPortDefinitionInput->nBufferSize = INPORT_BUFFER_SIZE;
    paramPortDefinitionInput->format.video.cMIMEType = (OMX_STRING)AVC_MIME_TYPE;
    paramPortDefinitionInput->format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;

    // OMX_VIDEO_PARAM_AVCTYPE
    memset(&mParamAvc, 0, sizeof(mParamAvc));
    SetTypeHeader(&mParamAvc, sizeof(mParamAvc));
    mParamAvc.nPortIndex = INPORT_INDEX;
    // TODO: check eProfile/eLevel
    mParamAvc.eProfile = OMX_VIDEO_AVCProfileHigh; //OMX_VIDEO_AVCProfileBaseline;
    mParamAvc.eLevel = OMX_VIDEO_AVCLevel41; //OMX_VIDEO_AVCLevel1;

    // PREPRODUCTION: allocate 16MB region off the heap
    g_SECRegionTest_REMOVE_ME = new uint8_t[SEC_REGION_SIZE];
    if(!g_SECRegionTest_REMOVE_ME) {
        return OMX_ErrorInsufficientResources;
    }

    // Set up SEC-addressable memory region
    InitSECRegion(g_SECRegionTest_REMOVE_ME, SEC_REGION_SIZE);

    // Set memory allocator
    this->ports[INPORT_INDEX]->SetMemAllocator(MemAllocSEC, MemFreeSEC, this);

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ProcessorInit(void) {


    mSessionPaused = false;
    return OMXVideoDecoderBase::ProcessorInit();
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ProcessorDeinit(void) {
    // Session should be torn down in ProcessorStop, delayed to ProcessorDeinit
    // to allow remaining frames completely rendered.

    return OMXVideoDecoderBase::ProcessorDeinit();
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ProcessorStart(void) {
    uint32_t secOffset = 0;
    uint32_t secBufferSize = SEC_BUFFER_SIZE;
    uint32_t sessionID;

    mSessionPaused = false;
    return OMXVideoDecoderBase::ProcessorStart();
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ProcessorStop(void) {
    // destroy PAVP session
    if(mpLibInstance) {
        pavp_lib_session::pavp_lib_code rc = pavp_lib_session::status_ok;
        LOGI("Destroying the PAVP session...\n");
        rc = mpLibInstance->pavp_destroy_session();
            if (rc != pavp_lib_session::status_ok)
                LOGE("pavp_destroy_session failed with error 0x%x\n", rc);
    }
    meimm_free_memory(&mMeiMm);
    meimm_deinit(&mMeiMm);
    return OMXVideoDecoderBase::ProcessorStop();
}


OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ProcessorFlush(OMX_U32 portIndex) {
    return OMXVideoDecoderBase::ProcessorFlush(portIndex);
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ProcessorProcess(
        OMX_BUFFERHEADERTYPE ***pBuffers,
        buffer_retain_t *retains,
        OMX_U32 numberBuffers) {

    OMX_BUFFERHEADERTYPE *pInput = *pBuffers[INPORT_INDEX];
    SECFrameBuffer *secBuffer = (SECFrameBuffer *)pInput->pBuffer;
    if (secBuffer->size == 0) {
        // error occurs during decryption.
        LOGW("size of returned SEC buffer is 0, decryption fails.");
        mVideoDecoder->flush();
        usleep(FLUSH_WAIT_INTERVAL);
        OMX_BUFFERHEADERTYPE *pOutput = *pBuffers[OUTPORT_INDEX];
        pOutput->nFilledLen = 0;
        // reset SEC buffer size
        secBuffer->size = INPORT_BUFFER_SIZE;
        this->ports[INPORT_INDEX]->FlushPort();
        this->ports[OUTPORT_INDEX]->FlushPort();
        return OMX_ErrorNone;
    }

    OMX_ERRORTYPE ret;
    ret = OMXVideoDecoderBase::ProcessorProcess(pBuffers, retains, numberBuffers);
    if (ret != OMX_ErrorNone) {
        LOGE("OMXVideoDecoderBase::ProcessorProcess failed. Result: %#x", ret);
        return ret;
    }

    if (mSessionPaused && (retains[OUTPORT_INDEX] == BUFFER_RETAIN_GETAGAIN)) {
        retains[OUTPORT_INDEX] = BUFFER_RETAIN_NOT_RETAIN;
        OMX_BUFFERHEADERTYPE *pOutput = *pBuffers[OUTPORT_INDEX];
        pOutput->nFilledLen = 0;
        this->ports[INPORT_INDEX]->FlushPort();
        this->ports[OUTPORT_INDEX]->FlushPort();
    }

    return ret;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ProcessorPause(void) {
    return OMXVideoDecoderBase::ProcessorPause();
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ProcessorResume(void) {
    return OMXVideoDecoderBase::ProcessorResume();
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::PrepareConfigBuffer(VideoConfigBuffer *p) {
    OMX_ERRORTYPE ret;
    ret = OMXVideoDecoderBase::PrepareConfigBuffer(p);
    CHECK_RETURN_VALUE("OMXVideoDecoderBase::PrepareConfigBuffer");
    p->flag |=  WANT_SURFACE_PROTECTION;
    return ret;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::PrepareDecodeBuffer(OMX_BUFFERHEADERTYPE *buffer, buffer_retain_t *retain, VideoDecodeBuffer *p) {
    OMX_ERRORTYPE ret;
    ret = OMXVideoDecoderBase::PrepareDecodeBuffer(buffer, retain, p);
    CHECK_RETURN_VALUE("OMXVideoDecoderBase::PrepareDecodeBuffer");

    if (buffer->nFilledLen == 0) {
        return OMX_ErrorNone;
    }
    // OMX_BUFFERFLAG_CODECCONFIG is an optional flag
    // if flag is set, buffer will only contain codec data.
    if (buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
        LOGV("Received AVC codec data.");
        return ret;
    }
    p->flag |= HAS_COMPLETE_FRAME;

    if (buffer->nOffset != 0) {
        LOGW("buffer offset %d is not zero!!!", buffer->nOffset);
    }

    SECFrameBuffer *secBuffer = (SECFrameBuffer *)buffer->pBuffer;
    pavp_lib_session::pavp_lib_code rc = pavp_lib_session::status_ok;
    uint32_t parse_size = 0;

    if(!mpLibInstance && secBuffer->pLibInstance) {
        pavp_lib_session::pavp_lib_code rc = pavp_lib_session::status_ok;
        LOGE("PAVP Heavy session creation...");
        rc = secBuffer->pLibInstance->pavp_create_session(true);
        if (rc != pavp_lib_session::status_ok) {
            LOGE("PAVP Heavy: pavp_create_session failed with error 0x%x", rc);
            secBuffer->size = 0;
            ret = OMX_ErrorNotReady;
        } else {
            LOGE("PAVP Heavy session created succesfully");
	    mpLibInstance = secBuffer->pLibInstance;
            mLock =  secBuffer->pWVPAVPLock;
        }
        if ( ret == OMX_ErrorNone) {
            pavp_lib_session::pavp_lib_code rc = pavp_lib_session::status_ok;
            wv_set_xcript_key_in input;
            wv_set_xcript_key_out output;
 
            input.Header.ApiVersion = WV_API_VERSION;
            input.Header.CommandId =  wv_set_xcript_key;
            input.Header.Status = 0;
            input.Header.BufferLength = sizeof(input)-sizeof(PAVP_CMD_HEADER);
 
            if (secBuffer->pLibInstance) {
                rc = secBuffer->pLibInstance->sec_pass_through(
                    reinterpret_cast<BYTE*>(&input),
                    sizeof(input),
                    reinterpret_cast<BYTE*>(&output),
                    sizeof(output));
            }
 
            if (rc != pavp_lib_session::status_ok)
                LOGE("sec_pass_through:wv_set_xcript_key() failed with error 0x%x", rc);

            if (output.Header.Status) {
                LOGE("SEC failed: wv_set_xcript_key() FAILED 0x%x", output.Header.Status);
                secBuffer->size = 0;
                ret = OMX_ErrorNotReady;
            }
        }
    }

    if(secBuffer->pWVPAVPLock)
        mLock =  secBuffer->pWVPAVPLock;
    
    if(mpLibInstance) {
	bool balive = false;
        pavp_lib_session::pavp_lib_code rc = pavp_lib_session::status_ok;
        rc = mpLibInstance->pavp_is_session_alive(&balive);
        if (rc != pavp_lib_session::status_ok)
            LOGE("pavp_is_session_alive failed with error 0x%x", rc);
	if (balive == false || (ret == OMX_ErrorNotReady)) {
            LOGE("PAVP session is %s", balive?"active":"in-active");
            secBuffer->size = 0;
            ret = OMX_ErrorNotReady;
            //Destroy & re-create
            LOGI("Destroying the PAVP session...");
            rc = mpLibInstance->pavp_destroy_session();
            if (rc != pavp_lib_session::status_ok)
                LOGE("pavp_destroy_session failed with error 0x%x", rc);

            mpLibInstance = NULL;
        }
    }
    if ( ret == OMX_ErrorNone) {
        android::Mutex::Autolock autoLock(*mLock);
        wv_heci_process_video_frame_in input;
        wv_heci_process_video_frame_out output;
        sec_wv_packet_metadata metadata;

        input.Header.ApiVersion = WV_API_VERSION;
        input.Header.CommandId = wv_process_video_frame;
        input.Header.Status = 0;
        input.Header.BufferLength = sizeof(input) - sizeof(PAVP_CMD_HEADER);

        input.num_of_packets = secBuffer->num_entries;
        input.is_frame_not_encrypted = secBuffer->clear;
        input.src_offset = 0x0;                 //Src Frame offset is 0
        input.dest_offset = 1024 * 512;         //Dest Frame offset is 512KB
        input.metadata_offset = 1024 * 1024;    //Metadata offset is 1MB
        input.header_offset = (1024*1024)+512;  //Header offset is 1M + 512

        memset(&output, 0, sizeof(wv_heci_process_video_frame_out));

        for(int pes_count=0, pesoffset =0, dmaoffset=0; pes_count < secBuffer->num_entries; pes_count++) {

             dmaoffset = WV_CEILING(dmaoffset,32);

             metadata.packet_byte_size = secBuffer->packet_metadata[pes_count].packet_byte_size;
             memset(&metadata.packet_iv[0], 0x0, sizeof(metadata.packet_iv));
             memcpy(&secBuffer->data[(1024*1024)] + (pes_count * sizeof(metadata)), &metadata, sizeof(metadata));

             //copy frame data
             meimm_memcpy(&mMeiMm, dmaoffset, (secBuffer->data+pesoffset), metadata.packet_byte_size);
             //copy meta data
             meimm_memcpy(&mMeiMm, ((1024 * 1024)+(pes_count * sizeof(metadata))), &metadata, sizeof(metadata));
             //update offset
             dmaoffset += metadata.packet_byte_size;
             pesoffset += metadata.packet_byte_size;
        }

        if (secBuffer->pLibInstance) {
            rc = secBuffer->pLibInstance->sec_pass_through(
                      reinterpret_cast<BYTE*>(&input),
                      sizeof(input),
                      reinterpret_cast<BYTE*>(&output),
                      sizeof(output));

            if (rc != pavp_lib_session::status_ok) {
                LOGE("%s PAVP Failed: 0x%x", __FUNCTION__, rc);
                secBuffer->size = 0;
                ret = OMX_ErrorNotReady;
            }

            if (output.Header.Status != 0x0) {
                LOGE("%s SEC Failed:0x%x", __FUNCTION__, output.Header.Status);
                secBuffer->size = 0;
                ret = OMX_ErrorNotReady;
            } else {
                memcpy((unsigned char *)(secBuffer->data), (const unsigned int*) (mVADmaBase + (1024*512)), buffer->nFilledLen);
                parse_size = output.parsed_data_size;
                memcpy((unsigned char *)(secBuffer->data + buffer->nFilledLen + 4), (const unsigned int*) (mVADmaBase + ((1024*1024)+512)), output.parsed_data_size);
                memcpy(&outiv, output.iv, WV_AES_IV_SIZE);
            }
        }
    }

    if(ret == OMX_ErrorNone) {
        p->data = secBuffer->data + buffer->nOffset;
        p->size = buffer->nFilledLen;

        // Call "SEC" to parse frame
        SECParsedFrame* parsedFrame = &(mParsedFrames[secBuffer->index]);
        memcpy(parsedFrame->nalu_data, (unsigned char *)(secBuffer->data + buffer->nFilledLen + 4), parse_size);
        parsedFrame->nalu_data_size = parse_size;
        memcpy(parsedFrame->pavp_info->iv, outiv, WV_AES_IV_SIZE);

        // construct frame_info
        ret = ConstructFrameInfo(p->data, p->size, parsedFrame->pavp_info,
            parsedFrame->nalu_data, parsedFrame->nalu_data_size, &(parsedFrame->frame_info));

        if (parsedFrame->frame_info.num_nalus == 0 ) {
            LOGE("NALU parsing failed - num_nalus = 0!");
            secBuffer->size = 0;
            ret = OMX_ErrorNotReady;
        } 
#ifdef PASS_FRAME_INFO
        // Pass frame info to VideoDecoderAVCSecure in VideoDecodeBuffer
        p->data = (uint8_t *)&(parsedFrame->frame_info);
        p->size = sizeof(frame_info_t);
        p->flag = p->flag | IS_SECURE_DATA;
#else
        // Pass decrypted frame
        p->data = secBuffer->data + buffer->nOffset;
        p->size = buffer->nFilledLen;
#endif
    }
    return ret;
}

OMX_COLOR_FORMATTYPE OMXVideoDecoderAVCSecure::GetOutputColorFormat(int width, int height) {
    // BYT HWC expects Tiled output color format for all resolution
    return OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar_Tiled;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::BuildHandlerList(void) {
    OMXVideoDecoderBase::BuildHandlerList();
    AddHandler(OMX_IndexParamVideoAvc, GetParamVideoAvc, SetParamVideoAvc);
    AddHandler(OMX_IndexParamVideoProfileLevelQuerySupported, GetParamVideoAVCProfileLevel, SetParamVideoAVCProfileLevel);
    AddHandler(static_cast<OMX_INDEXTYPE> (OMX_IndexExtEnableNativeBuffer), GetNativeBufferMode, SetNativeBufferMode);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::GetParamVideoAvc(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_AVCTYPE *p = (OMX_VIDEO_PARAM_AVCTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, INPORT_INDEX);

    memcpy(p, &mParamAvc, sizeof(*p));
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::SetParamVideoAvc(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_AVCTYPE *p = (OMX_VIDEO_PARAM_AVCTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, INPORT_INDEX);
    CHECK_SET_PARAM_STATE();

    // TODO: do we need to check if port is enabled?
    // TODO: see SetPortAvcParam implementation - Can we make simple copy????
    memcpy(&mParamAvc, p, sizeof(mParamAvc));
    return OMX_ErrorNone;
}


OMX_ERRORTYPE OMXVideoDecoderAVCSecure::GetParamVideoAVCProfileLevel(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_PROFILELEVELTYPE *p = (OMX_VIDEO_PARAM_PROFILELEVELTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, INPORT_INDEX);
    CHECK_ENUMERATION_RANGE(p->nProfileIndex,1);

    p->eProfile = mParamAvc.eProfile;
    p->eLevel = mParamAvc.eLevel;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::SetParamVideoAVCProfileLevel(OMX_PTR pStructure) {
    LOGW("SetParamVideoAVCProfileLevel is not supported.");
    return OMX_ErrorUnsupportedSetting;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::GetNativeBufferMode(OMX_PTR pStructure) {
    LOGE("GetNativeBufferMode is not implemented");
    return OMX_ErrorNotImplemented;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::SetNativeBufferMode(OMX_PTR pStructure) {
    OMXVideoDecoderBase::SetNativeBufferMode(pStructure);
    PortVideo *port = NULL;
    port = static_cast<PortVideo *>(this->ports[OUTPORT_INDEX]);

    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    memcpy(&port_def,port->GetPortDefinition(),sizeof(port_def));
    port_def.format.video.eColorFormat = OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar_Tiled;
    port->SetPortDefinition(&port_def,true);

    return OMX_ErrorNone;
}

OMX_U8* OMXVideoDecoderAVCSecure::MemAllocSEC(OMX_U32 nSizeBytes, OMX_PTR pUserData) {
    OMXVideoDecoderAVCSecure* p = (OMXVideoDecoderAVCSecure *)pUserData;
    if (p) {
        return p->MemAllocSEC(nSizeBytes);
    }
    LOGE("NULL pUserData.");
    return NULL;
}

void OMXVideoDecoderAVCSecure::MemFreeSEC(OMX_U8 *pBuffer, OMX_PTR pUserData) {
    OMXVideoDecoderAVCSecure* p = (OMXVideoDecoderAVCSecure *)pUserData;
    if (p) {
        p->MemFreeSEC(pBuffer);
        return;
    }
    LOGE("NULL pUserData.");
}

OMX_U8* OMXVideoDecoderAVCSecure::MemAllocSEC(OMX_U32 nSizeBytes) {
    if (nSizeBytes > INPORT_BUFFER_SIZE) {
        LOGE("Invalid size (%lu) of memory to allocate.", nSizeBytes);
        return NULL;
    }
    LOGW_IF(nSizeBytes != INPORT_BUFFER_SIZE, "WARNING:MemAllocSEC asked to allocate buffer of size %lu (expected %lu)", nSizeBytes, INPORT_BUFFER_SIZE);

    int index = 0;
    for (; index < INPORT_ACTUAL_BUFFER_COUNT; index++) {
        if(!mSECRegion.frameBuffers.buffers[index].allocated) {
        break;
	}
    }
    if(index >= INPORT_ACTUAL_BUFFER_COUNT) {
        LOGE("No free buffers");
        return NULL;
    }

    SECFrameBuffer *pBuffer = new SECFrameBuffer;
    if (pBuffer == NULL) {
        LOGE("Failed to allocate SECFrameBuffer.");
        return NULL;
    }

    if(!mVADmaBase) {
        int status = meimm_init(&mMeiMm, true);

        if (status)
            LOGE("meimm_init FAILED ret: %#x", status);

        status =  meimm_alloc_map_memory(&mMeiMm, DMA_BUFFER_SIZE);
        if (status)
           LOGE("meimm_alloc_map_memory FAILED ret: %#x", status);

        mVADmaBase = (uint32_t)meimm_get_addr(&mMeiMm);

        LOGI("mVADMAOffset: %#x", mVADmaBase);
    }

    pBuffer->index = index;
    pBuffer->MeiMm = mMeiMm;
    pBuffer->VADmaBase = mVADmaBase;
    pBuffer->data = mSECRegion.frameBuffers.buffers[index].base;
    pBuffer->size = mSECRegion.frameBuffers.buffers[index].size;
    mParsedFrames[index].nalu_data = mSECRegion.naluBuffers.buffers[index].base;
    mParsedFrames[index].nalu_data_size = mSECRegion.naluBuffers.buffers[index].size;
    mParsedFrames[index].pavp_info = (pavp_info_t*)mSECRegion.pavpInfo.buffers[index].base;

    mSECRegion.frameBuffers.buffers[index].allocated = 1;
    mSECRegion.naluBuffers.buffers[index].allocated = 1;
    mSECRegion.pavpInfo.buffers[index].allocated = 1;

    return (OMX_U8 *) pBuffer;
}

void OMXVideoDecoderAVCSecure::MemFreeSEC(OMX_U8 *pBuffer) {
    SECFrameBuffer *p = (SECFrameBuffer*) pBuffer;
    if (p == NULL) {
        return;
    }

    mSECRegion.frameBuffers.buffers[p->index].allocated = 0;
    mSECRegion.naluBuffers.buffers[p->index].allocated = 0;
    mSECRegion.pavpInfo.buffers[p->index].allocated = 0;

    delete(p);
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::InitSECRegion(uint8_t* region, uint32_t size)
{
    if(mSECRegion.initialized) {
        return OMX_ErrorNone;
    }

    mSECRegion.base = region;
    mSECRegion.size = size;

    // Partition the SEC region
    mSECRegion.frameBuffers.base = mSECRegion.base + SEC_REGION_FRAME_BUFFERS_OFFSET;
    mSECRegion.frameBuffers.size = SEC_REGION_FRAME_BUFFERS_SIZE;

    mSECRegion.naluBuffers.base = mSECRegion.base + SEC_REGION_NALU_BUFFERS_OFFSET;
    mSECRegion.naluBuffers.size = SEC_REGION_NALU_BUFFERS_SIZE;

    mSECRegion.pavpInfo.base = mSECRegion.base + SEC_REGION_PAVP_INFO_OFFSET;
    mSECRegion.pavpInfo.size = SEC_REGION_PAVP_INFO_SIZE;

    for(int i = 0; i < INPORT_ACTUAL_BUFFER_COUNT; i++) {
        mSECRegion.frameBuffers.buffers[i].allocated = 0;
        mSECRegion.frameBuffers.buffers[i].base = mSECRegion.frameBuffers.base + (i*INPORT_BUFFER_SIZE);
        mSECRegion.frameBuffers.buffers[i].size = INPORT_BUFFER_SIZE;
        mSECRegion.naluBuffers.buffers[i].allocated = 0;
        mSECRegion.naluBuffers.buffers[i].base = mSECRegion.naluBuffers.base + (i*NALU_BUFFER_SIZE);
        mSECRegion.naluBuffers.buffers[i].size = NALU_BUFFER_SIZE;
        mSECRegion.pavpInfo.buffers[i].allocated = 0;
        mSECRegion.pavpInfo.buffers[i].base = mSECRegion.pavpInfo.base + (i*sizeof(pavp_info_t));
        mSECRegion.pavpInfo.buffers[i].size = sizeof(pavp_info_t);
    }

    mSECRegion.initialized = 1;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVCSecure::ConstructFrameInfo(
    uint8_t* frame_data,
    uint32_t frame_size,
    pavp_info_t* pavp_info,
    uint8_t* nalu_data,
    uint32_t nalu_data_size,
    frame_info_t* frame_info) {

    uint32_t* dword_ptr = (uint32_t*)nalu_data;
    uint8_t* byte_ptr = NULL;
    uint32_t data_size = 0;

    frame_info->data = frame_data;
    frame_info->length = frame_size;
    frame_info->pavp = pavp_info;

    frame_info->num_nalus = byteswap_32(*dword_ptr);
    dword_ptr++;
    for(uint32_t n = 0; n < frame_info->num_nalus; n++) {
        // Byteswap offset
        frame_info->nalus[n].offset = byteswap_32(*dword_ptr);
        dword_ptr++;

       // Byteswap nalu_size
        frame_info->nalus[n].length = byteswap_32(*dword_ptr);
        dword_ptr++;

        // Byteswap data_size
        data_size = byteswap_32(*dword_ptr);
        dword_ptr++;

        byte_ptr = (uint8_t*)dword_ptr;
        frame_info->nalus[n].type = *byte_ptr;
        switch(frame_info->nalus[n].type & 0x1F) {
        case h264_NAL_UNIT_TYPE_SPS:
        case h264_NAL_UNIT_TYPE_PPS:
        case h264_NAL_UNIT_TYPE_SEI:
            // Point to cleartext in nalu data buffer
            frame_info->nalus[n].data = byte_ptr;
            frame_info->nalus[n].slice_header = NULL;
            break;
        case h264_NAL_UNIT_TYPE_SLICE:
        case h264_NAL_UNIT_TYPE_IDR:
            // Point to ciphertext in frame buffer
            frame_info->nalus[n].data = frame_info->data + frame_info->nalus[n].offset;
            byteswap_slice_header((slice_header_t*)byte_ptr);
            frame_info->nalus[n].slice_header = (slice_header_t*)byte_ptr;

            frame_info->dec_ref_pic_marking = NULL;
            if(data_size > sizeof(slice_header_t)) {
                byte_ptr += sizeof(slice_header_t);
                frame_info->dec_ref_pic_marking = (dec_ref_pic_marking_t*)byte_ptr;
            }
            break;
        default:
            LOGE("ERROR: SEC returned an unsupported NALU type: %x", frame_info->nalus[n].type);
            frame_info->nalus[n].data = NULL;
            frame_info->nalus[n].slice_header = NULL;
            break;
        }

        // Advance to next NALU (including padding)
        dword_ptr += (data_size + 3) >> 2;
    }

    return OMX_ErrorNone;
}

DECLARE_OMX_COMPONENT("OMX.Intel.hw_vd.h264.secure", "video_decoder.avc", OMXVideoDecoderAVCSecure);
