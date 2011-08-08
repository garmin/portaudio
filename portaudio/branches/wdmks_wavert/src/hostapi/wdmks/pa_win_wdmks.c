/*
* $Id$
* PortAudio Windows WDM-KS interface
*
* Author: Andrew Baldwin, Robert Bielik (WaveRT)
* Based on the Open Source API proposed by Ross Bencina
* Copyright (c) 1999-2004 Andrew Baldwin, Ross Bencina, Phil Burk
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files
* (the "Software"), to deal in the Software without restriction,
* including without limitation the rights to use, copy, modify, merge,
* publish, distribute, sublicense, and/or sell copies of the Software,
* and to permit persons to whom the Software is furnished to do so,
* subject to the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
* ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
* CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
* WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
* The text above constitutes the entire PortAudio license; however, 
* the PortAudio community also makes the following non-binding requests:
*
* Any person wishing to distribute modifications to the Software is
* requested to send the modifications to the original developer so that
* they can be incorporated into the canonical version. It is also 
* requested that these non-binding requests be included along with the 
* license above.
*/

/** @file
@ingroup hostapi_src
@brief Portaudio WDM-KS host API.

@note This is the implementation of the Portaudio host API using the
Windows WDM/Kernel Streaming API in order to enable very low latency
playback and recording on all modern Windows platforms (e.g. 2K, XP, Vista, Win7)
Note: This API accesses the device drivers below the usual KMIXER
component which is normally used to enable multi-client mixing and
format conversion. That means that it will lock out all other users
of a device for the duration of active stream using those devices
*/

#include <stdio.h>

#if (defined(WIN32) && (defined(_MSC_VER) && (_MSC_VER >= 1200))) /* MSC version 6 and above */
#pragma comment( lib, "setupapi.lib" )
#endif

/* Debugging/tracing support */

#define PA_LOGE_
#define PA_LOGL_

#ifdef __GNUC__
#include <initguid.h>
#define _WIN32_WINNT 0x0501
#define WINVER 0x0501
#endif

#include <string.h> /* strlen() */
#include <assert.h>

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"
#include "portaudio.h"
#include "pa_debugprint.h"
#include "pa_memorybarrier.h"
#include "pa_ringbuffer.h"
#include "pa_trace.h"
#include "pa_win_waveformat.h"

#include "pa_win_wdmks.h"

#include <windows.h>
#include <winioctl.h>
#include <process.h>

#include <math.h>

/* The PA_HP_TRACE macro is used in RT parts, so it can be switched off without affecting
the rest of the debug tracing */
#if 1
#define PA_HP_TRACE(x)  PaUtil_AddHighSpeedLogMessage x ;
#else
#define PA_HP_TRACE(x)
#endif

#ifdef UNICODE
#define copywidestring    wcsncpy
#define copytowidestring  wcsncpy
#define formatstring      _snwprintf
#define cmpstring         wcscmp
#define cmpstringnocase   wcsicmp
#else
#define copywidestring    wcstombs
#define copytowidestring  mbstowcs
#define formatstring      _snprintf
#define cmpstring         strcmp
#define cmpstringnocase   _stricmp
#endif

#ifdef __GNUC__
#undef PA_LOGE_
#define PA_LOGE_ PA_DEBUG(("%s {\n",__FUNCTION__))
#undef PA_LOGL_
#define PA_LOGL_ PA_DEBUG(("} %s\n",__FUNCTION__))
/* These defines are set in order to allow the WIndows DirectX
* headers to compile with a GCC compiler such as MinGW
* NOTE: The headers may generate a few warning in GCC, but
* they should compile */
#define _INC_MMSYSTEM
#define _INC_MMREG
#define _NTRTL_ /* Turn off default definition of DEFINE_GUIDEX */
#define DEFINE_GUID_THUNK(name,guid) DEFINE_GUID(name,guid)
#define DEFINE_GUIDEX(n) DEFINE_GUID_THUNK( n, STATIC_##n )
#if !defined( DEFINE_WAVEFORMATEX_GUID )
#define DEFINE_WAVEFORMATEX_GUID(x) (USHORT)(x), 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
#endif
#define  WAVE_FORMAT_ADPCM      0x0002
#define  WAVE_FORMAT_IEEE_FLOAT 0x0003
#define  WAVE_FORMAT_ALAW       0x0006
#define  WAVE_FORMAT_MULAW      0x0007
#define  WAVE_FORMAT_MPEG       0x0050
#define  WAVE_FORMAT_DRM        0x0009
#define DYNAMIC_GUID_THUNK(l,w1,w2,b1,b2,b3,b4,b5,b6,b7,b8) {l,w1,w2,{b1,b2,b3,b4,b5,b6,b7,b8}}
#define DYNAMIC_GUID(data) DYNAMIC_GUID_THUNK(data)
#endif

/* use CreateThread for CYGWIN/Windows Mobile, _beginthreadex for all others */
#if !defined(__CYGWIN__) && !defined(_WIN32_WCE)
#define CREATE_THREAD_FUNCTION (HANDLE)_beginthreadex
#define PA_THREAD_FUNC static unsigned WINAPI
#else
#define CREATE_THREAD_FUNCTION CreateThread
#define PA_THREAD_FUNC static DWORD WINAPI
#endif

#ifdef _MSC_VER
#define NOMMIDS
#define DYNAMIC_GUID(data) {data}
#define _NTRTL_ /* Turn off default definition of DEFINE_GUIDEX */
#undef DEFINE_GUID
#define DEFINE_GUID(n,data) EXTERN_C const GUID n = {data}
#define DEFINE_GUID_THUNK(n,data) DEFINE_GUID(n,data)
#define DEFINE_GUIDEX(n) DEFINE_GUID_THUNK(n, STATIC_##n)
#endif

#include <setupapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <tchar.h>
#include <assert.h>
#include <stdio.h>

/* These next definitions allow the use of the KSUSER DLL */
typedef KSDDKAPI DWORD WINAPI KSCREATEPIN(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE);
extern HMODULE      DllKsUser;
extern KSCREATEPIN* FunctionKsCreatePin;

/* These definitions allows the use of AVRT.DLL on Vista and later OSs */
extern HMODULE      DllAvRt;
typedef HANDLE WINAPI AVSETMMTHREADCHARACTERISTICS(LPCSTR, LPDWORD TaskIndex);
typedef BOOL WINAPI AVREVERTMMTHREADCHARACTERISTICS(HANDLE);
typedef enum _PA_AVRT_PRIORITY
{
    PA_AVRT_PRIORITY_LOW = -1,
    PA_AVRT_PRIORITY_NORMAL,
    PA_AVRT_PRIORITY_HIGH,
    PA_AVRT_PRIORITY_CRITICAL
} PA_AVRT_PRIORITY, *PPA_AVRT_PRIORITY;
typedef BOOL WINAPI AVSETMMTHREADPRIORITY(HANDLE, PA_AVRT_PRIORITY);
extern AVSETMMTHREADCHARACTERISTICS* FunctionAvSetMmThreadCharacteristics;
extern AVREVERTMMTHREADCHARACTERISTICS* FunctionAvRevertMmThreadCharacteristics;
extern AVSETMMTHREADPRIORITY* FunctionAvSetMmThreadPriority;

/* An unspecified channel count (-1) is not treated correctly, so we replace it with
* an arbitrarily large number */ 
#define MAXIMUM_NUMBER_OF_CHANNELS 256

/* Forward definition to break circular type reference between pin and filter */
struct __PaWinWdmFilter;
typedef struct __PaWinWdmFilter PaWinWdmFilter;

struct __PaWinWdmPin;
typedef struct __PaWinWdmPin PaWinWdmPin;

struct __PaWinWdmStream;
typedef struct __PaWinWdmStream PaWinWdmStream;

/* Function prototype for getting audio position */
typedef PaError (*FunctionGetPinAudioPosition)(PaWinWdmPin*, unsigned long*);

/* Function prototype for memory barrier */
typedef void (*FunctionMemoryBarrier)(void);

struct __PaProcessThreadInfo;
typedef struct __PaProcessThreadInfo PaProcessThreadInfo;

typedef PaError (*FunctionPinHandler)(PaProcessThreadInfo* pInfo, unsigned eventIndex);

typedef enum __PaStreamStartEnum
{
    StreamStart_kOk,
    StreamStart_kFailed,
    StreamStart_kCnt
} PaStreamStartEnum;

/* Multiplexed input structure.
*  Very often several physical inputs are multiplexed through a MUX node (represented in the topology filter) */
typedef struct __PaWinWdmMuxedInput
{
    TCHAR                       friendlyName[MAX_PATH];
    ULONG                       muxPinId;
    ULONG                       muxNodeId;
    ULONG                       endpointPinId;
} PaWinWdmMuxedInput;

/* The Pin structure
* A pin is an input or output node, e.g. for audio flow */
struct __PaWinWdmPin
{
    HANDLE                      handle;
    PaWinWdmMuxedInput**        inputs;
    unsigned                    inputCount;
    TCHAR                       friendlyName[MAX_PATH];

    PaWinWdmFilter*             parentFilter;
    unsigned long               pinId;
    unsigned long               endpointPinId;  /* For output pins */
    KSPIN_CONNECT*              pinConnect;
    unsigned long               pinConnectSize;
    KSDATAFORMAT_WAVEFORMATEX*  ksDataFormatWfx;
    KSPIN_COMMUNICATION         communication;
    KSDATARANGE*                dataRanges;
    KSMULTIPLE_ITEM*            dataRangesItem;
    KSPIN_DATAFLOW              dataFlow;
    KSPIN_CINSTANCES            instances;
    unsigned long               frameSize;
    int                         maxChannels;
    unsigned long               formats;
    int                         bestSampleRate;
    ULONG                       *positionRegister;  /* WaveRT */
    ULONG                       hwLatency;          /* WaveRT */
    FunctionMemoryBarrier       fnMemBarrier;       /* WaveRT */
    FunctionGetPinAudioPosition fnAudioPosition;    /* WaveRT */
    FunctionPinHandler          fnEventHandler;
    FunctionPinHandler          fnSubmitHandler;
};

/* The Filter structure
* A filter has a number of pins and a "friendly name" */
struct __PaWinWdmFilter
{
    HANDLE         handle;
    PaWDMKSType    waveType;
    PaWDMKSSubType waveSubType;
    DWORD          deviceNode;
    int            pinCount;
    PaWinWdmPin**  pins;
    PaWinWdmFilter*  topologyFilter;
    TCHAR          filterName[MAX_PATH];
    TCHAR          friendlyName[MAX_PATH];
    int              validPinCount;
    int            usageCount;
    KSMULTIPLE_ITEM* connections;
    KSMULTIPLE_ITEM* nodes;
};


typedef struct __PaWinWdmDeviceInfo
{
    PaDeviceInfo    inheritedDeviceInfo;
    char             compositeName[MAX_PATH];   /* Composite name consists of pin name + device name */
    PaWinWdmFilter* filter;
    unsigned long   pin;
    int             muxPosition;    /* Used only for input devices */
    int             endpointPinId;
}
PaWinWdmDeviceInfo;

/* PaWinWdmHostApiRepresentation - host api datastructure specific to this implementation */
typedef struct __PaWinWdmHostApiRepresentation
{
    PaUtilHostApiRepresentation  inheritedHostApiRep;
    PaUtilStreamInterface        callbackStreamInterface;
    PaUtilStreamInterface        blockingStreamInterface;

    PaUtilAllocationGroup*       allocations;
    PaWinWdmFilter**             filters;
    int                          filterCount;
}
PaWinWdmHostApiRepresentation;

typedef struct __DATAPACKET
{
    KSSTREAM_HEADER  Header;
    OVERLAPPED       Signal;
} DATAPACKET;

typedef struct __PaIOPacket
{
    DATAPACKET*     packet;
    unsigned        startByte;
    unsigned        lengthBytes;
} PaIOPacket;

typedef struct __PaWinWdmIOInfo
{
    PaWinWdmPin*        pPin;
    char*               hostBuffer;
    unsigned            hostBufferSize;
    unsigned            framesPerBuffer;
    unsigned            bytesPerFrame;
    unsigned            bytesPerSample;
    unsigned            noOfPackets;    /* Only used in WaveCyclic */
    HANDLE              *events;        /* noOfPackets handles (WaveCyclic) 1 (WaveRT) */
    DATAPACKET          *packets;       /* noOfPackets packets (WaveCyclic) 2 (WaveRT) */
    /* WaveRT polled mode */
    unsigned            lastPosition; 
    unsigned            pollCntr;
} PaWinWdmIOInfo;

/* PaWinWdmStream - a stream data structure specifically for this implementation */
struct __PaWinWdmStream
{
    PaUtilStreamRepresentation  streamRepresentation;
    PaWDMKSSpecificStreamInfo   hostApiStreamInfo;
    PaUtilCpuLoadMeasurer       cpuLoadMeasurer;
    PaUtilBufferProcessor       bufferProcessor;

#if PA_TRACE_REALTIME_EVENTS
    LogHandle                   hLog;
#endif

    PaUtilAllocationGroup*      allocGroup;
    PaWinWdmIOInfo              capture;
    PaWinWdmIOInfo              render;
    int                         streamStarted;
    int                         streamActive;
    int                         streamStop;
    int                         streamAbort;
    int                         oldProcessPriority;
    HANDLE                      streamThread;
    HANDLE                      eventAbort;
    HANDLE                      eventStreamStart[StreamStart_kCnt];        /* 0 = OK, 1 = Failed */
    PaError                     threadResult;
    PaStreamFlags               streamFlags;
    int                         disableTimeout;
    /* Capture ring buffer */
    PaUtilRingBuffer            ringBuffer;
    char*                       ringBufferData;

    /* These values handle the case where the user wants to use fewer
    * channels than the device has */
    int                         userInputChannels;
    int                         deviceInputChannels;
    int                         userOutputChannels;
    int                         deviceOutputChannels;
};

/* Gather all processing variables in a struct */
struct __PaProcessThreadInfo 
{
    PaWinWdmStream              *stream;
    PaStreamCallbackTimeInfo    ti;
    PaStreamCallbackFlags       underover;
    int                         cbResult;
    volatile int                pending;
    volatile int                priming;
    volatile int                pinsStarted;
    unsigned long               timeout;
    unsigned                    captureHead;
    unsigned                    captureTail;
    unsigned                    renderHead;
    unsigned                    renderTail;
    PaIOPacket                  capturePackets[4];
    PaIOPacket                  renderPackets[4];
};

static const unsigned cPacketsArrayMask = 3;

HMODULE      DllKsUser = NULL;
KSCREATEPIN* FunctionKsCreatePin = NULL;

HMODULE         DllAvRt = NULL;
AVSETMMTHREADCHARACTERISTICS* FunctionAvSetMmThreadCharacteristics = NULL;
AVREVERTMMTHREADCHARACTERISTICS* FunctionAvRevertMmThreadCharacteristics = NULL;
AVSETMMTHREADPRIORITY* FunctionAvSetMmThreadPriority = NULL;

/* prototypes for functions declared in this file */

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

    PaError PaWinWdm_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );

#ifdef __cplusplus
}
#endif /* __cplusplus */

/* Low level I/O functions */
static PaError WdmSyncIoctl(HANDLE handle,
                            unsigned long ioctlNumber,
                            void* inBuffer,
                            unsigned long inBufferCount,
                            void* outBuffer,
                            unsigned long outBufferCount,
                            unsigned long* bytesReturned);
static PaError WdmGetPropertySimple(HANDLE handle,
                                    const GUID* const guidPropertySet,
                                    unsigned long property,
                                    void* value,
                                    unsigned long valueCount,
                                    void* instance,
                                    unsigned long instanceCount);
static PaError WdmSetPropertySimple(HANDLE handle,
                                    const GUID* const guidPropertySet,
                                    unsigned long property,
                                    void* value,
                                    unsigned long valueCount,
                                    void* instance,
                                    unsigned long instanceCount);

static PaError WdmGetPinPropertySimple(HANDLE  handle,
                                       unsigned long pinId,
                                       const GUID* const guidPropertySet,
                                       unsigned long property,
                                       void* value,
                                       unsigned long valueCount,
                                       unsigned long* byteCount);
static PaError WdmGetPinPropertyMulti(HANDLE  handle,
                                      unsigned long pinId,
                                      const GUID* const guidPropertySet,
                                      unsigned long property,
                                      KSMULTIPLE_ITEM** ksMultipleItem);
static PaError WdmGetPropertyMulti(HANDLE handle,
                                   const GUID* const guidPropertySet,
                                   unsigned long property,
                                   KSMULTIPLE_ITEM** ksMultipleItem);

static PaError WdmSetMuxNodeProperty(HANDLE handle,
                                     ULONG nodeId,
                                     ULONG pinId);


/** Pin management functions */
static PaWinWdmPin* PinNew(PaWinWdmFilter* parentFilter, unsigned long pinId, PaError* error);
static void PinFree(PaWinWdmPin* pin);
static void PinClose(PaWinWdmPin* pin);
static PaError PinInstantiate(PaWinWdmPin* pin);
/*static PaError PinGetState(PaWinWdmPin* pin, KSSTATE* state); NOT USED */
static PaError PinSetState(PaWinWdmPin* pin, KSSTATE state);
static PaError PinSetFormat(PaWinWdmPin* pin, const WAVEFORMATEX* format);
static PaError PinIsFormatSupported(PaWinWdmPin* pin, const WAVEFORMATEX* format);
/* WaveRT support */
static PaError PinGetBufferWithNotification(PaWinWdmPin* pPin, void** pBuffer, DWORD* pRequestedBufSize, BOOL* pbCallMemBarrier);
static PaError PinGetBufferWithoutNotification(PaWinWdmPin* pPin, void** pBuffer, DWORD* pRequestedBufSize, BOOL* pbCallMemBarrier);
static PaError PinRegisterPositionRegister(PaWinWdmPin* pPin);
static PaError PinRegisterNotificationHandle(PaWinWdmPin* pPin, HANDLE handle);
static PaError PinUnregisterNotificationHandle(PaWinWdmPin* pPin, HANDLE handle);
static PaError PinGetHwLatency(PaWinWdmPin* pPin, ULONG* pFifoSize, ULONG* pChipsetDelay, ULONG* pCodecDelay);
static PaError PinGetAudioPositionDirect(PaWinWdmPin* pPin, ULONG* pPosition);
static PaError PinGetAudioPositionViaIOCTL(PaWinWdmPin* pPin, ULONG* pPosition);

/* Filter management functions */
static PaWinWdmFilter* FilterNew(PaWDMKSType type, DWORD devNode, TCHAR* filterName, TCHAR* friendlyName, PaError* error);
static PaError FilterInitializePins(PaWinWdmFilter* filter);
static void FilterFree(PaWinWdmFilter* filter);
static PaWinWdmPin* FilterCreateRenderPin(
    PaWinWdmFilter* filter,
    int pinId,
    const WAVEFORMATEX* wfex,
    PaError* error);
static PaWinWdmPin* FilterCreateCapturePin(
    PaWinWdmFilter* filter,
    int pinId,
    const WAVEFORMATEX* wfex,
    PaError* error);
static PaError FilterUse(
                         PaWinWdmFilter* filter);
static void FilterRelease(
                          PaWinWdmFilter* filter);

/* Hot plug functions */
static BOOL IsDeviceTheSame(const PaWinWdmDeviceInfo* pDev1,
                            const PaWinWdmDeviceInfo* pDev2);

/* Interface functions */
static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError IsFormatSupported(
struct PaUtilHostApiRepresentation *hostApi,
    const PaStreamParameters *inputParameters,
    const PaStreamParameters *outputParameters,
    double sampleRate );
static PaError OpenStream(
struct PaUtilHostApiRepresentation *hostApi,
    PaStream** s,
    const PaStreamParameters *inputParameters,
    const PaStreamParameters *outputParameters,
    double sampleRate,
    unsigned long framesPerBuffer,
    PaStreamFlags streamFlags,
    PaStreamCallback *streamCallback,
    void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError ReadStream(
                          PaStream* stream,
                          void *buffer,
                          unsigned long frames );
static PaError WriteStream(
                           PaStream* stream,
                           const void *buffer,
                           unsigned long frames );
static signed long GetStreamReadAvailable( PaStream* stream );
static signed long GetStreamWriteAvailable( PaStream* stream );

/* Utility functions */
static unsigned long GetWfexSize(const WAVEFORMATEX* wfex);
static PaWinWdmFilter** BuildFilterList(int* filterCount, int* noOfPaDevices, PaError* result);
static BOOL PinWrite(HANDLE h, DATAPACKET* p);
static BOOL PinRead(HANDLE h, DATAPACKET* p);
static void DuplicateFirstChannelInt16(void* buffer, int channels, int samples);
static void DuplicateFirstChannelInt24(void* buffer, int channels, int samples);
PA_THREAD_FUNC ProcessingThread(void*);

/* Pin handler functions */
static PaError PaPinCaptureEventHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static PaError PaPinCaptureSubmitHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex);

static PaError PaPinRenderEventHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static PaError PaPinRenderSubmitHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex);

static PaError PaPinCaptureEventHandler_WaveRTEvent(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static PaError PaPinCaptureEventHandler_WaveRTPolled(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static PaError PaPinCaptureSubmitHandler_WaveRTEvent(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static PaError PaPinCaptureSubmitHandler_WaveRTPolled(PaProcessThreadInfo* pInfo, unsigned eventIndex);

static PaError PaPinRenderEventHandler_WaveRTEvent(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static PaError PaPinRenderEventHandler_WaveRTPolled(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static PaError PaPinRenderSubmitHandler_WaveRTEvent(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static PaError PaPinRenderSubmitHandler_WaveRTPolled(PaProcessThreadInfo* pInfo, unsigned eventIndex);

/* Function bodies */

#if defined(_DEBUG) && defined(PA_ENABLE_DEBUG_OUTPUT)
#define PA_WDMKS_SET_TREF
static PaTime tRef = 0;

static void PaWinWdmDebugPrintf(const char* fmt, ...)
{
    va_list list;
    char buffer[1024];
    PaTime t = PaUtil_GetTime() - tRef;
    va_start(list, fmt);
    _vsnprintf(buffer, 1023, fmt, list);
    va_end(list);
    PaUtil_DebugPrint("%6.3lf: %s", t, buffer);
}

#ifdef PA_DEBUG
#undef PA_DEBUG
#define PA_DEBUG(x)    PaWinWdmDebugPrintf x ;
#endif
#endif

static BOOL IsDeviceTheSame(const PaWinWdmDeviceInfo* pDev1,
                            const PaWinWdmDeviceInfo* pDev2)
{
    if (pDev1 == NULL || pDev2 == NULL)
        return FALSE;

    if (pDev1 == pDev2)
        return TRUE;

    if (strcmp(pDev1->compositeName, pDev2->compositeName) == 0)
        return TRUE;

    return FALSE;
}

static BOOL IsEarlierThanVista()
{
    OSVERSIONINFO osvi;
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (GetVersionEx(&osvi) && osvi.dwMajorVersion<6)
    {
        return TRUE;
    }
    return FALSE;
}



static void MemoryBarrierDummy(void)
{
    /* Do nothing */
}

static void MemoryBarrierRead(void)
{
    PaUtil_ReadMemoryBarrier();
}

static void MemoryBarrierWrite(void)
{
    PaUtil_WriteMemoryBarrier();
}

static unsigned long GetWfexSize(const WAVEFORMATEX* wfex)
{
    if( wfex->wFormatTag == WAVE_FORMAT_PCM )
    {
        return sizeof( WAVEFORMATEX );
    }
    else
    {
        return (sizeof( WAVEFORMATEX ) + wfex->cbSize);
    }
}

static void PaWinWDM_SetLastErrorInfo(long errCode, const char* fmt, ...)
{
    va_list list;
    char buffer[1024];
    va_start(list, fmt);
    _vsnprintf(buffer, 1023, fmt, list);
    va_end(list);
    PaUtil_SetLastHostErrorInfo(paWDMKS, errCode, buffer);
}

/*
Low level pin/filter access functions
*/
static PaError WdmSyncIoctl(
                            HANDLE handle,
                            unsigned long ioctlNumber,
                            void* inBuffer,
                            unsigned long inBufferCount,
                            void* outBuffer,
                            unsigned long outBufferCount,
                            unsigned long* bytesReturned)
{
    PaError result = paNoError;
    unsigned long dummyBytesReturned = 0;
    BOOL bRes;

    if( !bytesReturned )
    {
        /* Use a dummy as the caller hasn't supplied one */
        bytesReturned = &dummyBytesReturned;
    }

    bRes = DeviceIoControl(handle, ioctlNumber, inBuffer, inBufferCount, outBuffer, outBufferCount, bytesReturned, NULL);
    if (!bRes)
    {
        unsigned long error = GetLastError();
        if ( !(((error == ERROR_INSUFFICIENT_BUFFER ) || ( error == ERROR_MORE_DATA )) && 
            ( ioctlNumber == IOCTL_KS_PROPERTY ) &&
            ( outBufferCount == 0 ) ) ) 
        {
            result = paUnanticipatedHostError;
        }
    }
    return result;
}

static PaError WdmGetPropertySimple(HANDLE handle,
                                    const GUID* const guidPropertySet,
                                    unsigned long property,
                                    void* value,
                                    unsigned long valueCount,
                                    void* instance,
                                    unsigned long instanceCount)
{
    PaError result;
    KSPROPERTY* ksProperty;
    unsigned long propertyCount;

    propertyCount = sizeof(KSPROPERTY) + instanceCount;
    ksProperty = (KSPROPERTY*)_alloca( propertyCount );
    if( !ksProperty )
    {
        return paInsufficientMemory;
    }

    FillMemory((void*)ksProperty,sizeof(ksProperty),0);
    ksProperty->Set = *guidPropertySet;
    ksProperty->Id = property;
    ksProperty->Flags = KSPROPERTY_TYPE_GET;

    if( instance )
    {
        memcpy( (void*)(((char*)ksProperty)+sizeof(KSPROPERTY)), instance, instanceCount );
    }

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        ksProperty,
        propertyCount,
        value,
        valueCount,
        NULL);

    return result;
}

static PaError WdmSetPropertySimple(
                                    HANDLE handle,
                                    const GUID* const guidPropertySet,
                                    unsigned long property,
                                    void* value,
                                    unsigned long valueCount,
                                    void* instance,
                                    unsigned long instanceCount)
{
    PaError result;
    KSPROPERTY* ksProperty;
    unsigned long propertyCount  = 0;

    propertyCount = sizeof(KSPROPERTY) + instanceCount;
    ksProperty = (KSPROPERTY*)_alloca( propertyCount );
    if( !ksProperty )
    {
        return paInsufficientMemory;
    }

    ksProperty->Set = *guidPropertySet;
    ksProperty->Id = property;
    ksProperty->Flags = KSPROPERTY_TYPE_SET;

    if( instance )
    {
        memcpy((void*)((char*)ksProperty + sizeof(KSPROPERTY)), instance, instanceCount);
    }

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        ksProperty,
        propertyCount,
        value,
        valueCount,
        NULL);

    return result;
}

static PaError WdmGetPinPropertySimple(
                                       HANDLE  handle,
                                       unsigned long pinId,
                                       const GUID* const guidPropertySet,
                                       unsigned long property,
                                       void* value,
                                       unsigned long valueCount,
                                       unsigned long *byteCount)
{
    PaError result;

    KSP_PIN ksPProp;
    ksPProp.Property.Set = *guidPropertySet;
    ksPProp.Property.Id = property;
    ksPProp.Property.Flags = KSPROPERTY_TYPE_GET;
    ksPProp.PinId = pinId;
    ksPProp.Reserved = 0;

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        &ksPProp,
        sizeof(KSP_PIN),
        value,
        valueCount,
        byteCount);

    return result;
}

static PaError WdmGetPinPropertyMulti(
                                      HANDLE handle,
                                      unsigned long pinId,
                                      const GUID* const guidPropertySet,
                                      unsigned long property,
                                      KSMULTIPLE_ITEM** ksMultipleItem)
{
    PaError result;
    unsigned long multipleItemSize = 0;
    KSP_PIN ksPProp;

    ksPProp.Property.Set = *guidPropertySet;
    ksPProp.Property.Id = property;
    ksPProp.Property.Flags = KSPROPERTY_TYPE_GET;
    ksPProp.PinId = pinId;
    ksPProp.Reserved = 0;

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        &ksPProp.Property,
        sizeof(KSP_PIN),
        NULL,
        0,
        &multipleItemSize);
    if( result != paNoError )
    {
        return result;
    }

    *ksMultipleItem = (KSMULTIPLE_ITEM*)PaUtil_AllocateMemory( multipleItemSize );
    if( !*ksMultipleItem )
    {
        return paInsufficientMemory;
    }

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        &ksPProp,
        sizeof(KSP_PIN),
        (void*)*ksMultipleItem,
        multipleItemSize,
        NULL);

    if( result != paNoError )
    {
        PaUtil_FreeMemory( ksMultipleItem );
    }

    return result;
}

static PaError WdmGetPropertyMulti(HANDLE handle,
                                   const GUID* const guidPropertySet,
                                   unsigned long property,
                                   KSMULTIPLE_ITEM** ksMultipleItem)
{
    PaError result;
    unsigned long multipleItemSize = 0;
    KSPROPERTY ksProp;

    ksProp.Set = *guidPropertySet;
    ksProp.Id = property;
    ksProp.Flags = KSPROPERTY_TYPE_GET;

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        &ksProp,
        sizeof(KSPROPERTY),
        NULL,
        0,
        &multipleItemSize);
    if( result != paNoError )
    {
        return result;
    }

    *ksMultipleItem = (KSMULTIPLE_ITEM*)PaUtil_AllocateMemory( multipleItemSize );
    if( !*ksMultipleItem )
    {
        return paInsufficientMemory;
    }

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        &ksProp,
        sizeof(KSPROPERTY),
        (void*)*ksMultipleItem,
        multipleItemSize,
        NULL);

    if( result != paNoError )
    {
        PaUtil_FreeMemory( ksMultipleItem );
    }

    return result;
}

static PaError WdmSetMuxNodeProperty(HANDLE handle,
                                     ULONG nodeId,
                                     ULONG pinId)
{
    PaError result = paNoError;
    KSNODEPROPERTY prop;
    prop.Property.Set = KSPROPSETID_Audio;
    prop.Property.Id = KSPROPERTY_AUDIO_MUX_SOURCE;
    prop.Property.Flags = KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_TOPOLOGY;
    prop.NodeId = nodeId;
    prop.Reserved = 0;

    result = WdmSyncIoctl(handle, IOCTL_KS_PROPERTY, &prop, sizeof(KSNODEPROPERTY), &pinId, sizeof(ULONG), NULL);

    return result;
}

/* Used when traversing topology for outputs */
static const KSTOPOLOGY_CONNECTION* GetConnectionTo(const KSTOPOLOGY_CONNECTION* pFrom, PaWinWdmFilter* filter, int muxIdx)
{
    unsigned i;
    const KSTOPOLOGY_CONNECTION* connections = (const KSTOPOLOGY_CONNECTION*)(filter->connections + 1);
    (void)muxIdx;
    for (i = 0; i < filter->connections->Count; ++i)
    {
        const KSTOPOLOGY_CONNECTION* pConn = connections + i;
        if (pConn->FromNode == pFrom->ToNode &&
            (pFrom->ToNode != KSFILTER_NODE || pConn->FromNodePin == pFrom->ToNodePin))
        {
            return pConn;
        }
    }
    return NULL;
}

/* Used when traversing topology for inputs */
static const KSTOPOLOGY_CONNECTION* GetConnectionFrom(const KSTOPOLOGY_CONNECTION* pTo, PaWinWdmFilter* filter, int muxIdx)
{
    unsigned i;
    const KSTOPOLOGY_CONNECTION* connections = (const KSTOPOLOGY_CONNECTION*)(filter->connections + 1);
    int muxCntr = 0;
    for (i = 0; i < filter->connections->Count; ++i)
    {
        const KSTOPOLOGY_CONNECTION* pConn = connections + i;
        if (pConn->ToNode == pTo->FromNode &&
            (pTo->FromNode != KSFILTER_NODE || pConn->ToNodePin == pTo->FromNodePin))
        {
            if (muxIdx >= 0)
            {
                if (muxCntr < muxIdx)
                {
                    ++muxCntr;
                    continue;
                }
            }
            return pConn;
        }
    }
    return NULL;
}

static ULONG GetNumberOfConnectionsTo(const KSTOPOLOGY_CONNECTION* pTo, PaWinWdmFilter* filter)
{
    ULONG retval = 0;
    unsigned i;
    const KSTOPOLOGY_CONNECTION* connections = (const KSTOPOLOGY_CONNECTION*)(filter->connections + 1);
    for (i = 0; i < filter->connections->Count; ++i)
    {
        const KSTOPOLOGY_CONNECTION* pConn = connections + i;
        if (pConn->ToNode == pTo->FromNode &&
            (pTo->FromNode != KSFILTER_NODE || pConn->ToNodePin == pTo->FromNodePin))
        {
            ++retval;
        }
    }
    return retval;
}

typedef const KSTOPOLOGY_CONNECTION *(*TFnGetConnection)(const KSTOPOLOGY_CONNECTION*, PaWinWdmFilter*, int);

static ULONG GetConnectedPin(ULONG startPin, BOOL forward, PaWinWdmFilter* filter, int muxPosition, ULONG *muxInputPinId, ULONG *muxNodeId)
{
    KSTOPOLOGY_CONNECTION startConnection = { KSFILTER_NODE, startPin,  KSFILTER_NODE, startPin };
    const KSTOPOLOGY_CONNECTION *pFrom = &startConnection;
    TFnGetConnection fnGetConnection = forward ? GetConnectionTo : GetConnectionFrom ;
    while (1)
    {
        const KSTOPOLOGY_CONNECTION * conn = fnGetConnection(pFrom, filter, -1);
        if (forward ? conn->ToNode == KSFILTER_NODE : conn->FromNode == KSFILTER_NODE)
        {
            return forward ? conn->ToNodePin : conn->FromNodePin;
        }
        else
        {
            if (filter->nodes->Count > 0 && !forward && muxPosition >= 0)
            {
                const GUID* nodes = (const GUID*)(filter->nodes + 1);
                if (IsEqualGUID(&nodes[conn->FromNode], &KSNODETYPE_MUX))
                {
                    ULONG nConn = GetNumberOfConnectionsTo(conn, filter);
                    conn = fnGetConnection(conn, filter, muxPosition);
                    if (conn == NULL)
                    {
                        break;
                    }
                    if (muxInputPinId != 0)
                    {
                        *muxInputPinId = conn->ToNodePin;
                    }
                    if (muxNodeId != 0)
                    {
                        *muxNodeId = conn->ToNode;
                    }
                }
            }
        }
        pFrom = conn;
    }
    return KSFILTER_NODE;
}

typedef struct __PaUsbTerminalGUIDToName 
{
    USHORT     usbGUID;
    TCHAR      name[64];
} PaUsbTerminalGUIDToName;

static const PaUsbTerminalGUIDToName kNames[] =
{
    /* Types copied from: http://msdn.microsoft.com/en-us/library/ff537742(v=vs.85).aspx */
    /* Input terminal types */
    { 0x0201, TEXT("Microphone") },
    { 0x0202, TEXT("Desktop Microphone") },
    { 0x0203, TEXT("Personal Microphone") },
    { 0x0204, TEXT("Omni Directional Microphone") },
    { 0x0205, TEXT("Microphone Array") },
    { 0x0206, TEXT("Processing Microphone Array") },
    /* Output terminal types */
    { 0x0301, TEXT("Speakers") },
    { 0x0302, TEXT("Headphones") },
    { 0x0303, TEXT("Head Mounted Display Audio") },
    { 0x0304, TEXT("Desktop Speaker") },
    { 0x0305, TEXT("Room Speaker") },
    { 0x0306, TEXT("Communication Speaker") },
    { 0x0307, TEXT("LFE Speakers") },
    /* External terminal types */
    { 0x0601, TEXT("Analog") },
    { 0x0602, TEXT("Digital") },
    { 0x0603, TEXT("Line") },
    { 0x0604, TEXT("Audio") },
    { 0x0605, TEXT("SPDIF") },
};

static const unsigned kNamesCnt = sizeof(kNames)/sizeof(PaUsbTerminalGUIDToName);

static int PaUsbTerminalGUIDToNameCmp(const void* lhs, const void* rhs)
{
    const PaUsbTerminalGUIDToName* pL = (const PaUsbTerminalGUIDToName*)lhs;
    const PaUsbTerminalGUIDToName* pR = (const PaUsbTerminalGUIDToName*)rhs;
    return ((int)(pL->usbGUID) - (int)(pR->usbGUID));
}

static PaError GetNameFromCategory(const GUID* pGUID, BOOL input, TCHAR* name, unsigned length)
{
    PaError result = paUnanticipatedHostError;
    USHORT usbTerminalGUID = (USHORT)(pGUID->Data1 - 0xDFF219E0);

    if (input && usbTerminalGUID >= 0x301 && usbTerminalGUID < 0x400)
    {
        /* Output terminal name for an input !? Set it to Line! */
        usbTerminalGUID = 0x603;
    }
    if (!input && usbTerminalGUID >= 0x201 && usbTerminalGUID < 0x300)
    {
        /* Input terminal name for an output !? Set it to Line! */
        usbTerminalGUID = 0x603;
    }
    if (usbTerminalGUID >= 0x201 && usbTerminalGUID < 0x713)
    {
        PaUsbTerminalGUIDToName s = { usbTerminalGUID };
        const PaUsbTerminalGUIDToName* ptr = bsearch(
            &s,
            kNames,
            kNamesCnt,
            sizeof(PaUsbTerminalGUIDToName),
            PaUsbTerminalGUIDToNameCmp
            );
        if (ptr != 0)
        {
            if (name != NULL && length > 0)
            {
                int n = formatstring(name, length, TEXT("%s"), ptr->name);
                if (usbTerminalGUID >= 0x601 && usbTerminalGUID < 0x700)
                {
                    formatstring(name + n, length - n, TEXT(" %s"), (input ? TEXT("In"):TEXT("Out")));
                }
            }
            result = paNoError;
        }
    }
    return result;
}


/*
Create a new pin object belonging to a filter
The pin object holds all the configuration information about the pin
before it is opened, and then the handle of the pin after is opened
*/
static PaWinWdmPin* PinNew(PaWinWdmFilter* parentFilter, unsigned long pinId, PaError* error)
{
    PaWinWdmPin* pin;
    PaError result;
    unsigned long i;
    KSMULTIPLE_ITEM* item = NULL;
    KSIDENTIFIER* identifier;
    KSDATARANGE* dataRange;
    const ULONG streamingId = (parentFilter->waveType == Type_kWaveRT) ? KSINTERFACE_STANDARD_LOOPED_STREAMING : KSINTERFACE_STANDARD_STREAMING;

    PA_LOGE_;
    PA_DEBUG(("Creating pin %d:\n",pinId));

    /* Allocate the new PIN object */
    pin = (PaWinWdmPin*)PaUtil_AllocateMemory( sizeof(PaWinWdmPin) );
    if( !pin )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* Zero the pin object */
    /* memset( (void*)pin, 0, sizeof(PaWinWdmPin) ); */

    pin->parentFilter = parentFilter;
    pin->pinId = pinId;

    /* Allocate a connect structure */
    pin->pinConnectSize = sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEX);
    pin->pinConnect = (KSPIN_CONNECT*)PaUtil_AllocateMemory( pin->pinConnectSize );
    if( !pin->pinConnect )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* Configure the connect structure with default values */
    pin->pinConnect->Interface.Set               = KSINTERFACESETID_Standard;
    pin->pinConnect->Interface.Id                = streamingId;
    pin->pinConnect->Interface.Flags             = 0;
    pin->pinConnect->Medium.Set                  = KSMEDIUMSETID_Standard;
    pin->pinConnect->Medium.Id                   = KSMEDIUM_TYPE_ANYINSTANCE;
    pin->pinConnect->Medium.Flags                = 0;
    pin->pinConnect->PinId                       = pinId;
    pin->pinConnect->PinToHandle                 = NULL;
    pin->pinConnect->Priority.PriorityClass      = KSPRIORITY_NORMAL;
    pin->pinConnect->Priority.PrioritySubClass   = 1;
    pin->ksDataFormatWfx = (KSDATAFORMAT_WAVEFORMATEX*)(pin->pinConnect + 1);
    pin->ksDataFormatWfx->DataFormat.FormatSize  = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    pin->ksDataFormatWfx->DataFormat.Flags       = 0;
    pin->ksDataFormatWfx->DataFormat.Reserved    = 0;
    pin->ksDataFormatWfx->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    pin->ksDataFormatWfx->DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
    pin->ksDataFormatWfx->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

    pin->frameSize = 0; /* Unknown until we instantiate pin */

    /* Get the COMMUNICATION property */
    result = WdmGetPinPropertySimple(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_COMMUNICATION,
        &pin->communication,
        sizeof(KSPIN_COMMUNICATION),
        NULL);
    if( result != paNoError )
        goto error;

    if( /*(pin->communication != KSPIN_COMMUNICATION_SOURCE) &&*/
        (pin->communication != KSPIN_COMMUNICATION_SINK) &&
        (pin->communication != KSPIN_COMMUNICATION_BOTH) )
    {
        PA_DEBUG(("Not source/sink\n"));
        result = paInvalidDevice;
        goto error;
    }

    /* Get dataflow information */
    result = WdmGetPinPropertySimple(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_DATAFLOW,
        &pin->dataFlow,
        sizeof(KSPIN_DATAFLOW),
        NULL);

    if( result != paNoError )
        goto error;

    /* Get the INTERFACE property list */
    result = WdmGetPinPropertyMulti(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_INTERFACES,
        &item);

    if( result != paNoError )
        goto error;

    identifier = (KSIDENTIFIER*)(item+1);

    /* Check that at least one interface is STANDARD_STREAMING */
    result = paUnanticipatedHostError;
    for( i = 0; i < item->Count; i++ )
    {
        if( IsEqualGUID(&identifier[i].Set, &KSINTERFACESETID_Standard) && ( identifier[i].Id == streamingId ) )
        {
            result = paNoError;
            break;
        }
    }

    if( result != paNoError )
    {
        PA_DEBUG(("No %s streaming\n", streamingId==KSINTERFACE_STANDARD_LOOPED_STREAMING?"looped":"standard"));
        goto error;
    }

    /* Don't need interfaces any more */
    PaUtil_FreeMemory( item );
    item = NULL;

    /* Get the MEDIUM properties list */
    result = WdmGetPinPropertyMulti(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_MEDIUMS,
        &item);

    if( result != paNoError )
        goto error;

    identifier = (KSIDENTIFIER*)(item+1); /* Not actually necessary... */

    /* Check that at least one medium is STANDARD_DEVIO */
    result = paUnanticipatedHostError;
    for( i = 0; i < item->Count; i++ )
    {
        if( IsEqualGUID(&identifier[i].Set, &KSMEDIUMSETID_Standard) && ( identifier[i].Id == KSMEDIUM_STANDARD_DEVIO ) )
        {
            result = paNoError;
            break;
        }
    }

    if( result != paNoError )
    {
        PA_DEBUG(("No standard devio\n"));
        goto error;
    }
    /* Don't need mediums any more */
    PaUtil_FreeMemory( item );
    item = NULL;

    /* Get DATARANGES */
    result = WdmGetPinPropertyMulti(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_DATARANGES,
        &pin->dataRangesItem);

    if( result != paNoError )
        goto error;

    pin->dataRanges = (KSDATARANGE*)(pin->dataRangesItem +1);

    /* Check that at least one datarange supports audio */
    result = paUnanticipatedHostError;
    dataRange = pin->dataRanges;
    pin->maxChannels = 0;
    pin->bestSampleRate = 0;
    pin->formats = 0;
    for( i = 0; i <pin->dataRangesItem->Count; i++)
    {
        PA_DEBUG(("DR major format %x\n",*(unsigned long*)(&(dataRange->MajorFormat))));
        /* Check that subformat is WAVEFORMATEX, PCM or WILDCARD */
        if( IS_VALID_WAVEFORMATEX_GUID(&dataRange->SubFormat) ||
            IsEqualGUID(&dataRange->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM) ||
            IsEqualGUID(&dataRange->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ||
            IsEqualGUID(&dataRange->SubFormat, &KSDATAFORMAT_SUBTYPE_WILDCARD) ||
            IsEqualGUID(&dataRange->MajorFormat, &KSDATAFORMAT_TYPE_AUDIO) )
        {
            result = paNoError;
            /* Record the maximum possible channels with this pin */
            if( ((KSDATARANGE_AUDIO*)dataRange)->MaximumChannels == (ULONG) -1 )
            {
                pin->maxChannels = MAXIMUM_NUMBER_OF_CHANNELS;
            }
            else if( (int) ((KSDATARANGE_AUDIO*)dataRange)->MaximumChannels > pin->maxChannels )
            {
                pin->maxChannels = (int) ((KSDATARANGE_AUDIO*)dataRange)->MaximumChannels;
            }
            PA_DEBUG(("MaxChannel: %d\n",pin->maxChannels));

            /* Record the formats (bit depths) that are supported */
            if( ((KSDATARANGE_AUDIO*)dataRange)->MinimumBitsPerSample <= 8 &&
                ((KSDATARANGE_AUDIO*)dataRange)->MaximumBitsPerSample >= 8)
            {
                pin->formats |= paInt8;
                PA_DEBUG(("Format PCM 8 bit supported\n"));
            }
            if( ((KSDATARANGE_AUDIO*)dataRange)->MinimumBitsPerSample <= 16 &&
                ((KSDATARANGE_AUDIO*)dataRange)->MaximumBitsPerSample >= 16)
            {
                pin->formats |= paInt16;
                PA_DEBUG(("Format PCM 16 bit supported\n"));
            }
            if( ((KSDATARANGE_AUDIO*)dataRange)->MinimumBitsPerSample <= 24 &&
                ((KSDATARANGE_AUDIO*)dataRange)->MaximumBitsPerSample >= 24 )
            {
                pin->formats |= paInt24;
                PA_DEBUG(("Format PCM 24 bit supported\n"));
            }
            if( ((KSDATARANGE_AUDIO*)dataRange)->MinimumBitsPerSample <= 32 &&
                ((KSDATARANGE_AUDIO*)dataRange)->MaximumBitsPerSample >= 32 )
            {
                if (IsEqualGUID(&dataRange->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
                {
                    pin->formats |= paFloat32;
                    PA_DEBUG(("Format IEEE float 32 bit supported\n"));
                }
                else
                {
                    pin->formats |= paInt32;
                    PA_DEBUG(("Format PCM 32 bit supported\n"));
                }
            }
            if( ( pin->bestSampleRate != 48000) &&
                (((KSDATARANGE_AUDIO*)dataRange)->MaximumSampleFrequency >= 48000) &&
                (((KSDATARANGE_AUDIO*)dataRange)->MinimumSampleFrequency <= 48000) )
            {
                pin->bestSampleRate = 48000;
                PA_DEBUG(("48kHz supported\n"));
            }
            else if(( pin->bestSampleRate != 48000) && ( pin->bestSampleRate != 44100 ) &&
                (((KSDATARANGE_AUDIO*)dataRange)->MaximumSampleFrequency >= 44100) &&
                (((KSDATARANGE_AUDIO*)dataRange)->MinimumSampleFrequency <= 44100) )
            {
                pin->bestSampleRate = 44100;
                PA_DEBUG(("44.1kHz supported\n"));
            }
            else
            {
                pin->bestSampleRate = ((KSDATARANGE_AUDIO*)dataRange)->MaximumSampleFrequency;
            }
        }
        dataRange = (KSDATARANGE*)( ((char*)dataRange) + dataRange->FormatSize);
    }

    if( result != paNoError )
        goto error;

    /* Get instance information */
    result = WdmGetPinPropertySimple(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_CINSTANCES,
        &pin->instances,
        sizeof(KSPIN_CINSTANCES),
        NULL);

    if( result != paNoError )
        goto error;

    /* Query pin name (which means we need to traverse to non IRP pin, via physical connection to topology filter pin, through
    its nodes to the endpoint pin, and get that ones name... phew...) */
    {
        ULONG topoPinId = GetConnectedPin(pinId, (pin->dataFlow == KSPIN_DATAFLOW_IN), parentFilter, -1, NULL, NULL);

        if (topoPinId != KSFILTER_NODE)
        {
            /* Get physical connection for topo pin */
            unsigned long cbBytes = 0;
            result = WdmGetPinPropertySimple(parentFilter->handle,
                topoPinId,
                &KSPROPSETID_Pin,
                KSPROPERTY_PIN_PHYSICALCONNECTION,
                0,
                0,
                &cbBytes
                );

            if (result != paNoError)
            {
                /* No physical connection -> there is no topology filter! So we get the name of the pin! */
                wchar_t buffer[MAX_PATH] = {0};
                result = WdmGetPinPropertySimple(parentFilter->handle,
                    topoPinId,
                    &KSPROPSETID_Pin,
                    KSPROPERTY_PIN_NAME,
                    buffer,
                    MAX_PATH,
                    NULL);
                if (result == paNoError && wcslen(buffer) > 0)
                {
                    copywidestring(pin->friendlyName, buffer, MAX_PATH);
                }
                else
                {
                    GUID category = {0};

                    /* Get pin category information */
                    result = WdmGetPinPropertySimple(parentFilter->handle,
                        topoPinId,
                        &KSPROPSETID_Pin,
                        KSPROPERTY_PIN_CATEGORY,
                        &category,
                        sizeof(GUID),
                        NULL);

                    if (result == paNoError)
                    {
                        result = GetNameFromCategory(&category, (pin->dataFlow == KSPIN_DATAFLOW_OUT), pin->friendlyName, MAX_PATH);
                    }
                }

                /* This is then == the endpoint pin */
                pin->endpointPinId = topoPinId;
            }
            else
            {
                KSPIN_PHYSICALCONNECTION* pc = (KSPIN_PHYSICALCONNECTION*)PaUtil_AllocateMemory(cbBytes + 2);
                if (pc == NULL)
                {
                    result = paInsufficientMemory;
                    goto error;
                }
                result = WdmGetPinPropertySimple(parentFilter->handle,
                    topoPinId,
                    &KSPROPSETID_Pin,
                    KSPROPERTY_PIN_PHYSICALCONNECTION,
                    pc,
                    cbBytes,
                    NULL
                    );
                if (result == paNoError)
                {
                    TCHAR mbsSymbLinkName[MAX_PATH];
                    copywidestring(mbsSymbLinkName, pc->SymbolicLinkName, MAX_PATH);
                    if (pin->parentFilter->topologyFilter == NULL)
                    {
                        pin->parentFilter->topologyFilter = FilterNew(Type_kNotUsed, 0, mbsSymbLinkName, TEXT(""), &result);
                        if (pin->parentFilter->topologyFilter == NULL)
                        {
                            result = paUnanticipatedHostError;
                            PaWinWDM_SetLastErrorInfo(result, "Failed to create topology filter '%s'", mbsSymbLinkName);
                            goto error;
                        }
                    }
                    else
                    {
                        /* Must be the same */
                        assert(cmpstring(mbsSymbLinkName, pin->parentFilter->topologyFilter->filterName) == 0);
                    }

                    result = FilterUse(pin->parentFilter->topologyFilter);
                    if (result == paNoError)
                    {
                        unsigned long endpointPinId;

                        if (pin->dataFlow == KSPIN_DATAFLOW_IN)
                        {
                            /* The "endpointPinId" is what WASAPI looks at for pin names */
                            GUID category = {0};

                            endpointPinId = GetConnectedPin(pc->Pin, TRUE, pin->parentFilter->topologyFilter, -1, NULL, NULL);

                            if (endpointPinId == KSFILTER_NODE)
                            {
                                result = paUnanticipatedHostError;
                                PaWinWDM_SetLastErrorInfo(result, "Failed to get endpoint pin ID on topology filter!");
                                goto error;
                            }

                            /* Get pin category information */
                            result = WdmGetPinPropertySimple(pin->parentFilter->topologyFilter->handle,
                                endpointPinId,
                                &KSPROPSETID_Pin,
                                KSPROPERTY_PIN_CATEGORY,
                                &category,
                                sizeof(GUID),
                                NULL);

                            if (result == paNoError)
                            {
                                wchar_t pinName[MAX_PATH];

                                /* Ok, try pin name also, and favor that if available */
                                result = WdmGetPinPropertySimple(pin->parentFilter->topologyFilter->handle,
                                    endpointPinId,
                                    &KSPROPSETID_Pin,
                                    KSPROPERTY_PIN_NAME,
                                    pinName,
                                    MAX_PATH,
                                    NULL);

                                if (result == paNoError && wcslen(pinName)>0)
                                {
                                    copywidestring(pin->friendlyName, pinName, MAX_PATH);
                                }
                                else
                                {
                                    result = GetNameFromCategory(&category, (pin->dataFlow == KSPIN_DATAFLOW_OUT), pin->friendlyName, MAX_PATH);

                                    if (result != paNoError)
                                    {
                                        copywidestring(pin->friendlyName, L"Output", MAX_PATH);
                                    }
                                }
                            }

                            /* Set endpoint pin ID */
                            pin->endpointPinId = endpointPinId;
                        }
                        else
                        {
                            unsigned muxCount = 0;
                            int muxPos = 0;
                            /* Max 64 multiplexer inputs... sanity check :) */
                            for (i = 0; i < 64; ++i)
                            {
                                ULONG muxNodeIdTest = (unsigned)-1;
                                endpointPinId = GetConnectedPin(pc->Pin,
                                    FALSE,
                                    pin->parentFilter->topologyFilter,
                                    (int)i,
                                    NULL,
                                    &muxNodeIdTest);

                                if (endpointPinId != KSFILTER_NODE)
                                {
                                    /* The "endpointPinId" is what WASAPI looks at for pin names */
                                    GUID category = {0};

                                    /* Get pin category information */
                                    result = WdmGetPinPropertySimple(pin->parentFilter->topologyFilter->handle,
                                        endpointPinId,
                                        &KSPROPSETID_Pin,
                                        KSPROPERTY_PIN_CATEGORY,
                                        &category,
                                        sizeof(GUID),
                                        NULL);

                                    if (result == paNoError)
                                    {
                                        if (muxNodeIdTest == (unsigned)-1)
                                        {
                                            /* The case where there is no MUX in the path */
                                            wchar_t pinName[MAX_PATH];

                                            /* Ok, try pin name, and favor that if available */
                                            result = WdmGetPinPropertySimple(pin->parentFilter->topologyFilter->handle,
                                                endpointPinId,
                                                &KSPROPSETID_Pin,
                                                KSPROPERTY_PIN_NAME,
                                                pinName,
                                                MAX_PATH,
                                                NULL);

                                            if (result == paNoError && wcslen(pinName)>0)
                                            {
                                                copywidestring(pin->friendlyName, pinName, MAX_PATH);
                                            }
                                            else
                                            {
                                                result = GetNameFromCategory(&category, TRUE, pin->friendlyName, MAX_PATH);

                                                if( result != paNoError )
                                                {
                                                    copywidestring(pin->friendlyName, L"Input", MAX_PATH);
                                                }
                                            }
                                            break;
                                        }
                                        else
                                        {
                                            result = GetNameFromCategory(&category, TRUE, NULL, 0);

                                            if (result == paNoError)
                                            {
                                                ++muxCount;
                                            }
                                        }
                                    }
                                }
                                else
                                {
                                    /* We're done */
                                    break;
                                }
                            }

                            if (muxCount > 0)
                            {
                                /* Now we redo the operation once known how many multiplexer positions there are */
                                pin->inputs = (PaWinWdmMuxedInput**)PaUtil_AllocateMemory(muxCount * sizeof(PaWinWdmMuxedInput*));
                                if (pin->inputs == NULL)
                                {
                                    FilterRelease(pin->parentFilter->topologyFilter);
                                    result = paInsufficientMemory;
                                    goto error;
                                }
                                pin->inputCount = muxCount;

                                for (i = 0; i < muxCount; ++muxPos)
                                {
                                    if (pin->inputs[i] == NULL)
                                    {
                                        pin->inputs[i] = (PaWinWdmMuxedInput*)PaUtil_AllocateMemory(sizeof(PaWinWdmMuxedInput));
                                        if (pin->inputs[i] == NULL)
                                        {
                                            FilterRelease(pin->parentFilter->topologyFilter);
                                            result = paInsufficientMemory;
                                            goto error;
                                        }
                                    }

                                    endpointPinId = GetConnectedPin(pc->Pin,
                                        FALSE,
                                        pin->parentFilter->topologyFilter,
                                        muxPos,
                                        &pin->inputs[i]->muxPinId, 
                                        &pin->inputs[i]->muxNodeId);

                                    if (endpointPinId != KSFILTER_NODE)
                                    {
                                        /* The "endpointPinId" is what WASAPI looks at for pin names */
                                        GUID category = {0};

                                        /* Set input endpoint ID */
                                        pin->inputs[i]->endpointPinId = endpointPinId;

                                        /* Get pin category information */
                                        result = WdmGetPinPropertySimple(pin->parentFilter->topologyFilter->handle,
                                            endpointPinId,
                                            &KSPROPSETID_Pin,
                                            KSPROPERTY_PIN_CATEGORY,
                                            &category,
                                            sizeof(GUID),
                                            NULL);

                                        if (result == paNoError)
                                        {
                                            result = GetNameFromCategory(&category, TRUE, pin->inputs[i]->friendlyName, MAX_PATH);
                                            if (result == paNoError)
                                            {
                                                wchar_t pinName[MAX_PATH];

                                                /* Try pin name also, and if that is defined, use that instead */
                                                result = WdmGetPinPropertySimple(pin->parentFilter->topologyFilter->handle,
                                                    endpointPinId,
                                                    &KSPROPSETID_Pin,
                                                    KSPROPERTY_PIN_NAME,
                                                    pinName,
                                                    MAX_PATH,
                                                    NULL);

                                                if (result == paNoError && wcslen(pinName) > 0)
                                                {
                                                    copywidestring(pin->inputs[i]->friendlyName, pinName, MAX_PATH);
                                                }

                                                ++i;
                                            }
                                        }
                                    }
                                    else
                                    {
                                        /* Should never come here! */
                                        assert(FALSE);
                                    }
                                }
                            }
                        }

                        FilterRelease(pin->parentFilter->topologyFilter);
                    }
                }
                PaUtil_FreeMemory(pc);
            }
        }
    }

    /* Success */
    *error = paNoError;
    PA_DEBUG(("Pin created successfully\n"));
    PA_LOGL_;
    return pin;

error:
    /*
    Error cleanup
    */

    PaUtil_FreeMemory( item );
    if( pin )
    {
        if (pin->parentFilter->topologyFilter && pin->parentFilter->topologyFilter->handle != NULL)
        {
            FilterRelease(pin->parentFilter->topologyFilter);
        }

        PaUtil_FreeMemory( pin->pinConnect );
        PaUtil_FreeMemory( pin->dataRangesItem );
        PaUtil_FreeMemory( pin );
    }
    *error = result;
    PA_LOGL_;
    return NULL;
}

/*
Safely free all resources associated with the pin
*/
static void PinFree(PaWinWdmPin* pin)
{
    unsigned i;
    PA_LOGE_;
    if( pin )
    {
        PinClose(pin);
        if( pin->pinConnect )
        {
            PaUtil_FreeMemory( pin->pinConnect );
        }
        if( pin->dataRangesItem )
        {
            PaUtil_FreeMemory( pin->dataRangesItem );
        }
        if( pin->inputs )
        {
            for (i = 0; i < pin->inputCount; ++i)
            {
                PaUtil_FreeMemory( pin->inputs[i] );
            }
            PaUtil_FreeMemory( pin->inputs );
        }
        PaUtil_FreeMemory( pin );
    }
    PA_LOGL_;
}

/*
If the pin handle is open, close it
*/
static void PinClose(PaWinWdmPin* pin)
{
    PA_LOGE_;
    if( pin == NULL )
    {
        PA_DEBUG(("Closing NULL pin!"));
        PA_LOGL_;
        return;
    }
    if( pin->handle != NULL )
    {
        PinSetState( pin, KSSTATE_PAUSE );
        PinSetState( pin, KSSTATE_STOP );
        CloseHandle( pin->handle );
        pin->handle = NULL;
        FilterRelease(pin->parentFilter);
    }
    PA_LOGL_;
}

/*
Set the state of this (instantiated) pin
*/
static PaError PinSetState(PaWinWdmPin* pin, KSSTATE state)
{
    PaError result = paNoError;
    KSPROPERTY prop;

    PA_LOGE_;
    prop.Set = KSPROPSETID_Connection;
    prop.Id  = KSPROPERTY_CONNECTION_STATE;
    prop.Flags = KSPROPERTY_TYPE_SET;

    if( pin == NULL )
        return paInternalError;
    if( pin->handle == NULL )
        return paInternalError;

    result = WdmSyncIoctl(pin->handle, IOCTL_KS_PROPERTY, &prop, sizeof(KSPROPERTY), &state, sizeof(KSSTATE), NULL);

    PA_LOGL_;
    return result;
}

static PaError PinInstantiate(PaWinWdmPin* pin)
{
    PaError result;
    unsigned long createResult;
    KSALLOCATOR_FRAMING ksaf;
    KSALLOCATOR_FRAMING_EX ksafex;

    PA_LOGE_;

    if( pin == NULL )
        return paInternalError;
    if(!pin->pinConnect)
        return paInternalError;

    FilterUse(pin->parentFilter);

    createResult = FunctionKsCreatePin(
        pin->parentFilter->handle,
        pin->pinConnect,
        GENERIC_WRITE | GENERIC_READ,
        &pin->handle
        );

    PA_DEBUG(("Pin create result = %x\n",createResult));
    if( createResult != ERROR_SUCCESS )
    {
        FilterRelease(pin->parentFilter);
        pin->handle = NULL;
        switch (createResult)
        {
        case ERROR_INVALID_PARAMETER:
            /* First case when pin actually don't support the format */
            return paSampleFormatNotSupported;
        case ERROR_BAD_COMMAND:
            /* Case when pin is occupied (by another application) */
            return paDeviceUnavailable;
        default:
            /* All other cases */
            return paInvalidDevice;
        }
    }

    if (pin->parentFilter->waveType == Type_kWaveCyclic)
    {
        /* Framing size query only valid for WaveCyclic devices */
        result = WdmGetPropertySimple(
            pin->handle,
            &KSPROPSETID_Connection,
            KSPROPERTY_CONNECTION_ALLOCATORFRAMING,
            &ksaf,
            sizeof(ksaf),
            NULL,
            0);

        if( result != paNoError )
        {
            result = WdmGetPropertySimple(
                pin->handle,
                &KSPROPSETID_Connection,
                KSPROPERTY_CONNECTION_ALLOCATORFRAMING_EX,
                &ksafex,
                sizeof(ksafex),
                NULL,
                0);
            if( result == paNoError )
            {
                pin->frameSize = ksafex.FramingItem[0].FramingRange.Range.MinFrameSize;
            }
        }
        else
        {
            pin->frameSize = ksaf.FrameSize;
        }
    }

    PA_LOGL_;

    return paNoError;
}

static PaError PinSetFormat(PaWinWdmPin* pin, const WAVEFORMATEX* format)
{
    unsigned long size;
    void* newConnect;

    PA_LOGE_;

    if( pin == NULL )
        return paInternalError;
    if( format == NULL )
        return paInternalError;

    size = GetWfexSize(format) + sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEX) - sizeof(WAVEFORMATEX);

    if( pin->pinConnectSize != size )
    {
        newConnect = PaUtil_AllocateMemory( size );
        if( newConnect == NULL )
            return paInsufficientMemory;
        memcpy( newConnect, (void*)pin->pinConnect, min(pin->pinConnectSize,size) );
        PaUtil_FreeMemory( pin->pinConnect );
        pin->pinConnect = (KSPIN_CONNECT*)newConnect;
        pin->pinConnectSize = size;
        pin->ksDataFormatWfx = (KSDATAFORMAT_WAVEFORMATEX*)((KSPIN_CONNECT*)newConnect + 1);
        pin->ksDataFormatWfx->DataFormat.FormatSize = size - sizeof(KSPIN_CONNECT);
    }

    memcpy( (void*)&(pin->ksDataFormatWfx->WaveFormatEx), format, GetWfexSize(format) );
    pin->ksDataFormatWfx->DataFormat.SampleSize = (unsigned short)(format->nChannels * (format->wBitsPerSample / 8));

    PA_LOGL_;

    return paNoError;
}

static PaError PinIsFormatSupported(PaWinWdmPin* pin, const WAVEFORMATEX* format)
{
    KSDATARANGE_AUDIO* dataRange;
    unsigned long count;
    GUID guid = DYNAMIC_GUID( DEFINE_WAVEFORMATEX_GUID(format->wFormatTag) );
    PaError result = paInvalidDevice;

    PA_LOGE_;

    if( format->wFormatTag == WAVE_FORMAT_EXTENSIBLE )
    {
        guid = ((WAVEFORMATEXTENSIBLE*)format)->SubFormat;
    }
    dataRange = (KSDATARANGE_AUDIO*)pin->dataRanges;
    for(count = 0; count<pin->dataRangesItem->Count; count++)
    {
        if ( IsEqualGUID(&(dataRange->DataRange.MajorFormat), &KSDATAFORMAT_TYPE_AUDIO) || 
            IsEqualGUID(&(dataRange->DataRange.MajorFormat), &KSDATAFORMAT_TYPE_WILDCARD) )
        {
            /* This is an audio or wildcard datarange... */
            if ( IsEqualGUID(&(dataRange->DataRange.SubFormat), &KSDATAFORMAT_SUBTYPE_WILDCARD) ||
                IsEqualGUID(&(dataRange->DataRange.SubFormat), &KSDATAFORMAT_SUBTYPE_PCM) ||
                IsEqualGUID(&(dataRange->DataRange.SubFormat), &guid) )
            {
                if ( IsEqualGUID(&(dataRange->DataRange.Specifier),&KSDATAFORMAT_SPECIFIER_WILDCARD) ||
                    IsEqualGUID(&(dataRange->DataRange.Specifier),&KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) )
                {

                    PA_DEBUG(("Pin:%x, DataRange:%d\n",(void*)pin,count));
                    PA_DEBUG(("\tFormatSize:%d, SampleSize:%d\n",dataRange->DataRange.FormatSize,dataRange->DataRange.SampleSize));
                    PA_DEBUG(("\tMaxChannels:%d\n",dataRange->MaximumChannels));
                    PA_DEBUG(("\tBits:%d-%d\n",dataRange->MinimumBitsPerSample,dataRange->MaximumBitsPerSample));
                    PA_DEBUG(("\tSampleRate:%d-%d\n",dataRange->MinimumSampleFrequency,dataRange->MaximumSampleFrequency));

                    if( dataRange->MaximumChannels != (ULONG)-1 && 
                        dataRange->MaximumChannels < format->nChannels )
                    {
                        result = paInvalidChannelCount;
                        continue;
                    }
                    if( dataRange->MinimumBitsPerSample > format->wBitsPerSample )
                    {
                        result = paSampleFormatNotSupported;
                        continue;
                    }
                    if( dataRange->MaximumBitsPerSample < format->wBitsPerSample )
                    {
                        result = paSampleFormatNotSupported;
                        continue;
                    }
                    if( dataRange->MinimumSampleFrequency > format->nSamplesPerSec )
                    {
                        result = paInvalidSampleRate;
                        continue;
                    }
                    if( dataRange->MaximumSampleFrequency < format->nSamplesPerSec )
                    {
                        result = paInvalidSampleRate;
                        continue;
                    }
                    /* Success! */
                    PA_LOGL_;
                    return paNoError;
                }
            }
        }
        dataRange = (KSDATARANGE_AUDIO*)( ((char*)dataRange) + dataRange->DataRange.FormatSize);
    }

    PA_LOGL_;

    return result;
}

static PaError PinGetBufferWithNotification(PaWinWdmPin* pPin, void** pBuffer, DWORD* pRequestedBufSize, BOOL* pbCallMemBarrier)
{
    PaError result = paNoError;
    KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION propIn;
    KSRTAUDIO_BUFFER propOut;

    propIn.BaseAddress = NULL;
    propIn.NotificationCount = 2;
    propIn.RequestedBufferSize = *pRequestedBufSize;
    propIn.Property.Set = KSPROPSETID_RtAudio;
    propIn.Property.Id = KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION;
    propIn.Property.Flags = KSPROPERTY_TYPE_GET;

    result = WdmSyncIoctl(pPin->handle, IOCTL_KS_PROPERTY,
        &propIn,
        sizeof(KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION),
        &propOut,
        sizeof(KSRTAUDIO_BUFFER),
        NULL);

    if (result == paNoError) 
    {
        *pBuffer = propOut.BufferAddress;
        *pRequestedBufSize = propOut.ActualBufferSize;
        *pbCallMemBarrier = propOut.CallMemoryBarrier;
    }
    else 
    {
        PA_DEBUG(("Failed to get buffer with notification\n"));
    }

    return result;
}

static PaError PinGetBufferWithoutNotification(PaWinWdmPin* pPin, void** pBuffer, DWORD* pRequestedBufSize, BOOL* pbCallMemBarrier)
{
    PaError result = paNoError;
    KSRTAUDIO_BUFFER_PROPERTY propIn;
    KSRTAUDIO_BUFFER propOut;

    propIn.BaseAddress = NULL;
    propIn.RequestedBufferSize = *pRequestedBufSize;
    propIn.Property.Set = KSPROPSETID_RtAudio;
    propIn.Property.Id = KSPROPERTY_RTAUDIO_BUFFER;
    propIn.Property.Flags = KSPROPERTY_TYPE_GET;

    result = WdmSyncIoctl(pPin->handle, IOCTL_KS_PROPERTY,
        &propIn,
        sizeof(KSRTAUDIO_BUFFER_PROPERTY),
        &propOut,
        sizeof(KSRTAUDIO_BUFFER),
        NULL);

    if (result == paNoError)
    {
        *pBuffer = propOut.BufferAddress;
        *pRequestedBufSize = propOut.ActualBufferSize;
        *pbCallMemBarrier = propOut.CallMemoryBarrier;
    }
    else 
    {
        PA_DEBUG(("Failed to get buffer without notification\n"));
    }

    return result;
}


static PaError PinRegisterPositionRegister(PaWinWdmPin* pPin) 
{
    PaError result = paNoError;
    KSRTAUDIO_HWREGISTER_PROPERTY propIn;
    KSRTAUDIO_HWREGISTER propOut;

    PA_LOGE_;

    propIn.BaseAddress = NULL;
    propIn.Property.Set = KSPROPSETID_RtAudio;
    propIn.Property.Id = KSPROPERTY_RTAUDIO_POSITIONREGISTER;
    propIn.Property.Flags = KSPROPERTY_TYPE_GET;

    result = WdmSyncIoctl(pPin->handle, IOCTL_KS_PROPERTY,
        &propIn,
        sizeof(KSRTAUDIO_HWREGISTER_PROPERTY),
        &propOut,
        sizeof(KSRTAUDIO_HWREGISTER),
        NULL);

    if (result == paNoError) 
    {
        pPin->positionRegister = (ULONG*)propOut.Register;
    }
    else
    {
        PA_DEBUG(("Failed to register position register\n"));
    }

    PA_LOGL_;

    return result;
}

static PaError PinRegisterNotificationHandle(PaWinWdmPin* pPin, HANDLE handle) 
{
    PaError result = paNoError;
    KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY prop;

    PA_LOGE_;

    prop.NotificationEvent = handle;
    prop.Property.Set = KSPROPSETID_RtAudio;
    prop.Property.Id = KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT;
    prop.Property.Flags = KSPROPERTY_TYPE_GET;

    result = WdmSyncIoctl(pPin->handle,
        IOCTL_KS_PROPERTY,
        &prop,
        sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY),
        &prop,
        sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY),
        NULL);

    if (result != paNoError) {
        PA_DEBUG(("Failed to register notification handle 0x%08X\n", handle));
    }

    PA_LOGL_;

    return result;
}

static PaError PinUnregisterNotificationHandle(PaWinWdmPin* pPin, HANDLE handle) 
{
    PaError result = paNoError;
    KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY prop;

    PA_LOGE_;

    if (handle != NULL)
    {
        prop.NotificationEvent = handle;
        prop.Property.Set = KSPROPSETID_RtAudio;
        prop.Property.Id = KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT;
        prop.Property.Flags = KSPROPERTY_TYPE_GET;

        result = WdmSyncIoctl(pPin->handle,
            IOCTL_KS_PROPERTY,
            &prop,
            sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY),
            &prop,
            sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY),
            NULL);

        if (result != paNoError) {
            PA_DEBUG(("Failed to unregister notification handle 0x%08X\n", handle));
        }
    }
    PA_LOGL_;

    return result;
}

static PaError PinGetHwLatency(PaWinWdmPin* pPin, ULONG* pFifoSize, ULONG* pChipsetDelay, ULONG* pCodecDelay)
{
    PaError result = paNoError;
    KSPROPERTY propIn;
    KSRTAUDIO_HWLATENCY propOut;

    PA_LOGE_;

    propIn.Set = KSPROPSETID_RtAudio;
    propIn.Id = KSPROPERTY_RTAUDIO_HWLATENCY;
    propIn.Flags = KSPROPERTY_TYPE_GET;

    result = WdmSyncIoctl(pPin->handle, IOCTL_KS_PROPERTY,
        &propIn,
        sizeof(KSPROPERTY),
        &propOut,
        sizeof(KSRTAUDIO_HWLATENCY),
        NULL);

    if (result == paNoError)
    {
        *pFifoSize = propOut.FifoSize;
        *pChipsetDelay = propOut.ChipsetDelay;
        *pCodecDelay = propOut.CodecDelay;
    }
    else
    {
        PA_DEBUG(("Failed to retrieve hardware FIFO size!\n"));
    }

    PA_LOGL_;

    return result;
}

/* This one is used for WaveRT */
static PaError PinGetAudioPositionDirect(PaWinWdmPin* pPin, ULONG* pPosition)
{
    *pPosition = (*pPin->positionRegister);
    return paNoError;
}

/* This one also, but in case the driver hasn't implemented memory mapped access to the position register */
static PaError PinGetAudioPositionViaIOCTL(PaWinWdmPin* pPin, ULONG* pPosition)
{
    PaError result = paNoError;
    KSPROPERTY propIn;
    KSAUDIO_POSITION propOut;

    PA_LOGE_;

    propIn.Set = KSPROPSETID_Audio;
    propIn.Id = KSPROPERTY_AUDIO_POSITION;
    propIn.Flags = KSPROPERTY_TYPE_GET;

    result = WdmSyncIoctl(pPin->handle,
        IOCTL_KS_PROPERTY,
        &propIn, sizeof(KSPROPERTY),
        &propOut, sizeof(KSAUDIO_POSITION),
        NULL);

    if (result == paNoError)
    {
        *pPosition = (ULONG)(propOut.PlayOffset);
    }
    else
    {
        PA_DEBUG(("Failed to get audio position!\n"));
    }

    PA_LOGL_;

    return result;

}

/***********************************************************************************************/

/**
* Create a new filter object. If initPins is TRUE, the pins will be instantiated aswell. Why would you not want this, you
* may ask ? Well, in a hot plug/unplug situation, you might want to init a filter object without pins, since the latter will
* fail if the device is in use, but you don't want that as an indication that the filter creation failed.
*/
static PaWinWdmFilter* FilterNew( PaWDMKSType type, DWORD devNode, TCHAR* filterName, TCHAR* friendlyName, PaError* error )
{
    PaWinWdmFilter* filter = 0;
    PaError result;

    /* Allocate the new filter object */
    filter = (PaWinWdmFilter*)PaUtil_AllocateMemory( sizeof(PaWinWdmFilter) );
    if( !filter )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* Set type flag */
    filter->waveType = type;

    /* Store device node */
    filter->deviceNode = devNode;

    /* Zero the filter object - done by AllocateMemory */
    /* memset( (void*)filter, 0, sizeof(PaWinWdmFilter) ); */

    /* Copy the filter name */
    _tcsncpy(filter->filterName, filterName, MAX_PATH);

    /* Copy the friendly name */
    _tcsncpy(filter->friendlyName, friendlyName, MAX_PATH);

    /* Open the filter handle */
    result = FilterUse(filter);
    if( result != paNoError )
    {
        goto error;
    }

    /* Get pin count */
    result = WdmGetPinPropertySimple
        (
        filter->handle,
        0,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_CTYPES,
        &filter->pinCount,
        sizeof(filter->pinCount),
        NULL);

    if( result != paNoError)
    {
        goto error;
    }

    /* Get connections & nodes for filter */
    result = WdmGetPropertyMulti(
        filter->handle,
        &KSPROPSETID_Topology,
        KSPROPERTY_TOPOLOGY_CONNECTIONS,
        &filter->connections);

    if( result != paNoError)
    {
        goto error;
    }

    result = WdmGetPropertyMulti(
        filter->handle,
        &KSPROPSETID_Topology,
        KSPROPERTY_TOPOLOGY_NODES,
        &filter->nodes);

    if( result != paNoError)
    {
        goto error;
    }

    /* This section is not executed for topology filters */
    if (type != Type_kNotUsed)
    {
        /* Initialize the pins */
        result = FilterInitializePins(filter);

        if( result != paNoError)
        {
            goto error;
        }
    }

    /* Close the filter handle for now
    * It will be opened later when needed */
    FilterRelease(filter);

    *error = paNoError;
    return filter;

error:
    /*
    Error cleanup
    */
    FilterFree(filter);

    *error = result;
    return NULL;
}

/**
* Initialize the pins of the filter. This is separated from FilterNew because this might fail if there is another
* process using the pin(s).
*/
PaError FilterInitializePins( PaWinWdmFilter* filter )
{
    PaError result = paNoError;
    int pinId;

    if (filter->waveType == Type_kNotUsed)
        return paNoError;

    if (filter->pins != NULL)
        return paNoError;   

    /* Allocate pointer array to hold the pins */
    filter->pins = (PaWinWdmPin**)PaUtil_AllocateMemory( sizeof(PaWinWdmPin*) * filter->pinCount );
    if( !filter->pins )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* Create all the pins we can */
    for(pinId = 0; pinId < filter->pinCount; pinId++)
    {
        /* Create the pin with this Id */
        PaWinWdmPin* newPin;
        newPin = PinNew(filter, pinId, &result);
        if( result == paInsufficientMemory )
            goto error;
        if( newPin != NULL )
        {
            filter->pins[pinId] = newPin;
            ++filter->validPinCount;
        }
    }

    if (filter->validPinCount == 0)
    {
        result = paDeviceUnavailable;
        goto error;
    }

    return paNoError;

error:

    if (filter->pins)
    {
        for (pinId = 0; pinId < filter->pinCount; ++pinId)
        {
            if (filter->pins[pinId])
            {
                PaUtil_FreeMemory(filter->pins[pinId]);
                filter->pins[pinId] = 0;
            }
        }
        PaUtil_FreeMemory( filter->pins );
        filter->pins = 0;
    }

    return result;
}


/**
* Free a previously created filter
*/
static void FilterFree(PaWinWdmFilter* filter)
{
    int pinId;
    PA_LOGL_;
    if( filter )
    {
        if (filter->topologyFilter)
        {
            FilterFree(filter->topologyFilter);
            filter->topologyFilter = 0;
        }
        if ( filter->pins )
        {
            for( pinId = 0; pinId < filter->pinCount; pinId++ )
                PinFree(filter->pins[pinId]);
            PaUtil_FreeMemory( filter->pins );
            filter->pins = 0;
        }
        if( filter->connections )
        {
            PaUtil_FreeMemory(filter->connections);
            filter->connections = 0;
        }
        if( filter->nodes )
        {
            PaUtil_FreeMemory(filter->nodes);
            filter->nodes = 0;
        }
        if( filter->handle )
            CloseHandle( filter->handle );
        PaUtil_FreeMemory( filter );
    }
    PA_LOGE_;
}

/**
* Reopen the filter handle if necessary so it can be used
**/
static PaError FilterUse(PaWinWdmFilter* filter)
{
    assert( filter );

    PA_LOGE_;
    if( filter->handle == NULL )
    {
        /* Open the filter */
        filter->handle = CreateFile(
            filter->filterName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            NULL);

        if( filter->handle == NULL )
        {
            return paDeviceUnavailable;
        }
    }
    filter->usageCount++;
    PA_LOGL_;
    return paNoError;
}

/**
* Release the filter handle if nobody is using it
**/
static void FilterRelease(PaWinWdmFilter* filter)
{
    assert( filter );
    assert( filter->usageCount > 0 );

    PA_LOGE_;
    /* Check first topology filter, if used */
    if (filter->topologyFilter != NULL && filter->topologyFilter->handle != NULL)
    {
        FilterRelease(filter->topologyFilter);
    }

    filter->usageCount--;
    if( filter->usageCount == 0 )
    {
        if( filter->handle != NULL )
        {
            CloseHandle( filter->handle );
            filter->handle = NULL;
        }
    }
    PA_LOGL_;
}

/**
* Create a render (playback) Pin using the supplied format
**/
static PaWinWdmPin* FilterCreateRenderPin(PaWinWdmFilter* filter,
                                          int pinId,
                                          const WAVEFORMATEX* wfex,
                                          PaError* error)
{
    PaError result = paNoError;
    PaWinWdmPin* pin = NULL;
    assert( filter );
    assert( pinId < filter->pinCount );
    pin = filter->pins[pinId];
    assert( pin );
    result = PinSetFormat(pin,wfex);
    if( result == paNoError )
    {
        result = PinInstantiate(pin);
    }
    *error = result;
    return pin;
}


/**
* Create a capture (record) Pin using the supplied format
**/
static PaWinWdmPin* FilterCreateCapturePin(PaWinWdmFilter* filter,
                                           int pinId,
                                           const WAVEFORMATEX* wfex,
                                           PaError* error)
{
    PaError result = paNoError;
    PaWinWdmPin* pin = NULL;
    assert( filter );
    assert( pinId < filter->pinCount );
    pin = filter->pins[pinId];
    assert( pin );
    result = PinSetFormat(pin,wfex);
    if( result == paNoError )
    {
        result = PinInstantiate(pin);
    }
    *error = result;
    return pin;
}

static ULONG GetHash(const TCHAR* str)
{
    const ULONG fnv_prime = 0x811C9DC5;
    ULONG hash      = 0;
    for(; *str != 0; str++)
    {
        hash *= fnv_prime;
        hash ^= (*str);
    }
    assert(hash != 0);
    return hash;
}


static BOOL IsUSBDevice(const TCHAR* devicePath)
{
    return (cmpstring(devicePath, TEXT("\\\\?\\USB")) == 0);
}

static BOOL IsNameUSBAudioDevice(const TCHAR* friendlyName)
{
    return (cmpstring(friendlyName, TEXT("USB Audio Device")) == 0);
}

typedef enum _tag_EAlias
{
    Alias_kRender   = (1<<0),
    Alias_kCapture  = (1<<1),
    Alias_kRealtime = (1<<2),
} EAlias;

/**
* Build the list of available filters
* Use the SetupDi API to enumerate all devices in the KSCATEGORY_AUDIO which 
* have a KSCATEGORY_RENDER or KSCATEGORY_CAPTURE alias. For each of these 
* devices initialise a PaWinWdmFilter structure by calling our NewFilter() 
* function. We enumerate devices twice, once to count how many there are, 
* and once to initialize the PaWinWdmFilter structures.
*
* Vista and later: Also check KSCATEGORY_REALTIME for WaveRT devices.
*/
//PaError BuildFilterList( PaWinWdmHostApiRepresentation* wdmHostApi, int* noOfPaDevices )
PaWinWdmFilter** BuildFilterList( int* pFilterCount, int* pNoOfPaDevices, PaError* pResult )
{
    PaWinWdmFilter** ppFilters = NULL;
    HDEVINFO handle = NULL;
    int device;
    int invalidDevices;
    int slot;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    SP_DEVICE_INTERFACE_DATA aliasData;
    SP_DEVINFO_DATA devInfoData;
    int noError;
    const int sizeInterface = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA) + (MAX_PATH * sizeof(WCHAR));
    unsigned char interfaceDetailsArray[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA) + (MAX_PATH * sizeof(WCHAR))];
    SP_DEVICE_INTERFACE_DETAIL_DATA* devInterfaceDetails = (SP_DEVICE_INTERFACE_DETAIL_DATA*)interfaceDetailsArray;
    TCHAR friendlyName[MAX_PATH];
    HKEY hkey;
    DWORD sizeFriendlyName;
    DWORD type;
    PaWinWdmFilter* newFilter;
    GUID* category = (GUID*)&KSCATEGORY_AUDIO;
    GUID* alias_render = (GUID*)&KSCATEGORY_RENDER;
    GUID* alias_capture = (GUID*)&KSCATEGORY_CAPTURE;
    GUID* category_realtime = (GUID*)&KSCATEGORY_REALTIME;
    DWORD aliasFlags;
    PaWDMKSType streamingType;
    int filterCount = 0;
    int noOfPaDevices = 0;

    PA_LOGE_;

    assert(pFilterCount != NULL);
    assert(pNoOfPaDevices != NULL);
    assert(pResult != NULL);

    devInterfaceDetails->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    *pFilterCount = 0;
    *pNoOfPaDevices = 0;

    /* Open a handle to search for devices (filters) */
    handle = SetupDiGetClassDevs(category,NULL,NULL,DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if( handle == INVALID_HANDLE_VALUE )
    {
        *pResult = paUnanticipatedHostError;
        return NULL;
    }
    PA_DEBUG(("Setup called\n"));

    /* First let's count the number of devices so we can allocate a list */
    invalidDevices = 0;
    for( device = 0;;device++ )
    {
        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        interfaceData.Reserved = 0;
        aliasData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        aliasData.Reserved = 0;
        noError = SetupDiEnumDeviceInterfaces(handle,NULL,category,device,&interfaceData);
        PA_DEBUG(("Enum called\n"));
        if( !noError )
            break; /* No more devices */

        /* Check this one has the render or capture alias */
        aliasFlags = 0;
        noError = SetupDiGetDeviceInterfaceAlias(handle,&interfaceData,alias_render,&aliasData);
        PA_DEBUG(("noError = %d\n",noError));
        if(noError)
        {
            if(aliasData.Flags && (!(aliasData.Flags & SPINT_REMOVED)))
            {
                PA_DEBUG(("Device %d has render alias\n",device));
                aliasFlags |= Alias_kRender; /* Has render alias */
            }
            else
            {
                PA_DEBUG(("Device %d has no render alias\n",device));
            }
        }
        noError = SetupDiGetDeviceInterfaceAlias(handle,&interfaceData,alias_capture,&aliasData);
        if(noError)
        {
            if(aliasData.Flags && (!(aliasData.Flags & SPINT_REMOVED)))
            {
                PA_DEBUG(("Device %d has capture alias\n",device));
                aliasFlags |= Alias_kCapture; /* Has capture alias */
            }
            else
            {
                PA_DEBUG(("Device %d has no capture alias\n",device));
            }
        }
        if(!aliasFlags)
            invalidDevices++; /* This was not a valid capture or render audio device */
    }
    /* Remember how many there are */
    filterCount = device-invalidDevices;

    PA_DEBUG(("Interfaces found: %d\n",device-invalidDevices));

    /* Now allocate the list of pointers to devices */
    ppFilters  = (PaWinWdmFilter**)PaUtil_AllocateMemory( sizeof(PaWinWdmFilter*) * filterCount);
    if( ppFilters == 0 )
    {
        if(handle != NULL)
            SetupDiDestroyDeviceInfoList(handle);
        *pResult = paInsufficientMemory;
        return NULL;
    }

    /* Now create filter objects for each interface found */
    slot = 0;
    for( device = 0;;device++ )
    {
        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        interfaceData.Reserved = 0;
        aliasData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        aliasData.Reserved = 0;
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
        devInfoData.Reserved = 0;
        streamingType = Type_kWaveCyclic;

        noError = SetupDiEnumDeviceInterfaces(handle,NULL,category,device,&interfaceData);
        if( !noError )
            break; /* No more devices */

        /* Check this one has the render or capture alias */
        aliasFlags = 0;
        noError = SetupDiGetDeviceInterfaceAlias(handle,&interfaceData,alias_render,&aliasData);
        if(noError)
        {
            if(aliasData.Flags && (!(aliasData.Flags & SPINT_REMOVED)))
            {
                PA_DEBUG(("Device %d has render alias\n",device));
                aliasFlags |= Alias_kRender; /* Has render alias */
            }
        }
        noError = SetupDiGetDeviceInterfaceAlias(handle,&interfaceData,alias_capture,&aliasData);
        if(noError)
        {
            if(aliasData.Flags && (!(aliasData.Flags & SPINT_REMOVED)))
            {
                PA_DEBUG(("Device %d has capture alias\n",device));
                aliasFlags |= Alias_kCapture; /* Has capture alias */
            }
        }
        if(!aliasFlags)
        {
            continue; /* This was not a valid capture or render audio device */
        }
        else
        {
            /* Check if filter is WaveRT, if not it is a WaveCyclic */
            noError = SetupDiGetDeviceInterfaceAlias(handle,&interfaceData,category_realtime,&aliasData);
            if (noError)
            {
                PA_DEBUG(("Device %d has realtime alias\n",device));
                aliasFlags |= Alias_kRealtime;
                streamingType = Type_kWaveRT;
            }
        }

        noError = SetupDiGetDeviceInterfaceDetail(handle,&interfaceData,devInterfaceDetails,sizeInterface,NULL,&devInfoData);
        if( noError )
        {
            PaError result = paNoError;
            /* Try to get the "friendly name" for this interface */
            sizeFriendlyName = sizeof(friendlyName);
            friendlyName[0] = 0;

            if (IsEarlierThanVista() && IsUSBDevice(devInterfaceDetails->DevicePath))
            {
                /* XP and USB audio device needs to look elsewhere, otherwise it'll only be a "USB Audio Device". Not
                very literate. */
                if (!SetupDiGetDeviceRegistryProperty(handle,
                    &devInfoData,
                    SPDRP_LOCATION_INFORMATION, 
                    &type,
                    (BYTE*)friendlyName,
                    sizeof(friendlyName),
                    NULL))
                {
                    friendlyName[0] = 0;
                }
            }

            /* Try to get DriverDesc, to get same names as WASAPI exposes */
            if (friendlyName[0] == 0 || IsNameUSBAudioDevice(friendlyName))
            {
                if (!SetupDiGetDeviceRegistryProperty(handle,
                    &devInfoData,
                    SPDRP_DEVICEDESC,
                    &type,
                    (BYTE*)friendlyName,
                    sizeof(friendlyName),
                    NULL))
                {
                    friendlyName[0] = 0;
                }
            }

            if (friendlyName[0] == 0 || IsNameUSBAudioDevice(friendlyName))
            {
                /* Fix contributed by Ben Allison
                * Removed KEY_SET_VALUE from flags on following call
                * as its causes failure when running without admin rights
                * and it was not required */
                hkey=SetupDiOpenDeviceInterfaceRegKey(handle,&interfaceData,0,KEY_QUERY_VALUE);
                if(hkey!=INVALID_HANDLE_VALUE)
                {
                    noError = RegQueryValueEx(hkey,TEXT("FriendlyName"),0,&type,(BYTE*)friendlyName,&sizeFriendlyName);
                    if( noError == ERROR_SUCCESS )
                    {
                        PA_DEBUG(("Interface %d, Name: %s\n",device,friendlyName));
                        RegCloseKey(hkey);
                    }
                    else
                    {
                        friendlyName[0] = 0;
                    }
                }
            }

            newFilter = FilterNew(streamingType, 
                devInfoData.DevInst,
                devInterfaceDetails->DevicePath,
                friendlyName,
                &result);

            if( result == paNoError )
            {
                int pin;
                unsigned filterIOs = 0;

                /* Increment number of "devices" */
                for (pin = 0; pin < newFilter->pinCount; ++pin)
                {
                    PaWinWdmPin* pPin = newFilter->pins[pin];
                    if (pPin == NULL)
                        continue;

                    filterIOs += max(1, pPin->inputCount);
                }

                noOfPaDevices += filterIOs;

                PA_DEBUG(("Filter (%s) created with %d valid pins (total I/Os: %u)\n", ((newFilter->waveType==Type_kWaveRT)?"WaveRT":"WaveCyclic"), newFilter->validPinCount, filterIOs));

                assert(slot < filterCount);

                ppFilters[slot] = newFilter;

                slot++;
            }
            else
            {
                PA_DEBUG(("Filter NOT created\n"));
                /* As there are now less filters than we initially thought
                * we must reduce the count by one */
                filterCount--;
            }
        }
    }

    /* Clean up */
    if(handle != NULL)
        SetupDiDestroyDeviceInfoList(handle);

    *pFilterCount = filterCount;
    *pNoOfPaDevices = noOfPaDevices;

    return ppFilters;
}

PaError PaWinWdm_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int idxFilter = 0;
    int idxDevice = 0;
    int totalDeviceCount = 0;
    PaWinWdmHostApiRepresentation *wdmHostApi;
    PaWinWdmDeviceInfo *deviceInfoArray;

    PA_LOGE_;

#ifdef PA_WDMKS_SET_TREF
    tRef = PaUtil_GetTime();
#endif

    /*
    Attempt to load the KSUSER.DLL without which we cannot create pins
    We will unload this on termination
    */
    if(DllKsUser == NULL)
    {
        DllKsUser = LoadLibrary(TEXT("ksuser.dll"));
        if(DllKsUser == NULL)
            goto error;
    }

    /* Attempt to load AVRT.DLL, if we can't, then we'll just use time critical prio instead... */
    if(DllAvRt == NULL)
    {
        DllAvRt = LoadLibrary(TEXT("avrt.dll"));
        if (DllAvRt != NULL)
        {
            FunctionAvSetMmThreadCharacteristics = (AVSETMMTHREADCHARACTERISTICS*)GetProcAddress(DllAvRt,"AvSetMmThreadCharacteristicsA");
            FunctionAvRevertMmThreadCharacteristics = (AVREVERTMMTHREADCHARACTERISTICS*)GetProcAddress(DllAvRt, "AvRevertMmThreadCharacteristics");
            FunctionAvSetMmThreadPriority = (AVSETMMTHREADPRIORITY*)GetProcAddress(DllAvRt, "AvSetMmThreadPriority");
        }
    }

    FunctionKsCreatePin = (KSCREATEPIN*)GetProcAddress(DllKsUser, "KsCreatePin");
    if(FunctionKsCreatePin == NULL)
        goto error;

    wdmHostApi = (PaWinWdmHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaWinWdmHostApiRepresentation) );
    if( !wdmHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    wdmHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !wdmHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    wdmHostApi->filters = BuildFilterList( &wdmHostApi->filterCount, &totalDeviceCount, &result );
    if( result != paNoError )
    {
        goto error;
    }

    *hostApi = &wdmHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paWDMKS;
    (*hostApi)->info.name = "Windows WDM-KS";
    (*hostApi)->info.defaultInputDevice = paNoDevice;
    (*hostApi)->info.defaultOutputDevice = paNoDevice;

    if( totalDeviceCount > 0 )
    {
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
            wdmHostApi->allocations, sizeof(PaDeviceInfo*) * totalDeviceCount );
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all device info structs in a contiguous block */
        deviceInfoArray = (PaWinWdmDeviceInfo*)PaUtil_GroupAllocateMemory(
            wdmHostApi->allocations, sizeof(PaWinWdmDeviceInfo) * totalDeviceCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }

        idxDevice = 0;
        for (idxFilter = 0; idxFilter < wdmHostApi->filterCount; ++idxFilter)
        {
            int i;
            PaWinWdmFilter* pFilter = wdmHostApi->filters[idxFilter];
            if( pFilter == NULL )
                continue;

            for (i = 0; i < pFilter->pinCount; ++i)
            {
                size_t n;
                unsigned m;
                ULONG nameIndex = 0;
                ULONG nameIndexHash = 0;
                PaWinWdmDeviceInfo* wdmDeviceInfo;
                PaWinWdmPin* pin = pFilter->pins[i];

                if (pin == NULL)
                    continue;

                for (m = 0; m < max(1, pin->inputCount); ++m)
                {
                    PaDeviceInfo *deviceInfo;
                    wdmDeviceInfo = &deviceInfoArray[idxDevice];
                    deviceInfo = &wdmDeviceInfo->inheritedDeviceInfo;

                    wdmDeviceInfo->filter = pFilter;

                    deviceInfo->structVersion = 2;
                    deviceInfo->hostApi = hostApiIndex;
                    deviceInfo->name = wdmDeviceInfo->compositeName;

                    wdmDeviceInfo->pin = pin->pinId;

                    /* Create the name of the "device" */
                    if (pin->inputs == NULL)
                    {
                        n = _snprintf(wdmDeviceInfo->compositeName, MAX_PATH, "%s", pin->friendlyName);
                        _snprintf(wdmDeviceInfo->compositeName+n, MAX_PATH-n, " (%s)", pFilter->friendlyName);
                        wdmDeviceInfo->muxPosition = -1;
                        wdmDeviceInfo->endpointPinId = pin->endpointPinId;
                    }
                    else
                    {
                        PaWinWdmMuxedInput* input = pin->inputs[m];
                        /* Check if there are more inputs with same name (which might very well be the case), if there
                        are, the name will be postfixed with an index. Note that current implementation only deals with
                        recurrence of one name, not N this and M that... ;) */
                        if (nameIndex == 0)
                        {
                            unsigned j = m + 1;
                            for (; j < pin->inputCount; ++j)
                            {
                                if (cmpstring(pin->inputs[j]->friendlyName, input->friendlyName) == 0)
                                {
                                    nameIndex = 1;
                                    nameIndexHash = GetHash(input->friendlyName);
                                    break;
                                }
                            }
                        }
                        n = _snprintf(wdmDeviceInfo->compositeName, MAX_PATH, "%s", input->friendlyName);
                        if (nameIndex > 0 && (nameIndexHash == GetHash(input->friendlyName)))
                        {
                            n += _snprintf(wdmDeviceInfo->compositeName+n, MAX_PATH-n, " %u", nameIndex);
                            ++nameIndex;
                        }
                        _snprintf(wdmDeviceInfo->compositeName+n, MAX_PATH-n, " (%s)", pFilter->friendlyName);
                        wdmDeviceInfo->muxPosition = (int)m;
                        wdmDeviceInfo->endpointPinId = input->endpointPinId;
                    }

                    if (pin->dataFlow == KSPIN_DATAFLOW_IN)
                    {
                        /* OUTPUT ! */
                        deviceInfo->maxInputChannels  = 0;
                        deviceInfo->maxOutputChannels = pin->maxChannels;
                        if((*hostApi)->info.defaultOutputDevice == paNoDevice)
                        {
                            (*hostApi)->info.defaultOutputDevice = idxDevice;
                        }

                    }
                    else
                    {
                        /* INPUT ! */
                        deviceInfo->maxInputChannels  = pin->maxChannels;
                        deviceInfo->maxOutputChannels = 0;
                        if ((*hostApi)->info.defaultInputDevice == paNoDevice)
                        {
                            (*hostApi)->info.defaultInputDevice = idxDevice;
                        }
                    }

                    /* These low values are not very useful because
                    * a) The lowest latency we end up with can depend on many factors such
                    *    as the device buffer sizes/granularities, sample rate, channels and format
                    * b) We cannot know the device buffer sizes until we try to open/use it at
                    *    a particular setting
                    * So: we give 512x48000Hz frames as the default low input latency
                    **/
                    switch (pFilter->waveType)
                    {
                    case Type_kWaveCyclic:
                        if (IsEarlierThanVista())
                        {
                            /* XP doesn't tolerate low latency, unless the Process Priority Class is set to REALTIME_PRIORITY_CLASS 
                            through SetPriorityClass, then 10 ms is quite feasible. However, one should then bear in mind that ALL of
                            the process is running in REALTIME_PRIORITY_CLASS, which might not be appropriate for an application with
                            a GUI . In this case it is advisable to separate the audio engine in another process and use IPC to communicate
                            with it. */
                            deviceInfo->defaultLowInputLatency = 0.02;
                            deviceInfo->defaultLowOutputLatency = 0.02;
                        }
                        else
                        {
                            /* This is a conservative estimate. Most WaveCyclic drivers will limit the available latency, but f.i. my Edirol
                            PCR-A30 can reach 3 ms latency easily... */
                            deviceInfo->defaultLowInputLatency = 0.01;
                            deviceInfo->defaultLowOutputLatency = 0.01;
                        }
                        deviceInfo->defaultHighInputLatency = (4096.0/48000.0);
                        deviceInfo->defaultHighOutputLatency = (4096.0/48000.0);
                        deviceInfo->defaultSampleRate = (double)(pin->bestSampleRate);
                        break;
                    case Type_kWaveRT:
                        /* This is also a conservative estimate, based on WaveRT polled mode. In polled mode, the latency will be dictated
                        by the buffer size given by the driver. */
                        deviceInfo->defaultLowInputLatency = 0.005;
                        deviceInfo->defaultLowOutputLatency = 0.005;
                        deviceInfo->defaultHighInputLatency = (512/48000.0);
                        deviceInfo->defaultHighOutputLatency = (512/48000.0);
                        deviceInfo->defaultSampleRate = (double)(pin->bestSampleRate);
                        break;
                    default:
                        assert(0);
                        break;
                    }
                    assert(idxDevice < totalDeviceCount);
                    (*hostApi)->deviceInfos[idxDevice] = deviceInfo;
                    ++idxDevice;
                }
            }
        }
    }

    (*hostApi)->info.deviceCount = idxDevice;

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &wdmHostApi->callbackStreamInterface, CloseStream, StartStream,
        StopStream, AbortStream, IsStreamStopped, IsStreamActive,
        GetStreamTime, GetStreamCpuLoad,
        PaUtil_DummyRead, PaUtil_DummyWrite,
        PaUtil_DummyGetReadAvailable, PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface( &wdmHostApi->blockingStreamInterface, CloseStream, StartStream,
        StopStream, AbortStream, IsStreamStopped, IsStreamActive,
        GetStreamTime, PaUtil_DummyGetCpuLoad,
        ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    PA_LOGL_;
    return result;

error:
    Terminate( (PaUtilHostApiRepresentation*)wdmHostApi );

    PA_LOGL_;
    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaWinWdmHostApiRepresentation *wdmHostApi = (PaWinWdmHostApiRepresentation*)hostApi;
    int i;
    PA_LOGE_;

    if( DllKsUser != NULL )
    {
        FreeLibrary( DllKsUser );
        DllKsUser = NULL;
    }

    if( DllAvRt != NULL )
    {
        FreeLibrary( DllAvRt );
        DllAvRt = NULL;
    }

    if( wdmHostApi)
    {
        if( wdmHostApi->filters )
        {
            for( i=0; i<wdmHostApi->filterCount; i++)
            {
                if( wdmHostApi->filters[i] != NULL )
                {
                    FilterFree( wdmHostApi->filters[i] );
                    wdmHostApi->filters[i] = NULL;
                }
            }
            PaUtil_FreeMemory( wdmHostApi->filters );
        }
        if( wdmHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( wdmHostApi->allocations );
            PaUtil_DestroyAllocationGroup( wdmHostApi->allocations );
        }
        PaUtil_FreeMemory( wdmHostApi );
    }
    PA_LOGL_;
}

static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                 const PaStreamParameters *inputParameters,
                                 const PaStreamParameters *outputParameters,
                                 double sampleRate )
{
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaWinWdmHostApiRepresentation *wdmHostApi = (PaWinWdmHostApiRepresentation*)hostApi;
    PaWinWdmFilter* pFilter;
    int result = paFormatIsSupported;
    WAVEFORMATEXTENSIBLE wfx;
    PaWinWaveFormatChannelMask channelMask;

    PA_LOGE_;

    if( inputParameters )
    {
        PaWinWdmDeviceInfo* pDeviceInfo = (PaWinWdmDeviceInfo*)wdmHostApi->inheritedHostApiRep.deviceInfos[inputParameters->device];
        PaWinWdmPin* pin;

        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
        this implementation doesn't support any custom sample formats */
        if( inputSampleFormat & paCustomFormat )
        {
            PaWinWDM_SetLastErrorInfo(paSampleFormatNotSupported, "Custom input format not supported");
            return paSampleFormatNotSupported;
        }

        /* unless alternate device specification is supported, reject the use of
        paUseHostApiSpecificDeviceSpecification */

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
        {
            PaWinWDM_SetLastErrorInfo(paInvalidDevice, "paUseHostApiSpecificDeviceSpecification not supported");
            return paInvalidDevice;
        }

        /* check that input device can support inputChannelCount */
        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
        {
            PaWinWDM_SetLastErrorInfo(paInvalidChannelCount, "Invalid input channel count");
            return paInvalidChannelCount;
        }

        /* validate inputStreamInfo */
        if( inputParameters->hostApiSpecificStreamInfo )
        {
            PaWinWDM_SetLastErrorInfo(paIncompatibleHostApiSpecificStreamInfo, "Host API stream info not supported");
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        }

        /* Check that the input format is supported */
        channelMask = PaWin_DefaultChannelMask(inputChannelCount);
        PaWin_InitializeWaveFormatExtensible((PaWinWaveFormat*)&wfx,
            inputChannelCount, 
            paInt16,
            PaWin_SampleFormatToLinearWaveFormatTag(paInt16),
            sampleRate,
            channelMask );

        pFilter = pDeviceInfo->filter;
        pin = pFilter->pins[pDeviceInfo->pin];

        result = PinIsFormatSupported(pin, (const WAVEFORMATEX*)&wfx);
        //result = FilterCanCreateCapturePin(pFilter,(const WAVEFORMATEX*)&wfx);
        if( result != paNoError )
        {
            /* Try a WAVE_FORMAT_PCM instead */
            PaWin_InitializeWaveFormatEx((PaWinWaveFormat*)&wfx,
                inputChannelCount, 
                paInt16,
                PaWin_SampleFormatToLinearWaveFormatTag(paInt16),
                sampleRate);

            result = PinIsFormatSupported(pin, (const WAVEFORMATEX*)&wfx);
            //result = FilterCanCreateCapturePin(pFilter,(const WAVEFORMATEX*)&wfx);
            if( result != paNoError )
            {
                PaWinWDM_SetLastErrorInfo(result, "FilterCanCreateCapturePin failed: sr=%u,ch=%u,bits=%u", wfx.Format.nSamplesPerSec, wfx.Format.nChannels, wfx.Format.wBitsPerSample);
                return result;
            }
        }
    }
    else
    {
        inputChannelCount = 0;
    }

    if( outputParameters )
    {
        PaWinWdmDeviceInfo* pDeviceInfo = (PaWinWdmDeviceInfo*)wdmHostApi->inheritedHostApiRep.deviceInfos[outputParameters->device];
        PaWinWdmPin* pin;

        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
        this implementation doesn't support any custom sample formats */
        if( outputSampleFormat & paCustomFormat )
        {
            PaWinWDM_SetLastErrorInfo(paSampleFormatNotSupported, "Custom output format not supported");
            return paSampleFormatNotSupported;
        }

        /* unless alternate device specification is supported, reject the use of
        paUseHostApiSpecificDeviceSpecification */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
        {
            PaWinWDM_SetLastErrorInfo(paInvalidDevice, "paUseHostApiSpecificDeviceSpecification not supported");
            return paInvalidDevice;
        }

        /* check that output device can support outputChannelCount */
        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
        {
            PaWinWDM_SetLastErrorInfo(paInvalidChannelCount, "Invalid output channel count");
            return paInvalidChannelCount;
        }

        /* validate outputStreamInfo */
        if( outputParameters->hostApiSpecificStreamInfo )
        {
            PaWinWDM_SetLastErrorInfo(paIncompatibleHostApiSpecificStreamInfo, "Host API stream info not supported");
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        }

        /* Check that the output format is supported */
        channelMask = PaWin_DefaultChannelMask(outputChannelCount);
        PaWin_InitializeWaveFormatExtensible((PaWinWaveFormat*)&wfx,
            outputChannelCount, 
            paInt16,
            PaWin_SampleFormatToLinearWaveFormatTag(paInt16),
            sampleRate,
            channelMask );


        pFilter = pDeviceInfo->filter;
        pin = pFilter->pins[pDeviceInfo->pin];

        result = PinIsFormatSupported(pin, (const WAVEFORMATEX*)&wfx);
        //result = FilterCanCreateRenderPin(pFilter,(const WAVEFORMATEX*)&wfx);
        if( result != paNoError )
        {
            /* Try a WAVE_FORMAT_PCM instead */
            PaWin_InitializeWaveFormatEx((PaWinWaveFormat*)&wfx,
                outputChannelCount, 
                paInt16,
                PaWin_SampleFormatToLinearWaveFormatTag(paInt16),
                sampleRate);
            result = PinIsFormatSupported(pin, (const WAVEFORMATEX*)&wfx);
            //result = FilterCanCreateRenderPin(pFilter,(const WAVEFORMATEX*)&wfx);
            if( result != paNoError )
            {
                PaWinWDM_SetLastErrorInfo(result, "FilterCanCreateRenderPin failed: %u,%u,%u", wfx.Format.nSamplesPerSec, wfx.Format.nChannels, wfx.Format.wBitsPerSample);
                return result;
            }
        }

    }
    else
    {
        outputChannelCount = 0;
    }

    /*
    IMPLEMENT ME:

    - if a full duplex stream is requested, check that the combination
    of input and output parameters is supported if necessary

    - check that the device supports sampleRate

    Because the buffer adapter handles conversion between all standard
    sample formats, the following checks are only required if paCustomFormat
    is implemented, or under some other unusual conditions.

    - check that input device can support inputSampleFormat, or that
    we have the capability to convert from inputSampleFormat to
    a native format

    - check that output device can support outputSampleFormat, or that
    we have the capability to convert from outputSampleFormat to
    a native format
    */
    if((inputChannelCount == 0)&&(outputChannelCount == 0))
    {
        PaWinWDM_SetLastErrorInfo(paSampleFormatNotSupported, "No input or output channels defined");
        result = paSampleFormatNotSupported; /* Not right error */
    }

    PA_LOGL_;
    return result;
}

static void ResetStreamEvents(PaWinWdmStream* stream) 
{
    unsigned i;
    ResetEvent(stream->eventAbort);
    ResetEvent(stream->eventStreamStart[StreamStart_kOk]);
    ResetEvent(stream->eventStreamStart[StreamStart_kFailed]);

    for (i=0; i<stream->capture.noOfPackets; ++i)
    {
        if (stream->capture.events && stream->capture.events[i])
        {
            ResetEvent(stream->capture.events[i]);
        }
    }

    for (i=0; i<stream->render.noOfPackets; ++i)
    {
        if (stream->render.events && stream->render.events[i])
        {
            ResetEvent(stream->render.events[i]);
        }
    }
}

static void CloseStreamEvents(PaWinWdmStream* stream) 
{
    unsigned i;
    if (stream->eventAbort)
    {
        CloseHandle(stream->eventAbort);
        stream->eventAbort = 0;
    }
    if (stream->eventStreamStart[StreamStart_kOk])
    {
        CloseHandle(stream->eventStreamStart[StreamStart_kOk]);
    }
    if (stream->eventStreamStart[StreamStart_kFailed])
    {
        CloseHandle(stream->eventStreamStart[StreamStart_kFailed]);
    }

    /* Unregister notification handles for WaveRT */
    if (stream->capture.pPin && stream->capture.pPin->parentFilter->waveType == Type_kWaveRT &&
        stream->capture.pPin->parentFilter->waveSubType == SubType_kNotification)
    {
        PinUnregisterNotificationHandle(stream->capture.pPin, stream->capture.events[0]);
    }
    if (stream->render.pPin && stream->render.pPin->parentFilter->waveType == Type_kWaveRT && 
        stream->render.pPin->parentFilter->waveSubType == SubType_kNotification)
    {
        PinUnregisterNotificationHandle(stream->render.pPin, stream->render.events[0]);
    }

    for (i=0; i<stream->capture.noOfPackets; ++i)
    {
        if (stream->capture.events && stream->capture.events[i])
        {
            CloseHandle(stream->capture.events[i]);
            stream->capture.events[i] = 0;
        }
    }

    for (i=0; i<stream->render.noOfPackets; ++i)
    {
        if (stream->render.events && stream->render.events[i])
        {
            CloseHandle(stream->render.events[i]);
            stream->render.events[i];
        }
    }
}

static unsigned NextPowerOf2(unsigned val)
{
    val--;
    val = (val >> 1) | val;
    val = (val >> 2) | val;
    val = (val >> 4) | val;
    val = (val >> 8) | val;
    val = (val >> 16) | val;
    return ++val;
}

static PaError ValidateSpecificStreamParameters(
    const PaStreamParameters *streamParameters,
    const PaWinWDMKSInfo *streamInfo)
{
    if( streamInfo )
    {
        if( streamInfo->size != sizeof( PaWinWDMKSInfo )
            || streamInfo->version != 1 )
        {
            PA_DEBUG(("Stream parameters: size or version not correct"));
            return paIncompatibleHostApiSpecificStreamInfo;
        }

        if (!!(streamInfo->flags & ~(paWinWDMKSOverrideFramesize | paWinWDMKSDisableTimeoutInProcessingThread)))
        {
            PA_DEBUG(("Stream parameters: non supported flags set"));
            return paIncompatibleHostApiSpecificStreamInfo;
        }

        if (streamInfo->noOfPackets != 0 &&
            (streamInfo->noOfPackets < 2 || streamInfo->noOfPackets > 8))
        {
            PA_DEBUG(("Stream parameters: noOfPackets out of range"));
            return paIncompatibleHostApiSpecificStreamInfo;
    }

    }

    return paNoError;
}



/* see pa_hostapi.h for a list of validity guarantees made about OpenStream parameters */

static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                          PaStream** s,
                          const PaStreamParameters *inputParameters,
                          const PaStreamParameters *outputParameters,
                          double sampleRate,
                          unsigned long framesPerUserBuffer,
                          PaStreamFlags streamFlags,
                          PaStreamCallback *streamCallback,
                          void *userData )
{
    PaError result = paNoError;
    PaWinWdmHostApiRepresentation *wdmHostApi = (PaWinWdmHostApiRepresentation*)hostApi;
    PaWinWdmStream *stream = 0;
    /* unsigned long framesPerHostBuffer; these may not be equivalent for all implementations */
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;
    int userInputChannels,userOutputChannels;
    WAVEFORMATEXTENSIBLE wfx;

    PA_LOGE_;
    PA_DEBUG(("OpenStream:sampleRate = %f\n",sampleRate));
    PA_DEBUG(("OpenStream:framesPerBuffer = %lu\n",framesPerUserBuffer));

    if( inputParameters )
    {
        userInputChannels = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
        paUseHostApiSpecificDeviceSpecification */

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
        {
            PaWinWDM_SetLastErrorInfo(paInvalidDevice, "paUseHostApiSpecificDeviceSpecification(in) not supported");
            return paInvalidDevice;
        }

        /* check that input device can support stream->userInputChannels */
        if( userInputChannels > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
        {
            PaWinWDM_SetLastErrorInfo(paInvalidChannelCount, "Invalid input channel count");
            return paInvalidChannelCount;
        }

        /* validate inputStreamInfo */
        result = ValidateSpecificStreamParameters(inputParameters, inputParameters->hostApiSpecificStreamInfo);
        if(result != paNoError)
        {
            PaWinWDM_SetLastErrorInfo(result, "Host API stream info not supported (in)");
            return result; /* this implementation doesn't use custom stream info */
        }
    }
    else
    {
        userInputChannels = 0;
        inputSampleFormat = hostInputSampleFormat = paInt16; /* Supress 'uninitialised var' warnings. */
    }

    if( outputParameters )
    {
        userOutputChannels = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
        paUseHostApiSpecificDeviceSpecification */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
        {
            PaWinWDM_SetLastErrorInfo(paInvalidDevice, "paUseHostApiSpecificDeviceSpecification(out) not supported");
            return paInvalidDevice;
        }

        /* check that output device can support stream->userInputChannels */
        if( userOutputChannels > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
        {
            PaWinWDM_SetLastErrorInfo(paInvalidChannelCount, "Invalid output channel count");
            return paInvalidChannelCount;
        }

        /* validate outputStreamInfo */
        result = ValidateSpecificStreamParameters( outputParameters, outputParameters->hostApiSpecificStreamInfo );
        if (result != paNoError)
        {
            PaWinWDM_SetLastErrorInfo(result, "Host API stream info not supported (out)");
            return result; /* this implementation doesn't use custom stream info */
        }
    }
    else
    {
        userOutputChannels = 0;
        outputSampleFormat = hostOutputSampleFormat = paInt16; /* Supress 'uninitialized var' warnings. */
    }

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
    {
        PaWinWDM_SetLastErrorInfo(paInvalidFlag, "Invalid flag supplied");
        return paInvalidFlag; /* unexpected platform specific flag */
    }

    stream = (PaWinWdmStream*)PaUtil_AllocateMemory( sizeof(PaWinWdmStream) );
    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* Create allocation group */
    stream->allocGroup = PaUtil_CreateAllocationGroup();
    if( !stream->allocGroup )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* Zero the stream object */
    /* memset((void*)stream,0,sizeof(PaWinWdmStream)); */

    if( streamCallback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
            &wdmHostApi->callbackStreamInterface, streamCallback, userData );
    }
    else
    {
        /* PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
        &wdmHostApi->blockingStreamInterface, streamCallback, userData ); */

        /* We don't support the blocking API yet */
        PA_DEBUG(("Blocking API not supported yet!\n"));
        PaWinWDM_SetLastErrorInfo(paUnanticipatedHostError, "Blocking API not supported yet");
        result = paUnanticipatedHostError;
        goto error;
    }

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    /* Instantiate the input pin if necessary */
    if(userInputChannels > 0)
    {
        PaWinWdmFilter* pFilter;
        PaWinWdmDeviceInfo* pDeviceInfo;
        PaWinWdmPin* pPin;
        PaWinWaveFormatChannelMask channelMask = PaWin_DefaultChannelMask( userInputChannels );

        result = paSampleFormatNotSupported;
        pDeviceInfo = (PaWinWdmDeviceInfo*)wdmHostApi->inheritedHostApiRep.deviceInfos[inputParameters->device];
        pFilter = pDeviceInfo->filter;
        pPin = pFilter->pins[pDeviceInfo->pin];

        stream->userInputChannels = userInputChannels;

        hostInputSampleFormat = PaUtil_SelectClosestAvailableFormat( pPin->formats, inputSampleFormat );
        if (hostInputSampleFormat == paSampleFormatNotSupported)
        {
            result = paUnanticipatedHostError;
            PaWinWDM_SetLastErrorInfo(result, "PU_SCAF(%X,%X) failed (input)", pPin->formats, inputSampleFormat);
            goto error;
        }

        while (hostInputSampleFormat <= paUInt8)
        {
            unsigned channelsToProbe = stream->userInputChannels;
            /* Some or all KS devices can only handle the exact number of channels
            * they specify. But PortAudio clients expect to be able to
            * at least specify mono I/O on a multi-channel device
            * If this is the case, then we will do the channel mapping internally
            * The following loop tests this case
            **/
            while (1)
            {
                PaWin_InitializeWaveFormatExtensible((PaWinWaveFormat*)&wfx,
                    channelsToProbe, 
                    hostInputSampleFormat,
                    PaWin_SampleFormatToLinearWaveFormatTag(hostInputSampleFormat),
                    sampleRate,
                    channelMask );
                stream->capture.bytesPerFrame = wfx.Format.nBlockAlign;
                stream->capture.pPin = FilterCreateCapturePin(pFilter, pPin->pinId, (WAVEFORMATEX*)&wfx, &result);
                stream->deviceInputChannels = channelsToProbe;

                if( result != paNoError && result != paDeviceUnavailable )
                {
                    /* Try a WAVE_FORMAT_PCM instead */
                    PaWin_InitializeWaveFormatEx((PaWinWaveFormat*)&wfx,
                        channelsToProbe, 
                        hostInputSampleFormat,
                        PaWin_SampleFormatToLinearWaveFormatTag(hostInputSampleFormat),
                        sampleRate);
                    stream->capture.pPin = FilterCreateCapturePin(pFilter, pPin->pinId, (const WAVEFORMATEX*)&wfx, &result);
                }

                if (result == paDeviceUnavailable) goto occupied;

                if (result == paNoError)
                {
                    /* We're done */
                    break;
                }

                if (channelsToProbe < (unsigned)pPin->maxChannels)
                {
                    /* Go to next multiple of 2 */
                    channelsToProbe = min((((channelsToProbe>>1)+1)<<1), (unsigned)pPin->maxChannels);
                    continue;
                }

                break;
            }

            if (result == paNoError)
            {
                /* We're done */
                break;
            }

            /* Go to next format in line with lower resolution */
            hostInputSampleFormat <<= 1;
        }

        if(stream->capture.pPin == NULL)
        {
            PaWinWDM_SetLastErrorInfo(result, "Failed to create capture pin: sr=%u,ch=%u,bits=%u,align=%u",
                wfx.Format.nSamplesPerSec, wfx.Format.nChannels, wfx.Format.wBitsPerSample, wfx.Format.nBlockAlign);
            goto error;
        }

        /* Select correct mux input on MUX node of topology filter */
        if (pDeviceInfo->muxPosition >= 0)
        {
            assert(pPin->parentFilter->topologyFilter != NULL);

            result = FilterUse(pPin->parentFilter->topologyFilter);
            if (result != paNoError)
            {
                PaWinWDM_SetLastErrorInfo(result, "Failed to open topology filter");
                goto error;
            }

            result = WdmSetMuxNodeProperty(pPin->parentFilter->topologyFilter->handle,
                pPin->inputs[pDeviceInfo->muxPosition]->muxNodeId,
                pPin->inputs[pDeviceInfo->muxPosition]->muxPinId);

            FilterRelease(pPin->parentFilter->topologyFilter);

            if(result != paNoError)
            {
                PaWinWDM_SetLastErrorInfo(result, "Failed to set topology mux node");
                goto error;
            }
        }

        stream->capture.bytesPerSample = stream->capture.bytesPerFrame / stream->deviceInputChannels;
        stream->capture.pPin->frameSize /= stream->capture.bytesPerFrame;
        PA_DEBUG(("Capture pin frames: %d\n",stream->capture.pPin->frameSize));
    }
    else
    {
        stream->capture.pPin = NULL;
        stream->capture.bytesPerFrame = 0;
    }

    /* Instantiate the output pin if necessary */
    if(userOutputChannels > 0)
    {
        PaWinWdmFilter* pFilter;
        PaWinWdmDeviceInfo* pDeviceInfo;
        PaWinWdmPin* pPin;
        PaWinWaveFormatChannelMask channelMask = PaWin_DefaultChannelMask( userOutputChannels );

        result = paSampleFormatNotSupported;
        pDeviceInfo = (PaWinWdmDeviceInfo*)wdmHostApi->inheritedHostApiRep.deviceInfos[outputParameters->device];
        pFilter = pDeviceInfo->filter;
        pPin = pFilter->pins[pDeviceInfo->pin];

        stream->userOutputChannels = userOutputChannels;

        hostOutputSampleFormat = PaUtil_SelectClosestAvailableFormat( pPin->formats, outputSampleFormat );
        if (hostOutputSampleFormat == paSampleFormatNotSupported)
        {
            result = paUnanticipatedHostError;
            PaWinWDM_SetLastErrorInfo(result, "PU_SCAF(%X,%X) failed (output)", pPin->formats, hostOutputSampleFormat);
            goto error;
        }

        while (hostOutputSampleFormat <= paUInt8)
        {
            unsigned channelsToProbe = stream->userOutputChannels;
            /* Some or all KS devices can only handle the exact number of channels
            * they specify. But PortAudio clients expect to be able to
            * at least specify mono I/O on a multi-channel device
            * If this is the case, then we will do the channel mapping internally
            * The following loop tests this case
            **/
            while (1)
            {
                PaWin_InitializeWaveFormatExtensible((PaWinWaveFormat*)&wfx,
                    channelsToProbe, 
                    hostOutputSampleFormat,
                    PaWin_SampleFormatToLinearWaveFormatTag(hostOutputSampleFormat),
                    sampleRate,
                    channelMask );
                stream->render.bytesPerFrame = wfx.Format.nBlockAlign;
                stream->render.pPin = FilterCreateRenderPin(pFilter, pPin->pinId, (WAVEFORMATEX*)&wfx, &result);
                stream->deviceOutputChannels = channelsToProbe;

                if( result != paNoError && result != paDeviceUnavailable )
                {
                    PaWin_InitializeWaveFormatEx((PaWinWaveFormat*)&wfx,
                        channelsToProbe, 
                        hostOutputSampleFormat,
                        PaWin_SampleFormatToLinearWaveFormatTag(hostOutputSampleFormat),
                        sampleRate);
                    stream->render.pPin = FilterCreateRenderPin(pFilter, pPin->pinId, (const WAVEFORMATEX*)&wfx, &result);
                }

                if (result == paDeviceUnavailable) goto occupied;

                if (result == paNoError)
                {
                    /* We're done */
                    break;
                }

                if (channelsToProbe < (unsigned)pPin->maxChannels)
                {
                    /* Go to next multiple of 2 */
                    channelsToProbe = min((((channelsToProbe>>1)+1)<<1), (unsigned)pPin->maxChannels);
                    continue;
                }

                break;
            };

            if (result == paNoError)
            {
                /* We're done */
                break;
            }

            /* Go to next format in line with lower resolution */
            hostOutputSampleFormat <<= 1;
        }

        if(stream->render.pPin == NULL)
        {
            PaWinWDM_SetLastErrorInfo(result, "Failed to create render pin: sr=%u,ch=%u,bits=%u,align=%u",
                wfx.Format.nSamplesPerSec, wfx.Format.nChannels, wfx.Format.wBitsPerSample, wfx.Format.nBlockAlign);
            goto error;
        }

        stream->render.bytesPerSample = stream->render.bytesPerFrame / stream->deviceOutputChannels;
        stream->render.pPin->frameSize /= stream->render.bytesPerFrame;
        PA_DEBUG(("Render pin frames: %d\n",stream->render.pPin->frameSize));
    }
    else
    {
        stream->render.pPin = NULL;
        stream->render.bytesPerFrame = 0;
    }

    /* Calculate the framesPerHostXxxxBuffer size based upon the suggested latency values */
    /* Record the buffer length */
    if(inputParameters)
    {
        /* Calculate the frames from the user's value - add a bit to round up */
        stream->capture.framesPerBuffer = (unsigned long)((inputParameters->suggestedLatency*sampleRate)+0.0001);
        if(stream->capture.framesPerBuffer > (unsigned long)sampleRate)
        { /* Upper limit is 1 second */
            stream->capture.framesPerBuffer = (unsigned long)sampleRate;
        }
        else if(stream->capture.framesPerBuffer < stream->capture.pPin->frameSize)
        {
            stream->capture.framesPerBuffer = stream->capture.pPin->frameSize;
        }
        PA_DEBUG(("Input frames chosen:%ld\n",stream->capture.framesPerBuffer));

        /* Setup number of packets to use */
        stream->capture.noOfPackets = 2;

        if (inputParameters->hostApiSpecificStreamInfo)
        {
            PaWinWDMKSInfo* pInfo = (PaWinWDMKSInfo*)inputParameters->hostApiSpecificStreamInfo;
            stream->disableTimeout |= !!(pInfo->flags & paWinWDMKSDisableTimeoutInProcessingThread);

            if (stream->capture.pPin->parentFilter->waveType == Type_kWaveCyclic &&
                pInfo->noOfPackets != 0)
            {
                stream->capture.noOfPackets = pInfo->noOfPackets;
            }
        }
    }

    if(outputParameters)
    {
        /* Calculate the frames from the user's value - add a bit to round up */
        stream->render.framesPerBuffer = (unsigned long)((outputParameters->suggestedLatency*sampleRate)+0.0001);
        if(stream->render.framesPerBuffer > (unsigned long)sampleRate)
        { /* Upper limit is 1 second */
            stream->render.framesPerBuffer = (unsigned long)sampleRate;
        }
        else if(stream->render.framesPerBuffer < stream->render.pPin->frameSize)
        {
            stream->render.framesPerBuffer = stream->render.pPin->frameSize;
        }
        PA_DEBUG(("Output frames chosen:%ld\n",stream->render.framesPerBuffer));

        /* Setup number of packets to use */
        stream->render.noOfPackets = 2;

        if (outputParameters->hostApiSpecificStreamInfo)
        {
            PaWinWDMKSInfo* pInfo = (PaWinWDMKSInfo*)outputParameters->hostApiSpecificStreamInfo;
            stream->disableTimeout |= !!(pInfo->flags & paWinWDMKSDisableTimeoutInProcessingThread);

            if (stream->render.pPin->parentFilter->waveType == Type_kWaveCyclic &&
                pInfo->noOfPackets != 0)
            {
                stream->render.noOfPackets = pInfo->noOfPackets;
            }
        }
    }

    /* Host buffer size is bound to the largest of the input and output frame sizes */
    result =  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
        stream->userInputChannels, inputSampleFormat, hostInputSampleFormat,
        stream->userOutputChannels, outputSampleFormat, hostOutputSampleFormat,
        sampleRate, streamFlags, framesPerUserBuffer,
        max(stream->capture.framesPerBuffer, stream->render.framesPerBuffer), 
        paUtilBoundedHostBufferSize,
        streamCallback, userData );
    if( result != paNoError )
    {
        PaWinWDM_SetLastErrorInfo(result, "PaUtil_InitializeBufferProcessor failed: ich=%u, isf=%u, hisf=%u, och=%u, osf=%u, hosf=%u, sr=%lf, flags=0x%X, fpub=%u, fphb=%u",
            stream->userInputChannels, inputSampleFormat, hostInputSampleFormat,
            stream->userOutputChannels, outputSampleFormat, hostOutputSampleFormat,
            sampleRate, streamFlags, framesPerUserBuffer,
            max(stream->capture.framesPerBuffer, stream->render.framesPerBuffer));
        goto error;
    }

    /* Allocate/get all the buffers for host I/O */
    if (stream->userInputChannels > 0)
    {
        stream->streamRepresentation.streamInfo.inputLatency = stream->capture.framesPerBuffer / sampleRate;

        switch (stream->capture.pPin->parentFilter->waveType)
        {
        case Type_kWaveCyclic:
            {
                unsigned size = stream->capture.noOfPackets * stream->capture.framesPerBuffer * stream->capture.bytesPerFrame;
                /* Allocate input host buffer */
                stream->capture.hostBuffer = (char*)PaUtil_GroupAllocateMemory(stream->allocGroup, size);
                PA_DEBUG(("Input buffer allocated (size = %u)\n", size));
                if( !stream->capture.hostBuffer )
                {
                    PA_DEBUG(("Cannot allocate host input buffer!\n"));
                    PaWinWDM_SetLastErrorInfo(paInsufficientMemory, "Failed to allocate input buffer");
                    result = paInsufficientMemory;
                    goto error;
                }
                stream->capture.hostBufferSize = size;
                PA_DEBUG(("Input buffer start = %p (size=%u)\n",stream->capture.hostBuffer, stream->capture.hostBufferSize));
                stream->capture.pPin->fnEventHandler = PaPinCaptureEventHandler_WaveCyclic;
                stream->capture.pPin->fnSubmitHandler = PaPinCaptureSubmitHandler_WaveCyclic;
            }
            break;
        case Type_kWaveRT:
            {
                const DWORD dwTotalSize = 2 * stream->capture.framesPerBuffer * stream->capture.bytesPerFrame;
                DWORD dwRequestedSize = dwTotalSize;
                BOOL bCallMemoryBarrier = FALSE;
                ULONG hwFifoLatency = 0;
                ULONG dummy;
                stream->capture.pPin->parentFilter->waveSubType = SubType_kNotification;
                result = PinGetBufferWithNotification(stream->capture.pPin, (void**)&stream->capture.hostBuffer, &dwRequestedSize, &bCallMemoryBarrier);
                if (result != paNoError)
                {
                    result = PinGetBufferWithoutNotification(stream->capture.pPin, (void**)&stream->capture.hostBuffer, &dwRequestedSize, &bCallMemoryBarrier);
                    if (result==paNoError)
                    {
                        stream->capture.pPin->parentFilter->waveSubType = SubType_kPolled;
                    }
                }
                if (!result) 
                {
                    PA_DEBUG(("Input buffer start = %p, size = %u\n", stream->capture.hostBuffer, dwRequestedSize));
                    if (dwRequestedSize != dwTotalSize)
                    {
                        PA_DEBUG(("Buffer length changed by driver from %u to %u !\n", dwTotalSize, dwRequestedSize));
                        /* Recalculate to what the driver has given us */
                        stream->capture.framesPerBuffer = dwRequestedSize / (2 * stream->capture.bytesPerFrame);
                    }
                    stream->capture.hostBufferSize = dwRequestedSize;

                    if (stream->capture.pPin->parentFilter->waveSubType == SubType_kPolled)
                    {
                        stream->capture.pPin->fnEventHandler = PaPinCaptureEventHandler_WaveRTPolled;
                        stream->capture.pPin->fnSubmitHandler = PaPinCaptureSubmitHandler_WaveRTPolled;
                    }
                    else
                    {
                        stream->capture.pPin->fnEventHandler = PaPinCaptureEventHandler_WaveRTEvent;
                        stream->capture.pPin->fnSubmitHandler = PaPinCaptureSubmitHandler_WaveRTEvent;
                    }

                    stream->capture.pPin->fnMemBarrier = bCallMemoryBarrier ? MemoryBarrierRead : MemoryBarrierDummy;
                }
                else 
                {
                    PA_DEBUG(("Failed to get input buffer (WaveRT)\n"));
                    PaWinWDM_SetLastErrorInfo(paUnanticipatedHostError, "Failed to get input buffer (WaveRT)");
                    result = paUnanticipatedHostError;
                    goto error;
                }

                /* Get latency */
                result = PinGetHwLatency(stream->capture.pPin, &hwFifoLatency, &dummy, &dummy);
                if (result == paNoError)
                {
                    stream->capture.pPin->hwLatency = hwFifoLatency;

                    /* Add HW latency into total input latency */
                    stream->streamRepresentation.streamInfo.inputLatency += ((hwFifoLatency / stream->capture.bytesPerFrame) / sampleRate);
                }
                else
                {
                    PA_DEBUG(("Failed to get size of FIFO hardware buffer (is set to zero)\n"));
                    stream->capture.pPin->hwLatency = 0;
                }
            }
            break;
        default:
            /* Undefined wave type!! */
            assert(0);
            result = paInternalError;
            PaWinWDM_SetLastErrorInfo(result, "Wave type %u ??", stream->capture.pPin->parentFilter->waveType);
            goto error;
        }
    }
    else 
    {
        stream->capture.hostBuffer = 0;
    }

    if (stream->userOutputChannels > 0)
    {
        stream->streamRepresentation.streamInfo.outputLatency = stream->render.framesPerBuffer / sampleRate;

        switch (stream->render.pPin->parentFilter->waveType)
        {
        case Type_kWaveCyclic:
            {
                unsigned size = stream->render.noOfPackets * stream->render.framesPerBuffer * stream->render.bytesPerFrame;
                /* Allocate output device buffer */
                stream->render.hostBuffer = (char*)PaUtil_GroupAllocateMemory(stream->allocGroup, size);
                PA_DEBUG(("Output buffer allocated (size = %u)\n", size));
                if( !stream->render.hostBuffer )
                {
                    PA_DEBUG(("Cannot allocate host output buffer!\n"));
                    PaWinWDM_SetLastErrorInfo(paInsufficientMemory, "Failed to allocate output buffer");
                    result = paInsufficientMemory;
                    goto error;
                }
                stream->render.hostBufferSize = size;
                PA_DEBUG(("Output buffer start = %p (size=%u)\n",stream->render.hostBuffer, stream->render.hostBufferSize));

                stream->render.pPin->fnEventHandler = PaPinRenderEventHandler_WaveCyclic;
                stream->render.pPin->fnSubmitHandler = PaPinRenderSubmitHandler_WaveCyclic;
            }
            break;
        case Type_kWaveRT:
            {
                const DWORD dwTotalSize = 2 * stream->render.framesPerBuffer * stream->render.bytesPerFrame;
                DWORD dwRequestedSize = dwTotalSize;
                BOOL bCallMemoryBarrier = FALSE;
                ULONG hwFifoLatency = 0;
                ULONG dummy;
                stream->render.pPin->parentFilter->waveSubType = SubType_kNotification;
                result = PinGetBufferWithNotification(stream->render.pPin, (void**)&stream->render.hostBuffer, &dwRequestedSize, &bCallMemoryBarrier);
                if (result != paNoError)
                {
                    result = PinGetBufferWithoutNotification(stream->render.pPin, (void**)&stream->render.hostBuffer, &dwRequestedSize, &bCallMemoryBarrier);
                    if (result==paNoError)
                    {
                        stream->render.pPin->parentFilter->waveSubType = SubType_kPolled;
                    }
                }
                if (!result) 
                {
                    PA_DEBUG(("Output buffer start = %p, size = %u\n", stream->render.hostBuffer, dwRequestedSize));
                    if (dwRequestedSize != dwTotalSize)
                    {
                        PA_DEBUG(("Buffer length changed by driver from %u to %u !\n", dwTotalSize, dwRequestedSize));
                        /* Recalculate to what the driver has given us */
                        stream->render.framesPerBuffer = dwRequestedSize / (2 * stream->render.bytesPerFrame);
                    }
                    stream->render.hostBufferSize = dwRequestedSize;

                    if (stream->render.pPin->parentFilter->waveSubType == SubType_kPolled)
                    {
                        stream->render.pPin->fnEventHandler = PaPinRenderEventHandler_WaveRTPolled;
                        stream->render.pPin->fnSubmitHandler = PaPinRenderSubmitHandler_WaveRTPolled;
                    }
                    else
                    {
                        stream->render.pPin->fnEventHandler = PaPinRenderEventHandler_WaveRTEvent;
                        stream->render.pPin->fnSubmitHandler = PaPinRenderSubmitHandler_WaveRTEvent;
                    }

                    stream->render.pPin->fnMemBarrier = bCallMemoryBarrier ? MemoryBarrierWrite : MemoryBarrierDummy;
                }
                else 
                {
                    PA_DEBUG(("Failed to get output buffer (with notification)\n"));
                    PaWinWDM_SetLastErrorInfo(paUnanticipatedHostError, "Failed to get output buffer (with notification)");
                    result = paUnanticipatedHostError;
                    goto error;
                }

                /* Get latency */
                result = PinGetHwLatency(stream->render.pPin, &hwFifoLatency, &dummy, &dummy);
                if (result == paNoError)
                {
                    stream->render.pPin->hwLatency = hwFifoLatency;

                    /* Add HW latency into total output latency */
                    stream->streamRepresentation.streamInfo.outputLatency += ((hwFifoLatency / stream->render.bytesPerFrame) / sampleRate);
                }
                else
                {
                    PA_DEBUG(("Failed to get size of FIFO hardware buffer (is set to zero)\n"));
                    stream->render.pPin->hwLatency = 0;
                }
            }
            break;
        default:
            /* Undefined wave type!! */
            assert(0);
            result = paInternalError;
            PaWinWDM_SetLastErrorInfo(result, "Wave type %u ??", stream->capture.pPin->parentFilter->waveType);
            goto error;
        }
    }
    else 
    {
        stream->render.hostBuffer = 0;
    }

    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    PA_DEBUG(("BytesPerInputFrame = %d\n",stream->capture.bytesPerFrame));
    PA_DEBUG(("BytesPerOutputFrame = %d\n",stream->render.bytesPerFrame));

    /* memset(stream->hostBuffer,0,size); */

    /* Abort */
    stream->eventAbort          = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (stream->eventAbort == 0)
    {
        result = paInsufficientMemory;
        goto error;
    }
    stream->eventStreamStart[0] = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (stream->eventStreamStart[0] == 0)
    {
        result = paInsufficientMemory;
        goto error;
    }
    stream->eventStreamStart[1] = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (stream->eventStreamStart[1] == 0)
    {
        result = paInsufficientMemory;
        goto error;
    }

    if(stream->userInputChannels > 0)
    {
        const unsigned bufferSizeInBytes = stream->capture.framesPerBuffer * stream->capture.bytesPerFrame;
        const unsigned ringBufferFrameSize = NextPowerOf2( 1024 + 2 * max(stream->capture.framesPerBuffer, stream->render.framesPerBuffer) );

        stream->capture.events = (HANDLE*)PaUtil_GroupAllocateMemory(stream->allocGroup, stream->capture.noOfPackets * sizeof(HANDLE));
        if (stream->capture.events == NULL)
        {
            result = paInsufficientMemory;
            goto error;
        }

        stream->capture.packets = (DATAPACKET*)PaUtil_GroupAllocateMemory(stream->allocGroup, stream->capture.noOfPackets * sizeof(DATAPACKET));
        if (stream->capture.packets == NULL)
        {
            result = paInsufficientMemory;
            goto error;
        }

        switch(stream->capture.pPin->parentFilter->waveType)
        {
        case Type_kWaveCyclic:
            {
                /* WaveCyclic case */
                unsigned i;
                for (i = 0; i < stream->capture.noOfPackets; ++i)
                {
                    /* Set up the packets */
                    DATAPACKET *p = stream->capture.packets + i;

                    /* Record event */
                    stream->capture.events[i] = CreateEvent(NULL, FALSE, FALSE, NULL);

                    p->Signal.hEvent = stream->capture.events[i];
                    p->Header.Data = stream->capture.hostBuffer + (i*bufferSizeInBytes);
                    p->Header.FrameExtent = bufferSizeInBytes;
                    p->Header.DataUsed = 0;
                    p->Header.Size = sizeof(p->Header);
                    p->Header.PresentationTime.Numerator = 1;
                    p->Header.PresentationTime.Denominator = 1;
                }
            }
            break;
        case Type_kWaveRT:
            {
                /* Set up the "packets" */
                DATAPACKET *p = stream->capture.packets + 0;

                /* Record event: WaveRT has a single event for 2 notification per buffer */
                stream->capture.events[0] = CreateEvent(NULL, FALSE, FALSE, NULL);

                p->Header.Data = stream->capture.hostBuffer;
                p->Header.FrameExtent = bufferSizeInBytes;
                p->Header.DataUsed = 0;
                p->Header.Size = sizeof(p->Header);
                p->Header.PresentationTime.Numerator = 1;
                p->Header.PresentationTime.Denominator = 1;

                ++p;
                p->Header.Data = stream->capture.hostBuffer + bufferSizeInBytes;
                p->Header.FrameExtent = bufferSizeInBytes;
                p->Header.DataUsed = 0;
                p->Header.Size = sizeof(p->Header);
                p->Header.PresentationTime.Numerator = 1;
                p->Header.PresentationTime.Denominator = 1;

                if (stream->capture.pPin->parentFilter->waveSubType == SubType_kNotification)
                {
                    result = PinRegisterNotificationHandle(stream->capture.pPin, stream->capture.events[0]);

                    if (result != paNoError)
                    {
                        PA_DEBUG(("Failed to register capture notification handle\n"));
                        PaWinWDM_SetLastErrorInfo(paUnanticipatedHostError, "Failed to register capture notification handle");
                        result = paUnanticipatedHostError;
                        goto error;
                    }
                }

                result = PinRegisterPositionRegister(stream->capture.pPin);

                if (result != paNoError)
                {
                    PA_DEBUG(("Failed to register capture position register, using PinGetAudioPositionViaIOCTL\n"));
                    stream->capture.pPin->fnAudioPosition = PinGetAudioPositionViaIOCTL;
                }
                else
                {
                    stream->capture.pPin->fnAudioPosition = PinGetAudioPositionDirect;
                }
            }
            break;
        default:
            /* Undefined wave type!! */
            assert(0);
            result = paInternalError;
            PaWinWDM_SetLastErrorInfo(result, "Wave type %u ??", stream->capture.pPin->parentFilter->waveType);
            goto error;
        }

        /* Setup the input ring buffer here */
        stream->ringBufferData = (char*)PaUtil_GroupAllocateMemory(stream->allocGroup, ringBufferFrameSize * stream->capture.bytesPerFrame);
        if (stream->ringBufferData == NULL)
        {
            result = paInsufficientMemory;
            goto error;
        }
        PaUtil_InitializeRingBuffer(&stream->ringBuffer, stream->capture.bytesPerFrame, ringBufferFrameSize, stream->ringBufferData);
    }
    if(stream->userOutputChannels > 0)
    {
        const unsigned bufferSizeInBytes = stream->render.framesPerBuffer * stream->render.bytesPerFrame;

        stream->render.events = (HANDLE*)PaUtil_GroupAllocateMemory(stream->allocGroup, stream->render.noOfPackets * sizeof(HANDLE));
        if (stream->render.events == NULL)
        {
            result = paInsufficientMemory;
            goto error;
        }

        stream->render.packets = (DATAPACKET*)PaUtil_GroupAllocateMemory(stream->allocGroup, stream->render.noOfPackets * sizeof(DATAPACKET));
        if (stream->render.packets == NULL)
        {
            result = paInsufficientMemory;
            goto error;
        }

        switch(stream->render.pPin->parentFilter->waveType)
        {
        case Type_kWaveCyclic:
            {
                /* WaveCyclic case */
                unsigned i;
                for (i = 0; i < stream->render.noOfPackets; ++i)
                {
                    /* Set up the packets */
                    DATAPACKET *p = stream->render.packets + i;

                    /* Playback event */
                    stream->render.events[i] = CreateEvent(NULL, FALSE, FALSE, NULL);

                    /* In this case, we just use the packets as ptr to the device buffer */
                    p->Signal.hEvent = stream->render.events[i];
                    p->Header.Data = stream->render.hostBuffer + (i*bufferSizeInBytes);
                    p->Header.FrameExtent = bufferSizeInBytes;
                    p->Header.DataUsed = bufferSizeInBytes;
                    p->Header.Size = sizeof(p->Header);
                    p->Header.PresentationTime.Numerator = 1;
                    p->Header.PresentationTime.Denominator = 1;
                }
            }
            break;
        case Type_kWaveRT:
            {
                /* WaveRT case */

                /* Set up the "packets" */
                DATAPACKET *p = stream->render.packets;

                /* The only playback event */
                stream->render.events[0] = CreateEvent(NULL, FALSE, FALSE, NULL);

                /* In this case, we just use the packets as ptr to the device buffer */
                p->Header.Data = stream->render.hostBuffer;
                p->Header.FrameExtent = stream->render.framesPerBuffer*stream->render.bytesPerFrame;
                p->Header.DataUsed = stream->render.framesPerBuffer*stream->render.bytesPerFrame;
                p->Header.Size = sizeof(p->Header);
                p->Header.PresentationTime.Numerator = 1;
                p->Header.PresentationTime.Denominator = 1;

                ++p;
                p->Header.Data = stream->render.hostBuffer + stream->render.framesPerBuffer*stream->render.bytesPerFrame;
                p->Header.FrameExtent = stream->render.framesPerBuffer*stream->render.bytesPerFrame;
                p->Header.DataUsed = stream->render.framesPerBuffer*stream->render.bytesPerFrame;
                p->Header.Size = sizeof(p->Header);
                p->Header.PresentationTime.Numerator = 1;
                p->Header.PresentationTime.Denominator = 1;

                if (stream->render.pPin->parentFilter->waveSubType == SubType_kNotification)
                {
                    result = PinRegisterNotificationHandle(stream->render.pPin, stream->render.events[0]);

                    if (result != paNoError)
                    {
                        PA_DEBUG(("Failed to register rendering notification handle\n"));
                        PaWinWDM_SetLastErrorInfo(paUnanticipatedHostError, "Failed to register rendering notification handle");
                        result = paUnanticipatedHostError;
                        goto error;
                    }
                }

                result = PinRegisterPositionRegister(stream->render.pPin);

                if (result != paNoError)
                {
                    PA_DEBUG(("Failed to register rendering position register, using PinGetAudioPositionViaIOCTL\n"));
                    stream->render.pPin->fnAudioPosition = PinGetAudioPositionViaIOCTL;
                }
                else
                {
                    stream->render.pPin->fnAudioPosition = PinGetAudioPositionDirect;
                }
            }
            break;
        default:
            /* Undefined wave type!! */
            assert(0);
            result = paInternalError;
            PaWinWDM_SetLastErrorInfo(result, "Wave type %u ??", stream->capture.pPin->parentFilter->waveType);
            goto error;
        }
    }

    stream->streamStarted = 0;
    stream->streamActive = 0;
    stream->streamStop = 0;
    stream->streamAbort = 0;
    stream->streamFlags = streamFlags;
    stream->oldProcessPriority = REALTIME_PRIORITY_CLASS;

    /* Ok, now update our host API specific stream info */
    if (stream->userInputChannels)
    {
        PaWinWdmDeviceInfo *pDeviceInfo = (PaWinWdmDeviceInfo*)wdmHostApi->inheritedHostApiRep.deviceInfos[inputParameters->device];
        copytowidestring(stream->hostApiStreamInfo.input.filterName, stream->capture.pPin->parentFilter->filterName, MAX_PATH);
        if (stream->capture.pPin->parentFilter->topologyFilter != NULL)
        {
            copytowidestring(stream->hostApiStreamInfo.input.topologyName, stream->capture.pPin->parentFilter->topologyFilter->filterName, MAX_PATH);
        }
        stream->hostApiStreamInfo.input.streamingType = stream->capture.pPin->parentFilter->waveType;
        stream->hostApiStreamInfo.input.streamingSubType = stream->capture.pPin->parentFilter->waveSubType;
        stream->hostApiStreamInfo.input.endpointPinId = pDeviceInfo->endpointPinId;
        stream->hostApiStreamInfo.input.muxNodeId = -1;
        stream->hostApiStreamInfo.input.channels = stream->deviceInputChannels;
        if (stream->capture.pPin->inputs)
        {
            stream->hostApiStreamInfo.input.muxNodeId = stream->capture.pPin->inputs[pDeviceInfo->muxPosition]->muxNodeId;
        }
    }
    if (stream->userOutputChannels)
    {
        copytowidestring(stream->hostApiStreamInfo.output.filterName, stream->render.pPin->parentFilter->filterName, MAX_PATH);
        if (stream->render.pPin->parentFilter->topologyFilter != NULL)
        {
            copytowidestring(stream->hostApiStreamInfo.output.topologyName, stream->render.pPin->parentFilter->topologyFilter->filterName, MAX_PATH);
        }
        stream->hostApiStreamInfo.output.streamingType = stream->render.pPin->parentFilter->waveType;
        stream->hostApiStreamInfo.output.streamingSubType = stream->render.pPin->parentFilter->waveSubType;
        stream->hostApiStreamInfo.output.endpointPinId = stream->render.pPin->endpointPinId;
        stream->hostApiStreamInfo.output.muxNodeId = -1;
        stream->hostApiStreamInfo.output.channels = stream->deviceOutputChannels;
    }
    stream->streamRepresentation.streamInfo.hostApiTypeId = paWDMKS;
    stream->streamRepresentation.streamInfo.hostApiSpecificStreamInfo = &stream->hostApiStreamInfo;
    stream->streamRepresentation.streamInfo.structVersion = 2;

    *s = (PaStream*)stream;

    PA_LOGL_;
    return result;

occupied:
    /* Ok, someone else is hogging the pin, bail out */
    assert (result == paDeviceUnavailable);
    PaWinWDM_SetLastErrorInfo(result, "Device is occupied");

error:
    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );

    CloseStreamEvents(stream);

    if (stream->allocGroup)
    {
        PaUtil_FreeAllAllocations(stream->allocGroup);
        PaUtil_DestroyAllocationGroup(stream->allocGroup);
        stream->allocGroup = 0;
    }

    if(stream->render.pPin)
        PinClose(stream->render.pPin);
    if(stream->capture.pPin)
        PinClose(stream->capture.pPin);

    PaUtil_FreeMemory( stream );

    PA_LOGL_;
    return result;
}

/*
When CloseStream() is called, the multi-api layer ensures that
the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaWinWdmStream *stream = (PaWinWdmStream*)s;

    PA_LOGE_;

    assert(!stream->streamStarted);
    assert(!stream->streamActive);

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );

    CloseStreamEvents(stream);

    if (stream->allocGroup)
    {
        PaUtil_FreeAllAllocations(stream->allocGroup);
        PaUtil_DestroyAllocationGroup(stream->allocGroup);
        stream->allocGroup = 0;
    }

    if(stream->render.pPin)
        PinClose(stream->render.pPin);
    if(stream->capture.pPin)
        PinClose(stream->capture.pPin);

    PaUtil_FreeMemory( stream );

    PA_LOGL_;
    return result;
}

/*
Write the supplied packet to the pin
Asynchronous
Should return paNoError on success
*/
static PaError PinWrite(HANDLE h, DATAPACKET* p)
{
    PaError result = paNoError;
    unsigned long cbReturned = 0;
    BOOL fRes = DeviceIoControl(h,
        IOCTL_KS_WRITE_STREAM,
        NULL,
        0,
        &p->Header,
        p->Header.Size,
        &cbReturned,
        &p->Signal);
    if (!fRes)
    {
        unsigned long error = GetLastError();
        if (error != ERROR_IO_PENDING)
        {
            result = paInternalError;
        }
    }
    return result;
}

/*
Read to the supplied packet from the pin
Asynchronous
Should return paNoError on success
*/
static PaError PinRead(HANDLE h, DATAPACKET* p)
{
    PaError result = paNoError;
    unsigned long cbReturned = 0;
    BOOL fRes = DeviceIoControl(h,
        IOCTL_KS_READ_STREAM,
        NULL,
        0,
        &p->Header,
        p->Header.Size,
        &cbReturned,
        &p->Signal);
    if (!fRes)
    {
        unsigned long error = GetLastError();
        if (error != ERROR_IO_PENDING)
        {
            result = paInternalError;
        }
    }
    return result;
}

/*
Copy the first interleaved channel of 16 bit data to the other channels
*/
static void DuplicateFirstChannelInt16(void* buffer, int channels, int samples)
{
    unsigned short* data = (unsigned short*)buffer;
    int channel;
    unsigned short sourceSample;
    while( samples-- )
    {
        sourceSample = *data++;
        channel = channels-1;
        while( channel-- )
        {
            *data++ = sourceSample;
        }
    }
}

/*
Copy the first interleaved channel of 24 bit data to the other channels
*/
static void DuplicateFirstChannelInt24(void* buffer, int channels, int samples)
{
    unsigned char* data = (unsigned char*)buffer;
    int channel;
    unsigned char sourceSample[3];
    while( samples-- )
    {
        sourceSample[0] = data[0];
        sourceSample[1] = data[1];
        sourceSample[2] = data[2];
        data += 3;
        channel = channels-1;
        while( channel-- )
        {
            data[0] = sourceSample[0];
            data[1] = sourceSample[1];
            data[2] = sourceSample[2];
            data += 3;
        }
    }
}

/*
Copy the first interleaved channel of 32 bit data to the other channels
*/
static void DuplicateFirstChannelInt32(void* buffer, int channels, int samples)
{
    unsigned long* data = (unsigned long*)buffer;
    int channel;
    unsigned long sourceSample;
    while( samples-- )
    {
        sourceSample = *data++;
        channel = channels-1;
        while( channel-- )
        {
            *data++ = sourceSample;
        }
    }
}

/*
Increase the priority of the calling thread to RT 
*/
static HANDLE BumpThreadPriority() 
{
    HANDLE hThread = GetCurrentThread();
    DWORD dwTask = 0;
    HANDLE hAVRT = NULL;

    /* If we have access to AVRT.DLL (Vista and later), use it */
    if (FunctionAvSetMmThreadCharacteristics != NULL) 
    {
        hAVRT = FunctionAvSetMmThreadCharacteristics("Pro Audio", &dwTask);
        if (hAVRT != NULL && hAVRT != INVALID_HANDLE_VALUE) 
        {
            BOOL bret = FunctionAvSetMmThreadPriority(hAVRT, PA_AVRT_PRIORITY_CRITICAL);
            if (!bret)
            {
                PA_DEBUG(("Set mm thread prio to critical failed!\n"));
            }
            else
            {
            return hAVRT;
        }
        }
        else
        {
            PA_DEBUG(("Set mm thread characteristic to 'Pro Audio' failed, reverting to SetThreadPriority\n"));
        }
    }

    /* For XP and earlier, or if AvSetMmThreadCharacteristics fails (MMCSS disabled ?) */
    if (timeBeginPeriod(1) != TIMERR_NOERROR) {
        PA_DEBUG(("timeBeginPeriod(1) failed!\n"));
    }

    if (!SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL)) {
        PA_DEBUG(("SetThreadPriority failed!\n"));
    }

    return hAVRT;
}

/*
Decrease the priority of the calling thread to normal
*/
static void DropThreadPriority(HANDLE hAVRT)
{
    HANDLE hThread = GetCurrentThread();

    if (hAVRT != NULL) 
    {
        FunctionAvSetMmThreadPriority(hAVRT, PA_AVRT_PRIORITY_NORMAL);
        FunctionAvRevertMmThreadCharacteristics(hAVRT);
        return;
    }

        SetThreadPriority(hThread, THREAD_PRIORITY_NORMAL);
        timeEndPeriod(1);
    }

static PaError PreparePinForStart(PaWinWdmPin* pin)
{
    PaError result;
    result = PinSetState(pin, KSSTATE_ACQUIRE);
    if (result != paNoError)
    {
        goto error;
    }
    result = PinSetState(pin, KSSTATE_PAUSE);
    if (result != paNoError)
    {
        goto error;
    }
    return result;

error:
    PinSetState(pin, KSSTATE_STOP);
    return result;
}

static PaError PreparePinsForStart(PaProcessThreadInfo* pInfo)
{
    PaError result = paNoError;
    /* Submit buffers */
    if (pInfo->stream->capture.pPin)
    {
        if ((result = PreparePinForStart(pInfo->stream->capture.pPin)) != paNoError)
        {
            goto error;
        }

        if (pInfo->stream->capture.pPin->parentFilter->waveType == Type_kWaveCyclic)
        {
            unsigned i;
            for(i=0; i < pInfo->stream->capture.noOfPackets; ++i)
    {
                if ((result = PinRead(pInfo->stream->capture.pPin->handle, pInfo->stream->capture.packets + i)) != paNoError)
        {
            goto error;
        }
                /* Reset the events as some devices SET the event on PinRead */
                ResetEvent(pInfo->stream->capture.packets[i].Signal.hEvent);
                ++pInfo->pending;
    }
            }
        else
            {
            pInfo->pending = 2;
            }
        }

    if(pInfo->stream->render.pPin)
    {
        if ((result = PreparePinForStart(pInfo->stream->render.pPin)) != paNoError)
        {
            goto error;
        }

        pInfo->priming += pInfo->stream->render.noOfPackets;
        ++pInfo->pending;
        SetEvent(pInfo->stream->render.events[0]);
        if (pInfo->stream->render.pPin->parentFilter->waveType == Type_kWaveCyclic) 
        {
            unsigned i;
            for(i=1; i < pInfo->stream->render.noOfPackets; ++i)
            {
                SetEvent(pInfo->stream->render.events[i]);
            ++pInfo->pending;
        }
    }
    }

error:
    PA_DEBUG(("PreparePinsForStart = %d\n", result));
    return result;
}

static PaError StartPin(PaWinWdmPin* pin)
{
    return PinSetState(pin, KSSTATE_RUN);
}

static PaError StartPins(PaProcessThreadInfo* pInfo)
{
    PaError result = paNoError;
    /* Start the pins as synced as possible */
    if (pInfo->stream->capture.pPin)
    {
        result = StartPin(pInfo->stream->capture.pPin);
    }
    if(pInfo->stream->render.pPin)
    {
        result = StartPin(pInfo->stream->render.pPin);
    }
    PA_DEBUG(("StartPins = %d\n", result));
    return result;
}


static PaError StopPin(PaWinWdmPin* pin)
{
    PinSetState(pin, KSSTATE_PAUSE);
    PinSetState(pin, KSSTATE_STOP);
    return paNoError;
}


static PaError StopPins(PaProcessThreadInfo* pInfo)
{
    PaError result = paNoError;
    if(pInfo->stream->render.pPin)
    {
        StopPin(pInfo->stream->render.pPin);
    }
    if(pInfo->stream->capture.pPin)
    {
        StopPin(pInfo->stream->capture.pPin);
    }
    return result;
}

typedef void (*TSetInputFrameCount)(PaUtilBufferProcessor*, unsigned long);
typedef void (*TSetInputChannel)(PaUtilBufferProcessor*, unsigned int, void *, unsigned int);
static const TSetInputFrameCount fnSetInputFrameCount[2] = { PaUtil_SetInputFrameCount, PaUtil_Set2ndInputFrameCount };
static const TSetInputChannel fnSetInputChannel[2] = { PaUtil_SetInputChannel, PaUtil_Set2ndInputChannel };

static PaError PaDoProcessing(PaProcessThreadInfo* pInfo)
{
    PaError result = paNoError;
    int i, framesProcessed = 0, doChannelCopy = 0;
    ring_buffer_size_t inputFramesAvailable = PaUtil_GetRingBufferReadAvailable(&pInfo->stream->ringBuffer);

    /* Do necessary buffer processing (which will invoke user callback if necessary) */
    if (pInfo->cbResult == paContinue &&
        (pInfo->renderHead != pInfo->renderTail || inputFramesAvailable))
    {
        unsigned processFullDuplex = pInfo->stream->capture.pPin && pInfo->stream->render.pPin && (!pInfo->priming);

        PA_HP_TRACE((pInfo->stream->hLog, "DoProcessing: InputFrames=%u", inputFramesAvailable));

        PaUtil_BeginCpuLoadMeasurement( &pInfo->stream->cpuLoadMeasurer );

        pInfo->ti.currentTime = PaUtil_GetTime();

        PaUtil_BeginBufferProcessing(&pInfo->stream->bufferProcessor, &pInfo->ti, pInfo->underover);
        pInfo->underover = 0; /* Reset the (under|over)flow status */

        if (pInfo->renderTail != pInfo->renderHead)
        {
            DATAPACKET* packet = pInfo->renderPackets[pInfo->renderTail & cPacketsArrayMask].packet;

            assert(packet != 0);
            assert(packet->Header.Data != 0);

            PaUtil_SetOutputFrameCount(&pInfo->stream->bufferProcessor, pInfo->stream->render.framesPerBuffer);

            for(i=0;i<pInfo->stream->userOutputChannels;i++)
            {
                /* Only write the user output channels. Leave the rest blank */
                PaUtil_SetOutputChannel(&pInfo->stream->bufferProcessor,
                    i,
                    ((unsigned char*)(packet->Header.Data))+(i*pInfo->stream->render.bytesPerSample),
                    pInfo->stream->deviceOutputChannels);
            }

            /* We will do a copy to the other channels after the data has been written */
            doChannelCopy = ( pInfo->stream->userOutputChannels == 1 );
        }

        if (inputFramesAvailable && (!pInfo->stream->userOutputChannels || inputFramesAvailable >= (int)pInfo->stream->render.framesPerBuffer))
        {
            unsigned wrapCntr = 0;
            char* data[2] = {0,0};
            ring_buffer_size_t size[2] = {0,0};

            /* If full-duplex, we just extract output buffer number of frames */
            if (pInfo->stream->userOutputChannels)
            {
                inputFramesAvailable = min(inputFramesAvailable, (int)pInfo->stream->render.framesPerBuffer);
            }

            inputFramesAvailable = PaUtil_GetRingBufferReadRegions(&pInfo->stream->ringBuffer,
                inputFramesAvailable,
                &data[0],
                &size[0],
                &data[1],
                &size[1]);

            for (wrapCntr = 0; wrapCntr < 2; ++wrapCntr)
            {
                if (size[wrapCntr] == 0)
                    break;

                fnSetInputFrameCount[wrapCntr](&pInfo->stream->bufferProcessor, size[wrapCntr]);
                for(i=0;i<pInfo->stream->userInputChannels;i++)
                {
                    /* Only read as many channels as the user wants */
                    fnSetInputChannel[wrapCntr](&pInfo->stream->bufferProcessor,
                        i,
                        ((unsigned char*)(data[wrapCntr]))+(i*pInfo->stream->capture.bytesPerSample),
                        pInfo->stream->deviceInputChannels);
                }
            }
#if 0
            if (pInfo->stream->userOutputChannels && pInfo->stream->capture.framesPerBuffer <= pInfo->stream->render.framesPerBuffer)
            {
                ring_buffer_size_t n = PaUtil_GetRingBufferReadAvailable(&pInfo->stream->ringBuffer);
                if (n - inputFramesAvailable > 0)
                {
                    PA_HP_TRACE((pInfo->stream->hLog, "Synchronizing input buffer (read advance = %d) ", n - inputFramesAvailable));
                    PaUtil_AdvanceRingBufferReadIndex(&pInfo->stream->ringBuffer, n - inputFramesAvailable);
                }
            }
#endif
        }
        else
        {
            /* We haven't consumed anything from the ring buffer... */
            inputFramesAvailable = 0;
            /* If we have full-duplex, this is at startup, so mark no-input! */
            if (pInfo->stream->userOutputChannels>0 && pInfo->stream->userInputChannels>0)
            {
                PA_HP_TRACE((pInfo->stream->hLog, "Input startup, marking no input."));
                PaUtil_SetNoInput(&pInfo->stream->bufferProcessor);
            }
        }

        if (processFullDuplex) /* full duplex */
        {
            /* Only call the EndBufferProcessing function when the total input frames == total output frames */
            const unsigned long totalInputFrameCount = pInfo->stream->bufferProcessor.hostInputFrameCount[0] + pInfo->stream->bufferProcessor.hostInputFrameCount[1];
            const unsigned long totalOutputFrameCount = pInfo->stream->bufferProcessor.hostOutputFrameCount[0] + pInfo->stream->bufferProcessor.hostOutputFrameCount[1];

            if(totalInputFrameCount == totalOutputFrameCount && totalOutputFrameCount != 0)
            {
                framesProcessed = PaUtil_EndBufferProcessing(&pInfo->stream->bufferProcessor, &pInfo->cbResult);
            }
            else
            {
                framesProcessed = 0;
            }
        }
        else 
        {
            framesProcessed = PaUtil_EndBufferProcessing(&pInfo->stream->bufferProcessor, &pInfo->cbResult);
        }

        PA_HP_TRACE((pInfo->stream->hLog, "Frames processed: %u %s", framesProcessed, (pInfo->priming ? "(priming)":"")));

        if( doChannelCopy )
        {
            DATAPACKET* packet = pInfo->renderPackets[pInfo->renderTail & cPacketsArrayMask].packet;
            /* Copy the first output channel to the other channels */
            switch (pInfo->stream->render.bytesPerSample)
            {
            case 2:
                DuplicateFirstChannelInt16(packet->Header.Data, pInfo->stream->deviceOutputChannels, pInfo->stream->render.framesPerBuffer);
                break;
            case 3:
                DuplicateFirstChannelInt24(packet->Header.Data, pInfo->stream->deviceOutputChannels, pInfo->stream->render.framesPerBuffer);
                break;
            case 4:
                DuplicateFirstChannelInt32(packet->Header.Data, pInfo->stream->deviceOutputChannels, pInfo->stream->render.framesPerBuffer);
                break;
            default:
                assert(0); /* Unsupported format! */
                break;
            }
        }
        PaUtil_EndCpuLoadMeasurement( &pInfo->stream->cpuLoadMeasurer, framesProcessed );

        if (inputFramesAvailable)
        {
            PaUtil_AdvanceRingBufferReadIndex(&pInfo->stream->ringBuffer, inputFramesAvailable);
        }

        if (pInfo->renderTail != pInfo->renderHead)
        {
            if (!pInfo->stream->streamStop)
            {
                result = pInfo->stream->render.pPin->fnSubmitHandler(pInfo, pInfo->renderTail);
                if (result != paNoError)
                {
                    PA_HP_TRACE((pInfo->stream->hLog, "Capture submit handler failed with result %d", result));
                    return result;
                }
            }
            pInfo->renderTail++;
            if (!pInfo->pinsStarted && pInfo->priming == 0)
            {
                /* We start the pins here to allow "prime time" */
                if ((result = StartPins(pInfo)) == paNoError)
                {
                    PA_HP_TRACE((pInfo->stream->hLog, "Starting pins!"));
                    pInfo->pinsStarted = 1;
                }
            }
        }
    }

    return result;
}

static VOID CALLBACK TimerCallbackWaveRTPolledMode(
    PVOID lpParameter,
    BOOLEAN TimerOrWaitFired
    )
{
    HANDLE* pHandles = (HANDLE*)lpParameter;
    if (pHandles[0]) SetEvent(pHandles[0]);
    if (pHandles[1]) SetEvent(pHandles[1]);
}

#pragma comment( lib, "ws2_32.lib" )


int usleep(long usec)
{
    struct timeval tv;
    fd_set dummy;
    SOCKET s;
    static BOOLEAN bInit = FALSE;
    if (!bInit)
    {
        WORD wVersionRequested = MAKEWORD(1,0);
        WSADATA wsaData;
        WSAStartup(wVersionRequested, &wsaData);
        bInit = TRUE;
    }

    s = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    FD_ZERO(&dummy);
    FD_SET(s, &dummy);
    tv.tv_sec = usec/1000000L;
    tv.tv_usec = usec%1000000L;
    return select(0, 0, 0, &dummy, &tv);
}

PA_THREAD_FUNC ProcessingThread(void* pParam)
{
    PaError result = paNoError;
    HANDLE hAVRT = NULL;
    HANDLE *handleArray = NULL;
    HANDLE timerEventHandles[2];
    HANDLE timerQueue = NULL;
    HANDLE timerQueueTimer = NULL;
    unsigned noOfHandles = 0;
    unsigned captureEvents = 0;
    unsigned renderEvents = 0;

    PaProcessThreadInfo info;
    memset(&info, 0, sizeof(PaProcessThreadInfo));
    info.stream = (PaWinWdmStream*)pParam;

    info.stream->threadResult = paNoError;

    PA_LOGE_;

    info.ti.inputBufferAdcTime = 0.0;
    info.ti.currentTime = 0.0;
    info.ti.outputBufferDacTime = 0.0;

    PA_DEBUG(("In  buffer len: %.3f ms\n",(2000*info.stream->capture.framesPerBuffer) / info.stream->streamRepresentation.streamInfo.sampleRate));
    PA_DEBUG(("Out buffer len: %.3f ms\n",(2000*info.stream->render.framesPerBuffer) / info.stream->streamRepresentation.streamInfo.sampleRate));
    info.timeout = (DWORD)max(
        (2000*info.stream->render.framesPerBuffer/info.stream->streamRepresentation.streamInfo.sampleRate + 0.5),
        (2000*info.stream->capture.framesPerBuffer/info.stream->streamRepresentation.streamInfo.sampleRate + 0.5));
    info.timeout = (4 * max(info.timeout+1,1));
    PA_DEBUG(("Timeout = %ld ms\n",info.timeout));

    /* Allocate handle array */
    handleArray = (HANDLE*)PaUtil_AllocateMemory((info.stream->capture.noOfPackets + info.stream->render.noOfPackets + 1) * sizeof(HANDLE));

    /* Setup handle array for WFMO */
    if (info.stream->capture.pPin != 0)
    {
        handleArray[noOfHandles++] = info.stream->capture.events[0];
        if (info.stream->capture.pPin->parentFilter->waveType == Type_kWaveCyclic)
        {
            unsigned i;
            for(i=1; i < info.stream->capture.noOfPackets; ++i)
            {
                handleArray[noOfHandles++] = info.stream->capture.events[i];
            }
        }
        captureEvents = noOfHandles;
        renderEvents = noOfHandles;
    }

    if (info.stream->render.pPin != 0)
    {
        handleArray[noOfHandles++] = info.stream->render.events[0];
        if (info.stream->render.pPin->parentFilter->waveType == Type_kWaveCyclic)
        {
            unsigned i;
            for(i=1; i < info.stream->render.noOfPackets; ++i)
            {
                handleArray[noOfHandles++] = info.stream->render.events[i];
            }
        }
        renderEvents = noOfHandles;
    }
    handleArray[noOfHandles++] = info.stream->eventAbort;
    assert(noOfHandles <= (info.stream->capture.noOfPackets + info.stream->render.noOfPackets + 1));

    /* Prepare render and capture pins */
    if ((result = PreparePinsForStart(&info)) != paNoError) 
    {
        PA_DEBUG(("Failed to prepare device(s)!\n"));
        goto error;
    }

    /* Init high speed logger */
    if (PaUtil_InitializeHighSpeedLog(&info.stream->hLog, 1000000) != paNoError)
    {
        PA_DEBUG(("Failed to init high speed logger!\n"));
        goto error;
    }

    /* Heighten priority here */
    hAVRT = BumpThreadPriority();

    /* If input only, we start the pins immediately */
    if (info.stream->render.pPin == 0)
    {
        if ((result = StartPins(&info)) != paNoError)
        {
            PA_DEBUG(("Failed to start device(s)!\n"));
            goto error;
        }
        info.pinsStarted = 1;
    }

    /* Handle timer for WaveRT polled mode */
    {
        const unsigned fs = (unsigned)info.stream->streamRepresentation.streamInfo.sampleRate;
        unsigned timerPeriod = (unsigned)-1;
        if (info.stream->capture.pPin != 0 && info.stream->capture.pPin->parentFilter->waveSubType == SubType_kPolled)
        {
            timerEventHandles[0] = info.stream->capture.events[0];
            timerPeriod = min(timerPeriod, (1000*info.stream->capture.framesPerBuffer)/fs);
        }
        else
        {
            timerEventHandles[0] = 0;
        }
        if (info.stream->render.pPin != 0 && info.stream->render.pPin->parentFilter->waveSubType == SubType_kPolled)
        {
            timerEventHandles[1] = info.stream->render.events[0];
            timerPeriod = min(timerPeriod, (1000*info.stream->render.framesPerBuffer)/fs);
        }
        else
        {
            timerEventHandles[1] = 0;
        }
        if (timerEventHandles[0] || timerEventHandles[1])
        {
            timerQueue = CreateTimerQueue();
            if (timerQueue==NULL)
            {
                PA_DEBUG(("CreateTimerQueue failed!\n"));
                result = paUnanticipatedHostError;
                PaWinWDM_SetLastErrorInfo(result, "CreateTimerQueue failed", timerPeriod);
                goto error;
            }

            timerPeriod=max(timerPeriod/5,1);

            PA_HP_TRACE((info.stream->hLog, "Timer event handles=0x%04X,0x%04X period=%u ms", timerEventHandles[0], timerEventHandles[1], timerPeriod));

            if (!CreateTimerQueueTimer(&timerQueueTimer, timerQueue, TimerCallbackWaveRTPolledMode, timerEventHandles, timerPeriod, timerPeriod, WT_EXECUTEINTIMERTHREAD /* WT_EXECUTEINPERSISTENTTHREAD*/))
            {
                PA_DEBUG(("CreateTimerQueueTimer failed!? (period=%u)\n", timerPeriod));
                result = paUnanticipatedHostError;
                PaWinWDM_SetLastErrorInfo(result, "CreateTimerQueueTimer failed (period=%u)", timerPeriod);
                goto error;
            }
        }
    }

    /* Up and running... */
    SetEvent(info.stream->eventStreamStart[StreamStart_kOk]);

    while(!info.stream->streamAbort)
    {
        unsigned doProcessing = 1;
        unsigned wait = WaitForMultipleObjects(noOfHandles, handleArray, FALSE, 0);
        unsigned eventSignalled = wait - WAIT_OBJECT_0;

        if (wait == WAIT_FAILED) 
        {
            PA_HP_TRACE((info.stream->hLog, "Wait failed = %ld! \n",wait));
            break;
        }

        if (wait == WAIT_TIMEOUT)
        {
            wait = WaitForMultipleObjects(noOfHandles, handleArray, FALSE, info.timeout);
            eventSignalled = wait - WAIT_OBJECT_0;
        }
        else
        {
            if (eventSignalled < captureEvents)
            {
                if (PaUtil_GetRingBufferWriteAvailable(&info.stream->ringBuffer) == 0)
                {
                    PA_HP_TRACE((info.stream->hLog, "!!!!! Input overflow !!!!!"));
                    info.underover |= paInputOverflow;
                }
            }
            else if (eventSignalled < renderEvents) {
                if (!info.priming && info.renderHead - info.renderTail > 1)
                {
                    PA_HP_TRACE((info.stream->hLog, "!!!!! Output underflow !!!!!"));
                    info.underover |= paOutputUnderflow;
                }
            }

        }

        if (wait == WAIT_TIMEOUT)
        {
            if (info.stream->disableTimeout)
            {
                PA_HP_TRACE((info.stream->hLog, "WFMO(2) signalled timeout (to=%u) NO EXIT!", info.timeout));
            }
            else
            {
            PA_HP_TRACE((info.stream->hLog, "WFMO(2) signalled timeout (to=%u)", info.timeout));
            result = paUnanticipatedHostError;
            PaWinWDM_SetLastErrorInfo(result, "WFMO(2) signalled timeout (to=%u)", info.timeout);
            break;
        }
        }
        else
        {
            if (eventSignalled < captureEvents)
            {
                info.stream->capture.pPin->fnEventHandler(&info, eventSignalled);
                /* Since we use the ring buffer, we can submit the buffers directly */
                if (!info.stream->streamStop)
                {
                    result = info.stream->capture.pPin->fnSubmitHandler(&info, info.captureTail);
                    if (result != paNoError)
                    {
                        PA_HP_TRACE((info.stream->hLog, "Capture submit handler failed with result %d", result));
                        break;
                    }
                }
                ++info.captureTail;
                /* If full-duplex, let _only_ render event trigger processing. We still need the stream stop
                handling working, so let that be processed anyways... */
                if (info.stream->userOutputChannels > 0)
                {
                    doProcessing = 0;
                }
            }
            else if (eventSignalled < renderEvents)
            {
                eventSignalled -= captureEvents;
                info.stream->render.pPin->fnEventHandler(&info, eventSignalled);
            }
            else {
                assert(info.stream->streamAbort);
                PA_HP_TRACE((info.stream->hLog, "Stream abort!"));
                continue;
            }
        }

        /* Handle processing */
        if (doProcessing)
        {
            result = PaDoProcessing(&info);
            if (result != paNoError)
            {
            PA_HP_TRACE((info.stream->hLog, "PaDoProcessing failed!"));
            break;
        }
        }

        if(info.stream->streamStop && info.cbResult != paComplete)
        {
            PA_HP_TRACE((info.stream->hLog, "Stream stop! pending=%d",info.pending));
            info.cbResult = paComplete; /* Stop, but play remaining buffers */
        }

        if(info.pending<=0)
        {
            PA_HP_TRACE((info.stream->hLog, "pending==0 finished..."));
            break;
        }
        if((!info.stream->render.pPin)&&(info.cbResult!=paContinue))
        {
            PA_HP_TRACE((info.stream->hLog, "record only cbResult=%d...",info.cbResult));
            break;
        }
    }

    PA_DEBUG(("Finished processing loop\n"));

    goto bailout;

error:
    PA_DEBUG(("Error starting processing thread\n"));
    /* Set the "error" event together with result */
    info.stream->threadResult = result;
    SetEvent(info.stream->eventStreamStart[StreamStart_kFailed]);

bailout:
    if (timerQueue != NULL)
    {
        DeleteTimerQueue(timerQueue);
        timerQueue = 0;
    }

    if (info.pinsStarted)
    {
        StopPins(&info);
    }

    /* Lower prio here */
    DropThreadPriority(hAVRT);

    if (handleArray != NULL)
    {
        PaUtil_FreeMemory(handleArray);
    }

#if PA_TRACE_REALTIME_EVENTS
    if (info.stream->hLog)
    {
        PaUtil_DumpHighSpeedLog(info.stream->hLog, "hp_trace.log");
        PaUtil_DiscardHighSpeedLog(info.stream->hLog);
        info.stream->hLog = 0;
    }
#endif
    info.stream->streamActive = 0;

    if((!info.stream->streamStop)&&(!info.stream->streamAbort))
    {
        /* Invoke the user stream finished callback */
        /* Only do it from here if not being stopped/aborted by user */
        if( info.stream->streamRepresentation.streamFinishedCallback != 0 )
            info.stream->streamRepresentation.streamFinishedCallback( info.stream->streamRepresentation.userData );
    }
    info.stream->streamStop = 0;
    info.stream->streamAbort = 0;

    PA_LOGL_;
    return 0;
}


static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    DWORD dwID;

    PA_LOGE_;

    stream->streamStop = 0;
    stream->streamAbort = 0;

    ResetStreamEvents(stream);

    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    stream->oldProcessPriority = GetPriorityClass(GetCurrentProcess());
    /* Uncomment the following line to enable dynamic boosting of the process
    * priority to real time for best low latency support
    * Disabled by default because RT processes can easily block the OS */
    /*ret = SetPriorityClass(GetCurrentProcess(),REALTIME_PRIORITY_CLASS);
    PA_DEBUG(("Class ret = %d;",ret));*/

    stream->streamThread = CREATE_THREAD_FUNCTION (NULL, 0, ProcessingThread, stream, CREATE_SUSPENDED, &dwID);
    if(stream->streamThread == NULL)
    {
        result = paInsufficientMemory;
        goto end;
    }
    ResumeThread(stream->streamThread);

    switch (WaitForMultipleObjects(2, stream->eventStreamStart, FALSE, 5000))
    {
    case WAIT_OBJECT_0 + StreamStart_kOk:
        PA_DEBUG(("Processing thread started!\n"));
        result = paNoError;
        stream->streamStarted = 1;
        stream->streamActive = 1;
        break;
    case WAIT_OBJECT_0 + StreamStart_kFailed:
        PA_DEBUG(("Processing thread start failed! (result=%d)\n", stream->threadResult));
        result = stream->threadResult;
        break;
    case WAIT_TIMEOUT:
    default:
        result = paTimedOut;
        PaWinWDM_SetLastErrorInfo(result, "Failed to start processing thread (timeout)!");
        break;
    }

end:
    PA_LOGL_;
    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    int doCb = 0;

    PA_LOGE_;

    if(stream->streamActive)
    {
        DWORD dwExitCode;
        doCb = 1;
        stream->streamStop = 1;
        if (GetExitCodeThread(stream->streamThread, &dwExitCode) && dwExitCode == STILL_ACTIVE)
        {
        if (WaitForSingleObject(stream->streamThread, INFINITE) != WAIT_OBJECT_0)
        {
            PA_DEBUG(("StopStream: stream thread terminated\n"));
            TerminateThread(stream->streamThread, -1);
            result = paTimedOut;
        }
    }
    }

    CloseHandle(stream->streamThread);
    stream->streamThread = 0;
    stream->streamStarted = 0;

    if(doCb)
    {
        /* Do user callback now after all state has been reset */
        /* This means it should be safe for the called function */
        /* to invoke e.g. StartStream */
        if( stream->streamRepresentation.streamFinishedCallback != 0 )
            stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
    }

    PA_LOGL_;
    return result;
}

static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    int doCb = 0;

    PA_LOGE_;

    if(stream->streamActive)
    {
        doCb = 1;
        stream->streamAbort = 1;
        SetEvent(stream->eventAbort); /* Signal immediately */
        if (WaitForSingleObject(stream->streamThread, 10000) != WAIT_OBJECT_0)
        {
            PA_DEBUG(("AbortStream: stream thread terminated\n"));
            TerminateThread(stream->streamThread, -1);
            result = paTimedOut;
        }
        assert(!stream->streamActive);
    }
    CloseHandle(stream->streamThread);
    stream->streamThread = NULL;
    stream->streamStarted = 0;

    if(doCb)
    {
        /* Do user callback now after all state has been reset */
        /* This means it should be safe for the called function */
        /* to invoke e.g. StartStream */
        if( stream->streamRepresentation.streamFinishedCallback != 0 )
            stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
    }

    stream->streamActive = 0;
    stream->streamStarted = 0;

    PA_LOGL_;
    return result;
}


static PaError IsStreamStopped( PaStream *s )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    int result = 0;

    PA_LOGE_;

    if(!stream->streamStarted)
        result = 1;

    PA_LOGL_;
    return result;
}


static PaError IsStreamActive( PaStream *s )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    int result = 0;

    PA_LOGE_;

    if(stream->streamActive)
        result = 1;

    PA_LOGL_;
    return result;
}


static PaTime GetStreamTime( PaStream* s )
{
    PA_LOGE_;
    PA_LOGL_;
    (void)s;
    return PaUtil_GetTime();
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    double result;
    PA_LOGE_;
    result = PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
    PA_LOGL_;
    return result;
}


/*
As separate stream interfaces are used for blocking and callback
streams, the following functions can be guaranteed to only be called
for blocking streams.
*/

static PaError ReadStream( PaStream* s,
                          void *buffer,
                          unsigned long frames )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;

    PA_LOGE_;

    /* suppress unused variable warnings */
    (void) buffer;
    (void) frames;
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    PA_LOGL_;
    return paInternalError;
}


static PaError WriteStream( PaStream* s,
                           const void *buffer,
                           unsigned long frames )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;

    PA_LOGE_;

    /* suppress unused variable warnings */
    (void) buffer;
    (void) frames;
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    PA_LOGL_;
    return paInternalError;
}


static signed long GetStreamReadAvailable( PaStream* s )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;

    PA_LOGE_;

    /* suppress unused variable warnings */
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    PA_LOGL_;
    return 0;
}


static signed long GetStreamWriteAvailable( PaStream* s )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;

    PA_LOGE_;
    /* suppress unused variable warnings */
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    PA_LOGL_;
    return 0;
}

/***************************************************************************************/
/* Event and submit handlers for WaveCyclic                                            */
/***************************************************************************************/

static PaError PaPinCaptureEventHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    ring_buffer_size_t frameCount;
    DATAPACKET* packet = pInfo->stream->capture.packets + eventIndex;

    assert( eventIndex < pInfo->stream->capture.noOfPackets );

    pInfo->capturePackets[pInfo->captureHead & cPacketsArrayMask].packet = packet;

    frameCount = PaUtil_WriteRingBuffer(&pInfo->stream->ringBuffer, packet->Header.Data, pInfo->stream->capture.framesPerBuffer);

    PA_HP_TRACE((pInfo->stream->hLog, ">>> Capture event: idx=%u (frames=%u)", eventIndex, frameCount));
    ++pInfo->captureHead;
    --pInfo->pending;
    return paNoError;
}

static PaError PaPinCaptureSubmitHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    PaError result = paNoError;
    DATAPACKET* packet = pInfo->capturePackets[pInfo->captureTail & cPacketsArrayMask].packet;
    pInfo->capturePackets[pInfo->captureTail & cPacketsArrayMask].packet = 0;
    assert(packet != 0);
    PA_HP_TRACE((pInfo->stream->hLog, "Capture submit: %u", eventIndex));
    packet->Header.DataUsed = 0; /* Reset for reuse */
    result = PinRead(pInfo->stream->capture.pPin->handle, packet);
    /* Reset event, as some drivers might not do so (or even set the event!!), resulting in a 100% CPU capture loop */
    ResetEvent(packet->Signal.hEvent);
    ++pInfo->pending;
    return result;
}

static PaError PaPinRenderEventHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    assert( eventIndex < pInfo->stream->render.noOfPackets );

    pInfo->renderPackets[pInfo->renderHead & cPacketsArrayMask].packet = pInfo->stream->render.packets + eventIndex;
    PA_HP_TRACE((pInfo->stream->hLog, "<<< Render event : idx=%u head=%u", eventIndex, pInfo->renderHead));
    ++pInfo->renderHead;
    --pInfo->pending;
    return paNoError;
}

static PaError PaPinRenderSubmitHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    PaError result = paNoError;
    DATAPACKET* packet = pInfo->renderPackets[pInfo->renderTail & cPacketsArrayMask].packet;
    pInfo->renderPackets[pInfo->renderTail & cPacketsArrayMask].packet = 0;
    assert(packet != 0);

    PA_HP_TRACE((pInfo->stream->hLog, "Render submit : %u idx=%u", pInfo->renderTail, (unsigned)(packet - pInfo->stream->render.packets)));
    result = PinWrite(pInfo->stream->render.pPin->handle, packet);
    /* Reset event, just in case we have an analogous situation to capture (see PaPinCaptureSubmitHandler_WaveCyclic) */
    ResetEvent(packet->Signal.hEvent);
    ++pInfo->pending;
    if (pInfo->priming)
    {
        --pInfo->priming;
    }
    return result;
}

/***************************************************************************************/
/* Event and submit handlers for WaveRT                                                */
/***************************************************************************************/

static PaError PaPinCaptureEventHandler_WaveRTEvent(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    unsigned long pos;
    unsigned realInBuf;
    unsigned frameCount;
    PaWinWdmIOInfo* pCapture = &pInfo->stream->capture;
    const unsigned halfInputBuffer = pCapture->hostBufferSize >> 1;
    PaWinWdmPin* pin = pCapture->pPin;
    DATAPACKET* packet = 0;

    /* Get hold of current ADC position */
    pin->fnAudioPosition(pin, &pos);
    /* Wrap it (robi: why not use hw latency compensation here ?? because pos then gets _way_ off from
    where it should be, i.e. at beginning or half buffer position. Why? No idea.)  */

    pos %= pCapture->hostBufferSize;
    /* Then realInBuf will point to "other" half of double buffer */
    realInBuf = pos < halfInputBuffer ? 1U : 0U;

    packet = pInfo->stream->capture.packets + realInBuf;

    /* Call barrier (or dummy) */
    pin->fnMemBarrier();

    /* Put it in queue */
    frameCount = PaUtil_WriteRingBuffer(&pInfo->stream->ringBuffer, packet->Header.Data, pCapture->framesPerBuffer);

    pInfo->capturePackets[pInfo->captureHead & cPacketsArrayMask].packet = packet;

    PA_HP_TRACE((pInfo->stream->hLog, "Capture event (WaveRT): idx=%u head=%u (pos = %4.1lf%%, frames=%u)", realInBuf, pInfo->captureHead, (pos * 100.0 / pCapture->hostBufferSize), frameCount));

    ++pInfo->captureHead;
    --pInfo->pending;

    return paNoError;
}

static PaError PaPinCaptureEventHandler_WaveRTPolled(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    unsigned long pos;
    unsigned bytesToRead;
    PaWinWdmIOInfo* pCapture = &pInfo->stream->capture;
    const unsigned halfInputBuffer = pCapture->hostBufferSize>>1;
    PaWinWdmPin* pin = pInfo->stream->capture.pPin;

    /* Get hold of current ADC position */
    pin->fnAudioPosition(pin, &pos);
    /* Wrap it (robi: why not use hw latency compensation here ?? because pos then gets _way_ off from
    where it should be, i.e. at beginning or half buffer position. Why? No idea.)  */
    /* Compensate for HW FIFO to get to last read buffer position */
    pos += pin->hwLatency;
    pos %= pCapture->hostBufferSize;
    /* Need to align position on frame boundary */
    pos &= ~(pCapture->bytesPerFrame - 1);

    /* Call barrier (or dummy) */
    pin->fnMemBarrier();

    /* Put it in "queue" */
    bytesToRead = (pCapture->hostBufferSize + pos - pCapture->lastPosition) % pCapture->hostBufferSize;
    if (bytesToRead > 0)
    {
        unsigned frameCount = PaUtil_WriteRingBuffer(&pInfo->stream->ringBuffer,
            pCapture->hostBuffer + pCapture->lastPosition,
            bytesToRead / pCapture->bytesPerFrame);

        pCapture->lastPosition = (pCapture->lastPosition + frameCount * pCapture->bytesPerFrame) % pCapture->hostBufferSize;

        PA_HP_TRACE((pInfo->stream->hLog, "Capture event (WaveRTPolled): pos = %4.1lf%%, framesRead=%u", (pos * 100.0 / pCapture->hostBufferSize), frameCount));
        ++pInfo->captureHead;
        --pInfo->pending;
    }
    return paNoError;
}

static PaError PaPinCaptureSubmitHandler_WaveRTEvent(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    pInfo->capturePackets[pInfo->captureTail & cPacketsArrayMask].packet = 0;
    ++pInfo->pending;
    return paNoError;
}

static PaError PaPinCaptureSubmitHandler_WaveRTPolled(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    pInfo->capturePackets[pInfo->captureTail & cPacketsArrayMask].packet = 0;
    ++pInfo->pending;
    return paNoError;
}

static PaError PaPinRenderEventHandler_WaveRTEvent(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    unsigned long pos;
    unsigned realOutBuf;
    PaWinWdmIOInfo* pRender = &pInfo->stream->render;
    const unsigned halfOutputBuffer = pRender->hostBufferSize >> 1;
    PaWinWdmPin* pin = pInfo->stream->render.pPin;
    PaIOPacket* ioPacket = &pInfo->renderPackets[pInfo->renderHead & cPacketsArrayMask];

    /* Get hold of current DAC position */
    pin->fnAudioPosition(pin, &pos);
    /* Compensate for HW FIFO to get to last read buffer position */
    pos += pin->hwLatency;
    /* Wrap it */
    pos %= pRender->hostBufferSize;
    /* And align it, not sure its really needed though */
    pos &= ~(pRender->bytesPerFrame - 1);
    /* Then realOutBuf will point to "other" half of double buffer */
    realOutBuf = pos < halfOutputBuffer ? 1U : 0U;

    if (pInfo->priming)
    {
        realOutBuf = pInfo->renderHead & 0x1;
    }
    ioPacket->packet = pInfo->stream->render.packets + realOutBuf;
    ioPacket->startByte = realOutBuf * halfOutputBuffer;
    ioPacket->lengthBytes = halfOutputBuffer;

    PA_HP_TRACE((pInfo->stream->hLog, "Render event (WaveRT) : idx=%u head=%u (pos = %4.1lf%%)", realOutBuf, pInfo->renderHead, (pos * 100.0 / pRender->hostBufferSize) ));

    ++pInfo->renderHead;
    --pInfo->pending;
    return paNoError;
}

static PaError PaPinRenderEventHandler_WaveRTPolled(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    unsigned long pos;
    unsigned realOutBuf;
    unsigned bytesToWrite;

    PaWinWdmIOInfo* pRender = &pInfo->stream->render;
    const unsigned halfOutputBuffer = pRender->hostBufferSize >> 1;
    PaWinWdmPin* pin = pInfo->stream->render.pPin;
    PaIOPacket* ioPacket = &pInfo->renderPackets[pInfo->renderHead & cPacketsArrayMask];

    /* Get hold of current DAC position */
    pin->fnAudioPosition(pin, &pos);
    /* Compensate for HW FIFO to get to last read buffer position */
    pos += pin->hwLatency;
    /* Wrap it */
    pos %= pRender->hostBufferSize;
    /* And align it, not sure its really needed though */
    pos &= ~(pRender->bytesPerFrame - 1);


    if (pInfo->priming)
    {
        realOutBuf = pInfo->renderHead & 0x1;
        ioPacket->packet = pInfo->stream->render.packets + realOutBuf;
        ioPacket->startByte = realOutBuf * halfOutputBuffer;
        ioPacket->lengthBytes = halfOutputBuffer;
        ++pInfo->renderHead;
        --pInfo->pending;
    }
    else
    {
        bytesToWrite = (pRender->hostBufferSize + pos - pRender->lastPosition) % pRender->hostBufferSize;
        ++pRender->pollCntr;
        if (bytesToWrite >= halfOutputBuffer)
        {
            realOutBuf = (pos < halfOutputBuffer) ? 1U : 0U;
            ioPacket->packet = pInfo->stream->render.packets + realOutBuf;
            pRender->lastPosition = realOutBuf ? 0U : halfOutputBuffer;
            ioPacket->startByte = realOutBuf * halfOutputBuffer;
            ioPacket->lengthBytes = halfOutputBuffer;
            ++pInfo->renderHead;
            --pInfo->pending;
            PA_HP_TRACE((pInfo->stream->hLog, "Render event (WaveRTPolled) : idx=%u head=%u (pos = %4.1lf%%, cnt=%u)", realOutBuf, pInfo->renderHead, (pos * 100.0 / pRender->hostBufferSize), pRender->pollCntr));
            pRender->pollCntr = 0;
        }
    }
    return paNoError;
}

static PaError PaPinRenderSubmitHandler_WaveRTEvent(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    PaWinWdmPin* pin = pInfo->stream->render.pPin;
    pInfo->renderPackets[pInfo->renderTail & cPacketsArrayMask].packet = 0;
    /* Call barrier (if needed) */
    pin->fnMemBarrier();
    PA_HP_TRACE((pInfo->stream->hLog, "Render submit (WaveRT) : submit=%u", pInfo->renderTail));
    ++pInfo->pending;
    if (pInfo->priming)
    {
        --pInfo->priming;
        if (pInfo->priming)
        {
            PA_HP_TRACE((pInfo->stream->hLog, "Setting WaveRT event for priming (2)"));
            SetEvent(pInfo->stream->render.events[0]);
        }
    }
    return paNoError;
}

static PaError PaPinRenderSubmitHandler_WaveRTPolled(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    PaWinWdmPin* pin = pInfo->stream->render.pPin;
    pInfo->renderPackets[pInfo->renderTail & cPacketsArrayMask].packet = 0;
    /* Call barrier (if needed) */
    pin->fnMemBarrier();
    PA_HP_TRACE((pInfo->stream->hLog, "Render submit (WaveRTPolled) : submit=%u", pInfo->renderTail));
    ++pInfo->pending;
    if (pInfo->priming)
    {
        --pInfo->priming;
        if (pInfo->priming)
        {
            PA_HP_TRACE((pInfo->stream->hLog, "Setting WaveRT event for priming (2)"));
            SetEvent(pInfo->stream->render.events[0]);
        }
    }
    return paNoError;
}

