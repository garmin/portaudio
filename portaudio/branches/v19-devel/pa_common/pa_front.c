/*
 * $Id$
 * Portable Audio I/O Library Multi-Host API front end
 * Validate function parameters and manage multiple host APIs.
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
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
 */

/* doxygen index page */
/** @mainpage

PortAudio is an open-source cross-platform �C� library for audio input
and output. It is designed to simplify the porting of audio applications
between various platforms, and also to simplify the development of audio
software in general by hiding the complexities of device interfacing.

See the PortAudio website for further information http://www.portaudio.com/

This documentation pertains to PortAudio V19, API version 2.0 which is
currently under development. API version 2.0 differs in a number of ways from
previous versions, please consult the enhancement proposals for further details:
http://www.portaudio.com/docs/proposals/index.html

This documentation is under construction. Things you might be interested in
include:

- The PortAudio API 2.0 documented in portaudio.h

- The possibly incomplete and totally unorganised <a href="todo.html">Todo List</a>
*/

#include <stdio.h>
#include <stdarg.h>
#include <memory.h>
#include <string.h>

#include "portaudio.h"
#include "pa_util.h"
#include "pa_hostapi.h"
#include "pa_stream.h"

#include "pa_trace.h"


#define PA_VERSION_  1899
#define PA_VERSION_TEXT_ "PortAudio V19-devel"



/* #define PA_LOG_API_CALLS  */

/*
    The basic format for log messages is as follows:
 
    - entry (void function):
 
    "FunctionName called.\n"
 
    - entry (non void function):
 
    "FunctionName called:\n"
    "\tParam1Type param1: param1Value\n"
    "\tParam2Type param2: param2Value\n"      (etc...)
 
 
    - exit (no return value)
 
    "FunctionName returned.\n"
 
    - exit (simple return value)
 
    "FunctionName returned:\n"
    "\tReturnType: returnValue\n\n"
 
    if the return type is an error code, the error text is displayed in ()
 
    if the return type is not an error code, but has taken a special value
    because an error occurred, then the reason for the error is shown in []
 
    if the return type is a struct ptr, the struct is dumped.
 
    see the code for more detailed examples
*/

int Pa_GetVersion( void )
{
    return PA_VERSION_;
}


const char* Pa_GetVersionText( void )
{
    return PA_VERSION_TEXT_;
}



#define PA_LAST_HOST_ERROR_TEXT_LENGTH_  1024

static char lastHostErrorText_[ PA_LAST_HOST_ERROR_TEXT_LENGTH_ + 1 ] = {0};

static PaHostErrorInfo lastHostErrorInfo_ = { -1, 0, lastHostErrorText_ };


void PaUtil_SetLastHostErrorInfo( PaHostApiTypeId hostApiType, long errorCode,
        const char *errorText )
{
    lastHostErrorInfo_.hostApiType = hostApiType;
    lastHostErrorInfo_.errorCode = errorCode;

    strncpy( lastHostErrorText_, errorText, PA_LAST_HOST_ERROR_TEXT_LENGTH_ );
}


void PaUtil_DebugPrint( const char *format, ... )
{
    va_list ap;

    va_start( ap, format );
    vfprintf( stderr, format, ap );
    va_end( ap );

    fflush( stderr );
}


static PaUtilHostApiRepresentation **hostApis_ = 0;
static int hostApisCount_ = 0;
static int initializationCount_ = 0;
static int deviceCount_ = 0;

PaUtilStreamRepresentation *firstOpenStream_ = NULL;


#define PA_IS_INITIALISED_ (initializationCount_ != 0)


static int CountHostApiInitializers( void )
{
    int result = 0;

    while( paHostApiInitializers[ result ] != 0 )
        ++result;
    return result;
}


static void TerminateHostApis( void )
{
    /* terminate in reverse order from initialization */

    while( hostApisCount_ > 0 )
    {
        --hostApisCount_;
        hostApis_[hostApisCount_]->Terminate( hostApis_[hostApisCount_] );
    }
    hostApisCount_ = 0;
    deviceCount_ = 0;

    if( hostApis_ != 0 )
        PaUtil_FreeMemory( hostApis_ );
    hostApis_ = 0;
}


static PaError InitializeHostApis( void )
{
    PaError result = paNoError;
    int i, initializerCount, baseDeviceIndex;

    initializerCount = CountHostApiInitializers();

    hostApis_ = PaUtil_AllocateMemory( sizeof(PaUtilHostApiRepresentation*) * initializerCount );
    if( !hostApis_ )
    {
        result = paInsufficientMemory;
        goto error; 
    }

    hostApisCount_ = 0;
    deviceCount_ = 0;
    baseDeviceIndex = 0;

    for( i=0; i< initializerCount; ++i )
    {
        hostApis_[hostApisCount_] = NULL;
        result = paHostApiInitializers[i]( &hostApis_[hostApisCount_], hostApisCount_ );
        if( result != paNoError )
            goto error;

        if( hostApis_[hostApisCount_] )
        {

            hostApis_[hostApisCount_]->privatePaFrontInfo.baseDeviceIndex = baseDeviceIndex;

            if( hostApis_[hostApisCount_]->info.defaultInputDevice != paNoDevice )
                hostApis_[hostApisCount_]->info.defaultInputDevice += baseDeviceIndex;

            if( hostApis_[hostApisCount_]->info.defaultOutputDevice != paNoDevice )
                hostApis_[hostApisCount_]->info.defaultOutputDevice += baseDeviceIndex;

            baseDeviceIndex += hostApis_[hostApisCount_]->info.deviceCount;
            deviceCount_ += hostApis_[hostApisCount_]->info.deviceCount;

            ++hostApisCount_;
        }
    }

    return result;

error:
    TerminateHostApis();
    return result;
}


/*
    FindHostApi() finds the index of the host api to which
    <device> belongs and returns it. if <hostSpecificDeviceIndex> is
    non-null, the host specific device index is returned in it.
    returns -1 if <device> is out of range.
 
*/
static int FindHostApi( PaDeviceIndex device, int *hostSpecificDeviceIndex )
{
    int i=0;

    if( !PA_IS_INITIALISED_ )
        return -1;

    if( device < 0 )
        return -1;

    while( i < hostApisCount_
            && device >= hostApis_[i]->info.deviceCount )
    {

        device -= hostApis_[i]->info.deviceCount;
        ++i;
    }

    if( i >= hostApisCount_ )
        return -1;

    if( hostSpecificDeviceIndex )
        *hostSpecificDeviceIndex = device;

    return i;
}


static void AddOpenStream( PaStream* stream )
{
    ((PaUtilStreamRepresentation*)stream)->nextOpenStream = firstOpenStream_;
    firstOpenStream_ = (PaUtilStreamRepresentation*)stream;
}


static void RemoveOpenStream( PaStream* stream )
{
    PaUtilStreamRepresentation *previous = NULL;
    PaUtilStreamRepresentation *current = firstOpenStream_;

    while( current != NULL )
    {
        if( ((PaStream*)current) == stream )
        {
            if( previous == NULL )
            {
                firstOpenStream_ = current->nextOpenStream;
            }
            else
            {
                previous->nextOpenStream = current->nextOpenStream;
            }
            return;
        }
        else
        {
            previous = current;
            current = current->nextOpenStream;
        }
    }
}


static void CloseOpenStreams( void )
{
    /* we call Pa_CloseStream() here to ensure that the same destruction
        logic is used for automatically closed streams */

    while( firstOpenStream_ != NULL )
        Pa_CloseStream( firstOpenStream_ );
}


PaError Pa_Initialize( void )
{
    PaError result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint( "Pa_Initialize called.\n" );
#endif

    if( PA_IS_INITIALISED_ )
    {
        ++initializationCount_;
        result = paNoError;
    }
    else
    {
        PaUtil_InitializeClock();
        PaUtil_ResetTraceMessages();

        result = InitializeHostApis();
        if( result == paNoError )
            ++initializationCount_;
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint( "Pa_Initialize returned:\n" );
    PaUtil_DebugPrint( "\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_Terminate( void )
{
    PaError result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_Terminate called.\n" );
#endif

    if( PA_IS_INITIALISED_ )
    {
        if( --initializationCount_ == 0 )
        {
            CloseOpenStreams();

            TerminateHostApis();

            PaUtil_DumpTraceMessages();
        }
        result = paNoError;
    }
    else
    {
        result=  paNotInitialized;
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_Terminate returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


const PaHostErrorInfo* Pa_GetLastHostErrorInfo( void )
{
    return &lastHostErrorInfo_;
}


const char *Pa_GetErrorText( PaError errorNumber )
{
    const char *result;

    switch( errorNumber )
    {
    case paNoError:                  result = "Success"; break;
    case paNotInitialized:           result = "PortAudio not initialized"; break;
    /** @todo could catenate the last host error text to result in the case of paUnanticipatedHostError */
    case paUnanticipatedHostError:   result = "Unanticipated host error"; break;
    case paInvalidChannelCount:      result = "Invalid number of channels"; break;
    case paInvalidSampleRate:        result = "Invalid sample rate"; break;
    case paInvalidDevice:            result = "Invalid device"; break;
    case paInvalidFlag:              result = "Invalid flag"; break;
    case paSampleFormatNotSupported: result = "Sample format not supported"; break;
    case paBadIODeviceCombination:   result = "Illegal combination of I/O devices"; break;
    case paInsufficientMemory:       result = "Insufficient memory"; break;
    case paBufferTooBig:             result = "Buffer too big"; break;
    case paBufferTooSmall:           result = "Buffer too small"; break;
    case paNullCallback:             result = "No callback routine specified"; break;
    case paBadStreamPtr:             result = "Invalid stream pointer"; break;
    case paTimedOut:                 result = "Wait timed out"; break;
    case paInternalError:            result = "Internal PortAudio error"; break;
    case paDeviceUnavailable:        result = "Device unavailable"; break;
    case paIncompatibleHostApiSpecificStreamInfo:   result = "Incompatible host API specific stream info"; break;
    case paStreamIsStopped:          result = "Stream is stopped"; break;
    case paStreamIsNotStopped:       result = "Stream is not stopped"; break;
    default:                         result = "Illegal error number"; break;
    }
    return result;
}


PaHostApiIndex Pa_HostApiTypeIdToHostApiIndex( PaHostApiTypeId type )
{
    PaHostApiIndex result;
    int i;
    
#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_HostApiTypeIdToHostApiIndex called:\n" );
    PaUtil_DebugPrint("\PaHostApiTypeId type: %d\n", type );
#endif

    if( !PA_IS_INITIALISED_ )
    {

        result = -1;
        
#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiTypeIdToHostApiIndex returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiIndex: -1 [ PortAudio not initialized ]\n\n" );
#endif

    }
    else
    {
        result = -1;
        
        for( i=0; i < hostApisCount_; ++i )
        {
            if( hostApis_[i]->info.type == type )
            {
                result = i;
                break;
            }         
        }

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiTypeIdToHostApiIndex returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiIndex: %d\n\n", result );
#endif
    }

    return result;
}


PaError PaUtil_GetHostApiRepresentation( struct PaUtilHostApiRepresentation **hostApi,
        PaHostApiTypeId type )
{
    PaError result;
    int i;
    
    if( !PA_IS_INITIALISED_ )
    {
        result = paNotInitialized;
    }
    else
    {
        result = paInternalError; /* @todo should return host API not found */
                
        for( i=0; i < hostApisCount_; ++i )
        {
            if( hostApis_[i]->info.type == type )
            {
                *hostApi = hostApis_[i];
                result = paNoError;
                break;
            }
        }
    }

    return result;
}


PaError PaUtil_DeviceIndexToHostApiDeviceIndex(
        PaDeviceIndex *hostApiDevice, PaDeviceIndex device, struct PaUtilHostApiRepresentation *hostApi )
{
    PaError result;
    PaDeviceIndex x;
    
    x = device - hostApi->privatePaFrontInfo.baseDeviceIndex;

    if( x < 0 || x >= hostApi->info.deviceCount )
    {
        result = paInvalidDevice;
    }
    else
    {
        *hostApiDevice = x;
        result = paNoError;
    }

    return result;
}


PaHostApiIndex Pa_CountHostApis( void )
{
    int result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_CountHostApis called.\n" );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        result = paNotInitialized;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_CountHostApis returned:\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

        return result;
    }
    else
    {

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_CountHostApis returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiIndex %d\n\n", hostApisCount_ );
#endif

        return hostApisCount_;
    }
}


PaHostApiIndex Pa_GetDefaultHostApi( void )
{
    int result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetDefaultHostApi called.\n" );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        result = paNotInitialized;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetDefaultHostApi returned:\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

        return result;
    }
    else
    {
        result = paDefaultHostApiIndex;

        /* internal consistency check: make sure that the default host api
         index is within range */

        if( result < 0 || result >= hostApisCount_ )
        {
            result = paInternalError;
            
#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetDefaultHostApi returned:\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif
        }
        else
        {
#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetDefaultHostApi returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiIndex %d\n\n", result );
#endif
        }

        return result;
    }
}


const PaHostApiInfo* Pa_GetHostApiInfo( PaHostApiIndex hostApi )
{
    PaHostApiInfo *info;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetHostApiInfo called:\n" );
    PaUtil_DebugPrint("\tPaHostApiIndex hostApi: %d\n", hostApi );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        info = NULL;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetHostApiInfo returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiInfo*: NULL [ PortAudio not initialized ]\n\n" );
#endif

    }
    else if( hostApi < 0 || hostApi >= hostApisCount_ )
    {
        info = NULL;
        
#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetHostApiInfo returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiInfo*: NULL [ hostApi out of range ]\n\n" );
#endif

    }
    else
    {
        info = &hostApis_[hostApi]->info;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetHostApiInfo returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiInfo*: 0x%p\n", info );
        PaUtil_DebugPrint("\t{" );
        PaUtil_DebugPrint("\t\tint structVersion: %d\n", info->structVersion );
        PaUtil_DebugPrint("\t\tPaHostApiTypeId type: %d\n", info->type );
        PaUtil_DebugPrint("\t\tconst char *name: %s\n\n", info->name );
        PaUtil_DebugPrint("\t}\n\n" );
#endif

    }

     return info;
}


PaDeviceIndex Pa_HostApiDeviceIndexToDeviceIndex( PaHostApiIndex hostApi, int hostApiDeviceIndex )
{
    PaDeviceIndex result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_HostApiDeviceIndexToPaDeviceIndex called:\n" );
    PaUtil_DebugPrint("\tPaHostApiIndex hostApi: %d\n", hostApi );
    PaUtil_DebugPrint("\tint hostApiDeviceIndex: %d\n", hostApiDeviceIndex );
#endif


    if( !PA_IS_INITIALISED_ )
    {
        result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDeviceIndexToPaDeviceIndex returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ PortAudio not initialized ]\n\n" );
#endif

    }
    else
    {
        if( hostApi < 0 || hostApi >= hostApisCount_ )
        {
            result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDeviceIndexToPaDeviceIndex returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ hostApi out of range ]\n\n" );
#endif

        }
        else
        {
            if( hostApiDeviceIndex < 0 ||
                    hostApiDeviceIndex >= hostApis_[hostApi]->info.deviceCount )
            {
                result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDeviceIndexToPaDeviceIndex returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ hostApiDeviceIndex out of range ]\n\n" );
#endif

            }
            else
            {
                result = hostApis_[hostApi]->privatePaFrontInfo.baseDeviceIndex + hostApiDeviceIndex;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDeviceIndexToPaDeviceIndex returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: %d\n\n", result );
#endif
            }
        }
    }

    return result;
}


PaDeviceIndex Pa_CountDevices( void )
{
    PaDeviceIndex result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_CountDevices called.\n" );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_CountDevices returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: 0 [ PortAudio not initialized ]\n\n" );
#endif

    }
    else
    {
        result = deviceCount_;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_CountDevices returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: %d\n\n", result );
#endif

    }

    return result;
}


PaDeviceIndex Pa_GetDefaultInputDevice( void )
{
    PaHostApiIndex hostApi;
    PaDeviceIndex result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetDefaultInputDevice called.\n" );
#endif

    hostApi = Pa_GetDefaultHostApi();
    if( hostApi < 0 )
    {
        result = paNoDevice;
    }
    else
    {
        result = hostApis_[hostApi]->info.defaultInputDevice;
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetDefaultInputDevice returned:\n" );
    PaUtil_DebugPrint("\tPaDeviceIndex: %d\n\n", result );
#endif

    return result;
}


PaDeviceIndex Pa_GetDefaultOutputDevice( void )
{
    PaHostApiIndex hostApi;
    PaDeviceIndex result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetDefaultOutputDevice called.\n" );
#endif

    hostApi = Pa_GetDefaultHostApi();
    if( hostApi < 0 )
    {
        result = paNoDevice;
    }
    else
    {
        result = hostApis_[hostApi]->info.defaultOutputDevice;
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetDefaultOutputDevice returned:\n" );
    PaUtil_DebugPrint("\tPaDeviceIndex: %d\n\n", result );
#endif

    return result;
}


const PaDeviceInfo* Pa_GetDeviceInfo( PaDeviceIndex device )
{
    int hostSpecificDeviceIndex;
    int hostApiIndex = FindHostApi( device, &hostSpecificDeviceIndex );
    PaDeviceInfo *result;


#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetDeviceInfo called:\n" );
    PaUtil_DebugPrint("\tPaDeviceIndex device: %d\n", device );
#endif

    if( hostApiIndex < 0 )
    {
        result = NULL;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetDeviceInfo returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceInfo* NULL [ invalid device index ]\n\n" );
#endif

    }
    else
    {
        result = hostApis_[hostApiIndex]->deviceInfos[ hostSpecificDeviceIndex ];

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetDeviceInfo returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceInfo*: 0x%p:\n", result );
        PaUtil_DebugPrint("\t{" );

        PaUtil_DebugPrint("\t\tint structVersion: %d\n", result->structVersion );
        PaUtil_DebugPrint("\t\tconst char *name: %s\n", result->name );
        PaUtil_DebugPrint("\t\tPaHostApiIndex hostApi: %d\n", result->hostApi );
        PaUtil_DebugPrint("\t\tint maxInputChannels: %d\n", result->maxInputChannels );
        PaUtil_DebugPrint("\t\tint maxOutputChannels: %d\n", result->maxOutputChannels );
        PaUtil_DebugPrint("\t}\n\n" );
#endif

    }

    return result;
}


/*
    SampleFormatIsValid() returns 1 if sampleFormat is a sample format
    defined in portaudio.h, or 0 otherwise.
*/
static int SampleFormatIsValid( PaSampleFormat format )
{
    switch( format & ~paNonInterleaved )
    {
    case paFloat32: return 1;
    case paInt16: return 1;
    case paInt32: return 1;
    case paInt24: return 1;
    case paInt8: return 1;
    case paUInt8: return 1;
    case paCustomFormat: return 1;
    default: return 0;
    }
}

/*
    NOTE: make sure this validation list is kept syncronised with the one in
            pa_hostapi.h

    ValidateOpenStreamParameters() checks that parameters to Pa_OpenStream()
    conform to the expected values as described below. This function is
    also designed to be used with the proposed Pa_IsFormatSupported() function.
    
    There are basically two types of validation that could be performed:
    Generic conformance validation, and device capability mismatch
    validation. This function performs only generic conformance validation.
    Validation that would require knowledge of device capabilities is
    not performed because of potentially complex relationships between
    combinations of parameters - for example, even if the sampleRate
    seems ok, it might not be for a duplex stream - we have no way of
    checking this in an API-neutral way, so we don't try.
 
    On success the function returns PaNoError and fills in hostApi,
    hostApiInputDeviceID, and hostApiOutputDeviceID fields. On failure
    the function returns an error code indicating the first encountered
    parameter error.
 
 
    If ValidateOpenStreamParameters() returns paNoError, the following
    assertions are guaranteed to be true.
 
    - at least one of inputParameters & outputParmeters is valid (not NULL)

    - if inputParameters & outputParmeters are both valid, that
        inputParameters->device & outputParmeters->device  both use the same host api
 
    PaDeviceIndex inputParameters->device
        - is within range (0 to Pa_CountDevices-1) Or:
        - is paUseHostApiSpecificDeviceSpecification and
            inputParameters->hostApiSpecificStreamInfo is non-NULL and refers
            to a valid host api

    int inputParameters->channelCount
        - if inputParameters->device is not paUseHostApiSpecificDeviceSpecification, channelCount is > 0
        - upper bound is NOT validated against device capabilities
 
    PaSampleFormat inputParameters->sampleFormat
        - is one of the sample formats defined in portaudio.h

    void *inputParameters->hostApiSpecificStreamInfo
        - if supplied its hostApi field matches the input device's host Api
 
    PaDeviceIndex outputParmeters->device
        - is within range (0 to Pa_CountDevices-1)
 
    int outputParmeters->channelCount
        - if inputDevice is valid, channelCount is > 0
        - upper bound is NOT validated against device capabilities
 
    PaSampleFormat outputParmeters->sampleFormat
        - is one of the sample formats defined in portaudio.h
        
    void *outputParmeters->hostApiSpecificStreamInfo
        - if supplied its hostApi field matches the output device's host Api
 
    double sampleRate
        - is not an 'absurd' rate (less than 1000. or greater than 200000.)
        - sampleRate is NOT validated against device capabilities
 
    PaStreamFlags streamFlags
        - unused platform neutral flags are zero
*/
static PaError ValidateOpenStreamParameters(
    const PaStreamParameters *inputParameters,
    const PaStreamParameters *outputParameters,
    double sampleRate,
    PaStreamFlags streamFlags,
    PaUtilHostApiRepresentation **hostApi,
    PaDeviceIndex *hostApiInputDevice,
    PaDeviceIndex *hostApiOutputDevice )
{
    int inputHostApiIndex, outputHostApiIndex;

    if( (inputParameters == NULL) && (outputParameters == NULL) )
    {

        return paInvalidDevice; /* @todo should be a new error code "invalid device parameters" or something */

    }
    else
    {
        if( inputParameters == NULL )
        {
            *hostApiInputDevice = paNoDevice;
        }
        else if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
        {
            if( inputParameters->hostApiSpecificStreamInfo )
            {
                inputHostApiIndex = Pa_HostApiTypeIdToHostApiIndex(
                        ((PaUtilHostApiSpecificStreamInfoHeader*)inputParameters->hostApiSpecificStreamInfo)->hostApiType );

                if( inputHostApiIndex != -1 )
                {
                    *hostApiInputDevice = paUseHostApiSpecificDeviceSpecification;
                    *hostApi = hostApis_[inputHostApiIndex];
                }
                else
                {
                    return paInvalidDevice;
                }
            }
            else
            {
                return paInvalidDevice;
            }
        }
        else
        {
            if( inputParameters->device < 0 || inputParameters->device >= deviceCount_ )
                return paInvalidDevice;

            inputHostApiIndex = FindHostApi( inputParameters->device, hostApiInputDevice );
            if( inputHostApiIndex < 0 )
                return paInternalError;

            *hostApi = hostApis_[inputHostApiIndex];

            if( inputParameters->channelCount <= 0 )
                return paInvalidChannelCount;

            if( !SampleFormatIsValid( inputParameters->sampleFormat ) )
                return paSampleFormatNotSupported;

            if( inputParameters->hostApiSpecificStreamInfo != NULL )
            {
                if( ((PaUtilHostApiSpecificStreamInfoHeader*)inputParameters->hostApiSpecificStreamInfo)->hostApiType
                        != (*hostApi)->info.type )
                    return paIncompatibleHostApiSpecificStreamInfo;
            }
        }

        if( outputParameters == NULL )
        {
            *hostApiOutputDevice = paNoDevice;
        }
        else if( outputParameters->device == paUseHostApiSpecificDeviceSpecification  )
        {
            if( outputParameters->hostApiSpecificStreamInfo )
            {
                outputHostApiIndex = Pa_HostApiTypeIdToHostApiIndex(
                        ((PaUtilHostApiSpecificStreamInfoHeader*)outputParameters->hostApiSpecificStreamInfo)->hostApiType );

                if( outputHostApiIndex != -1 )
                {
                    *hostApiOutputDevice = paUseHostApiSpecificDeviceSpecification;
                    *hostApi = hostApis_[outputHostApiIndex];
                }
                else
                {
                    return paInvalidDevice;
                }
            }
            else
            {
                return paInvalidDevice;
            }
        }
        else
        {
            if( outputParameters->device < 0 || outputParameters->device >= deviceCount_ )
                return paInvalidDevice;

            outputHostApiIndex = FindHostApi( outputParameters->device, hostApiOutputDevice );
            if( outputHostApiIndex < 0 )
                return paInternalError;

            *hostApi = hostApis_[outputHostApiIndex];

            if( outputParameters->channelCount <= 0 )
                return paInvalidChannelCount;

            if( !SampleFormatIsValid( outputParameters->sampleFormat ) )
                return paSampleFormatNotSupported;

            if( outputParameters->hostApiSpecificStreamInfo != NULL )
            {
                if( ((PaUtilHostApiSpecificStreamInfoHeader*)inputParameters->hostApiSpecificStreamInfo)->hostApiType
                        != (*hostApi)->info.type )
                    return paIncompatibleHostApiSpecificStreamInfo;
            }
        }   

        if( (inputParameters != NULL) && (outputParameters != NULL) )
        {
            /* ensure that both devices use the same API */
            if( inputHostApiIndex != outputHostApiIndex )
                return paBadIODeviceCombination;
        }
    }
    
    
    /* Check for absurd sample rates. */
    if( (sampleRate < 1000.0) || (sampleRate > 200000.0) )
        return paInvalidSampleRate;

    if( ((streamFlags & ~paPlatformSpecificFlags) & ~(paClipOff | paDitherOff)) != 0 ) return paInvalidFlag;

    return paNoError;
}


PaError Pa_IsFormatSupported( const PaStreamParameters *inputParameters,
                              const PaStreamParameters *outputParameters,
                              double sampleRate )
{
    PaError result;
    PaUtilHostApiRepresentation *hostApi;
    PaDeviceIndex hostApiInputDevice, hostApiOutputDevice;
    PaStreamParameters hostApiInputParameters, hostApiOutputParameters;
    PaStreamParameters *hostApiInputParametersPtr, *hostApiOutputParametersPtr;


#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_IsFormatSupported called:\n" );

    if( inputParameters == NULL ){
        PaUtil_DebugPrint("\PaStreamParameters *inputParameters: NULL\n" );
    }else{
        PaUtil_DebugPrint("\PaStreamParameters *inputParameters: 0x%p\n", inputParameters );
        PaUtil_DebugPrint("\tPaDeviceIndex inputParameters->device: %d\n", inputParameters->device );
        PaUtil_DebugPrint("\tint inputParameters->channelCount: %d\n", inputParameters->channelCount );
        PaUtil_DebugPrint("\tPaSampleFormat inputParameters->sampleFormat: %d\n", inputParameters->sampleFormat );
        PaUtil_DebugPrint("\tPaTime inputParameters->suggestedLatency: %f\n", inputParameters->suggestedLatency );
        PaUtil_DebugPrint("\tvoid *inputParameters->hostApiSpecificStreamInfo: 0x%p\n", inputParameters->hostApiSpecificStreamInfo );
    }

    if( outputParameters == NULL ){
        PaUtil_DebugPrint("\PaStreamParameters *outputParameters: NULL\n" );
    }else{
        PaUtil_DebugPrint("\PaStreamParameters *outputParameters: 0x%p\n", outputParameters );
        PaUtil_DebugPrint("\tPaDeviceIndex outputParameters->device: %d\n", outputParameters->device );
        PaUtil_DebugPrint("\tint outputParameters->channelCount: %d\n", outputParameters->channelCount );
        PaUtil_DebugPrint("\tPaSampleFormat outputParameters->sampleFormat: %d\n", outputParameters->sampleFormat );
        PaUtil_DebugPrint("\tPaTime outputParameters->suggestedLatency: %f\n", outputParameters->suggestedLatency );
        PaUtil_DebugPrint("\tvoid *outputParameters->hostApiSpecificStreamInfo: 0x%p\n", outputParameters->hostApiSpecificStreamInfo );
    }
    
    PaUtil_DebugPrint("\tdouble sampleRate: %g\n", sampleRate );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        result = paNotInitialized;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_IsFormatSupported returned:\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif
        return result;
    }

    result = ValidateOpenStreamParameters( inputParameters,
                                           outputParameters,
                                           sampleRate, paNoFlag,
                                           &hostApi,
                                           &hostApiInputDevice,
                                           &hostApiOutputDevice );
    if( result != paNoError )
    {
#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_IsFormatSupported returned:\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif
        return result;
    }
    

    if( inputParameters )
    {
        hostApiInputParameters.device = hostApiInputDevice;
        hostApiInputParameters.channelCount = inputParameters->channelCount;
        hostApiInputParameters.sampleFormat = inputParameters->sampleFormat;
        hostApiInputParameters.suggestedLatency = inputParameters->suggestedLatency;
        hostApiInputParameters.hostApiSpecificStreamInfo = inputParameters->hostApiSpecificStreamInfo;
        hostApiInputParametersPtr = &hostApiInputParameters;
    }
    else
    {
        hostApiInputParametersPtr = NULL;
    }

    if( outputParameters )
    {
        hostApiOutputParameters.device = hostApiOutputDevice;
        hostApiOutputParameters.channelCount = outputParameters->channelCount;
        hostApiOutputParameters.sampleFormat = outputParameters->sampleFormat;
        hostApiOutputParameters.suggestedLatency = outputParameters->suggestedLatency;
        hostApiOutputParameters.hostApiSpecificStreamInfo = outputParameters->hostApiSpecificStreamInfo;
        hostApiOutputParametersPtr = &hostApiOutputParameters;
    }
    else
    {
        hostApiOutputParametersPtr = NULL;
    }

    result = hostApi->IsFormatSupported( hostApi,
                                  hostApiInputParametersPtr, hostApiOutputParametersPtr,
                                  sampleRate );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_OpenStream returned:\n" );
    if( result == paFormatIsSupported )
        PaUtil_DebugPrint("\tPaError: 0 [ paFormatIsSupported ]\n\n" );
    else
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_OpenStream( PaStream** stream,
                       const PaStreamParameters *inputParameters,
                       const PaStreamParameters *outputParameters,
                       double sampleRate,
                       unsigned long framesPerBuffer,
                       PaStreamFlags streamFlags,
                       PaStreamCallback *streamCallback,
                       void *userData )
{
    PaError result;
    PaUtilHostApiRepresentation *hostApi;
    PaDeviceIndex hostApiInputDevice, hostApiOutputDevice;
    PaStreamParameters hostApiInputParameters, hostApiOutputParameters;
    PaStreamParameters *hostApiInputParametersPtr, *hostApiOutputParametersPtr;


#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_OpenStream called:\n" );
    PaUtil_DebugPrint("\tPaStream** stream: 0x%p\n", stream );

    if( inputParameters == NULL ){
        PaUtil_DebugPrint("\PaStreamParameters *inputParameters: NULL\n" );
    }else{
        PaUtil_DebugPrint("\PaStreamParameters *inputParameters: 0x%p\n", inputParameters );
        PaUtil_DebugPrint("\tPaDeviceIndex inputParameters->device: %d\n", inputParameters->device );
        PaUtil_DebugPrint("\tint inputParameters->channelCount: %d\n", inputParameters->channelCount );
        PaUtil_DebugPrint("\tPaSampleFormat inputParameters->sampleFormat: %d\n", inputParameters->sampleFormat );
        PaUtil_DebugPrint("\tPaTime inputParameters->suggestedLatency: %f\n", inputParameters->suggestedLatency );
        PaUtil_DebugPrint("\tvoid *inputParameters->hostApiSpecificStreamInfo: 0x%p\n", inputParameters->hostApiSpecificStreamInfo );
    }

    if( outputParameters == NULL ){
        PaUtil_DebugPrint("\PaStreamParameters *outputParameters: NULL\n" );
    }else{
        PaUtil_DebugPrint("\PaStreamParameters *outputParameters: 0x%p\n", outputParameters );
        PaUtil_DebugPrint("\tPaDeviceIndex outputParameters->device: %d\n", outputParameters->device );
        PaUtil_DebugPrint("\tint outputParameters->channelCount: %d\n", outputParameters->channelCount );
        PaUtil_DebugPrint("\tPaSampleFormat outputParameters->sampleFormat: %d\n", outputParameters->sampleFormat );
        PaUtil_DebugPrint("\tPaTime outputParameters->suggestedLatency: %f\n", outputParameters->suggestedLatency );
        PaUtil_DebugPrint("\tvoid *outputParameters->hostApiSpecificStreamInfo: 0x%p\n", outputParameters->hostApiSpecificStreamInfo );
    }
    
    PaUtil_DebugPrint("\tdouble sampleRate: %g\n", sampleRate );
    PaUtil_DebugPrint("\tunsigned long framesPerBuffer: %d\n", framesPerBuffer );
    PaUtil_DebugPrint("\tPaStreamFlags streamFlags: 0x%x\n", streamFlags );
    PaUtil_DebugPrint("\tPaStreamCallback *streamCallback: 0x%p\n", streamCallback );
    PaUtil_DebugPrint("\tvoid *userData: 0x%p\n", userData );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        result = paNotInitialized;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_OpenStream returned:\n" );
        PaUtil_DebugPrint("\t*(PaStream** stream): undefined\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif
        return result;
    }

    /* Check for parameter errors. */

    if( stream == NULL )
    {
        result = paBadStreamPtr;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_OpenStream returned:\n" );
        PaUtil_DebugPrint("\t*(PaStream** stream): undefined\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif
        return result;
    }

    result = ValidateOpenStreamParameters( inputParameters,
                                           outputParameters,
                                           sampleRate, streamFlags,
                                           &hostApi,
                                           &hostApiInputDevice,
                                           &hostApiOutputDevice );
    if( result != paNoError )
    {
#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_OpenStream returned:\n" );
        PaUtil_DebugPrint("\t*(PaStream** stream): undefined\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif
        return result;
    }
    

    if( inputParameters )
    {
        hostApiInputParameters.device = hostApiInputDevice;
        hostApiInputParameters.channelCount = inputParameters->channelCount;
        hostApiInputParameters.sampleFormat = inputParameters->sampleFormat;
        hostApiInputParameters.suggestedLatency = inputParameters->suggestedLatency;
        hostApiInputParameters.hostApiSpecificStreamInfo = inputParameters->hostApiSpecificStreamInfo;
        hostApiInputParametersPtr = &hostApiInputParameters;
    }
    else
    {
        hostApiInputParametersPtr = NULL;
    }

    if( outputParameters )
    {
        hostApiOutputParameters.device = hostApiOutputDevice;
        hostApiOutputParameters.channelCount = outputParameters->channelCount;
        hostApiOutputParameters.sampleFormat = outputParameters->sampleFormat;
        hostApiOutputParameters.suggestedLatency = outputParameters->suggestedLatency;
        hostApiOutputParameters.hostApiSpecificStreamInfo = outputParameters->hostApiSpecificStreamInfo;
        hostApiOutputParametersPtr = &hostApiOutputParameters;
    }
    else
    {
        hostApiOutputParametersPtr = NULL;
    }

    result = hostApi->OpenStream( hostApi, stream,
                                  hostApiInputParametersPtr, hostApiOutputParametersPtr,
                                  sampleRate, framesPerBuffer, streamFlags, streamCallback, userData );

    if( result == paNoError )
        AddOpenStream( *stream );


#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_OpenStream returned:\n" );
    PaUtil_DebugPrint("\t*(PaStream** stream): 0x%p\n", *stream );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_OpenDefaultStream( PaStream** stream,
                              int inputChannelCount,
                              int outputChannelCount,
                              PaSampleFormat sampleFormat,
                              double sampleRate,
                              unsigned long framesPerBuffer,
                              PaStreamCallback *streamCallback,
                              void *userData )
{
    PaError result;
    PaStreamParameters hostApiInputParameters, hostApiOutputParameters;
    PaStreamParameters *hostApiInputParametersPtr, *hostApiOutputParametersPtr;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_OpenDefaultStream called:\n" );
    PaUtil_DebugPrint("\tPaStream** stream: 0x%p\n", stream );
    PaUtil_DebugPrint("\tint inputChannelCount: %d\n", inputChannelCount );
    PaUtil_DebugPrint("\tint outputChannelCount: %d\n", outputChannelCount );
    PaUtil_DebugPrint("\tPaSampleFormat sampleFormat: %d\n", sampleFormat );
    PaUtil_DebugPrint("\tdouble sampleRate: %g\n", sampleRate );
    PaUtil_DebugPrint("\tunsigned long framesPerBuffer: %d\n", framesPerBuffer );
    PaUtil_DebugPrint("\tPaStreamCallback *streamCallback: 0x%p\n", streamCallback );
    PaUtil_DebugPrint("\tvoid *userData: 0x%p\n", userData );
#endif


    if( inputChannelCount > 0 )
    {
        hostApiInputParameters.device = Pa_GetDefaultInputDevice();
        hostApiInputParameters.channelCount = inputChannelCount;
        hostApiInputParameters.sampleFormat = sampleFormat;
        hostApiInputParameters.suggestedLatency =  /* REVIEW: should we be using high input latency here? */
             Pa_GetDeviceInfo( hostApiInputParameters.device )->defaultHighInputLatency;
        hostApiInputParameters.hostApiSpecificStreamInfo = NULL;
        hostApiInputParametersPtr = &hostApiInputParameters;
    }
    else
    {
        hostApiInputParametersPtr = NULL;
    }

    if( outputChannelCount > 0 )
    {
        hostApiOutputParameters.device = Pa_GetDefaultOutputDevice();
        hostApiOutputParameters.channelCount = outputChannelCount;
        hostApiOutputParameters.sampleFormat = sampleFormat;
        hostApiOutputParameters.suggestedLatency =  /* REVIEW: should we be using high input latency here? */
             Pa_GetDeviceInfo( hostApiOutputParameters.device )->defaultHighOutputLatency;
        hostApiOutputParameters.hostApiSpecificStreamInfo = NULL;
        hostApiOutputParametersPtr = &hostApiOutputParameters;
    }
    else
    {
        hostApiOutputParametersPtr = NULL;
    }


    result = Pa_OpenStream(
                 stream, hostApiInputParametersPtr, hostApiOutputParametersPtr,
                 sampleRate, framesPerBuffer, paNoFlag, streamCallback, userData );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_OpenDefaultStream returned:\n" );
    PaUtil_DebugPrint("\t*(PaStream** stream): 0x%p", *stream );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


static PaError ValidateStream( PaStream* stream )
{
    if( !PA_IS_INITIALISED_ ) return paNotInitialized;

    if( stream == NULL ) return paBadStreamPtr;

    if( ((PaUtilStreamRepresentation*)stream)->magic != PA_STREAM_MAGIC )
        return paBadStreamPtr;

    return paNoError;
}


PaError Pa_CloseStream( PaStream* stream )
{
    PaUtilStreamInterface *interface;
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_CloseStream called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    /* always remove the open stream from our list, even if this function
        eventually returns an error. Otherwise CloseOpenStreams() will
        get stuck in an infinite loop */
    RemoveOpenStream( stream ); /* be sure to call this _before_ closing the stream */

    if( result == paNoError )
    {
        interface = PA_STREAM_INTERFACE(stream);
        if( !interface->IsStopped( stream ) )
        {
            result = interface->Abort( stream );
        }

        if( result == paNoError )                 /* REVIEW: shouldn't we close anyway? */
            result = interface->Close( stream );
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_CloseStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_StartStream( PaStream *stream )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_StartStream called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( result == paNoError )
    {
        if( !PA_STREAM_INTERFACE(stream)->IsStopped( stream ) )
        {
            result = paStreamIsNotStopped ;
        }
        else
        {
            result = PA_STREAM_INTERFACE(stream)->Start( stream );
        }
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_StartStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_StopStream( PaStream *stream )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_StopStream called\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( result == paNoError )
    {
        if( PA_STREAM_INTERFACE(stream)->IsStopped( stream ) )
        {
            result = paStreamIsStopped;
        }
        else
        {
            result = PA_STREAM_INTERFACE(stream)->Stop( stream );
        }
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_StopStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_AbortStream( PaStream *stream )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_AbortStream called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( result == paNoError )
    {
        if( PA_STREAM_INTERFACE(stream)->IsStopped( stream ) )
        {
            result = paStreamIsStopped;
        }
        else
        {
            result = PA_STREAM_INTERFACE(stream)->Abort( stream );
        }
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_AbortStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_IsStreamStopped( PaStream *stream )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_IsStreamStopped called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( result == paNoError )
        result = PA_STREAM_INTERFACE(stream)->IsStopped( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_IsStreamStopped returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_IsStreamActive( PaStream *stream )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_IsStreamActive called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( result == paNoError )
        result = PA_STREAM_INTERFACE(stream)->IsActive( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_IsStreamActive returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaTime Pa_GetStreamInputLatency( PaStream *stream )
{
    PaError error = ValidateStream( stream );
    PaTime result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetStreamInputLatency called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( error != paNoError )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamInputLatency returned:\n" );
        PaUtil_DebugPrint("\tPaTime: 0 [PaError error:%d ( %s )]\n\n", result, error, Pa_GetErrorText( error ) );
#endif

    }
    else
    {
        result = PA_STREAM_INTERFACE(stream)->GetInputLatency( stream );

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamInputLatency returned:\n" );
        PaUtil_DebugPrint("\tPaTime: %g\n\n", result );
#endif

    }

    return result;
}


PaTime Pa_GetStreamOutputLatency( PaStream *stream )
{
    PaError error = ValidateStream( stream );
    PaTime result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetStreamOutputLatency called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

if( error != paNoError )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamOutputLatency returned:\n" );
        PaUtil_DebugPrint("\tPaTime: 0 [PaError error:%d ( %s )]\n\n", result, error, Pa_GetErrorText( error ) );
#endif

    }
    else
    {
        result = PA_STREAM_INTERFACE(stream)->GetOutputLatency( stream );

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamOutputLatency returned:\n" );
        PaUtil_DebugPrint("\tPaTime: %g\n\n", result );
#endif

    }

    return result;
}


PaTime Pa_GetStreamTime( PaStream *stream )
{
    PaError error = ValidateStream( stream );
    PaTime result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetStreamTime called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( error != paNoError )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamTime returned:\n" );
        PaUtil_DebugPrint("\tPaTime: 0 [PaError error:%d ( %s )]\n\n", result, error, Pa_GetErrorText( error ) );
#endif

    }
    else
    {
        result = PA_STREAM_INTERFACE(stream)->GetTime( stream );

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamTime returned:\n" );
        PaUtil_DebugPrint("\tPaTime: %g\n\n", result );
#endif

    }

    return result;
}


double Pa_GetStreamCpuLoad( PaStream* stream )
{
    PaError error = ValidateStream( stream );
    double result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetStreamCpuLoad called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( error != paNoError )
    {

        result = 0.0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamCpuLoad returned:\n" );
        PaUtil_DebugPrint("\tdouble: 0.0 [PaError error: %d ( %s )]\n\n", error, Pa_GetErrorText( error ) );
#endif

    }
    else
    {
        result = PA_STREAM_INTERFACE(stream)->GetCpuLoad( stream );

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamCpuLoad returned:\n" );
        PaUtil_DebugPrint("\tdouble: %g\n\n", result );
#endif

    }

    return result;
}


PaError Pa_ReadStream( PaStream* stream,
                       void *buffer,
                       unsigned long frames )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_ReadStream called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    /* @todo should return an error if buffer is zero or frames <= 0 */
    if( frames > 0 && buffer != 0 )
        result = PA_STREAM_INTERFACE(stream)->Read( stream, buffer, frames );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_ReadStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_WriteStream( PaStream* stream,
                        void *buffer,
                        unsigned long frames )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_WriteStream called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    /* @todo should return an error if buffer is zero or frames <= 0 */
    if( frames > 0 && buffer != 0 )
        result = PA_STREAM_INTERFACE(stream)->Write( stream, buffer, frames );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_WriteStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}

signed long Pa_GetStreamReadAvailable( PaStream* stream )
{
    PaError error = ValidateStream( stream );
    signed long result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetStreamReadAvailable called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( error != paNoError )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamReadAvailable returned:\n" );
        PaUtil_DebugPrint("\tunsigned long: 0 [ PaError error: %d ( %s ) ]\n\n", error, Pa_GetErrorText( error ) );
#endif

    }
    else
    {
        result = PA_STREAM_INTERFACE(stream)->GetReadAvailable( stream );

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamReadAvailable returned:\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    }

    return result;
}


signed long Pa_GetStreamWriteAvailable( PaStream* stream )
{
    PaError error = ValidateStream( stream );
    signed long result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetStreamWriteAvailable called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( error != paNoError )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamWriteAvailable returned:\n" );
        PaUtil_DebugPrint("\tunsigned long: 0 [ PaError error: %d ( %s ) ]\n\n", error, Pa_GetErrorText( error ) );
#endif

    }
    else
    {
        result = PA_STREAM_INTERFACE(stream)->GetWriteAvailable( stream );

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamWriteAvailable returned:\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    }

    return result;
}


PaError Pa_GetSampleSize( PaSampleFormat format )
{
    int result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetSampleSize called:\n" );
    PaUtil_DebugPrint("\tPaSampleFormat format: %d\n", format );
#endif

    switch( format & ~paNonInterleaved )
    {

    case paUInt8:
    case paInt8:
        result = 1;
        break;

    case paInt16:
        result = 2;
        break;

    case paInt24:
        result = 3;
        break;

    case paFloat32:
    case paInt32:
        result = 4;
        break;

    default:
        result = paSampleFormatNotSupported;
        break;
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetSampleSize returned:\n" );
    if( result > 0 )
        PaUtil_DebugPrint("\tint: %d\n\n", result );
    else
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return (PaError) result;
}

