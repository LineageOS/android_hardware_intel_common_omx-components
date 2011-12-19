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


// #define LOG_NDEBUG 0
#define LOG_TAG "OMXVideoDecoderWMV"
#include <utils/Log.h>
#include "OMXVideoDecoderWMV.h"

// Be sure to have an equal string in VideoDecoderHost.cpp (libmix)
static const char* WMV_MIME_TYPE = "video/wmv";

OMXVideoDecoderWMV::OMXVideoDecoderWMV() {
    LOGV("OMXVideoDecoderWMV is constructed.");
    mVideoDecoder = createVideoDecoder(WMV_MIME_TYPE);
    if (!mVideoDecoder) {
        LOGE("createVideoDecoder failed for \"%s\"", WMV_MIME_TYPE);
    }
    BuildHandlerList();
}

OMXVideoDecoderWMV::~OMXVideoDecoderWMV() {
    LOGV("OMXVideoDecoderWMV is destructed.");
}

OMX_ERRORTYPE OMXVideoDecoderWMV::InitInputPortFormatSpecific(OMX_PARAM_PORTDEFINITIONTYPE *paramPortDefinitionInput) {
    // OMX_PARAM_PORTDEFINITIONTYPE
    paramPortDefinitionInput->nBufferCountActual = INPORT_ACTUAL_BUFFER_COUNT;
    paramPortDefinitionInput->nBufferCountMin = INPORT_MIN_BUFFER_COUNT;
    paramPortDefinitionInput->nBufferSize = INPORT_BUFFER_SIZE;
    paramPortDefinitionInput->format.video.cMIMEType = (OMX_STRING)WMV_MIME_TYPE;
    paramPortDefinitionInput->format.video.eCompressionFormat = OMX_VIDEO_CodingWMV;

    // OMX_VIDEO_PARAM_WMVTYPE
    memset(&mParamWmv, 0, sizeof(mParamWmv));
    SetTypeHeader(&mParamWmv, sizeof(mParamWmv));
    mParamWmv.nPortIndex = INPORT_INDEX;
    mParamWmv.eFormat = OMX_VIDEO_WMVFormat9;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderWMV::ProcessorInit(void) {
    return OMXVideoDecoderBase::ProcessorInit();
}

OMX_ERRORTYPE OMXVideoDecoderWMV::ProcessorDeinit(void) {
    return OMXVideoDecoderBase::ProcessorDeinit();
}

OMX_ERRORTYPE OMXVideoDecoderWMV::ProcessorProcess(
        OMX_BUFFERHEADERTYPE ***pBuffers,
        buffer_retain_t *retains,
        OMX_U32 numberBuffers) {

    return OMXVideoDecoderBase::ProcessorProcess(pBuffers, retains, numberBuffers);
}

OMX_ERRORTYPE OMXVideoDecoderWMV::PrepareConfigBuffer(VideoConfigBuffer *p) {
    return OMXVideoDecoderBase::PrepareConfigBuffer(p);
}

OMX_ERRORTYPE OMXVideoDecoderWMV::PrepareDecodeBuffer(OMX_BUFFERHEADERTYPE *buffer, buffer_retain_t *retain, VideoDecodeBuffer *p) {
    return OMXVideoDecoderBase::PrepareDecodeBuffer(buffer, retain, p);
}

OMX_ERRORTYPE OMXVideoDecoderWMV::BuildHandlerList(void) {
    OMXVideoDecoderBase::BuildHandlerList();
    AddHandler(OMX_IndexParamVideoWmv, GetParamVideoWmv, SetParamVideoWmv);
    AddHandler(static_cast<OMX_INDEXTYPE>(OMX_IndexExtEnableNativeBuffer),GetNativeBufferMode,SetNativeBufferMode);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderWMV::GetParamVideoWmv(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_WMVTYPE *p = (OMX_VIDEO_PARAM_WMVTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, INPORT_INDEX);

    memcpy(p, &mParamWmv, sizeof(*p));
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderWMV::SetParamVideoWmv(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_WMVTYPE *p = (OMX_VIDEO_PARAM_WMVTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, INPORT_INDEX);
    CHECK_SET_PARAM_STATE();

    // TODO: do we need to check if port is enabled?
    memcpy(&mParamWmv, p, sizeof(mParamWmv));
    return OMX_ErrorNone;
}



OMX_ERRORTYPE OMXVideoDecoderWMV::GetNativeBufferMode(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    return OMX_ErrorNone; //would not be here
}

#define MAX_OUTPUT_BUFFER_COUNT_FOR_WMV 10
OMX_ERRORTYPE OMXVideoDecoderWMV::SetNativeBufferMode(OMX_PTR pStructure) {
    //OMX_ERRORTYPE ret;
    //EnableAndroidNativeBuffersParams *param = (EnableAndroidNativeBuffersParams*)pStructure;
    //CHECK_TYPE_HEADER(param);
    CHECK_SET_PARAM_STATE();
    mNativeBufferMode = true;
    PortVideo *port = NULL;
    port = static_cast<PortVideo *>(this->ports[OUTPORT_INDEX]);
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    memcpy(&port_def,port->GetPortDefinition(),sizeof(port_def));
    port_def.nBufferCountMin = 1;
    port_def.nBufferCountActual = MAX_OUTPUT_BUFFER_COUNT_FOR_WMV;
    port_def.format.video.cMIMEType = (OMX_STRING)"video/raw_ve";
    port_def.format.video.eColorFormat =static_cast<OMX_COLOR_FORMATTYPE>(0x7FA00EFF) ;//
    port->SetPortDefinition(&port_def,true);
    return OMX_ErrorNone;
}



DECLARE_OMX_COMPONENT("OMX.Intel.VideoDecoder.WMV", "video_decoder.wmv", OMXVideoDecoderWMV);


