/*
 * $Id$
 * pa_win_wmme.c
 * Implementation of PortAudio for Windows MultiMedia Extensions (WMME)
 *
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 *
 * Authors: Ross Bencina and Phil Burk
 * Copyright (c) 1999-2000 Ross Bencina and Phil Burk
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
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/* Modification History:
 PLB = Phil Burk
 JM = Julien Maillard
 RDB = Ross Bencina
 PLB20010402 - sDevicePtrs now allocates based on sizeof(pointer)
 PLB20010413 - check for excessive numbers of channels
 PLB20010422 - apply Mike Berry's changes for CodeWarrior on PC
               including conditional inclusion of memory.h,
               and explicit typecasting on memory allocation
 PLB20010802 - use GlobalAlloc for sDevicesPtr instead of PaHost_AllocFastMemory
 PLB20010816 - pass process instead of thread to SetPriorityClass()
 PLB20010927 - use number of frames instead of real-time for CPULoad calculation.
 JM20020118 - prevent hung thread when buffers underflow.
 PLB20020321 - detect Win XP versus NT, 9x; fix DBUG typo; removed init of CurrentCount
 RDB20020411 - various renaming cleanups, factored streamData alloc and cpu usage init
 RDB20020417 - stopped counting WAVE_MAPPER when there were no real devices
               refactoring, renaming and fixed a few edge case bugs
 RDB20020531 - converted to V19 framework
 ** NOTE  maintanance history is now stored in CVS **
*/

/** @file

	@todo Handle case where user supplied full duplex buffer sizes are not compatible
         (must be common multiples)
	
	@todo Fix buffer catch up code, can sometimes get stuck

	@todo Implement "close sample rate matching" if needed - is this really needed
        in mme?

	@todo Investigate supporting host buffer formats > 16 bits

	@todo Implement buffer size and number of buffers code, 
		this code should generate defaults the way the old code did

    @todo implement underflow/overflow streamCallback statusFlags, paNeverDropInput.

	@todo Fix fixmes

    @todo implement initialisation of PaDeviceInfo default*Latency fields (currently set to 0.)

    @todo implement ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable

    @todo implement IsFormatSupported

    @todo implement PaDeviceInfo.defaultSampleRate;
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <windows.h>
#include <mmsystem.h>
#include <process.h>
#include <assert.h>
/* PLB20010422 - "memory.h" doesn't work on CodeWarrior for PC. Thanks Mike Berry for the mod. */
#ifndef __MWERKS__
#include <malloc.h>
#include <memory.h>
#endif /* __MWERKS__ */

#include "portaudio.h"
#include "pa_trace.h"
#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_win_wmme.h"

/************************************************* Constants ********/

/* Switches for debugging. */
#define PA_SIMULATE_UNDERFLOW_    (0)  /* Set to one to force an underflow of the output buffer. */

#define PA_USE_HIGH_LATENCY_      (0)  /* For debugging glitches. */

#if PA_USE_HIGH_LATENCY_
 #define PA_MIN_MSEC_PER_HOST_BUFFER_  (100)
 #define PA_MAX_MSEC_PER_HOST_BUFFER_  (300) /* Do not exceed unless user buffer exceeds */
 #define PA_MIN_NUM_HOST_BUFFERS_      (4)
 #define PA_MAX_NUM_HOST_BUFFERS_      (16)  /* OK to exceed if necessary */
 #define PA_WIN_9X_LATENCY_            (400)
#else
 #define PA_MIN_MSEC_PER_HOST_BUFFER_  (10)
 #define PA_MAX_MSEC_PER_HOST_BUFFER_  (100) /* Do not exceed unless user buffer exceeds */
 #define PA_MIN_NUM_HOST_BUFFERS_      (3)
 #define PA_MAX_NUM_HOST_BUFFERS_      (16)  /* OK to exceed if necessary */
 #define PA_WIN_9X_LATENCY_            (200)
#endif

#define PA_MIN_TIMEOUT_MSEC_                 (1000)

/*
** Use higher latency for NT because it is even worse at real-time
** operation than Win9x.
*/
#define PA_WIN_NT_LATENCY_        (PA_WIN_9X_LATENCY_ * 2)
#define PA_WIN_WDM_LATENCY_       (PA_WIN_9X_LATENCY_)


static const char constInputMapperSuffix_[] = " - Input";
static const char constOutputMapperSuffix_[] = " - Output";


typedef struct PaWinMmeStream PaWinMmeStream;     /* forward reference */

/* prototypes for functions declared in this file */

PaError PaWinMme_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex );
static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** stream,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *streamCallback,
                           void *userData );
static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, void *buffer, unsigned long frames );
static signed long GetStreamReadAvailable( PaStream* stream );
static signed long GetStreamWriteAvailable( PaStream* stream );

static PaError UpdateStreamTime( PaWinMmeStream *stream );

/* macros for setting last host error information */
#define PA_MME_SET_LAST_WAVEIN_ERROR( mmresult ) \
    {                                                                   \
        char mmeErrorText[ MAXERRORLENGTH ];                            \
        waveInGetErrorText( mmresult, mmeErrorText, MAXERRORLENGTH );   \
        PaUtil_SetLastHostErrorInfo( paMME, mmresult, mmeErrorText );   \
    }

#define PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult ) \
    {                                                                   \
        char mmeErrorText[ MAXERRORLENGTH ];                            \
        waveOutGetErrorText( mmresult, mmeErrorText, MAXERRORLENGTH );  \
        PaUtil_SetLastHostErrorInfo( paMME, mmresult, mmeErrorText );   \
    }

static void PaMme_SetLastSystemError( DWORD errorCode )
{
    LPVOID lpMsgBuf;
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR) &lpMsgBuf,
        0,
        NULL
    );
    PaUtil_SetLastHostErrorInfo( paMME, errorCode, lpMsgBuf );
    LocalFree( lpMsgBuf );
}

#define PA_MME_SET_LAST_SYSTEM_ERROR( errorCode ) \
    PaMme_SetLastSystemError( errorCode )


/* PaWinMmeHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct
{
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;
    
    int numInputDevices, numOutputDevices;
}
PaWinMmeHostApiRepresentation;


static void InitializeDeviceCountsAndDefaultDevices( PaWinMmeHostApiRepresentation *hostApi )
{
    hostApi->numInputDevices = waveInGetNumDevs();
    if( hostApi->numInputDevices > 0 )
    {
        hostApi->numInputDevices += 1; /* add one extra for the WAVE_MAPPER */
        hostApi->inheritedHostApiRep.info.defaultInputDevice = 0;
    }
    else
    {
        hostApi->inheritedHostApiRep.info.defaultInputDevice = paNoDevice;
    }

    hostApi->numOutputDevices = waveOutGetNumDevs();
    if( hostApi->numOutputDevices > 0 )
    {
        hostApi->numOutputDevices += 1; /* add one extra for the WAVE_MAPPER */
        hostApi->inheritedHostApiRep.info.defaultOutputDevice = hostApi->numInputDevices;
    }
    else
    {
        hostApi->inheritedHostApiRep.info.defaultOutputDevice = paNoDevice;
    }

    hostApi->inheritedHostApiRep.info.deviceCount =
        hostApi->numInputDevices + hostApi->numOutputDevices;
}

/*************************************************************************
 * Returns recommended device ID.
 * On the PC, the recommended device can be specified by the user by
 * setting an environment variable. For example, to use device #1.
 *
 *    set PA_RECOMMENDED_OUTPUT_DEVICE=1
 *
 * The user should first determine the available device ID by using
 * the supplied application "pa_devs".
 */
#define PA_ENV_BUF_SIZE_  (32)
#define PA_REC_IN_DEV_ENV_NAME_  ("PA_RECOMMENDED_INPUT_DEVICE")
#define PA_REC_OUT_DEV_ENV_NAME_  ("PA_RECOMMENDED_OUTPUT_DEVICE")
static PaDeviceIndex GetEnvDefaultDeviceID( char *envName )
{
    PaDeviceIndex recommendedIndex = paNoDevice;
    DWORD   hresult;
    char    envbuf[PA_ENV_BUF_SIZE_];

    /* Let user determine default device by setting environment variable. */
    hresult = GetEnvironmentVariable( envName, envbuf, PA_ENV_BUF_SIZE_ );
    if( (hresult > 0) && (hresult < PA_ENV_BUF_SIZE_) )
    {
        recommendedIndex = atoi( envbuf );
    }
    return recommendedIndex;
}

static void InitializeDefaultDeviceIdsFromEnv( PaWinMmeHostApiRepresentation *hostApi )
{
    PaDeviceIndex device;

    /* input */
    device = GetEnvDefaultDeviceID( PA_REC_IN_DEV_ENV_NAME_ );
    if( device != paNoDevice &&
            ( device >= 0 && device < hostApi->inheritedHostApiRep.info.deviceCount ) &&
            hostApi->inheritedHostApiRep.deviceInfos[ device ]->maxInputChannels > 0 )
    {
        hostApi->inheritedHostApiRep.info.defaultInputDevice = device;
    }

    /* output */
    device = GetEnvDefaultDeviceID( PA_REC_OUT_DEV_ENV_NAME_ );
    if( device != paNoDevice &&
            ( device >= 0 && device < hostApi->inheritedHostApiRep.info.deviceCount ) &&
            hostApi->inheritedHostApiRep.deviceInfos[ device ]->maxOutputChannels > 0 )
    {
        hostApi->inheritedHostApiRep.info.defaultOutputDevice = device;
    }
}


/** Convert external PA ID to an internal ID that includes WAVE_MAPPER
    Note that WAVE_MAPPER is defined as -1
*/
static int LocalDeviceIndexToWinMmeDeviceId( PaWinMmeHostApiRepresentation *hostApi, PaDeviceIndex device )
{
    if( device < hostApi->numInputDevices )
        return device - 1;
    else
        return device - hostApi->numInputDevices - 1;
}


#define PA_NUM_STANDARDSAMPLINGRATES_   3   /* 11.025, 22.05, 44.1 */
#define PA_NUM_CUSTOMSAMPLINGRATES_     5   /* must be the same number of elements as in the array below */
#define PA_MAX_NUMSAMPLINGRATES_        (PA_NUM_STANDARDSAMPLINGRATES_+PA_NUM_CUSTOMSAMPLINGRATES_)
static DWORD customSamplingRates_[] = { 32000, 48000, 64000, 88200, 96000 };

static PaError InitializeInputDeviceInfo( PaWinMmeHostApiRepresentation *winMmeHostApi, PaDeviceInfo *deviceInfo, PaDeviceIndex deviceIndex )
{
    PaError result = paNoError;
    char *deviceName; /* non-const ptr */
    int inputWinMmeId;
    MMRESULT mmresult;
    WAVEINCAPS wic;
    int i;

    inputWinMmeId = LocalDeviceIndexToWinMmeDeviceId( winMmeHostApi, deviceIndex );

    mmresult = waveInGetDevCaps( inputWinMmeId, &wic, sizeof( WAVEINCAPS ) );
    if( mmresult != MMSYSERR_NOERROR )
    {
        result = paUnanticipatedHostError;
        PA_MME_SET_LAST_WAVEIN_ERROR( mmresult );
        goto error;
    }


    if( inputWinMmeId == WAVE_MAPPER )
    {
        /* Append I/O suffix to WAVE_MAPPER device. */
        deviceName = (char *)PaUtil_GroupAllocateMemory(
                    winMmeHostApi->allocations, strlen( wic.szPname ) + 1 + sizeof(constInputMapperSuffix_) );
        if( !deviceName )
        {
            result = paInsufficientMemory;
            goto error;
        }
        strcpy( deviceName, wic.szPname );
        strcat( deviceName, constInputMapperSuffix_ );
    }
    else
    {
        deviceName = (char*)PaUtil_GroupAllocateMemory(
                    winMmeHostApi->allocations, strlen( wic.szPname ) + 1 );
        if( !deviceName )
        {
            result = paInsufficientMemory;
            goto error;
        }
        strcpy( deviceName, wic.szPname  );
    }
    deviceInfo->name = deviceName;

    deviceInfo->maxInputChannels = wic.wChannels;
    /* Sometimes a device can return a rediculously large number of channels.
     * This happened with an SBLive card on a Windows ME box.
     * If that happens, then force it to 2 channels.  PLB20010413
     */
    if( (deviceInfo->maxInputChannels < 1) || (deviceInfo->maxInputChannels > 256) )
    {
        PA_DEBUG(("Pa_GetDeviceInfo: Num input channels reported as %d! Changed to 2.\n", deviceInfo->maxOutputChannels ));
        deviceInfo->maxInputChannels = 2;
    }

    deviceInfo->defaultSampleRate = 0.; /* @todo IMPLEMENT ME */

error:
    return result;
}


static PaError InitializeOutputDeviceInfo( PaWinMmeHostApiRepresentation *winMmeHostApi, PaDeviceInfo *deviceInfo, PaDeviceIndex deviceIndex )
{
    PaError result = paNoError;
    char *deviceName; /* non-const ptr */
    int outputWinMmeId;
    MMRESULT mmresult;
    WAVEOUTCAPS woc;
    int i;

    outputWinMmeId = LocalDeviceIndexToWinMmeDeviceId( winMmeHostApi, deviceIndex );

    mmresult = waveOutGetDevCaps( outputWinMmeId, &woc, sizeof( WAVEOUTCAPS ) );
    if( mmresult != MMSYSERR_NOERROR )
    {
        result = paUnanticipatedHostError;
        PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult );
        goto error;
    }

    if( outputWinMmeId == WAVE_MAPPER )
    {
        /* Append I/O suffix to WAVE_MAPPER device. */
        deviceName = (char *)PaUtil_GroupAllocateMemory(
                    winMmeHostApi->allocations, strlen( woc.szPname ) + 1 + sizeof(constOutputMapperSuffix_) );
        if( !deviceName )
        {
            result = paInsufficientMemory;
            goto error;
        }
        strcpy( deviceName, woc.szPname );
        strcat( deviceName, constOutputMapperSuffix_ );
    }
    else
    {
        deviceName = (char*)PaUtil_GroupAllocateMemory(
                    winMmeHostApi->allocations, strlen( woc.szPname ) + 1 );
        if( !deviceName )
        {
            result = paInsufficientMemory;
            goto error;
        }
        strcpy( deviceName, woc.szPname  );
    }
    deviceInfo->name = deviceName;

    deviceInfo->maxOutputChannels = woc.wChannels;
    /* Sometimes a device can return a rediculously large number of channels.
     * This happened with an SBLive card on a Windows ME box.
     * It also happens on Win XP!
     */
    if( (deviceInfo->maxOutputChannels < 1) || (deviceInfo->maxOutputChannels > 256) )
    {
#if 1
        deviceInfo->maxOutputChannels = 2;
#else
        /* If channel max is goofy, then query for max channels. PLB20020228
        * This doesn't seem to help. Disable code for now. Remove it later.
        */
        PA_DEBUG(("Pa_GetDeviceInfo: Num output channels reported as %d!", deviceInfo->maxOutputChannels ));
        deviceInfo->maxOutputChannels = 0;
        /* Attempt to find the correct maximum by querying the device. */
        for( i=2; i<16; i += 2 )
        {
            WAVEFORMATEX wfx;
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nSamplesPerSec = 44100;
            wfx.wBitsPerSample = 16;
            wfx.cbSize = 0; /* ignored */
            wfx.nChannels = (WORD) i;
            wfx.nAvgBytesPerSec = wfx.nChannels * wfx.nSamplesPerSec * sizeof(short);
            wfx.nBlockAlign = (WORD)(wfx.nChannels * sizeof(short));
            if( waveOutOpen( NULL, outputWinMmeId, &wfx, 0, 0, WAVE_FORMAT_QUERY ) == MMSYSERR_NOERROR )
            {
                deviceInfo->maxOutputChannels = i;
            }
            else
            {
                break;
            }
        }
#endif
        PA_DEBUG((" Changed to %d.\n", deviceInfo->maxOutputChannels ));
    }

    deviceInfo->defaultSampleRate = 0.; /* @todo IMPLEMENT ME */

error:
    return result;
}


PaError PaWinMme_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int i;
    PaWinMmeHostApiRepresentation *winMmeHostApi;
    PaDeviceInfo *deviceInfoArray;
    double *sampleRates; /* non-const ptr */

    winMmeHostApi = (PaWinMmeHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaWinMmeHostApiRepresentation) );
    if( !winMmeHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    winMmeHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !winMmeHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &winMmeHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paMME;
    (*hostApi)->info.name = "MME";

    InitializeDeviceCountsAndDefaultDevices( winMmeHostApi );

    if( (*hostApi)->info.deviceCount > 0 )
    {
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
                winMmeHostApi->allocations, sizeof(PaDeviceInfo*) * (*hostApi)->info.deviceCount );
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all device info structs in a contiguous block */
        deviceInfoArray = (PaDeviceInfo*)PaUtil_GroupAllocateMemory(
                winMmeHostApi->allocations, sizeof(PaDeviceInfo) * (*hostApi)->info.deviceCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i=0; i < (*hostApi)->info.deviceCount; ++i )
        {
            PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
            deviceInfo->structVersion = 2;
            deviceInfo->hostApi = hostApiIndex;

            deviceInfo->maxInputChannels = 0;
            deviceInfo->maxOutputChannels = 0;

            deviceInfo->defaultLowInputLatency = 0.;  /* @todo IMPLEMENT ME */
            deviceInfo->defaultLowOutputLatency = 0.;  /* @todo IMPLEMENT ME */
            deviceInfo->defaultHighInputLatency = 0.;  /* @todo IMPLEMENT ME */
            deviceInfo->defaultHighOutputLatency = 0.;  /* @todo IMPLEMENT ME */


            /* allocate space for all possible sample rates */
            sampleRates = (double*)PaUtil_GroupAllocateMemory(
                    winMmeHostApi->allocations, PA_MAX_NUMSAMPLINGRATES_ * sizeof(double) );
            if( !sampleRates )
            {
                result = paInsufficientMemory;
                goto error;
            }

            if( i < winMmeHostApi->numInputDevices )
            {
                result = InitializeInputDeviceInfo( winMmeHostApi, deviceInfo, i );
                if( result != paNoError )
                    goto error;

            }
            else
            {
                result = InitializeOutputDeviceInfo( winMmeHostApi, deviceInfo, i );
                if( result != paNoError )
                    goto error;
            }

            (*hostApi)->deviceInfos[i] = deviceInfo;
        }
    }

    InitializeDefaultDeviceIdsFromEnv( winMmeHostApi );

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &winMmeHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyReadWrite, PaUtil_DummyReadWrite, PaUtil_DummyGetAvailable, PaUtil_DummyGetAvailable );

    PaUtil_InitializeStreamInterface( &winMmeHostApi->blockingStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    return result;

error:
    if( winMmeHostApi )
    {
        if( winMmeHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( winMmeHostApi->allocations );
            PaUtil_DestroyAllocationGroup( winMmeHostApi->allocations );
        }
        
        PaUtil_FreeMemory( winMmeHostApi );
    }

    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaWinMmeHostApiRepresentation *winMmeHostApi = (PaWinMmeHostApiRepresentation*)hostApi;

    if( winMmeHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( winMmeHostApi->allocations );
        PaUtil_DestroyAllocationGroup( winMmeHostApi->allocations );
    }

    PaUtil_FreeMemory( winMmeHostApi );
}


static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    
    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
            return paInvalidChannelCount;

        /* validate inputStreamInfo */
        if( inputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
    }
    else
    {
        inputChannelCount = 0;
    }

    if( outputParameters )
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
        
        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that output device can support inputChannelCount */
        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
            return paInvalidChannelCount;

        /* validate outputStreamInfo */
        if( outputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
    }
    else
    {
        outputChannelCount = 0;
    }
    
    /*
        IMPLEMENT ME:
            - check that input device can support inputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - check that output device can support outputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - if a full duplex stream is requested, check that the combination
                of input and output parameters is supported

            - check that the device supports sampleRate
    */

    return paFormatIsSupported;
}


/* CalculateBufferSettings() fills the framesPerHostInputBuffer, numHostInputBuffers,
    framesPerHostOutputBuffer and numHostOutputBuffers parameters based on the values
    of the other parameters.

*/

static PaError CalculateBufferSettings(
        unsigned long *framesPerHostInputBuffer, unsigned long *numHostInputBuffers,
        unsigned long *framesPerHostOutputBuffer, unsigned long *numHostOutputBuffers,
        int inputChannelCount, PaSampleFormat hostInputSampleFormat,
        PaTime suggestedInputLatency, PaWinMmeStreamInfo *inputStreamInfo,
        int outputChannelCount, PaSampleFormat hostOutputSampleFormat,
        PaTime suggestedOutputLatency, PaWinMmeStreamInfo *outputStreamInfo,
        double sampleRate, unsigned long framesPerBuffer )
{
    PaError result = paNoError;

    if( inputChannelCount > 0 )
    {
        if( inputStreamInfo )
        {
            if( inputStreamInfo->flags & PaWinMmeUseLowLevelLatencyParameters )
            {
                if( inputStreamInfo->numBuffers <= 0
                        || inputStreamInfo->framesPerBuffer <= 0 )
                {
                    result = paIncompatibleHostApiSpecificStreamInfo;
                    goto error;
                }

                *framesPerHostInputBuffer = inputStreamInfo->framesPerBuffer;
                *numHostInputBuffers = inputStreamInfo->numBuffers;
            }
        }
        else
        {
            /* hardwire for now, FIXME */
            /* don't forget that there will be one more buffer than the number required to achieve the requested latency */
            *framesPerHostInputBuffer = 4096;
            *numHostInputBuffers = 4;
        }
    }
    else
    {
        *framesPerHostInputBuffer = 0;
        *numHostInputBuffers = 0;
    }

    if( outputChannelCount > 0 )
    {
        if( outputStreamInfo )
        {
            if( outputStreamInfo->flags & PaWinMmeUseLowLevelLatencyParameters )
            {
                if( outputStreamInfo->numBuffers <= 0
                        || outputStreamInfo->framesPerBuffer <= 0 )
                {
                    result = paIncompatibleHostApiSpecificStreamInfo;
                    goto error;
                }

                *framesPerHostOutputBuffer = outputStreamInfo->framesPerBuffer;
                *numHostOutputBuffers = outputStreamInfo->numBuffers;
            }
        }
        else
        {
            /* hardwire for now, FIXME */
            /* don't forget that there will be one more buffer than the number required to achieve the requested latency */
            *framesPerHostOutputBuffer = 4096;
            *numHostOutputBuffers = 4;
        }
    }
    else
    {
        *framesPerHostOutputBuffer = 0;
        *numHostOutputBuffers = 0;
    }

error:
    return result;
}



typedef HWAVEIN MmeHandle;

static PaError InitializeBufferSet( WAVEHDR **bufferSet, int numBuffers, int bufferBytes,
                                    int isInput, /* if 0, then output */
                                    MmeHandle mmeWaveHandle, int numDeviceChannels )
{
    PaError result = paNoError;
    MMRESULT mmresult;
    int i;

    *bufferSet = 0;

    /* Allocate an array to hold the buffer pointers. */
    *bufferSet = (WAVEHDR *) PaUtil_AllocateMemory( sizeof(WAVEHDR)*numBuffers );
    if( !*bufferSet )
    {
        result = paInsufficientMemory;
        goto error;
    }

    for( i=0; i<numBuffers; ++i )
    {
        (*bufferSet)[i].lpData = 0;
    }

    /* Allocate each buffer. */
    for( i=0; i<numBuffers; ++i )
    {
        (*bufferSet)[i].lpData = (char *)PaUtil_AllocateMemory( bufferBytes );
        if( !(*bufferSet)[i].lpData )
        {
            result = paInsufficientMemory;
            goto error;
        }
        (*bufferSet)[i].dwBufferLength = bufferBytes;
        (*bufferSet)[i].dwUser = 0xFFFFFFFF; /* indicates unprepared to error clean up code */

        if( isInput )
        {
            mmresult = waveInPrepareHeader( mmeWaveHandle, &(*bufferSet)[i], sizeof(WAVEHDR) );
            if( mmresult != MMSYSERR_NOERROR )
            {
                result = paUnanticipatedHostError;
                PA_MME_SET_LAST_WAVEIN_ERROR( mmresult );
                goto error;
            }
        }
        else /* output */
        {
            mmresult = waveOutPrepareHeader( (HWAVEOUT)mmeWaveHandle, &(*bufferSet)[i], sizeof(WAVEHDR) );
            if( mmresult != MMSYSERR_NOERROR )
            {
                result = paUnanticipatedHostError;
                PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult );
                goto error;
            }
        }

        (*bufferSet)[i].dwUser = numDeviceChannels;
    }

    return result;

error:
    if( *bufferSet )
    {
        for( i=0; i<numBuffers; ++i )
        {
            if( (*bufferSet)[i].lpData )
            {

                if( (*bufferSet)[i].dwUser != 0xFFFFFFFF )
                {
                    if( isInput )
                        waveInUnprepareHeader( mmeWaveHandle, &(*bufferSet)[i], sizeof(WAVEHDR) );
                    else
                        waveOutUnprepareHeader( (HWAVEOUT)mmeWaveHandle, &(*bufferSet)[i], sizeof(WAVEHDR) );
                }
                PaUtil_FreeMemory( (*bufferSet)[i].lpData );
            }
        }

        PaUtil_FreeMemory( *bufferSet );
    }

    return result;
}


static void TerminateBufferSet( WAVEHDR * *bufferSet, unsigned int numBuffers, int isInput, MmeHandle mmeWaveHandle )
{
    unsigned int i;

    for( i=0; i<numBuffers; ++i )
    {
        if( (*bufferSet)[i].lpData )
        {
            if( isInput )
                waveInUnprepareHeader( mmeWaveHandle, &(*bufferSet)[i], sizeof(WAVEHDR) );
            else
                waveOutUnprepareHeader( (HWAVEOUT)mmeWaveHandle, &(*bufferSet)[i], sizeof(WAVEHDR) );
                
            PaUtil_FreeMemory( (*bufferSet)[i].lpData );
        }
    }

    if( *bufferSet )
        PaUtil_FreeMemory( *bufferSet );
}


/* PaWinMmeStream - a stream data structure specifically for this implementation */

typedef struct PaWinMmeStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    CRITICAL_SECTION lock;

    /* Input -------------- */
    HWAVEIN *hWaveIns;
    unsigned int numInputDevices;
    /* unsigned int inputChannelCount; */
    WAVEHDR **inputBuffers;
    unsigned int numInputBuffers;
    unsigned int currentInputBufferIndex;
    unsigned int framesPerInputBuffer;
    unsigned int framesUsedInCurrentInputBuffer;
    
    /* Output -------------- */
    HWAVEOUT *hWaveOuts;
    unsigned int numOutputDevices;
    /* unsigned int outputChannelCount; */
    WAVEHDR **outputBuffers;
    unsigned int numOutputBuffers;
    unsigned int currentOutputBufferIndex;
    unsigned int framesPerOutputBuffer;
    unsigned int framesUsedInCurrentOutputBuffer;

    /* Processing thread management -------------- */
    HANDLE abortEvent;
    HANDLE bufferEvent;
    HANDLE processingThread;
    DWORD processingThreadId;

    char noHighPriorityProcessClass;
    char useTimeCriticalProcessingThreadPriority;
    char throttleProcessingThreadOnOverload; /* 0 -> don't throtte, non-0 -> throttle */
    int processingThreadPriority;
    int highThreadPriority;
    int throttledThreadPriority;

    volatile int isActive;
    volatile int stopProcessing; /* stop thread once existing buffers have been returned */
    volatile int abortProcessing; /* stop thread immediately */

    DWORD allBuffersDurationMs; /* used to calculate timeouts */

    /* @todo FIXME: we no longer need the following for GetStreamTime support */
    /* GetStreamTime() support ------------- */

    PaTime streamPosition;
    long previousStreamPosition;                /* used to track frames played. */
}
PaWinMmeStream;


/* the following macros are intended to improve the readability of the following code */
#define PA_IS_INPUT_STREAM_( stream ) ( stream ->hWaveIns )
#define PA_IS_OUTPUT_STREAM_( stream ) ( stream ->hWaveOuts )
#define PA_IS_FULL_DUPLEX_STREAM_( stream ) ( stream ->hWaveIns  && stream ->hWaveOuts )


static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *streamCallback,
                           void *userData )
{
    PaError result = paNoError;
    PaWinMmeHostApiRepresentation *winMmeHostApi = (PaWinMmeHostApiRepresentation*)hostApi;
    PaWinMmeStream *stream = 0;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    double suggestedInputLatency, suggestedOutputLatency;
    PaWinMmeStreamInfo *inputStreamInfo, *outputStreamInfo;
    unsigned long bytesPerInputFrame, bytesPerOutputFrame;
    unsigned long framesPerHostInputBuffer;
    unsigned long numHostInputBuffers;
    unsigned long framesPerHostOutputBuffer;
    unsigned long numHostOutputBuffers;
    unsigned long framesPerBufferProcessorCall;  
    int lockInited = 0;
    int bufferEventInited = 0;
    int abortEventInited = 0;
    WAVEFORMATEX wfx;
    MMRESULT mmresult;
    unsigned int i;
    int channelCount;
    PaWinMmeDeviceAndChannelCount *inputDevices = 0;
    unsigned long numInputDevices = (inputParameters) ? 1 : 0;
    PaWinMmeDeviceAndChannelCount *outputDevices = 0;
    unsigned long numOutputDevices = (outputParameters) ? 1 : 0;
    char noHighPriorityProcessClass = 0;
    char useTimeCriticalProcessingThreadPriority = 0;
    char throttleProcessingThreadOnOverload = 1;


    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
        suggestedInputLatency = inputParameters->suggestedLatency;
        
        /* check that input device can support inputChannelCount */
        if( (inputParameters->device != paUseHostApiSpecificDeviceSpecification) &&
                (inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels) )
            return paInvalidChannelCount;


        /* validate input hostApiSpecificStreamInfo */
        inputStreamInfo = (PaWinMmeStreamInfo*)inputParameters->hostApiSpecificStreamInfo;
        if( inputStreamInfo )
        {
            if( inputStreamInfo->size != sizeof( PaWinMmeStreamInfo )
                    || inputStreamInfo->version != 1 )
            {
                return paIncompatibleHostApiSpecificStreamInfo;
            }

            if( inputStreamInfo->flags & PaWinMmeNoHighPriorityProcessClass )
                noHighPriorityProcessClass = 1;
            if( inputStreamInfo->flags & PaWinMmeDontThrottleOverloadedProcessingThread )
                throttleProcessingThreadOnOverload = 0;
            if( inputStreamInfo->flags & PaWinMmeUseTimeCriticalThreadPriority )
                useTimeCriticalProcessingThreadPriority = 1;
            
            /* validate multidevice fields */

            if( inputStreamInfo->flags & PaWinMmeUseMultipleDevices )
            {
                int totalChannels = 0;
                for( i=0; i< inputStreamInfo->deviceCount; ++i )
                {
                    /* validate that the device number is within range, and that
                        the number of channels is legal */
                    PaDeviceIndex hostApiDevice;

                    if( inputParameters->device != paUseHostApiSpecificDeviceSpecification )
                        return paInvalidDevice;

                    channelCount = inputStreamInfo->devices[i].channelCount;

                    result = PaUtil_DeviceIndexToHostApiDeviceIndex( &hostApiDevice,
                                    inputStreamInfo->devices[i].device, hostApi );
                    if( result != paNoError )
                        return result;

                    if( channelCount < 1 || channelCount > hostApi->deviceInfos[ hostApiDevice ]->maxInputChannels )
                        return paInvalidChannelCount;

                    /* FIXME this validation might be easier and better if there was a pautil
                        function which performed the validation in pa_front:ValidateOpenStreamParameters() */

                    totalChannels += channelCount;
                }

                if( totalChannels != inputChannelCount )
                {
                    /* inputChannelCount must match total channels specified by multiple devices */
                    return paInvalidChannelCount; /* REVIEW use of this error code */
                }

                inputDevices = inputStreamInfo->devices;
                numInputDevices = inputStreamInfo->deviceCount;
            }
        }

        /* FIXME: establish which host formats are available */
        hostInputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( paInt16 /* native formats */, inputSampleFormat );

    }
    else
    {
        inputChannelCount = 0;
        inputStreamInfo = 0;
    }


    if( outputParameters )
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
        suggestedOutputLatency = outputParameters->suggestedLatency;
        
        /* check that input device can support inputChannelCount */
        if( (outputParameters->device != paUseHostApiSpecificDeviceSpecification) &&
                (inputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels) )
            return paInvalidChannelCount;


        /* validate input hostApiSpecificStreamInfo */
        outputStreamInfo = (PaWinMmeStreamInfo*)outputParameters->hostApiSpecificStreamInfo;
        if( outputStreamInfo )
        {
            if( outputStreamInfo->size != sizeof( PaWinMmeStreamInfo )
                    || outputStreamInfo->version != 1 )
            {
                return paIncompatibleHostApiSpecificStreamInfo;
            }

            if( outputStreamInfo->flags & PaWinMmeNoHighPriorityProcessClass )
                noHighPriorityProcessClass = 1;
            if( outputStreamInfo->flags & PaWinMmeDontThrottleOverloadedProcessingThread )
                throttleProcessingThreadOnOverload = 0;
            if( outputStreamInfo->flags & PaWinMmeUseTimeCriticalThreadPriority )
                useTimeCriticalProcessingThreadPriority = 1;
            
            /* validate multidevice fields */
        
            if( outputStreamInfo->flags & PaWinMmeUseMultipleDevices )
            {
                int totalChannels = 0;
                for( i=0; i< outputStreamInfo->deviceCount; ++i )
                {
                    /* validate that the device number is within range, and that
                        the number of channels is legal */
                    PaDeviceIndex hostApiDevice;

                    if( outputParameters->device != paUseHostApiSpecificDeviceSpecification )
                        return paInvalidDevice;

                    channelCount = outputStreamInfo->devices[i].channelCount;
                
                    result = PaUtil_DeviceIndexToHostApiDeviceIndex( &hostApiDevice,
                                    outputStreamInfo->devices[i].device,
                            hostApi );
                    if( result != paNoError )
                        return result;

                    if( channelCount < 1 || channelCount > hostApi->deviceInfos[ hostApiDevice ]->maxOutputChannels )
                        return paInvalidChannelCount;

                    /* FIXME this validation might be easier and better if there was a pautil
                        function which performed the validation in pa_front:ValidateOpenStreamParameters() */
                    
                    totalChannels += channelCount;
                }

                if( totalChannels != outputChannelCount )
                {
                    /* outputChannelCount must match total channels specified by multiple devices */
                    return paInvalidChannelCount; /* REVIEW use of this error code */
                }

                outputDevices = outputStreamInfo->devices;
                numOutputDevices = outputStreamInfo->deviceCount;
            }
        }

        /* FIXME: establish which host formats are available */
        hostOutputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( paInt16 /* native formats */, outputSampleFormat );
    }
    else
    {
        outputChannelCount = 0;
        outputStreamInfo = 0;
    }


    /*
        IMPLEMENT ME:
            - alter sampleRate to a close allowable rate if possible / necessary
    */


    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */


    result = CalculateBufferSettings( &framesPerHostInputBuffer, &numHostInputBuffers,
                &framesPerHostOutputBuffer, &numHostOutputBuffers,
                inputChannelCount, hostInputSampleFormat, suggestedInputLatency, inputStreamInfo,
                outputChannelCount, hostOutputSampleFormat, suggestedOutputLatency, outputStreamInfo,
                sampleRate, framesPerBuffer );
    if( result != paNoError )
        goto error;


    stream = (PaWinMmeStream*)PaUtil_AllocateMemory( sizeof(PaWinMmeStream) );
    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }

    stream->hWaveIns = 0;
    stream->inputBuffers = 0;
    stream->hWaveOuts = 0;
    stream->outputBuffers = 0;
    stream->processingThread = 0;

    stream->noHighPriorityProcessClass = noHighPriorityProcessClass;
    stream->useTimeCriticalProcessingThreadPriority = useTimeCriticalProcessingThreadPriority;
    stream->throttleProcessingThreadOnOverload = throttleProcessingThreadOnOverload;
    
    PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                           &winMmeHostApi->callbackStreamInterface, streamCallback, userData );

    stream->streamRepresentation.streamInfo.inputLatency = (double)(framesPerHostInputBuffer * (numHostInputBuffers-1)) / sampleRate;
    stream->streamRepresentation.streamInfo.outputLatency = (double)(framesPerHostOutputBuffer * (numHostOutputBuffers-1)) / sampleRate;
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );


    if( inputParameters && outputParameters ) /* full duplex */
    {
        /*
            either host input and output buffers must be the same size, or the
            larger one must be an integer multiple of the smaller one.
            FIXME: should this return an error if the host specific latency
            settings don't fulfill these constraints? rb: probably
        */

        if( framesPerHostInputBuffer < framesPerHostOutputBuffer )
        {
            assert( (framesPerHostOutputBuffer % framesPerHostInputBuffer) == 0 );

            framesPerBufferProcessorCall = framesPerHostInputBuffer;
        }
        else
        {
            assert( (framesPerHostInputBuffer % framesPerHostOutputBuffer) == 0 );
            
            framesPerBufferProcessorCall = framesPerHostOutputBuffer;
        }
    }
    else if( inputParameters )
    {
        framesPerBufferProcessorCall = framesPerHostInputBuffer;
    }
    else if( outputParameters )
    {
        framesPerBufferProcessorCall = framesPerHostOutputBuffer;
    }

    stream->framesPerInputBuffer = framesPerHostInputBuffer;
    stream->framesPerOutputBuffer = framesPerHostOutputBuffer;

    result =  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
                    inputChannelCount, inputSampleFormat, hostInputSampleFormat,
                    outputChannelCount, outputSampleFormat, hostOutputSampleFormat,
                    sampleRate, streamFlags, framesPerBuffer,
                    framesPerBufferProcessorCall, paUtilFixedHostBufferSize,
                    streamCallback, userData );
    if( result != paNoError )
        goto error;

    stream->isActive = 0;

    stream->streamPosition = 0.;
    stream->previousStreamPosition = 0;


    stream->bufferEvent = CreateEvent( NULL, FALSE, FALSE, NULL );
    if( stream->bufferEvent == NULL )
    {
        result = paUnanticipatedHostError;
        PA_MME_SET_LAST_SYSTEM_ERROR( GetLastError() );
        goto error;
    }
    bufferEventInited = 1;

    if( inputParameters )
    {
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nSamplesPerSec = (DWORD) sampleRate;
        wfx.cbSize = 0;

        stream->numInputDevices = numInputDevices;
        stream->hWaveIns = (HWAVEIN*)PaUtil_AllocateMemory( sizeof(HWAVEIN) * stream->numInputDevices );
        if( !stream->hWaveIns )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i = 0; i < stream->numInputDevices; ++i )
            stream->hWaveIns[i] = 0;

        for( i = 0; i < stream->numInputDevices; ++i )
        {
            int inputWinMmeId;
            
            if( inputDevices )
            {
                PaDeviceIndex hostApiDevice;

                result = PaUtil_DeviceIndexToHostApiDeviceIndex( &hostApiDevice,
                        inputDevices[i].device, hostApi );
                if( result != paNoError )
                    return result;

                inputWinMmeId = LocalDeviceIndexToWinMmeDeviceId( winMmeHostApi, hostApiDevice );
                wfx.nChannels = (WORD) inputDevices[i].channelCount;
            }
            else
            {
                inputWinMmeId = LocalDeviceIndexToWinMmeDeviceId( winMmeHostApi, inputParameters->device );
                wfx.nChannels = (WORD) inputChannelCount;
            }

            bytesPerInputFrame = wfx.nChannels * stream->bufferProcessor.bytesPerHostInputSample;

            wfx.nAvgBytesPerSec = (DWORD)(bytesPerInputFrame * sampleRate);
            wfx.nBlockAlign = (WORD)bytesPerInputFrame;
            wfx.wBitsPerSample = (WORD)((bytesPerInputFrame/wfx.nChannels) * 8);

            /* REVIEW: consider not firing an event for input when a full duplex stream is being used */

            mmresult = waveInOpen( &stream->hWaveIns[i], inputWinMmeId, &wfx,
                                   (DWORD)stream->bufferEvent, (DWORD) stream, CALLBACK_EVENT );
            if( mmresult != MMSYSERR_NOERROR )
            {
                switch( mmresult )
                {
                    case MMSYSERR_ALLOCATED:    /* Specified resource is already allocated. */
                        result = paDeviceUnavailable;
                        break;
                    case MMSYSERR_BADDEVICEID:	/* Specified device identifier is out of range. */
                        result = paInternalError;  /* portaudio should ensure that only good device ids are used */
                        break;
                    case MMSYSERR_NODRIVER:	    /* No device driver is present. */
                        result = paDeviceUnavailable;
                        break;
                    case MMSYSERR_NOMEM:	    /* Unable to allocate or lock memory. */
                        result = paInsufficientMemory;
                        break;
                    case WAVERR_BADFORMAT:      /* Attempted to open with an unsupported waveform-audio format. */
                        result = paInternalError; /* REVIEW: port audio shouldn't get this far without using compatible format info */ 
                        break;
                    default:
                        result = paUnanticipatedHostError;
                        PA_MME_SET_LAST_WAVEIN_ERROR( mmresult );
                }
                goto error;
            }
        }
    }

    if( outputParameters )
    {
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nSamplesPerSec = (DWORD) sampleRate;
        wfx.cbSize = 0;

        stream->numOutputDevices = numOutputDevices;
        stream->hWaveOuts = (HWAVEOUT*)PaUtil_AllocateMemory( sizeof(HWAVEOUT) * stream->numOutputDevices );
        if( !stream->hWaveOuts )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i = 0; i < stream->numOutputDevices; ++i )
            stream->hWaveOuts[i] = 0;

        for( i = 0; i < stream->numOutputDevices; ++i )
        {
            int outputWinMmeId;

            if( outputDevices )
            {
                PaDeviceIndex hostApiDevice;

                result = PaUtil_DeviceIndexToHostApiDeviceIndex( &hostApiDevice,
                        outputDevices[i].device, hostApi );
                if( result != paNoError )
                    return result;

                outputWinMmeId = LocalDeviceIndexToWinMmeDeviceId( winMmeHostApi, hostApiDevice );
                wfx.nChannels = (WORD) outputDevices[i].channelCount;
            }
            else
            {
                outputWinMmeId = LocalDeviceIndexToWinMmeDeviceId( winMmeHostApi, outputParameters->device );
                wfx.nChannels = (WORD) outputChannelCount;
            }

            bytesPerOutputFrame = wfx.nChannels * stream->bufferProcessor.bytesPerHostOutputSample;

            wfx.nAvgBytesPerSec = (DWORD)(bytesPerOutputFrame * sampleRate);
            wfx.nBlockAlign = (WORD)bytesPerOutputFrame;
            wfx.wBitsPerSample = (WORD)((bytesPerOutputFrame/wfx.nChannels) * 8);

            mmresult = waveOutOpen( &stream->hWaveOuts[i], outputWinMmeId, &wfx,
                                    (DWORD)stream->bufferEvent, (DWORD) stream, CALLBACK_EVENT );
            if( mmresult != MMSYSERR_NOERROR )
            {
                switch( mmresult )
                {
                    case MMSYSERR_ALLOCATED:    /* Specified resource is already allocated. */
                        result = paDeviceUnavailable;
                        break;
                    case MMSYSERR_BADDEVICEID:	/* Specified device identifier is out of range. */
                        result = paInternalError;  /* portaudio should ensure that only good device ids are used */
                        break;
                    case MMSYSERR_NODRIVER:	    /* No device driver is present. */
                        result = paDeviceUnavailable;
                        break;
                    case MMSYSERR_NOMEM:	    /* Unable to allocate or lock memory. */
                        result = paInsufficientMemory;
                        break;
                    case WAVERR_BADFORMAT:      /* Attempted to open with an unsupported waveform-audio format. */
                        result = paInternalError; /* REVIEW: port audio shouldn't get this far without using compatible format info */ 
                        break;
                    default:
                        result = paUnanticipatedHostError;
                        PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult );
                }
                goto error;
            }
        }
    }
    
    if( PA_IS_INPUT_STREAM_(stream) )
    {
        stream->inputBuffers = (WAVEHDR**)PaUtil_AllocateMemory( sizeof(WAVEHDR*) * stream->numInputDevices );
        if( stream->inputBuffers == 0 )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i =0; i < stream->numInputDevices; ++i )
            stream->inputBuffers[i] = 0;

        stream->numInputBuffers = numHostInputBuffers;

        for( i =0; i < stream->numInputDevices; ++i )
        {
            int hostInputBufferBytes = Pa_GetSampleSize( hostInputSampleFormat ) *
                framesPerHostInputBuffer *
                ((inputDevices) ? inputDevices[i].channelCount : inputChannelCount);
            if( hostInputBufferBytes < 0 )
            {
                result = paInternalError;
                goto error;
            }

            result = InitializeBufferSet( &stream->inputBuffers[i], numHostInputBuffers, hostInputBufferBytes,
                             1 /* isInput */,
                             (MmeHandle)stream->hWaveIns[i],
                             ((inputDevices) ? inputDevices[i].channelCount : inputChannelCount) );

            if( result != paNoError )
                goto error;
        }  
    }

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        stream->outputBuffers = (WAVEHDR**)PaUtil_AllocateMemory( sizeof(WAVEHDR*) * stream->numOutputDevices );
        if( stream->outputBuffers == 0 )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i =0; i < stream->numOutputDevices; ++i )
            stream->outputBuffers[i] = 0;

        stream->numOutputBuffers = numHostOutputBuffers;
        
        for( i=0; i < stream->numOutputDevices; ++i )
        {
            int hostOutputBufferBytes = Pa_GetSampleSize( hostOutputSampleFormat ) *
                    framesPerHostOutputBuffer *
                    ((outputDevices) ? outputDevices[i].channelCount  : outputChannelCount);
            if( hostOutputBufferBytes < 0 )
            {
                result = paInternalError;
                goto error;
            }

            result = InitializeBufferSet( &stream->outputBuffers[i], numHostOutputBuffers, hostOutputBufferBytes,
                                 0 /* not isInput */,
                                 (MmeHandle)stream->hWaveOuts[i],
                                 ((outputDevices) ? outputDevices[i].channelCount  : outputChannelCount) );

            if( result != paNoError )
                goto error;
        }
    }

    stream->abortEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
    if( stream->abortEvent == NULL )
    {
        result = paUnanticipatedHostError;
        PA_MME_SET_LAST_SYSTEM_ERROR( GetLastError() );
        goto error;
    }
    abortEventInited = 1;

    InitializeCriticalSection( &stream->lock );
    lockInited = 1;

    if( PA_IS_OUTPUT_STREAM_(stream) )
        stream->allBuffersDurationMs = (DWORD) (1000.0 * (framesPerHostOutputBuffer * stream->numOutputBuffers) / sampleRate);
    else
        stream->allBuffersDurationMs = (DWORD) (1000.0 * (framesPerHostInputBuffer * stream->numInputBuffers) / sampleRate);


    *s = (PaStream*)stream;

    return result;

error:
    if( lockInited )
        DeleteCriticalSection( &stream->lock );

    if( abortEventInited )
        CloseHandle( stream->abortEvent );


    if( stream->inputBuffers )
    {
        for( i =0 ; i< stream->numInputDevices; ++i )
        {
            if( stream->inputBuffers[i] )
            {
                TerminateBufferSet( &stream->inputBuffers[i], stream->numInputBuffers,
                        1 /* isInput */, (MmeHandle)stream->hWaveIns[i] );
            }
        }

        PaUtil_FreeMemory( stream->inputBuffers );
    }
    
    if( stream->outputBuffers )
    {
        for( i =0 ; i< stream->numOutputDevices; ++i )
        {
            if( stream->outputBuffers[i] )
            {
                TerminateBufferSet( &stream->outputBuffers[i], stream->numOutputBuffers,
                    0 /* not isInput */, (MmeHandle)stream->hWaveOuts[i] );
            }
        }

        PaUtil_FreeMemory( stream->outputBuffers );
    }

    if( stream->hWaveIns )
    {
        for( i =0 ; i< stream->numInputDevices; ++i )
        {
            if( stream->hWaveIns[i] )
                waveInClose( stream->hWaveIns[i] );
        }

        PaUtil_FreeMemory( stream->hWaveIns );
    }

    if( stream->hWaveOuts )
    {
        for( i =0 ; i< stream->numOutputDevices; ++i )
        {
            if( stream->hWaveOuts[i] )
                waveOutClose( stream->hWaveOuts[i] );
        }

        PaUtil_FreeMemory( stream->hWaveOuts );
    }

    if( bufferEventInited )
        CloseHandle( stream->bufferEvent );

    if( stream )
        PaUtil_FreeMemory( stream );

    return result;
}


/* return non-zero if any output buffers are queued */
static int OutputBuffersAreQueued( PaWinMmeStream *stream )
{
    int result = 0;
    unsigned int i, j;

    if( PA_IS_OUTPUT_STREAM_( stream ) )
    {
        for( i=0; i<stream->numOutputBuffers; ++i )
        {
            for( j=0; j < stream->numOutputDevices; ++j )
            {
                if( !( stream->outputBuffers[ j ][ i ].dwFlags & WHDR_DONE) )
                {
                    result++;
                }
            }
        }
    }

    return result;
}


static PaError AdvanceToNextInputBuffer( PaWinMmeStream *stream )
{
    PaError result = paNoError;
    MMRESULT mmresult;
    unsigned int i;

    for( i=0; i< stream->numInputDevices; ++i )
    {
        mmresult = waveInAddBuffer( stream->hWaveIns[i],
                                    &stream->inputBuffers[i][ stream->currentInputBufferIndex ],
                                    sizeof(WAVEHDR) );
        if( mmresult != MMSYSERR_NOERROR )
        {
            result = paUnanticipatedHostError;
            PA_MME_SET_LAST_WAVEIN_ERROR( mmresult );
        }
    }
    stream->currentInputBufferIndex = (stream->currentInputBufferIndex+1 >= stream->numInputBuffers) ?
                                      0 : stream->currentInputBufferIndex+1;

    stream->framesUsedInCurrentInputBuffer = 0;

    return result;
}


static PaError AdvanceToNextOutputBuffer( PaWinMmeStream *stream )
{
    PaError result = paNoError;
    MMRESULT mmresult;
    unsigned int i;

    for( i=0; i< stream->numOutputDevices; ++i )
    {
        mmresult = waveOutWrite( stream->hWaveOuts[i],
                                 &stream->outputBuffers[i][ stream->currentOutputBufferIndex ],
                                 sizeof(WAVEHDR) );
        if( mmresult != MMSYSERR_NOERROR )
        {
            result = paUnanticipatedHostError;
            PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult );
        }
    }

    stream->currentOutputBufferIndex = (stream->currentOutputBufferIndex+1 >= stream->numOutputBuffers) ?
                                       0 : stream->currentOutputBufferIndex+1;

    stream->framesUsedInCurrentOutputBuffer = 0;
    
    return result;
}


static DWORD WINAPI ProcessingThreadProc( void *pArg )
{
    PaWinMmeStream *stream = (PaWinMmeStream *)pArg;
    HANDLE events[2];
    int numEvents = 0;
    DWORD result = paNoError;
    DWORD waitResult;
    DWORD timeout = stream->allBuffersDurationMs * 0.5;
    DWORD numTimeouts = 0;
    int hostBuffersAvailable;
    signed int hostInputBufferIndex, hostOutputBufferIndex;
    int callbackResult;
    int done = 0;
    unsigned int channel, i, j;
    unsigned long framesProcessed;
    
    /* prepare event array for call to WaitForMultipleObjects() */
    events[numEvents++] = stream->bufferEvent;
    events[numEvents++] = stream->abortEvent;

    /* loop until something causes us to stop */
    while( !done )
    {
        /* wait for MME to signal that a buffer is available, or for
            the PA abort event to be signaled */
        waitResult = WaitForMultipleObjects( numEvents, events, FALSE, timeout );
        if( waitResult == WAIT_FAILED )
        {
            result = paUnanticipatedHostError;
            /* FIXME/REVIEW: can't return host error info from an asyncronous thread */
            done = 1;
        }
        else if( waitResult == WAIT_TIMEOUT )
        {
            /* if a timeout is encountered, continue */
            numTimeouts += 1;
        }

        if( stream->abortProcessing )
        {
            /* Pa_AbortStream() has been called, stop processing immediately */
            done = 1;
        }
        else if( stream->stopProcessing )
        {
            /* Pa_StopStream() has been called or the user callback returned
                non-zero, processing will continue until all output buffers
                are marked as done. The stream will stop immediately if it
                is input-only.
            */

            if( !OutputBuffersAreQueued( stream ) )
            {
                done = 1; /* Will cause thread to return. */
            }
        }
        else
        {
            hostBuffersAvailable = 1;

            /* process all available host buffers */
            do
            {
                hostInputBufferIndex = -1;
                hostOutputBufferIndex = -1;

                if( PA_IS_INPUT_STREAM_(stream))
                {
                    hostInputBufferIndex = stream->currentInputBufferIndex;
                    for( i=0; i<stream->numInputDevices; ++i )
                    {
                        if( !(stream->inputBuffers[i][ stream->currentInputBufferIndex ].dwFlags & WHDR_DONE) )
                        {
                            hostInputBufferIndex = -1;
                            break;
                        }         
                    }

                    if( hostInputBufferIndex != -1 )
                    {
                        /* if all of the other buffers are also ready then we dicard all but the
                            most recent. */
                        int inputCatchUp = 1;

                        for( i=0; i < stream->numInputBuffers && inputCatchUp == 1; ++i )
                        {
                            for( j=0; j<stream->numInputDevices; ++j )
                            {
                                if( !(stream->inputBuffers[ j ][ i ].dwFlags & WHDR_DONE) )
                                {
                                    inputCatchUp = 0;
                                    break;
                                }
                            }
                        }

                        if( inputCatchUp )
                        {
                            for( i=0; i < stream->numInputBuffers - 1; ++i )
                            {
                                result = AdvanceToNextInputBuffer( stream );
                                if( result != paNoError )
                                    done = 1;
                            }
                        }
                    }
                }

                if( PA_IS_OUTPUT_STREAM_(stream) )
                {
                    hostOutputBufferIndex = stream->currentOutputBufferIndex;
                    for( i=0; i<stream->numOutputDevices; ++i )
                    {
                        if( !(stream->outputBuffers[i][ stream->currentOutputBufferIndex ].dwFlags & WHDR_DONE) )
                        {
                            hostOutputBufferIndex = -1;
                            break;
                        }
                    }

                    if( hostOutputBufferIndex != - 1 )
                    {
                        /* if all of the other buffers are also ready, catch up by copying
                            the most recently generated buffer into all but one of the output
                            buffers */
                        int outputCatchUp = 1;

                        for( i=0; i < stream->numOutputBuffers && outputCatchUp == 1; ++i )
                        {
                            for( j=0; j<stream->numOutputDevices; ++j )
                            {
                                if( !(stream->outputBuffers[ j ][ i ].dwFlags & WHDR_DONE) )
                                {
                                    outputCatchUp = 0;
                                    break;
                                }
                            }
                        }

                        if( outputCatchUp )
                        {
                            /* FIXME: this is an output underflow buffer slip and should be flagged as such */
                            unsigned int previousBufferIndex = (stream->currentOutputBufferIndex==0)
                                    ? stream->numOutputBuffers - 1
                                    : stream->currentOutputBufferIndex - 1;

                            for( i=0; i < stream->numOutputBuffers - 1; ++i )
                            {
                                for( j=0; j<stream->numOutputDevices; ++j )
                                {
                                    if( stream->outputBuffers[j][ stream->currentOutputBufferIndex ].lpData
                                            != stream->outputBuffers[j][ previousBufferIndex ].lpData )
                                    {
                                        CopyMemory( stream->outputBuffers[j][ stream->currentOutputBufferIndex ].lpData,
                                                    stream->outputBuffers[j][ previousBufferIndex ].lpData,
                                                    stream->outputBuffers[j][ stream->currentOutputBufferIndex ].dwBufferLength );
                                    }
                                }

                                result = AdvanceToNextOutputBuffer( stream );
                                if( result != paNoError )
                                    done = 1;
                            }
                        }
                    }
                }

               
                if( (PA_IS_FULL_DUPLEX_STREAM_(stream) && hostInputBufferIndex != -1 && hostOutputBufferIndex != -1) ||
                        (!PA_IS_FULL_DUPLEX_STREAM_(stream) && ( hostInputBufferIndex != -1 || hostOutputBufferIndex != -1 ) ) )
                {
                    PaStreamCallbackTimeInfo timeInfo = {0,0,0}; /* @todo implement inputBufferAdcTime and currentTime */


                    if( hostOutputBufferIndex != -1 ){
                        MMTIME time;
                        double now;
                        long totalRingFrames;
                        long ringPosition;
                        long playbackPosition;

                        time.wType = TIME_SAMPLES;
                        waveOutGetPosition( stream->hWaveOuts[0], &time, sizeof(MMTIME) );
                        now = PaUtil_GetTime();

                        totalRingFrames = stream->numOutputBuffers * stream->bufferProcessor.framesPerHostBuffer;

                        ringPosition = stream->currentOutputBufferIndex * stream->bufferProcessor.framesPerHostBuffer;
                        
                        playbackPosition = time.u.sample % totalRingFrames;

                        if( playbackPosition >= ringPosition ){
                            timeInfo.outputBufferDacTime =
                                    now + ((double)( ringPosition + (totalRingFrames - playbackPosition) ) * stream->bufferProcessor.samplePeriod );
                        }else{
                            timeInfo.outputBufferDacTime =
                                    now + ((double)( ringPosition - playbackPosition ) * stream->bufferProcessor.samplePeriod );
                        }
                    }


                    PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

                    PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo );

                    if( hostInputBufferIndex != -1 )
                    {
                        PaUtil_SetInputFrameCount( &stream->bufferProcessor, 0 /* default to host buffer size */ );

                        channel = 0;
                        for( i=0; i<stream->numInputDevices; ++i )
                        {
                             /* we have stored the number of channels in the buffer in dwUser */
                            int channelCount = stream->inputBuffers[i][ hostInputBufferIndex ].dwUser;
                            
                            PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor, channel,
                                    stream->inputBuffers[i][ hostInputBufferIndex ].lpData +
                                        stream->framesUsedInCurrentInputBuffer * channelCount *
                                        stream->bufferProcessor.bytesPerHostInputSample,
                                    channelCount );
                                    

                            channel += channelCount;
                        }
                    }

                    if( hostOutputBufferIndex != -1 )
                    {
                        PaUtil_SetOutputFrameCount( &stream->bufferProcessor, 0 /* default to host buffer size */ );
                        
                        channel = 0;
                        for( i=0; i<stream->numOutputDevices; ++i )
                        {
                            /* we have stored the number of channels in the buffer in dwUser */
                            int channelCount = stream->outputBuffers[i][ hostOutputBufferIndex ].dwUser;

                            PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor, channel,
                                    stream->outputBuffers[i][ hostOutputBufferIndex ].lpData +
                                        stream->framesUsedInCurrentOutputBuffer * channelCount *
                                        stream->bufferProcessor.bytesPerHostOutputSample,
                                    channelCount );

                            /* we have stored the number of channels in the buffer in dwUser */
                            channel += channelCount;
                        }
                    }
                    
                    framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &callbackResult );

                    stream->framesUsedInCurrentInputBuffer += framesProcessed;
                    stream->framesUsedInCurrentOutputBuffer += framesProcessed;

                    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

                    if( callbackResult == paContinue )
                    {
                        /* nothing special to do */
                    }
                    else if( callbackResult == paAbort )
                    {
                        stream->abortProcessing = 1;
                        done = 1;
                        /* FIXME: should probably do a reset here */
                        result = paNoError;
                    }
                    else
                    {
                        /* User cllback has asked us to stop with paComplete or other non-zero value */
                        stream->stopProcessing = 1; /* stop once currently queued audio has finished */
                        result = paNoError;
                    }

                    /*
                    FIXME: the following code is incorrect, because stopProcessing should
                    still queue the current buffer.
                    */
                    if( stream->stopProcessing == 0 && stream->abortProcessing == 0 )
                    {
                        if( stream->throttleProcessingThreadOnOverload != 0 )
                        {
                            if( PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer ) > 1. )
                            {
                                if( stream->processingThreadPriority != stream->throttledThreadPriority )
                                {
                                    SetThreadPriority( stream->processingThread, stream->throttledThreadPriority );
                                    stream->processingThreadPriority = stream->throttledThreadPriority;
                                }

                                /* sleep for a quater of a buffer's duration to give other processes a go */
                                Sleep( stream->bufferProcessor.framesPerHostBuffer *
                                        stream->bufferProcessor.samplePeriod * .25 );
                            }
                            else
                            {
                                if( stream->processingThreadPriority != stream->highThreadPriority )
                                {
                                    SetThreadPriority( stream->processingThread, stream->highThreadPriority );
                                    stream->processingThreadPriority = stream->highThreadPriority;
                                }
                            }
                        }

                        if( PA_IS_INPUT_STREAM_(stream) &&
                                stream->framesUsedInCurrentInputBuffer == stream->framesPerInputBuffer )
                        {
                            result = AdvanceToNextInputBuffer( stream );
                            if( result != paNoError )
                                done = 1;
                        }

                        if( PA_IS_OUTPUT_STREAM_(stream) &&
                                stream->framesUsedInCurrentOutputBuffer == stream->framesPerOutputBuffer )
                        {
                            result = AdvanceToNextOutputBuffer( stream );
                            if( result != paNoError )
                                done = 1;
                        }
                    }
                }
                else
                {
                    hostBuffersAvailable = 0;
                }
            }
            while( hostBuffersAvailable &&
                    stream->stopProcessing == 0 &&
                    stream->abortProcessing == 0 &&
                    !done );
        }

        result = UpdateStreamTime( stream );
        if( result != paNoError )
            done = 1;
    }

    stream->isActive = 0;

    if( stream->streamRepresentation.streamFinishedCallback != 0 )
        stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
    
    return result;
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaWinMmeStream *stream = (PaWinMmeStream*)s;
    MMRESULT mmresult;
    unsigned int i;

    if( PA_IS_INPUT_STREAM_(stream) )
    {
        for( i=0; i<stream->numInputDevices; ++i )
        {
            TerminateBufferSet( &stream->inputBuffers[i], stream->numInputBuffers,
                    1 /* isInput */, (MmeHandle)stream->hWaveIns[i] );
        }

        PaUtil_FreeMemory( stream->inputBuffers );
    }
  
    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        for( i=0; i<stream->numOutputDevices; ++i )
        {
            TerminateBufferSet( &stream->outputBuffers[i], stream->numOutputBuffers,
                    0 /* not isInput */, (MmeHandle)stream->hWaveOuts[i] );
        }
        
        PaUtil_FreeMemory( stream->outputBuffers );
    }
  

    if( PA_IS_INPUT_STREAM_(stream) )
    {
        for( i=0; i<stream->numInputDevices; ++i )
        {
            mmresult = waveInClose( stream->hWaveIns[i] );
            if( mmresult != MMSYSERR_NOERROR )
            {
                result = paUnanticipatedHostError;
                PA_MME_SET_LAST_WAVEIN_ERROR( mmresult );
                goto error;
            }
        }

        PaUtil_FreeMemory( stream->hWaveIns );
    }

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        for( i=0; i<stream->numOutputDevices; ++i )
        {
            mmresult = waveOutClose( stream->hWaveOuts[i] );
            if( mmresult != MMSYSERR_NOERROR )
            {
                result = paUnanticipatedHostError;
                PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult );
                goto error;
            }
        }

        PaUtil_FreeMemory( stream->hWaveOuts );
    }

    if( CloseHandle( stream->bufferEvent ) == 0 )
    {
        result = paUnanticipatedHostError;
        PA_MME_SET_LAST_SYSTEM_ERROR( GetLastError() );
        goto error;
    }

    if( CloseHandle( stream->abortEvent ) == 0 )
    {
        result = paUnanticipatedHostError;
        PA_MME_SET_LAST_SYSTEM_ERROR( GetLastError() );
        goto error;
    }

    DeleteCriticalSection( &stream->lock );

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );

error:
    /* FIXME: consider how to best clean up on failure */
    return result;
}


static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinMmeStream *stream = (PaWinMmeStream*)s;
    MMRESULT mmresult;
    unsigned int i, j;

    if( PA_IS_INPUT_STREAM_(stream) )
    {
        for( i=0; i<stream->numInputBuffers; ++i )
        {
            for( j=0; j<stream->numInputDevices; ++j )
            {
                mmresult = waveInAddBuffer( stream->hWaveIns[j], &stream->inputBuffers[j][i], sizeof(WAVEHDR) );
                if( mmresult != MMSYSERR_NOERROR )
                {
                    result = paUnanticipatedHostError;
                    PA_MME_SET_LAST_WAVEIN_ERROR( mmresult );
                    goto error;
                }
            }
        }
        stream->currentInputBufferIndex = 0;
        stream->framesUsedInCurrentInputBuffer = 0;
    }

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        for( i=0; i<stream->numOutputDevices; ++i )
        {
            if( (mmresult = waveOutPause( stream->hWaveOuts[i] )) != MMSYSERR_NOERROR )
            {
                result = paUnanticipatedHostError;
                PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult );
                goto error;
            }
        }

        for( i=0; i<stream->numOutputBuffers; ++i )
        {
            for( j=0; j<stream->numOutputDevices; ++j )
            {
                ZeroMemory( stream->outputBuffers[j][i].lpData, stream->outputBuffers[j][i].dwBufferLength );
                mmresult = waveOutWrite( stream->hWaveOuts[j], &stream->outputBuffers[j][i], sizeof(WAVEHDR) );
                if( mmresult != MMSYSERR_NOERROR )
                {
                    result = paUnanticipatedHostError;
                    PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult );
                    goto error;
                }
            }
        }
        stream->currentOutputBufferIndex = 0;
        stream->framesUsedInCurrentOutputBuffer = 0;
    }

    stream->streamPosition = 0.;
    stream->previousStreamPosition = 0;

    stream->isActive = 1;
    stream->stopProcessing = 0;
    stream->abortProcessing = 0;

    if( ResetEvent( stream->bufferEvent ) == 0 )
    {
        result = paUnanticipatedHostError;
        PA_MME_SET_LAST_SYSTEM_ERROR( GetLastError() );
        goto error;
    }

    if( ResetEvent( stream->abortEvent ) == 0 )
    {
        result = paUnanticipatedHostError;
        PA_MME_SET_LAST_SYSTEM_ERROR( GetLastError() );
        goto error;
    }

    /* Create thread that waits for audio buffers to be ready for processing. */
    stream->processingThread = CreateThread( 0, 0, ProcessingThreadProc, stream, 0, &stream->processingThreadId );
    if( !stream->processingThread )
    {
        result = paUnanticipatedHostError;
        PA_MME_SET_LAST_SYSTEM_ERROR( GetLastError() );
        goto error;
    }

    /* I used to pass the thread which was failing. I now pass GetCurrentProcess().
     * This fix could improve latency for some applications. It could also result in CPU
     * starvation if the callback did too much processing.
     * I also added result checks, so we might see more failures at initialization.
     * Thanks to Alberto di Bene for spotting this.
     */
    /* REVIEW: should we reset the priority class when the stream has stopped?
        - would be best to refcount priority boosts incase more than one
        stream is open
    */

    if( !stream->noHighPriorityProcessClass )
    {
        if( !SetPriorityClass( GetCurrentProcess(), HIGH_PRIORITY_CLASS ) ) /* PLB20010816 */
        {
            result = paUnanticipatedHostError;
            PA_MME_SET_LAST_SYSTEM_ERROR( GetLastError() );
            goto error;
        }
    }

    if( stream->useTimeCriticalProcessingThreadPriority )
        stream->highThreadPriority = THREAD_PRIORITY_TIME_CRITICAL;
    else
        stream->highThreadPriority = THREAD_PRIORITY_HIGHEST;

    stream->throttledThreadPriority = THREAD_PRIORITY_NORMAL;

    if( !SetThreadPriority( stream->processingThread, stream->highThreadPriority ) )
    {
        result = paUnanticipatedHostError;
        PA_MME_SET_LAST_SYSTEM_ERROR( GetLastError() );
        goto error;
    }
    stream->processingThreadPriority = stream->highThreadPriority;


    if( PA_IS_INPUT_STREAM_(stream) )
    {
        for( i=0; i < stream->numInputDevices; ++i )
        {
            mmresult = waveInStart( stream->hWaveIns[i] );
            PA_DEBUG(("Pa_StartStream: waveInStart returned = 0x%X.\n", mmresult));
            if( mmresult != MMSYSERR_NOERROR )
            {
                result = paUnanticipatedHostError;
                PA_MME_SET_LAST_WAVEIN_ERROR( mmresult );
                goto error;
            }
        }
    }

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        for( i=0; i < stream->numOutputDevices; ++i )
        {
            if( (mmresult = waveOutRestart( stream->hWaveOuts[i] )) != MMSYSERR_NOERROR )
            {
                result = paUnanticipatedHostError;
                PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult );
                goto error;
            }
        }
    }

    return result;

error:
    /* FIXME: implement recovery as best we can
    This should involve rolling back to a state as-if this function had never been called
    */
    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinMmeStream *stream = (PaWinMmeStream*)s;
    int timeout;
    DWORD waitResult;
    MMRESULT mmresult;
    unsigned int i;
    
    /*
        FIXME: the error checking in this function needs review. the basic
        idea is to return from this function in a known state - for example
        there is no point avoiding calling waveInReset just because
        the thread times out.
    */


    /* Tell processing thread to stop generating more data and to let current data play out. */
    stream->stopProcessing = 1;

    /* Calculate timeOut longer than longest time it could take to return all buffers. */
    timeout = stream->allBuffersDurationMs * 1.5;
    if( timeout < PA_MIN_TIMEOUT_MSEC_ )
        timeout = PA_MIN_TIMEOUT_MSEC_;

    PA_DEBUG(("WinMME StopStream: waiting for background thread.\n"));

    waitResult = WaitForSingleObject( stream->processingThread, timeout );
    if( waitResult == WAIT_TIMEOUT )
    {
        /* try to abort */
        stream->abortProcessing = 1;
        SetEvent( stream->abortEvent );
        waitResult = WaitForSingleObject( stream->processingThread, timeout );
        if( waitResult == WAIT_TIMEOUT )
        {
            PA_DEBUG(("WinMME StopStream: timed out while waiting for background thread to finish.\n"));
            result = paTimedOut;
        }
    }

    CloseHandle( stream->processingThread );
    stream->processingThread = NULL;

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        for( i =0; i < stream->numOutputDevices; ++i )
        {
            mmresult = waveOutReset( stream->hWaveOuts[i] );
            if( mmresult != MMSYSERR_NOERROR )
            {
                result = paUnanticipatedHostError;
                PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult );
            }
        }
    }

    if( PA_IS_INPUT_STREAM_(stream) )
    {
        for( i=0; i < stream->numInputDevices; ++i )
        {
            mmresult = waveInReset( stream->hWaveIns[i] );
            if( mmresult != MMSYSERR_NOERROR )
            {
                result = paUnanticipatedHostError;
                PA_MME_SET_LAST_WAVEIN_ERROR( mmresult );
            }
        }
    }

    stream->isActive = 0;

    return result;
}


static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinMmeStream *stream = (PaWinMmeStream*)s;
    int timeout;
    DWORD waitResult;
    MMRESULT mmresult;
    unsigned int i;
    
    /*
        FIXME: the error checking in this function needs review. the basic
        idea is to return from this function in a known state - for example
        there is no point avoiding calling waveInReset just because
        the thread times out.
    */

    /* Tell processing thread to abort immediately */
    stream->abortProcessing = 1;
    SetEvent( stream->abortEvent );

    /* Calculate timeOut longer than longest time it could take to return all buffers. */
    timeout = stream->allBuffersDurationMs * 1.5;
    if( timeout < PA_MIN_TIMEOUT_MSEC_ )
        timeout = PA_MIN_TIMEOUT_MSEC_;

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        for( i =0; i < stream->numOutputDevices; ++i )
        {
            mmresult = waveOutReset( stream->hWaveOuts[i] );
            if( mmresult != MMSYSERR_NOERROR )
            {
                PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult );
                return paUnanticipatedHostError;
            }
        }
    }

    if( PA_IS_INPUT_STREAM_(stream) )
    {
        for( i=0; i < stream->numInputDevices; ++i )
        {
            mmresult = waveInReset( stream->hWaveIns[i] );
            if( mmresult != MMSYSERR_NOERROR )
            {
                PA_MME_SET_LAST_WAVEIN_ERROR( mmresult );
                return paUnanticipatedHostError;
            }
        }
    }


    PA_DEBUG(("WinMME AbortStream: waiting for background thread.\n"));

    waitResult = WaitForSingleObject( stream->processingThread, timeout );
    if( waitResult == WAIT_TIMEOUT )
    {
        PA_DEBUG(("WinMME AbortStream: timed out while waiting for background thread to finish.\n"));
        return paTimedOut;
    }

    CloseHandle( stream->processingThread );
    stream->processingThread = NULL;

    stream->isActive = 0;

    return result;
}


static PaError IsStreamStopped( PaStream *s )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    return ( stream->processingThread == NULL );
}


static PaError IsStreamActive( PaStream *s )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    return stream->isActive;
}


/*  UpdateStreamTime() must be called periodically because mmtime.u.sample
    is a DWORD and can wrap and lose sync after a few hours.
 */
static PaError UpdateStreamTime( PaWinMmeStream *stream )
{
    MMRESULT  mmresult;
    MMTIME    mmtime;
    mmtime.wType = TIME_SAMPLES;

    if( stream->hWaveOuts )
    {
        /* assume that all devices have the same position */
        mmresult = waveOutGetPosition( stream->hWaveOuts[0], &mmtime, sizeof(mmtime) );

        if( mmresult != MMSYSERR_NOERROR )
        {
            PA_MME_SET_LAST_WAVEOUT_ERROR( mmresult );
            return paUnanticipatedHostError;
        }
    }
    else
    {
        /* assume that all devices have the same position */
        mmresult = waveInGetPosition( stream->hWaveIns[0], &mmtime, sizeof(mmtime) );

        if( mmresult != MMSYSERR_NOERROR )
        {
            PA_MME_SET_LAST_WAVEIN_ERROR( mmresult );
            return paUnanticipatedHostError;
        }
    }


    /* This data has two variables and is shared by foreground and background.
     * So we need to make it thread safe. */
    EnterCriticalSection( &stream->lock );
    stream->streamPosition += ((long)mmtime.u.sample) - stream->previousStreamPosition;
    stream->previousStreamPosition = (long)mmtime.u.sample;
    LeaveCriticalSection( &stream->lock );

    return paNoError;
}


static PaTime GetStreamTime( PaStream *s )
{
/*
    new behavior for GetStreamTime is to return a stream based seconds clock
    used for the outTime parameter to the callback.
    FIXME: delete this comment when the other unnecessary related code has
    been cleaned from this file.

    PaWinMmeStream *stream = (PaWinMmeStream*)s;
    PaError error = UpdateStreamTime( stream );

    if( error == paNoError )
        return stream->streamPosition;
    else
        return 0;
*/
    (void) s; /* unused parameter */
    return PaUtil_GetTime();
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
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
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    (void) stream; /* unused parameters */
    (void) buffer;
    (void) frames;
    
    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            void *buffer,
                            unsigned long frames )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    (void) stream; /* unused parameters */
    (void) buffer;
    (void) frames;
    
    return paNoError;
}


static signed long GetStreamReadAvailable( PaStream* s )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    (void) stream; /* unused parameter */

    return 0;
}


static signed long GetStreamWriteAvailable( PaStream* s )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    (void) stream; /* unused parameter */

    return 0;
}


