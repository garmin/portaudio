/*
 * $Id$
 * Portable Audio I/O Library for ASIO Drivers
 *
 * Author: Stephane Letz
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 2000-2002 Stephane Letz, Phil Burk, Ross Bencina
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
 
/* Modification History

        08-03-01 First version : Stephane Letz
        08-06-01 Tweaks for PC, use C++, buffer allocation, Float32 to Int32 conversion : Phil Burk
        08-20-01 More conversion, PA_StreamTime, Pa_GetHostError : Stephane Letz
        08-21-01 PaUInt8 bug correction, implementation of ASIOSTFloat32LSB and ASIOSTFloat32MSB native formats : Stephane Letz
        08-24-01 MAX_INT32_FP hack, another Uint8 fix : Stephane and Phil
        08-27-01 Implementation of hostBufferSize < userBufferSize case, better management of the ouput buffer when
                 the stream is stopped : Stephane Letz
        08-28-01 Check the stream pointer for null in bufferSwitchTimeInfo, correct bug in bufferSwitchTimeInfo when 
                 the stream is stopped : Stephane Letz
        10-12-01 Correct the PaHost_CalcNumHostBuffers function: computes FramesPerHostBuffer to be the lowest that
                 respect requested FramesPerUserBuffer and userBuffersPerHostBuffer : Stephane Letz
        10-26-01 Management of hostBufferSize and userBufferSize of any size : Stephane Letz
        10-27-01 Improve calculus of hostBufferSize to be multiple or divisor of userBufferSize if possible : Stephane and Phil
        10-29-01 Change MAX_INT32_FP to (2147483520.0f) to prevent roundup to 0x80000000 : Phil Burk
        10-31-01 Clear the ouput buffer and user buffers in PaHost_StartOutput, correct bug in GetFirstMultiple : Stephane Letz 
        11-06-01 Rename functions : Stephane Letz 
        11-08-01 New Pa_ASIO_Adaptor_Init function to init Callback adpatation variables, cleanup of Pa_ASIO_Callback_Input: Stephane Letz 
        11-29-01 Break apart device loading to debug random failure in Pa_ASIO_QueryDeviceInfo ; Phil Burk
        01-03-02 Desallocate all resources in PaHost_Term for cases where Pa_CloseStream is not called properly :  Stephane Letz
        02-01-02 Cleanup, test of multiple-stream opening : Stephane Letz
        19-02-02 New Pa_ASIO_loadDriver that calls CoInitialize on each thread on Windows : Stephane Letz
        09-04-02 Correct error code management in PaHost_Term, removes various compiler warning : Stephane Letz
        12-04-02 Add Mac includes for <Devices.h> and <Timer.h> : Phil Burk
        13-04-02 Removes another compiler warning : Stephane Letz
        30-04-02 Pa_ASIO_QueryDeviceInfo bug correction, memory allocation checking, better error handling : D Viens, P Burk, S Letz
        12-05-02 Rehashed into new multi-api infrastructure, added support for all ASIO sample formats : Ross Bencina

        TO DO :
        - select buffer size based on latency parameters
            - use greater of input and output latency
            
        - implement GetStreamTime
        - implement block adaption        
        - work out how to implement stream stoppage from callback
        - implement IsStreamActive
        


        - rigorously check asio return codes and convert to pa error codes
        - Check Pa_StopSteam and Pa_AbortStream
        - Optimization for Input only or Ouput only (really necessary ??)



        Ross' notes about the old implementation:

        - Pa_ASIO_CreateBuffers sets up the callbacks and creates the host buffers.
            - i think the callback structure could just be a temp on the stack or as a static struct.
            - the buffer info structures will need to be dynamically allocated for the stream.


        Different channels of a multichannel stream can have different sample
        formats, but we assume that all are the same as the first channel for now.
*/





#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "portaudio.h"
#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

/*
#if MAC
#include <Devices.h>
#include <Timer.h>
#include <Math64.h>
#else
*/
/*
#include <math.h>
#include <windows.h>
#include <mmsystem.h>
*/
/*
#endif
*/

/* external references */
extern AsioDrivers* asioDrivers ;
bool loadAsioDriver(char *name);


/* We are trying to be compatible with CARBON but this has not been thoroughly tested. */
/* not tested at all since new code was introduced. */
#define CARBON_COMPATIBLE  (0)




/* prototypes for functions declared in this file */

extern "C" PaError PaAsio_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex );
static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           PaDeviceIndex inputDevice,
                           int numInputChannels,
                           PaSampleFormat inputSampleFormat,
                           unsigned long inputLatency,
                           void *inputStreamInfo,
                           PaDeviceIndex outputDevice,
                           int numOutputChannels,
                           PaSampleFormat outputSampleFormat,
                           unsigned long outputLatency,
                           void *outputStreamInfo,
                           double sampleRate,
                           unsigned long framesPerCallback,
                           PaStreamFlags streamFlags,
                           PortAudioCallback *callback,
                           void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTimestamp GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, void *buffer, unsigned long frames );
static unsigned long GetStreamReadAvailable( PaStream* stream );
static unsigned long GetStreamWriteAvailable( PaStream* stream );

/* our ASIO callback functions */

static void bufferSwitch(long index, ASIOBool processNow);
static ASIOTime *bufferSwitchTimeInfo(ASIOTime *timeInfo, long index, ASIOBool processNow);
static void sampleRateChanged(ASIOSampleRate sRate);
static long asioMessages(long selector, long value, void* message, double* opt);

static ASIOCallbacks asioCallbacks_ =
    { bufferSwitch, sampleRateChanged, asioMessages, bufferSwitchTimeInfo };


/* PaAsioHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct
{
    PaUtilHostApiRepresentation commonHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationContext *allocations;

    /* the ASIO C API only allows one ASIO driver to be open at a time,
        so we kee track of whether we have the driver open here, and
        use this information to return errors from OpenStream if the
        driver is already open.
    */
    int driverOpen;
}
PaAsioHostApiRepresentation;


/*
    Retrieve <driverCount> driver names from ASIO, returned in a char**
    allocated in <context>.
*/
static char **GetAsioDriverNames( PaUtilAllocationContext *context, long driverCount )
{
    char **result = 0;
    int i;
    
    result =(char**)PaUtil_ContextAllocateMemory(
            context, sizeof(char*) * driverCount );
    if( !result )
        goto error;

    result[0] = (char*)PaUtil_ContextAllocateMemory(
            context, 32 * driverCount );
    if( !result[0] )
        goto error;

    for( i=0; i<driverCount; ++i )
        result[i] = result[0] + (32 * i);

    asioDrivers->getDriverNames( result, driverCount );

error:
    return result;
}


static PaSampleFormat AsioSampleTypeToPaNativeSampleFormat(ASIOSampleType type)
{
    switch (type) {
        case ASIOSTInt16MSB:
        case ASIOSTInt16LSB:
                return paInt16;

        case ASIOSTFloat32MSB:
        case ASIOSTFloat32LSB:
        case ASIOSTFloat64MSB:
        case ASIOSTFloat64LSB:
                return paFloat32;

        case ASIOSTInt32MSB:
        case ASIOSTInt32LSB:
        case ASIOSTInt32MSB16:
        case ASIOSTInt32LSB16:
        case ASIOSTInt32MSB18:          
        case ASIOSTInt32MSB20:          
        case ASIOSTInt32MSB24:          
        case ASIOSTInt32LSB18:
        case ASIOSTInt32LSB20:
        case ASIOSTInt32LSB24:
                return paInt32;

        case ASIOSTInt24MSB:
        case ASIOSTInt24LSB:
                return paInt24;

        default:
                return paCustomFormat;
    }
}


static int BytesPerAsioSample( ASIOSampleType sampleType )
{
    switch (sampleType) {
        case ASIOSTInt16MSB:
        case ASIOSTInt16LSB:
            return 2;

        case ASIOSTFloat64MSB:
        case ASIOSTFloat64LSB:
            return 8;

        case ASIOSTFloat32MSB:
        case ASIOSTFloat32LSB:
        case ASIOSTInt32MSB:
        case ASIOSTInt32LSB:
        case ASIOSTInt32MSB16:
        case ASIOSTInt32LSB16:
        case ASIOSTInt32MSB18:
        case ASIOSTInt32MSB20:
        case ASIOSTInt32MSB24:
        case ASIOSTInt32LSB18:
        case ASIOSTInt32LSB20:
        case ASIOSTInt32LSB24:
            return 4;

        case ASIOSTInt24MSB:
        case ASIOSTInt24LSB:
            return 3;

        default:
            return 0;
    }
}


static void Swap16( void *buffer, long shift, long count )
{
    unsigned short *p = (unsigned short*)buffer;
    unsigned short temp;
    (void) shift; /* unused parameter */

    while( count-- )
    {
        temp = *p;
        *p++ = (temp<<8) | (temp>>8);
    }
}

static void Swap24( void *buffer, long shift, long count )
{
    unsigned char *p = (unsigned char*)buffer;
    unsigned char temp;
    (void) shift; /* unused parameter */

    while( count-- )
    {
        temp = *p;
        *p = *(p+2);
        *(p+2) = temp;
        p += 3;
    }
}

#define PA_SWAP32_( x ) ((x>>24) | ((x>>8)&0xFF00) | ((x<<8)&0xFF0000) | (x<<24));

static void Swap32( void *buffer, long shift, long count )
{
    unsigned long *p = (unsigned long*)buffer;
    unsigned long temp;
    (void) shift; /* unused parameter */

    while( count-- )
    {
        temp = *p;
        *p++ = PA_SWAP32_( temp);
    }
}

static void SwapShiftLeft32( void *buffer, long shift, long count )
{
    unsigned long *p = (unsigned long*)buffer;
    unsigned long temp;

    while( count-- )
    {
        temp = *p;
        temp = PA_SWAP32_( temp);
        *p++ = temp << shift;
    }
}

static void ShiftRightSwap32( void *buffer, long shift, long count )
{
    unsigned long *p = (unsigned long*)buffer;
    unsigned long temp;

    while( count-- )
    {
        temp = *p >> shift;
        *p++ = PA_SWAP32_( temp);
    }
}

static void ShiftLeft32( void *buffer, long shift, long count )
{
    unsigned long *p = (unsigned long*)buffer;
    unsigned long temp;

    while( count-- )
    {
        temp = *p;
        *p++ = temp << shift;
    }
}

static void ShiftRight32( void *buffer, long shift, long count )
{
    unsigned long *p = (unsigned long*)buffer;
    unsigned long temp;

    while( count-- )
    {
        temp = *p;
        *p++ = temp >> shift;
    }
}

#define PA_SWAP_( x, y ) temp=x; x = y; y = temp;

static void Swap64ConvertFloat64ToFloat32( void *buffer, long shift, long count )
{
    double *in = (double*)buffer;
    float *out = (float*)buffer;
    unsigned char *p;
    unsigned char temp;
    (void) shift; /* unused parameter */
    
    while( count-- )
    {
        p = (unsigned char*)in;
        PA_SWAP_( p[0], p[7] );
        PA_SWAP_( p[1], p[6] );
        PA_SWAP_( p[2], p[5] );
        PA_SWAP_( p[3], p[4] );
        
        *out++ = *in++;
    }
}

static void ConvertFloat64ToFloat32( void *buffer, long shift, long count )
{
    double *in = (double*)buffer;
    float *out = (float*)buffer;
    (void) shift; /* unused parameter */

    while( count-- )
        *out++ = *in++;
}

static void ConvertFloat32ToFloat64Swap64( void *buffer, long shift, long count )
{
    float *in = ((float*)buffer) + (count-1);
    double *out = ((double*)buffer) + (count-1);
    unsigned char *p;
    unsigned char temp;
    (void) shift; /* unused parameter */

    while( count-- )
    {
        *out = *in--;
        
        p = (unsigned char*)out;
        PA_SWAP_( p[0], p[7] );
        PA_SWAP_( p[1], p[6] );
        PA_SWAP_( p[2], p[5] );
        PA_SWAP_( p[3], p[4] );
        
        out--;
    }
}

static void ConvertFloat32ToFloat64( void *buffer, long shift, long count )
{
    float *in = ((float*)buffer) + (count-1);
    double *out = ((double*)buffer) + (count-1);
    (void) shift; /* unused parameter */

    while( count-- )
        *out-- = *in--;
}

#ifdef MAC
#define PA_MSB_IS_NATIVE_
#undef PA_LSB_IS_NATIVE_
#endif

#ifdef WINDOWS
#undef PA_MSB_IS_NATIVE_
#define PA_LSB_IS_NATIVE_
#endif

typedef void PaAsioBufferConverter( void *, long, long );

static void SelectAsioToPaConverter( ASIOSampleType type, PaAsioBufferConverter **converter, long *shift ) 
{
    *shift = 0;
    *converter = 0;
    
    switch (type) {
        case ASIOSTInt16MSB:
            /* dest: paInt16, no conversion necessary, possible byte swap*/
            #ifdef PA_LSB_IS_NATIVE_ 
                *converter = Swap16;
            #endif
            break;
        case ASIOSTInt16LSB:
            /* dest: paInt16, no conversion necessary, possible byte swap*/
            #ifdef PA_MSB_IS_NATIVE_
                *converter = Swap16;
            #endif
            break;
        case ASIOSTFloat32MSB:
            /* dest: paFloat32, no conversion necessary, possible byte swap*/
            #ifdef PA_LSB_IS_NATIVE_
                *converter = Swap32;
            #endif
            break;
        case ASIOSTFloat32LSB:
            /* dest: paFloat32, no conversion necessary, possible byte swap*/
            #ifdef PA_MSB_IS_NATIVE_
                *converter = Swap32;
            #endif
            break;
        case ASIOSTFloat64MSB:
            /* dest: paFloat32, in-place conversion to/from float32, possible byte swap*/
            #ifdef PA_LSB_IS_NATIVE_
                *converter = Swap64ConvertFloat64ToFloat32;
            #else
                *converter = ConvertFloat64ToFloat32;
            #endif
            break;
        case ASIOSTFloat64LSB:
            /* dest: paFloat32, in-place conversion to/from float32, possible byte swap*/
            #ifdef PA_MSB_IS_NATIVE_
                *converter = Swap64ConvertFloat64ToFloat32;
            #else
                *converter = ConvertFloat64ToFloat32;
            #endif
            break;
        case ASIOSTInt32MSB:
            /* dest: paInt32, no conversion necessary, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_
                *converter = Swap32;
            #endif
            break;
        case ASIOSTInt32LSB:
            /* dest: paInt32, no conversion necessary, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = Swap32;
            #endif
            break;
        case ASIOSTInt32MSB16:
            /* dest: paInt32, 16 bit shift, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_
                *converter = SwapShiftLeft32;
            #else
                *converter = ShiftLeft32;
            #endif
            *shift = 16;
            break;
        case ASIOSTInt32MSB18:
            /* dest: paInt32, 14 bit shift, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_
                *converter = SwapShiftLeft32;
            #else
                *converter = ShiftLeft32;
            #endif
            *shift = 14;
            break;
        case ASIOSTInt32MSB20:
            /* dest: paInt32, 12 bit shift, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_ )
                *converter = SwapShiftLeft32;
            #else
                *converter = ShiftLeft32;
            #endif
            *shift = 12;
            break;
        case ASIOSTInt32MSB24:
            /* dest: paInt32, 8 bit shift, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_
                *converter = SwapShiftLeft32;
            #else
                *converter = ShiftLeft32;
            #endif
            *shift = 8;
            break;
        case ASIOSTInt32LSB16:
            /* dest: paInt32, 16 bit shift, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = SwapShiftLeft32;
            #else
                *converter = ShiftLeft32;
            #endif
            *shift = 16;
            break;
        case ASIOSTInt32LSB18:
            /* dest: paInt32, 14 bit shift, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = SwapShiftLeft32;
            #else
                *converter = ShiftLeft32;
            #endif
            *shift = 14;
            break;
        case ASIOSTInt32LSB20:
            /* dest: paInt32, 12 bit shift, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = SwapShiftLeft32;
            #else
                *converter = ShiftLeft32;
            #endif
            *shift = 12;
            break;
        case ASIOSTInt32LSB24:
            /* dest: paInt32, 8 bit shift, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = SwapShiftLeft32;
            #else
                *converter = ShiftLeft32;
            #endif
            *shift = 8;
            break;
        case ASIOSTInt24MSB:
            /* dest: paInt24, no conversion necessary, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_
                *converter = Swap24;
            #endif
            break;
        case ASIOSTInt24LSB:
            /* dest: paInt24, no conversion necessary, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = Swap24;
            #endif
            break;
    }
}


static void SelectPaToAsioConverter( ASIOSampleType type, PaAsioBufferConverter **converter, long *shift )
{
    *shift = 0;
    *converter = 0;
    
    switch (type) {
        case ASIOSTInt16MSB:
            /* src: paInt16, no conversion necessary, possible byte swap*/
            #ifdef PA_LSB_IS_NATIVE_
                *converter = Swap16;
            #endif
            break;
        case ASIOSTInt16LSB:
            /* src: paInt16, no conversion necessary, possible byte swap*/
            #ifdef PA_MSB_IS_NATIVE_
                *converter = Swap16;
            #endif
            break;
        case ASIOSTFloat32MSB:
            /* src: paFloat32, no conversion necessary, possible byte swap*/
            #ifdef PA_LSB_IS_NATIVE_
                *converter = Swap32;
            #endif
            break;
        case ASIOSTFloat32LSB:
            /* src: paFloat32, no conversion necessary, possible byte swap*/
            #ifdef PA_MSB_IS_NATIVE_
                *converter = Swap32;
            #endif
            break;
        case ASIOSTFloat64MSB:
            /* src: paFloat32, in-place conversion to/from float32, possible byte swap*/
            #ifdef PA_LSB_IS_NATIVE_
                *converter = ConvertFloat32ToFloat64Swap64;
            #else
                *converter = ConvertFloat32ToFloat64;
            #endif
            break;
        case ASIOSTFloat64LSB:
            /* src: paFloat32, in-place conversion to/from float32, possible byte swap*/
            #ifdef PA_MSB_IS_NATIVE_
                *converter = ConvertFloat32ToFloat64Swap64;
            #else
                *converter = ConvertFloat32ToFloat64;
            #endif
            break;
        case ASIOSTInt32MSB:
            /* src: paInt32, no conversion necessary, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_
                *converter = Swap32;
            #endif
            break;
        case ASIOSTInt32LSB:
            /* src: paInt32, no conversion necessary, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = Swap32;
            #endif
            break;
        case ASIOSTInt32MSB16:
            /* src: paInt32, 16 bit shift, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_
                *converter = ShiftRightSwap32;
            #else
                *converter = ShiftRight32;
            #endif
            *shift = 16;
            break;
        case ASIOSTInt32MSB18:
            /* src: paInt32, 14 bit shift, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_
                *converter = ShiftRightSwap32;
            #else
                *converter = ShiftRight32;
            #endif
            *shift = 14;
            break;
        case ASIOSTInt32MSB20:
            /* src: paInt32, 12 bit shift, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_
                *converter = ShiftRightSwap32;
            #else
                *converter = ShiftRight32;
            #endif
            *shift = 12;
            break;
        case ASIOSTInt32MSB24:
            /* src: paInt32, 8 bit shift, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_
                *converter = ShiftRightSwap32;
            #else
                *converter = ShiftRight32;
            #endif
            *shift = 8;
            break;
        case ASIOSTInt32LSB16:
            /* src: paInt32, 16 bit shift, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = ShiftRightSwap32;
            #else
                *converter = ShiftRight32;
            #endif
            *shift = 16;
            break;
        case ASIOSTInt32LSB18:
            /* src: paInt32, 14 bit shift, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = ShiftRightSwap32;
            #else
                *converter = ShiftRight32;
            #endif
            *shift = 14;
            break;
        case ASIOSTInt32LSB20:
            /* src: paInt32, 12 bit shift, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = ShiftRightSwap32;
            #else
                *converter = ShiftRight32;
            #endif
            *shift = 12;
            break;
        case ASIOSTInt32LSB24:
            /* src: paInt32, 8 bit shift, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = ShiftRightSwap32;
            #else
                *converter = ShiftRight32;
            #endif
            *shift = 8;
            break;
        case ASIOSTInt24MSB:
            /* src: paInt24, no conversion necessary, possible byte swap */
            #ifdef PA_LSB_IS_NATIVE_
                *converter = Swap24;
            #endif
            break;
        case ASIOSTInt24LSB:
            /* src: paInt24, no conversion necessary, possible byte swap */
            #ifdef PA_MSB_IS_NATIVE_
                *converter = Swap24;
            #endif
            break;
    }
}


#define PA_NUM_POSSIBLESAMPLINGRATES_     12   /* must be the same number of elements as in the array below */
static ASIOSampleRate possibleSampleRates_[]
    = {8000.0, 9600.0, 11025.0, 12000.0, 16000.0, 22050.0, 24000.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0};


PaError PaAsio_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int i, j, driverCount;
    PaAsioHostApiRepresentation *asioHostApi;
    PaDeviceInfo *deviceInfoArray;
    char **names;
    ASIOError asioError;
    ASIODriverInfo asioDriverInfo;
    ASIOChannelInfo asioChannelInfo;
    long inputChannels, outputChannels;
    double *sampleRates;
    
    asioHostApi = (PaAsioHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaAsioHostApiRepresentation) );
    if( !asioHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    asioHostApi->allocations = PaUtil_CreateAllocationContext();
    if( !asioHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    asioHostApi->driverOpen = 0;

    *hostApi = &asioHostApi->commonHostApiRep;
    (*hostApi)->info.structVersion = 1;

    /* REVIEW: should we really have separate host api type ids for windows and macintosh? */
#if MAC
    (*hostApi)->info.type = paMacOSASIO;
#elif WINDOWS
    (*hostApi)->info.type = paWin32ASIO;
#endif

    (*hostApi)->info.name = "ASIO";
    (*hostApi)->deviceCount = 0;

    #ifdef WINDOWS
        CoInitialize(0);
    #endif
    
    /* MUST BE CHECKED : to force fragments loading on Mac */
    loadAsioDriver( "dummy" );

    
    /* driverCount is the number of installed drivers - not necessarily
        the number of installed physical devices. */
    #if MAC
        driverCount = asioDrivers->getNumFragments();
    #elif WINDOWS
        driverCount = asioDrivers->asioGetNumDev();
    #endif

    if( driverCount > 0 )
    {
        names = GetAsioDriverNames( asioHostApi->allocations, driverCount );
        if( !names )
        {
            result = paInsufficientMemory;
            goto error;
        }
        

        /* allocate enough space for all drivers, even if some aren't installed */

        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_ContextAllocateMemory(
                asioHostApi->allocations, sizeof(PaDeviceInfo*) * driverCount );
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all device info structs in a contiguous block */
        deviceInfoArray = (PaDeviceInfo*)PaUtil_ContextAllocateMemory(
                asioHostApi->allocations, sizeof(PaDeviceInfo) * driverCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }


        #if WINDOWS
            asioDriverInfo.asioVersion = 2; /* FIXME - is this right? PLB */
            asioDriverInfo.sysRef = GetDesktopWindow(); /* FIXME - is this right? PLB */
        #elif MAC
            /* REVIEW: is anything needed here?? RDB */
        #endif

        for( i=0; i < driverCount; ++i )
        {

            /* Attempt to load the asio driver... */
            if ( !loadAsioDriver( names[i] ) )
            {
                PA_DEBUG(("PaAsio_Initialize: could not loadAsioDriver %s\n", names[i]));
            }
            else if( (asioError = ASIOInit(&asioDriverInfo)) != ASE_OK )
            {
                PA_DEBUG(("PaAsio_Initialize: ASIOInit returned %d for %s\n", asioError, names[i]));
            }
            else if( (asioError = ASIOGetChannels(&inputChannels, &outputChannels)) != ASE_OK )
            {
                PA_DEBUG(("PaAsio_Initialize: could not ASIOGetChannels for %s\n", names[i]));

                /* you would think that we should unload the driver here, but it seems to cause crashes, so don't */
                /* ASIOExit(); */
            }
            else
            {
                PaDeviceInfo *deviceInfo = &deviceInfoArray[ (*hostApi)->deviceCount ];

                deviceInfo->structVersion = 2;
                deviceInfo->hostApi = hostApiIndex;

                deviceInfo->name = names[i];

                deviceInfo->maxInputChannels = inputChannels;
                deviceInfo->maxOutputChannels = outputChannels;

                PA_DEBUG(("PaAsio_Initialize: inputChannels = %d\n", inputChannels ));
                PA_DEBUG(("PaAsio_Initialize: outputChannels = %d\n", outputChannels ));

                deviceInfo->numSampleRates = 0;

                /* allocate space for all possible sample rates */
                sampleRates = (double*)PaUtil_ContextAllocateMemory(
                        asioHostApi->allocations, PA_NUM_POSSIBLESAMPLINGRATES_ * sizeof(double) );
                if( !sampleRates )
                {
                    ASIOExit();
                    result = paInsufficientMemory;
                    goto error;
                }

                deviceInfo->sampleRates = sampleRates;
                deviceInfo->numSampleRates = 0;

                /* Loop through the possible sampling rates and check each to see if the device supports it. */
                for( j = 0; j < PA_NUM_POSSIBLESAMPLINGRATES_; ++j )
                {
                    if( ASIOCanSampleRate(possibleSampleRates_[j]) != ASE_NoClock ){
                        PA_DEBUG(("PaAsio_Initialize : %s, possible sample rate = %d\n", names[i], (long)possibleSampleRates_[j]));
                        *sampleRates = possibleSampleRates_[j];
                        sampleRates++;
                        deviceInfo->numSampleRates += 1;
                    }
                }

                    
                /* We assume that all channels have the same SampleType, so check the first */
                asioChannelInfo.channel = 0;
                asioChannelInfo.isInput = 1;
                ASIOGetChannelInfo( &asioChannelInfo );
                    
                deviceInfo->nativeSampleFormats = AsioSampleTypeToPaNativeSampleFormat( asioChannelInfo.type );

                /* unload the driver */
                ASIOExit();

                (*hostApi)->deviceInfos[ (*hostApi)->deviceCount ] = deviceInfo;
                ++(*hostApi)->deviceCount;
            }
        }
    }

    if( (*hostApi)->deviceCount > 0 )
    {
        (*hostApi)->defaultInputDeviceIndex = 0;
        (*hostApi)->defaultOutputDeviceIndex = 0;
    }
    else
    {
        (*hostApi)->defaultInputDeviceIndex = paNoDevice;
        (*hostApi)->defaultOutputDeviceIndex = paNoDevice;
    }


    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;

    PaUtil_InitializeStreamInterface( &asioHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive, GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyReadWrite, PaUtil_DummyReadWrite, PaUtil_DummyGetAvailable, PaUtil_DummyGetAvailable );

    PaUtil_InitializeStreamInterface( &asioHostApi->blockingStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive, GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    return result;

error:
    if( asioHostApi )
    {
        if( asioHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( asioHostApi->allocations );
            PaUtil_DestroyAllocationContext( asioHostApi->allocations );
        }
                
        PaUtil_FreeMemory( asioHostApi );
    }
    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaAsioHostApiRepresentation *asioHostApi = (PaAsioHostApiRepresentation*)hostApi;

    /*
        IMPLEMENT ME:
            - clean up any resources not handled by the allocation context
    */

    if( asioHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( asioHostApi->allocations );
        PaUtil_DestroyAllocationContext( asioHostApi->allocations );
    }

    PaUtil_FreeMemory( asioHostApi );
}

typedef struct
{
    ASIODriverInfo asioDriverInfo;
    long numInputChannels, numOutputChannels;
    long bufferMinSize, bufferMaxSize, bufferPreferredSize, bufferGranularity;
    bool postOutput;
}
PaAsioDriverInfo;


/* PaAsioStream - a stream data structure specifically for this implementation */

typedef struct PaAsioStream
{ 
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    PaAsioHostApiRepresentation *asioHostApi;
    unsigned long framesPerHostCallback;

    /* ASIO driver info  - these may not be needed for the life of the stream,
        but store them here until we work out how format conversion is going
        to work. */

    ASIOBufferInfo *asioBufferInfos;
    ASIOChannelInfo *asioChannelInfos;
    long inputLatency, outputLatency;

    long numInputChannels, numOutputChannels;
    bool postOutput;

    void **bufferPtrs; /* this is carved up for inputBufferPtrs and outputBufferPtrs */
    void **inputBufferPtrs[2];
    void **outputBufferPtrs[2];

    PaAsioBufferConverter *inputBufferConverter;
    long inputShift;
    PaAsioBufferConverter *outputBufferConverter;
    long outputShift;

    volatile int stopProcessing; /* stop thread once existing buffers have been returned */
    volatile int abortProcessing; /* stop thread immediately */
}
PaAsioStream;

static PaAsioStream *theAsioStream = 0; /* due to ASIO sdk limitations there can be only one stream */


/*
    load the asio driver named by <driverName> and return statistics about
    the driver in info. If no error occurred, the driver will remain open
    and must be closed by the called by calling ASIOExit() - if an error
    is returned the driver will already be closed.
*/
static PaError LoadAsioDriver( const char *driverName, PaAsioDriverInfo *info )
{
    PaError result = paNoError;
    ASIOError asioError;
    int asioIsInitialized = 0;
    
    if( !loadAsioDriver( const_cast<char*>(driverName) ) )
    {
        result = paHostError;
        PaUtil_SetHostError( 0 );
        goto error;
    }

    if( (asioError = ASIOInit( &info->asioDriverInfo )) != ASE_OK )
    {
        result = paHostError;
        PaUtil_SetHostError( asioError );
        goto error;
    }
    else
    {
        asioIsInitialized = 1;
    }

    if( (asioError = ASIOGetChannels(&info->numInputChannels,
            &info->numOutputChannels)) != ASE_OK )
    {
        result = paHostError;
        PaUtil_SetHostError( asioError );
        goto error;
    }

    if( (asioError = ASIOGetBufferSize(&info->bufferMinSize,
            &info->bufferMaxSize, &info->bufferPreferredSize,
            &info->bufferGranularity)) != ASE_OK )
    {
        result = paHostError;
        PaUtil_SetHostError( asioError );
        goto error;
    }

    if( ASIOOutputReady() == ASE_OK )
        info->postOutput = true;
    else
        info->postOutput = false;

    return result;

error:
    if( asioIsInitialized )
        ASIOExit();
        
    return result;
}


static void ZeroOutputBuffers( PaAsioStream *stream, long index )
{
    int i;
    
    for( i=0; i < stream->numOutputChannels; ++i )
    {
        void *buffer = stream->asioBufferInfos[ i + stream->numInputChannels ].buffers[index];

        int bytesPerSample = BytesPerAsioSample( stream->asioChannelInfos[ i + stream->numInputChannels ].type );

        memset( buffer, 0, stream->framesPerHostCallback * bytesPerSample );
    }
}


/* see pa_hostapi.h for a list of validity guarantees made about OpenStream parameters */

static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           PaDeviceIndex inputDevice,
                           int numInputChannels,
                           PaSampleFormat inputSampleFormat,
                           unsigned long inputLatency,
                           void *inputStreamInfo,
                           PaDeviceIndex outputDevice,
                           int numOutputChannels,
                           PaSampleFormat outputSampleFormat,
                           unsigned long outputLatency,
                           void *outputStreamInfo,
                           double sampleRate,
                           unsigned long framesPerCallback,
                           PaStreamFlags streamFlags,
                           PortAudioCallback *callback,
                           void *userData )
{
    PaError result = paNoError;
    PaAsioHostApiRepresentation *asioHostApi = (PaAsioHostApiRepresentation*)hostApi;
    PaAsioStream *stream = 0;
    unsigned long framesPerHostBuffer = framesPerCallback; /* these may not be equivalent for all implementations */
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;
    const char *driverName;
    ASIOError asioError;
    int asioIsInitialized = 0;
    int asioBuffersCreated = 0;
    PaAsioDriverInfo driverInfo;
    int i;
    
    /* unless we move to using lower level ASIO calls, we can only have
        one device open at a time */
    if( asioHostApi->driverOpen )
        return paDeviceUnavailable;

    if( inputDevice != paNoDevice && outputDevice != paNoDevice )
    {
        /* full duplex ASIO stream must use the same device for input and output */

        if( inputDevice != outputDevice )
            return paBadIODeviceCombination;
    }

    if( inputDevice != paNoDevice )
    {
        driverName = asioHostApi->commonHostApiRep.deviceInfos[ inputDevice ]->name;
    }
    else
    {
        driverName = asioHostApi->commonHostApiRep.deviceInfos[ outputDevice ]->name;
    }

    result = LoadAsioDriver( driverName, &driverInfo );
    if( result == paNoError )
        asioIsInitialized = 1;
    else
        goto error;
    
    /* check that input device can support numInputChannels */
    if( inputDevice != paNoDevice )
    {
        if( numInputChannels > driverInfo.numInputChannels )
        {
            result = paInvalidChannelCount;
            goto error;
        }
    }

    /* check that output device can support numOutputChannels */
    if( outputDevice != paNoDevice )
    {
        if( numOutputChannels > driverInfo.numOutputChannels )
        {
            result = paInvalidChannelCount;
            goto error;
        }
    }

    /* Set sample rate */
    if( ASIOSetSampleRate( sampleRate ) != ASE_OK )
    {
        result = paInvalidSampleRate;
        goto error;
    }

    /*
        FIXME: for now we just force the user buffer size to conform
        with an allowable host buffer size...
    */

    framesPerHostBuffer = driverInfo.bufferPreferredSize;
    framesPerCallback = framesPerHostBuffer;
    
    /*
        IMPLEMENT ME:
            - if a full duplex stream is requested, check that the combination
                of input and output parameters is supported

            - validate inputLatency and outputLatency parameters,
                use default values where necessary
    */


    /* validate inputStreamInfo */
    if( inputStreamInfo )
        return paIncompatibleStreamInfo; /* this implementation doesn't use custom stream info */

    /* validate outputStreamInfo */
    if( outputStreamInfo )
        return paIncompatibleStreamInfo; /* this implementation doesn't use custom stream info */

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */


    stream = (PaAsioStream*)PaUtil_AllocateMemory( sizeof(PaAsioStream) );
    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }

    stream->asioBufferInfos = 0; /* for deallocation in error */
    stream->asioChannelInfos = 0; /* for deallocation in error */
    stream->bufferPtrs = 0; /* for deallocation in error */
    
    if( callback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &asioHostApi->callbackStreamInterface, callback, userData );
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &asioHostApi->blockingStreamInterface, callback, userData );
    }


    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    
    stream->asioBufferInfos = (ASIOBufferInfo*)PaUtil_AllocateMemory(
            sizeof(ASIOBufferInfo) * (numInputChannels + numOutputChannels) );
    if( !stream->asioBufferInfos )
    {
        result = paInsufficientMemory;
        goto error;
    }


    for( i=0; i < numInputChannels; ++i )
    {
        ASIOBufferInfo *info = &stream->asioBufferInfos[i];
        
        info->isInput = ASIOTrue;
        info->channelNum = i;
        info->buffers[0] = info->buffers[1] = 0;
    }
        
    for( i=0; i < numOutputChannels; ++i ){
        ASIOBufferInfo *info = &stream->asioBufferInfos[numInputChannels+i];
        
        info->isInput = ASIOFalse;
        info->channelNum = i;
        info->buffers[0] = info->buffers[1] = 0;
    }

    asioError = ASIOCreateBuffers( stream->asioBufferInfos, numInputChannels+numOutputChannels,
                                  framesPerHostBuffer, &asioCallbacks_ );
    if( asioError != ASE_OK )
    {
        result = paHostError;
        PaUtil_SetHostError( asioError );
        goto error;
    } 

    asioBuffersCreated = 1;

    stream->asioChannelInfos = (ASIOChannelInfo*)PaUtil_AllocateMemory(
            sizeof(ASIOChannelInfo) * (numInputChannels + numOutputChannels) );
    if( !stream->asioChannelInfos )
    {
        result = paInsufficientMemory;
        goto error;
    }

    for( i=0; i < numInputChannels + numOutputChannels; ++i )
    {
        stream->asioChannelInfos[i].channel = stream->asioBufferInfos[i].channelNum;
        stream->asioChannelInfos[i].isInput = stream->asioBufferInfos[i].isInput;
        asioError = ASIOGetChannelInfo( &stream->asioChannelInfos[i] );
        if( asioError != ASE_OK )
        {
            result = paHostError;
            PaUtil_SetHostError( asioError );
            goto error;
        }
    }

    stream->bufferPtrs = (void**)PaUtil_AllocateMemory(
            2 * sizeof(void*) * (numInputChannels + numOutputChannels) );
    if( !stream->bufferPtrs )
    {
        result = paInsufficientMemory;
        goto error;
    }

    if( numInputChannels > 0 )
    {
        stream->inputBufferPtrs[0] = stream-> bufferPtrs;
        stream->inputBufferPtrs[1] = &stream->bufferPtrs[numInputChannels];

        for( i=0; i<numInputChannels; ++i )
        {
            stream->inputBufferPtrs[0][i] = stream->asioBufferInfos[i].buffers[0];
            stream->inputBufferPtrs[1][i] = stream->asioBufferInfos[i].buffers[1];
        }
    }
    else
    {
        stream->inputBufferPtrs[0] = 0;
        stream->inputBufferPtrs[1] = 0;
    }

    if( numOutputChannels > 0 )
    {
        stream->outputBufferPtrs[0] = &stream->bufferPtrs[numInputChannels*2];
        stream->outputBufferPtrs[1] = &stream->bufferPtrs[numInputChannels*2 + numOutputChannels];

        for( i=0; i<numOutputChannels; ++i )
        {
            stream->outputBufferPtrs[0][i] = stream->asioBufferInfos[numInputChannels+i].buffers[0];
            stream->outputBufferPtrs[1][i] = stream->asioBufferInfos[numInputChannels+i].buffers[1];
        }
    }
    else
    {
        stream->outputBufferPtrs[0] = 0;
        stream->outputBufferPtrs[1] = 0;
    }


    ASIOGetLatencies( &stream->inputLatency, &stream->outputLatency );

    PA_DEBUG(("PaAsio : InputLatency = %ld latency = %ld msec \n",
            stream->inputLatency,
            (long)((stream->inputLatency*1000)/ sampleRate)));
    PA_DEBUG(("PaAsio : OuputLatency = %ld latency = %ld msec \n",
            stream->outputLatency,
            (long)((stream->outputLatency*1000)/ sampleRate)));


    if( numInputChannels > 0 )
    {
        /* FIXME: assume all channels use the same type for now */
        ASIOSampleType inputType = stream->asioChannelInfos[0].type;

        hostInputSampleFormat = AsioSampleTypeToPaNativeSampleFormat( inputType );

        SelectAsioToPaConverter( inputType, &stream->inputBufferConverter, &stream->inputShift );
    }

    if( numOutputChannels > 0 )
    {
        /* FIXME: assume all channels use the same type for now */
        ASIOSampleType outputType = stream->asioChannelInfos[numInputChannels].type;
        
        hostOutputSampleFormat = AsioSampleTypeToPaNativeSampleFormat( outputType );

        SelectPaToAsioConverter( outputType, &stream->outputBufferConverter, &stream->outputShift );
    }

    result =  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              numInputChannels, inputSampleFormat, hostInputSampleFormat,
              numOutputChannels, outputSampleFormat, hostOutputSampleFormat,
              sampleRate, streamFlags, framesPerCallback, framesPerHostBuffer,
              callback, userData );
    if( result != paNoError )
        goto error;

    stream->asioHostApi = asioHostApi;
    stream->framesPerHostCallback = framesPerHostBuffer;

    stream->numInputChannels = numInputChannels;
    stream->numOutputChannels = numOutputChannels;
    stream->postOutput = driverInfo.postOutput;

    asioHostApi->driverOpen = 1;

    *s = (PaStream*)stream;

    return result;

error:
    if( stream )
    {
        if( stream->asioBufferInfos )
            PaUtil_FreeMemory( stream->asioBufferInfos );

        if( stream->asioChannelInfos )
            PaUtil_FreeMemory( stream->asioChannelInfos );

        if( stream->bufferPtrs )
            PaUtil_FreeMemory( stream->bufferPtrs );

        PaUtil_FreeMemory( stream );
    }

    if( asioBuffersCreated )
        ASIODisposeBuffers();

    if( asioIsInitialized )
        ASIOExit();

    return result;
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaAsioStream *stream = (PaAsioStream*)s;

    /*
        IMPLEMENT ME:
            - additional stream closing + cleanup
    */

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );

    stream->asioHostApi->driverOpen = 0;

    PaUtil_FreeMemory( stream->asioBufferInfos );
    PaUtil_FreeMemory( stream->asioChannelInfos );
    PaUtil_FreeMemory( stream->bufferPtrs );
    PaUtil_FreeMemory( stream );

    ASIODisposeBuffers();
    ASIOExit();
    
    return result;
}


static void bufferSwitch(long index, ASIOBool processNow)
{
//TAKEN FROM THE ASIO SDK

    // the actual processing callback.
    // Beware that this is normally in a seperate thread, hence be sure that
    // you take care about thread synchronization. This is omitted here for
    // simplicity.

    // as this is a "back door" into the bufferSwitchTimeInfo a timeInfo needs
    // to be created though it will only set the timeInfo.samplePosition and
    // timeInfo.systemTime fields and the according flags
        
    ASIOTime  timeInfo;
    memset( &timeInfo, 0, sizeof (timeInfo) );

    // get the time stamp of the buffer, not necessary if no
    // synchronization to other media is required
    if( ASIOGetSamplePosition(&timeInfo.timeInfo.samplePosition, &timeInfo.timeInfo.systemTime) == ASE_OK)
            timeInfo.timeInfo.flags = kSystemTimeValid | kSamplePositionValid;
                
    // Call the real callback
    bufferSwitchTimeInfo( &timeInfo, index, processNow );
}


static ASIOTime *bufferSwitchTimeInfo( ASIOTime *timeInfo, long index, ASIOBool processNow )
{
    // the actual processing callback.
    // Beware that this is normally in a seperate thread, hence be sure that
    // you take care about thread synchronization. This is omitted here for simplicity.

#if 0
    // store the timeInfo for later use
    asioDriverInfo.tInfo = *timeInfo;

    // get the time stamp of the buffer, not necessary if no
    // synchronization to other media is required
        
    if (timeInfo->timeInfo.flags & kSystemTimeValid)
            asioDriverInfo.nanoSeconds = ASIO64toDouble(timeInfo->timeInfo.systemTime);
    else
            asioDriverInfo.nanoSeconds = 0;

    if (timeInfo->timeInfo.flags & kSamplePositionValid)
            asioDriverInfo.samples = ASIO64toDouble(timeInfo->timeInfo.samplePosition);
    else
            asioDriverInfo.samples = 0;

    if (timeInfo->timeCode.flags & kTcValid)
            asioDriverInfo.tcSamples = ASIO64toDouble(timeInfo->timeCode.timeCodeSamples);
    else
            asioDriverInfo.tcSamples = 0;

    // get the system reference time
    asioDriverInfo.sysRefTime = get_sys_reference_time();
#endif
    
#if 0
    // a few debug messages for the Windows device driver developer
    // tells you the time when driver got its interrupt and the delay until the app receives
    // the event notification.
    static double last_samples = 0;
    char tmp[128];
    sprintf (tmp, "diff: %d / %d ms / %d ms / %d samples                 \n", asioDriverInfo.sysRefTime - (long)(asioDriverInfo.nanoSeconds / 1000000.0), asioDriverInfo.sysRefTime, (long)(asioDriverInfo.nanoSeconds / 1000000.0), (long)(asioDriverInfo.samples - last_samples));
    OutputDebugString (tmp);
    last_samples = asioDriverInfo.samples;
#endif

    // Keep sample position
    // FIXME: asioDriverInfo.pahsc_NumFramesDone = timeInfo->timeInfo.samplePosition.lo;

    if( theAsioStream->stopProcessing || theAsioStream->abortProcessing ) {

        ZeroOutputBuffers( theAsioStream, index );

        // Finally if the driver supports the ASIOOutputReady() optimization,
        // do it here, all data are in place
        if( theAsioStream->postOutput )
            ASIOOutputReady();
       
    }
    else
    {
        PaUtil_BeginCpuLoadMeasurement( &theAsioStream->cpuLoadMeasurer, theAsioStream->framesPerHostCallback );


        PaTimestamp outTime = 0; /* FIXME */

        if( theAsioStream->numInputChannels > 0 && theAsioStream->inputBufferConverter )
        {
            for( int i=0; i<theAsioStream->numInputChannels; i++ )
            {
                theAsioStream->inputBufferConverter( theAsioStream->inputBufferPtrs[index][i],
                        theAsioStream->inputShift, theAsioStream->framesPerHostCallback );
            }
        }


        int callbackResult = PaUtil_ProcessNonInterleavedBuffers( &theAsioStream->bufferProcessor,
            theAsioStream->inputBufferPtrs[index], theAsioStream->outputBufferPtrs[index], outTime );


        if( theAsioStream->numOutputChannels > 0 && theAsioStream->outputBufferConverter )
        {
            for( int i=0; i<theAsioStream->numOutputChannels; i++ )
            {
                theAsioStream->outputBufferConverter( theAsioStream->outputBufferPtrs[index][i],
                        theAsioStream->outputShift, theAsioStream->framesPerHostCallback );
            }
        }


        PaUtil_EndCpuLoadMeasurement( &theAsioStream->cpuLoadMeasurer );

        // Finally if the driver supports the ASIOOutputReady() optimization,
        // do it here, all data are in place
        if( theAsioStream->postOutput )
            ASIOOutputReady();

        if( callbackResult == paContinue )
        {
            /* nothing special to do */
        }
        else if( callbackResult == paAbort )
        {
            /* IMPLEMENT ME - finish playback immediately  */
        }
        else
        {
            /* User callback has asked us to stop with paComplete or other non-zero value */

            /* IMPLEMENT ME - finish playback once currently queued audio has completed  */
        }
    }

    return 0L;
}


static void sampleRateChanged(ASIOSampleRate sRate)
{
// TAKEN FROM THE ASIO SDK
    // do whatever you need to do if the sample rate changed
    // usually this only happens during external sync.
    // Audio processing is not stopped by the driver, actual sample rate
    // might not have even changed, maybe only the sample rate status of an
    // AES/EBU or S/PDIF digital input at the audio device.
    // You might have to update time/sample related conversion routines, etc.
}

static long asioMessages(long selector, long value, void* message, double* opt)
{
// TAKEN FROM THE ASIO SDK
    // currently the parameters "value", "message" and "opt" are not used.
    long ret = 0;
    switch(selector)
    {
        case kAsioSelectorSupported:
            if(value == kAsioResetRequest
            || value == kAsioEngineVersion
            || value == kAsioResyncRequest
            || value == kAsioLatenciesChanged
            // the following three were added for ASIO 2.0, you don't necessarily have to support them
            || value == kAsioSupportsTimeInfo
            || value == kAsioSupportsTimeCode
            || value == kAsioSupportsInputMonitor)
                    ret = 1L;
            break;

        case kAsioBufferSizeChange:
            //printf("kAsioBufferSizeChange \n");
            break;

        case kAsioResetRequest:
            // defer the task and perform the reset of the driver during the next "safe" situation
            // You cannot reset the driver right now, as this code is called from the driver.
            // Reset the driver is done by completely destruct is. I.e. ASIOStop(), ASIODisposeBuffers(), Destruction
            // Afterwards you initialize the driver again.

            /*FIXME: commented the next line out */
            //asioDriverInfo.stopped;  // In this sample the processing will just stop
            ret = 1L;
            break;

        case kAsioResyncRequest:
            // This informs the application, that the driver encountered some non fatal data loss.
            // It is used for synchronization purposes of different media.
            // Added mainly to work around the Win16Mutex problems in Windows 95/98 with the
            // Windows Multimedia system, which could loose data because the Mutex was hold too long
            // by another thread.
            // However a driver can issue it in other situations, too.
            ret = 1L;
            break;

        case kAsioLatenciesChanged:
            // This will inform the host application that the drivers were latencies changed.
            // Beware, it this does not mean that the buffer sizes have changed!
            // You might need to update internal delay data.
            ret = 1L;
            //printf("kAsioLatenciesChanged \n");
            break;

        case kAsioEngineVersion:
            // return the supported ASIO version of the host application
            // If a host applications does not implement this selector, ASIO 1.0 is assumed
            // by the driver
            ret = 2L;
            break;

        case kAsioSupportsTimeInfo:
            // informs the driver wether the asioCallbacks.bufferSwitchTimeInfo() callback
            // is supported.
            // For compatibility with ASIO 1.0 drivers the host application should always support
            // the "old" bufferSwitch method, too.
            ret = 1;
            break;

        case kAsioSupportsTimeCode:
            // informs the driver wether application is interested in time code info.
            // If an application does not need to know about time code, the driver has less work
            // to do.
            ret = 0;
            break;
    }
    return ret;
}


static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaAsioStream *stream = (PaAsioStream*)s;
    ASIOError asioError;

    if( stream->numOutputChannels > 0 )
    {
        ZeroOutputBuffers( stream, 0 );
        ZeroOutputBuffers( stream, 1 );
    }
    
    stream->stopProcessing = 0;
    stream->abortProcessing = 0;
    
    theAsioStream = stream;
    asioError = ASIOStart();
    if( asioError != ASE_OK )
    {
        theAsioStream = 0;
        result = paHostError;
        PaUtil_SetHostError( asioError );
    }

    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaAsioStream *stream = (PaAsioStream*)s;
    ASIOError asioError;

    stream->stopProcessing = 1;
    stream->abortProcessing = 1;
    
    asioError = ASIOStop();
    if( asioError != ASE_OK )
    {
        result = paHostError;
        PaUtil_SetHostError( asioError );
    }

    theAsioStream = 0;
    
    return result;
}


static PaError AbortStream( PaStream *s )
{
    /* ASIO doesn't provide Abort behavior, so just stop instead */
    return StopStream( s );
}


static PaError IsStreamStopped( PaStream *s )
{
    PaAsioStream *stream = (PaAsioStream*)s;

    return theAsioStream == 0;
}


static PaError IsStreamActive( PaStream *s )
{
    PaAsioStream *stream = (PaAsioStream*)s;

    return theAsioStream != 0; /* FIXME: currently there is no way to stop the stream from the callback */
}


static PaTimestamp GetStreamTime( PaStream *s )
{
    PaAsioStream *stream = (PaAsioStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaAsioStream *stream = (PaAsioStream*)s;

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
    PaAsioStream *stream = (PaAsioStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            void *buffer,
                            unsigned long frames )
{
    PaAsioStream *stream = (PaAsioStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static unsigned long GetStreamReadAvailable( PaStream* s )
{
    PaAsioStream *stream = (PaAsioStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


static unsigned long GetStreamWriteAvailable( PaStream* s )
{
    PaAsioStream *stream = (PaAsioStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


// OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE
//  OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE
//   OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE
//  OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE
// OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE OLDCODE

#if (0) /* OLD CODE IFDEFED OUT */

/*
enum {
        // number of input and outputs supported by the host application
        // you can change these to higher or lower values
        kMaxInputChannels = 32,
        kMaxOutputChannels = 32
};
*/

/* ASIO specific device information. */
/*
typedef struct internalPortAudioDevice
{
        PaDeviceInfo    pad_Info;
} internalPortAudioDevice;
*/

/*  ASIO driver internal data storage */
/*
typedef struct PaHostSoundControl
{
        // ASIOInit()
        ASIODriverInfo  pahsc_driverInfo;

        // ASIOGetChannels()
        int32           pahsc_NumInputChannels;
        int32           pahsc_NumOutputChannels;

        // ASIOGetBufferSize() - sizes in frames per buffer
        int32           pahsc_minSize;
        int32           pahsc_maxSize;
        int32           pahsc_preferredSize;
        int32           pahsc_granularity;

        // ASIOGetSampleRate()
        ASIOSampleRate pahsc_sampleRate;

        // ASIOOutputReady()
        bool           pahsc_postOutput;

        // ASIOGetLatencies ()
        int32          pahsc_inputLatency;
        int32          pahsc_outputLatency;

        // ASIOCreateBuffers ()
        ASIOBufferInfo bufferInfos[kMaxInputChannels + kMaxOutputChannels]; // buffer info's

        // ASIOGetChannelInfo()
        ASIOChannelInfo pahsc_channelInfos[kMaxInputChannels + kMaxOutputChannels]; // channel info's
        // The above two arrays share the same indexing, as the data in them are linked together

        // Information from ASIOGetSamplePosition()
        // data is converted to double floats for easier use, however 64 bit integer can be used, too
        double         nanoSeconds;
        double         samples;
        double         tcSamples;       // time code samples

        // bufferSwitchTimeInfo()
        ASIOTime       tInfo;           // time info state
        unsigned long  sysRefTime;      // system reference time, when bufferSwitch() was called

        // Signal the end of processing in this example
        bool           stopped;

        ASIOCallbacks   pahsc_asioCallbacks;


        int32   pahsc_userInputBufferFrameOffset;   // Position in Input user buffer
        int32   pahsc_userOutputBufferFrameOffset;  // Position in Output user buffer
        int32   pahsc_hostOutputBufferFrameOffset;  // Position in Output ASIO buffer

        int32  past_FramesPerHostBuffer;        // Number of frames in ASIO buffer

        int32  pahsc_InputBufferOffset;         // Number of null frames for input buffer alignement
        int32  pahsc_OutputBufferOffset;        // Number of null frames for ouput buffer alignement

#if MAC
        UInt64   pahsc_EntryCount;
        UInt64   pahsc_LastExitCount;
#elif WINDOWS
        LARGE_INTEGER      pahsc_EntryCount;
        LARGE_INTEGER      pahsc_LastExitCount;
#endif

        PaTimestamp   pahsc_NumFramesDone;

        internalPortAudioStream   *past;

} PaHostSoundControl;
*/

#define PA_MAX_DEVICE_INFO (32)

#define MIN_INT8     (-0x80)
#define MAX_INT8     (0x7F)

#define MIN_INT8_FP  ((float)-0x80)
#define MAX_INT8_FP  ((float)0x7F)

#define MIN_INT16_FP ((float)-0x8000)
#define MAX_INT16_FP ((float)0x7FFF)

#define MIN_INT16    (-0x8000)
#define MAX_INT16    (0x7FFF)

#define MAX_INT32_FP (2147483520.0f)  /* 0x0x7FFFFF80 - seems safe */

/************************************************************************************/
/****************** Data ************************************************************/
/************************************************************************************/


static int                 sNumDevices = 0;
static internalPortAudioDevice sDevices[PA_MAX_DEVICE_INFO] = { 0 };
static int32               sPaHostError = 0;
static int                 sDefaultOutputDeviceID = 0;
static int                 sDefaultInputDeviceID = 0;

PaHostSoundControl asioDriverInfo = {0};

#ifdef MAC
static bool swap = true;
#elif WINDOWS
static bool swap = false;
#endif


// Prototypes
static void bufferSwitch(long index, ASIOBool processNow);
static ASIOTime *bufferSwitchTimeInfo(ASIOTime *timeInfo, long index, ASIOBool processNow);
static void sampleRateChanged(ASIOSampleRate sRate);
static long asioMessages(long selector, long value, void* message, double* opt);
static void Pa_StartUsageCalculation( internalPortAudioStream   *past );
static void Pa_EndUsageCalculation( internalPortAudioStream   *past );

static void Pa_ASIO_Convert_Inter_Input(
        ASIOBufferInfo* nativeBuffer, 
        void* inputBuffer,
        long NumInputChannels, 
        long NumOuputChannels,
        long framePerBuffer,
        long hostFrameOffset,
        long userFrameOffset,
        ASIOSampleType nativeFormat, 
        PaSampleFormat paFormat, 
        PaStreamFlags flags,
        long index);

static void Pa_ASIO_Convert_Inter_Output(
        ASIOBufferInfo* nativeBuffer, 
        void* outputBuffer,
        long NumInputChannels, 
        long NumOuputChannels,
        long framePerBuffer,
        long hostFrameOffset,
        long userFrameOffset,
        ASIOSampleType nativeFormat, 
        PaSampleFormat paFormat, 
        PaStreamFlags flags,
        long index);

static void Pa_ASIO_Clear_Output(ASIOBufferInfo* nativeBuffer, 
        ASIOSampleType nativeFormat,
        long NumInputChannels, 
        long NumOuputChannels,
        long index, 
        long hostFrameOffset, 
        long frames);

static void Pa_ASIO_Callback_Input(long index);
static void Pa_ASIO_Callback_Output(long index, long framePerBuffer);
static void Pa_ASIO_Callback_End();
static void Pa_ASIO_Clear_User_Buffers();

// Some external references
extern AsioDrivers* asioDrivers ;
bool loadAsioDriver(char *name);
unsigned long get_sys_reference_time();


/************************************************************************************/
/****************** Macro  ************************************************************/
/************************************************************************************/


#define SwapLong(v) ((((v)>>24)&0xFF)|(((v)>>8)&0xFF00)|(((v)&0xFF00)<<8)|(((v)&0xFF)<<24)) ;
#define SwapShort(v) ((((v)>>8)&0xFF)|(((v)&0xFF)<<8)) ;

#define ClipShort(v) (((v)<MIN_INT16)?MIN_INT16:(((v)>MAX_INT16)?MAX_INT16:(v)))
#define ClipChar(v) (((v)<MIN_INT8)?MIN_INT8:(((v)>MAX_INT8)?MAX_INT8:(v)))
#define ClipFloat(v) (((v)<-1.0f)?-1.0f:(((v)>1.0f)?1.0f:(v)))

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif

#ifndef max
#define max(a,b) ((a)>=(b)?(a):(b))
#endif


static bool Pa_ASIO_loadAsioDriver(char *name)
{
    #ifdef	WINDOWS
        CoInitialize(0);
    #endif
    return loadAsioDriver(name);
}

// Utilities for alignement buffer size computation
static int PGCD (int a, int b) {return (b == 0) ? a : PGCD (b,a%b);}
static int PPCM (int a, int b) {return (a*b) / PGCD (a,b);}

// Takes the size of host buffer and user buffer : returns the number of frames needed for buffer alignement
static int Pa_ASIO_CalcFrameShift (int M, int N)
{
        int res = 0;
        for (int i = M; i < PPCM (M,N) ; i+=M) { res = max (res, i%N); }
        return res;
}

// We have the following relation :
// Pa_ASIO_CalcFrameShift (M,N) + M = Pa_ASIO_CalcFrameShift (N,M) + N
                        
/* ASIO sample type to PortAudio sample type conversion */
static PaSampleFormat Pa_ASIO_Convert_SampleFormat(ASIOSampleType type) 
{
        switch (type) {
        
                case ASIOSTInt16MSB:
                case ASIOSTInt16LSB:
                case ASIOSTInt32MSB16:
                case ASIOSTInt32LSB16:
                        return paInt16;
                
                case ASIOSTFloat32MSB:
                case ASIOSTFloat32LSB:
                case ASIOSTFloat64MSB:
                case ASIOSTFloat64LSB:
                        return paFloat32;
                
                case ASIOSTInt32MSB:
                case ASIOSTInt32LSB:
                case ASIOSTInt32MSB18:          
                case ASIOSTInt32MSB20:          
                case ASIOSTInt32MSB24:          
                case ASIOSTInt32LSB18:
                case ASIOSTInt32LSB20:          
                case ASIOSTInt32LSB24:          
                        return paInt32;
                        
                case ASIOSTInt24MSB:
                case ASIOSTInt24LSB:
                        return paInt24;
                        
                default:
                        return paCustomFormat;
        }
}

/* Allocate ASIO buffers, initialise channels */
static ASIOError Pa_ASIO_CreateBuffers (PaHostSoundControl *asioDriverInfo, long InputChannels,
                                                                          long OutputChannels, long framesPerBuffer)
{
        ASIOError  err;
        int i;

        ASIOBufferInfo *info = asioDriverInfo->bufferInfos;

        // Check parameters
        if ((InputChannels > kMaxInputChannels) || (OutputChannels > kMaxInputChannels)) return ASE_InvalidParameter;

        for(i = 0; i < InputChannels; i++, info++){
                info->isInput = ASIOTrue;
                info->channelNum = i;
                info->buffers[0] = info->buffers[1] = 0;
        }
        
        for(i = 0; i < OutputChannels; i++, info++){
                info->isInput = ASIOFalse;
                info->channelNum = i;
                info->buffers[0] = info->buffers[1] = 0;
        }
        
        // Set up the asioCallback structure and create the ASIO data buffer
        asioDriverInfo->pahsc_asioCallbacks.bufferSwitch = &bufferSwitch;
        asioDriverInfo->pahsc_asioCallbacks.sampleRateDidChange = &sampleRateChanged;
        asioDriverInfo->pahsc_asioCallbacks.asioMessage = &asioMessages;
        asioDriverInfo->pahsc_asioCallbacks.bufferSwitchTimeInfo = &bufferSwitchTimeInfo;
        
        DBUG(("PortAudio : ASIOCreateBuffers with size = %ld \n", framesPerBuffer));
     
        err =  ASIOCreateBuffers( asioDriverInfo->bufferInfos, InputChannels+OutputChannels,
                                  framesPerBuffer, &asioDriverInfo->pahsc_asioCallbacks);
        if (err != ASE_OK) return err;
        
        // Initialise buffers
        for (i = 0; i < InputChannels + OutputChannels; i++)
        {
                asioDriverInfo->pahsc_channelInfos[i].channel = asioDriverInfo->bufferInfos[i].channelNum;
                asioDriverInfo->pahsc_channelInfos[i].isInput = asioDriverInfo->bufferInfos[i].isInput;
                err = ASIOGetChannelInfo(&asioDriverInfo->pahsc_channelInfos[i]);
                if (err != ASE_OK) break;
        }

        err = ASIOGetLatencies(&asioDriverInfo->pahsc_inputLatency, &asioDriverInfo->pahsc_outputLatency);
        
        DBUG(("PortAudio : InputLatency = %ld latency = %ld msec \n", 
                asioDriverInfo->pahsc_inputLatency,  
                (long)((asioDriverInfo->pahsc_inputLatency*1000)/ asioDriverInfo->past->past_SampleRate)));
        DBUG(("PortAudio : OuputLatency = %ld latency = %ld msec \n", 
                asioDriverInfo->pahsc_outputLatency,
                (long)((asioDriverInfo->pahsc_outputLatency*1000)/ asioDriverInfo->past->past_SampleRate)));
        
        return err;
}


//----------------------------------------------------------------------------------
// TAKEN FROM THE ASIO SDK: 
static void sampleRateChanged(ASIOSampleRate sRate)
{
        // do whatever you need to do if the sample rate changed
        // usually this only happens during external sync.
        // Audio processing is not stopped by the driver, actual sample rate
        // might not have even changed, maybe only the sample rate status of an
        // AES/EBU or S/PDIF digital input at the audio device.
        // You might have to update time/sample related conversion routines, etc.
}

//----------------------------------------------------------------------------------
// TAKEN FROM THE ASIO SDK: 
long asioMessages(long selector, long value, void* message, double* opt)
{
        // currently the parameters "value", "message" and "opt" are not used.
        long ret = 0;
        switch(selector)
        {
                case kAsioSelectorSupported:
                        if(value == kAsioResetRequest
                        || value == kAsioEngineVersion
                        || value == kAsioResyncRequest
                        || value == kAsioLatenciesChanged
                        // the following three were added for ASIO 2.0, you don't necessarily have to support them
                        || value == kAsioSupportsTimeInfo
                        || value == kAsioSupportsTimeCode
                        || value == kAsioSupportsInputMonitor)
                                ret = 1L;
                        break;
                        
                case kAsioBufferSizeChange:
                        //printf("kAsioBufferSizeChange \n");
                        break;
                        
                case kAsioResetRequest:
                        // defer the task and perform the reset of the driver during the next "safe" situation
                        // You cannot reset the driver right now, as this code is called from the driver.
                        // Reset the driver is done by completely destruct is. I.e. ASIOStop(), ASIODisposeBuffers(), Destruction
                        // Afterwards you initialize the driver again.
                        asioDriverInfo.stopped;  // In this sample the processing will just stop
                        ret = 1L;
                        break;
                case kAsioResyncRequest:
                        // This informs the application, that the driver encountered some non fatal data loss.
                        // It is used for synchronization purposes of different media.
                        // Added mainly to work around the Win16Mutex problems in Windows 95/98 with the
                        // Windows Multimedia system, which could loose data because the Mutex was hold too long
                        // by another thread.
                        // However a driver can issue it in other situations, too.
                        ret = 1L;
                        break;
                case kAsioLatenciesChanged:
                        // This will inform the host application that the drivers were latencies changed.
                        // Beware, it this does not mean that the buffer sizes have changed!
                        // You might need to update internal delay data.
                        ret = 1L;
                        //printf("kAsioLatenciesChanged \n");
                        break;
                case kAsioEngineVersion:
                        // return the supported ASIO version of the host application
                        // If a host applications does not implement this selector, ASIO 1.0 is assumed
                        // by the driver
                        ret = 2L;
                        break;
                case kAsioSupportsTimeInfo:
                        // informs the driver wether the asioCallbacks.bufferSwitchTimeInfo() callback
                        // is supported.
                        // For compatibility with ASIO 1.0 drivers the host application should always support
                        // the "old" bufferSwitch method, too.
                        ret = 1;
                        break;
                case kAsioSupportsTimeCode:
                        // informs the driver wether application is interested in time code info.
                        // If an application does not need to know about time code, the driver has less work
                        // to do.
                        ret = 0;
                        break;
        }
        return ret;
}


//----------------------------------------------------------------------------------
// conversion from 64 bit ASIOSample/ASIOTimeStamp to double float
#if NATIVE_INT64
        #define ASIO64toDouble(a)  (a)
#else
        const double twoRaisedTo32 = 4294967296.;
        #define ASIO64toDouble(a)  ((a).lo + (a).hi * twoRaisedTo32)
#endif


static ASIOTime *bufferSwitchTimeInfo(ASIOTime *timeInfo, long index, ASIOBool processNow)
{       
        // the actual processing callback.
        // Beware that this is normally in a seperate thread, hence be sure that you take care
        // about thread synchronization. This is omitted here for simplicity.

       // static processedSamples = 0;
        int  result = 0;

        // store the timeInfo for later use
        asioDriverInfo.tInfo = *timeInfo;

        // get the time stamp of the buffer, not necessary if no
        // synchronization to other media is required
        
        if (timeInfo->timeInfo.flags & kSystemTimeValid)
                asioDriverInfo.nanoSeconds = ASIO64toDouble(timeInfo->timeInfo.systemTime);
        else
                asioDriverInfo.nanoSeconds = 0;

        if (timeInfo->timeInfo.flags & kSamplePositionValid)
                asioDriverInfo.samples = ASIO64toDouble(timeInfo->timeInfo.samplePosition);
        else
                asioDriverInfo.samples = 0;

        if (timeInfo->timeCode.flags & kTcValid)
                asioDriverInfo.tcSamples = ASIO64toDouble(timeInfo->timeCode.timeCodeSamples);
        else
                asioDriverInfo.tcSamples = 0;

        // get the system reference time
        asioDriverInfo.sysRefTime = get_sys_reference_time();

#if 0
        // a few debug messages for the Windows device driver developer
        // tells you the time when driver got its interrupt and the delay until the app receives
        // the event notification.
        static double last_samples = 0;
        char tmp[128];
        sprintf (tmp, "diff: %d / %d ms / %d ms / %d samples                 \n", asioDriverInfo.sysRefTime - (long)(asioDriverInfo.nanoSeconds / 1000000.0), asioDriverInfo.sysRefTime, (long)(asioDriverInfo.nanoSeconds / 1000000.0), (long)(asioDriverInfo.samples - last_samples));
        OutputDebugString (tmp);
        last_samples = asioDriverInfo.samples;
#endif

        // To avoid the callback accessing a desallocated stream
        if( asioDriverInfo.past == NULL) return 0L;

        // Keep sample position
        asioDriverInfo.pahsc_NumFramesDone = timeInfo->timeInfo.samplePosition.lo;

        /*  Has a user callback returned '1' to indicate finished at the last ASIO callback? */
        if( asioDriverInfo.past->past_StopSoon ) {
        
                Pa_ASIO_Clear_Output(asioDriverInfo.bufferInfos, 
                        asioDriverInfo.pahsc_channelInfos[0].type,
                        asioDriverInfo.pahsc_NumInputChannels ,
                        asioDriverInfo.pahsc_NumOutputChannels,
                        index, 
                        0, 
                        asioDriverInfo.past_FramesPerHostBuffer);
                
                asioDriverInfo.past->past_IsActive = 0; 
                
                // Finally if the driver supports the ASIOOutputReady() optimization, do it here, all data are in place
                if (asioDriverInfo.pahsc_postOutput) ASIOOutputReady();
       
        }else {
                
                /* CPU usage */
                Pa_StartUsageCalculation(asioDriverInfo.past);
                
                Pa_ASIO_Callback_Input(index);
                         
                // Finally if the driver supports the ASIOOutputReady() optimization, do it here, all data are in place
                if (asioDriverInfo.pahsc_postOutput) ASIOOutputReady();
                
                Pa_ASIO_Callback_End();
                        
                /* CPU usage */
                Pa_EndUsageCalculation(asioDriverInfo.past);
        }
        
        return 0L;
}


//----------------------------------------------------------------------------------
void bufferSwitch(long index, ASIOBool processNow)
{       
        // the actual processing callback.
        // Beware that this is normally in a seperate thread, hence be sure that you take care
        // about thread synchronization. This is omitted here for simplicity.

        // as this is a "back door" into the bufferSwitchTimeInfo a timeInfo needs to be created
        // though it will only set the timeInfo.samplePosition and timeInfo.systemTime fields and the according flags
        
        ASIOTime  timeInfo;
        memset (&timeInfo, 0, sizeof (timeInfo));

        // get the time stamp of the buffer, not necessary if no
        // synchronization to other media is required
        if(ASIOGetSamplePosition(&timeInfo.timeInfo.samplePosition, &timeInfo.timeInfo.systemTime) == ASE_OK)
                timeInfo.timeInfo.flags = kSystemTimeValid | kSamplePositionValid;
                
        // Call the real callback
        bufferSwitchTimeInfo (&timeInfo, index, processNow);
}

//----------------------------------------------------------------------------------




//-------------------------------------------------------------------------------------------------------------------------------------------------------
static void Pa_ASIO_Adaptor_Init()
{
    if (asioDriverInfo.past->past_FramesPerUserBuffer <= asioDriverInfo.past_FramesPerHostBuffer) {
        asioDriverInfo.pahsc_hostOutputBufferFrameOffset = asioDriverInfo.pahsc_OutputBufferOffset;
        asioDriverInfo.pahsc_userInputBufferFrameOffset = 0; // empty 
        asioDriverInfo.pahsc_userOutputBufferFrameOffset = asioDriverInfo.past->past_FramesPerUserBuffer; // empty 
    }else {
        asioDriverInfo.pahsc_hostOutputBufferFrameOffset = 0; // empty 
        asioDriverInfo.pahsc_userInputBufferFrameOffset = asioDriverInfo.pahsc_InputBufferOffset;
        asioDriverInfo.pahsc_userOutputBufferFrameOffset = asioDriverInfo.past->past_FramesPerUserBuffer;	// empty 
    }
}


//-------------------------------------------------------------------------------------------------------------------------------------------------------
// FIXME : optimization for Input only or output only modes (really necessary ??)
static void Pa_ASIO_Callback_Input( long index)
{
        internalPortAudioStream  *past = asioDriverInfo.past;
        long framesInputHostBuffer = asioDriverInfo.past_FramesPerHostBuffer; // number of frames available into the host input buffer
        long framesInputUserBuffer;		// number of frames needed to complete the user input buffer
        long framesOutputHostBuffer;  	// number of frames needed to complete the host output buffer
        long framesOuputUserBuffer;		// number of frames available into the user output buffer
        long userResult;
        long tmp;
        
         /* Fill host ASIO output with remaining frames in user output */
        framesOutputHostBuffer = asioDriverInfo.past_FramesPerHostBuffer;
        framesOuputUserBuffer = asioDriverInfo.past->past_FramesPerUserBuffer - asioDriverInfo.pahsc_userOutputBufferFrameOffset;
        tmp = min(framesOutputHostBuffer, framesOuputUserBuffer);
        framesOutputHostBuffer -= tmp;
        Pa_ASIO_Callback_Output(index,tmp);
        
        /* Available frames in hostInputBuffer */
        while (framesInputHostBuffer  > 0) {
                
                /* Number of frames needed to complete an user input buffer */
                framesInputUserBuffer = asioDriverInfo.past->past_FramesPerUserBuffer - asioDriverInfo.pahsc_userInputBufferFrameOffset;
                                
                if (framesInputHostBuffer >= framesInputUserBuffer) {
                
                        /* Convert ASIO input to user input */
                        Pa_ASIO_Convert_Inter_Input (asioDriverInfo.bufferInfos, 
                                                    past->past_InputBuffer, 
                                                    asioDriverInfo.pahsc_NumInputChannels ,
                                                    asioDriverInfo.pahsc_NumOutputChannels,
                                                    framesInputUserBuffer,
                                                    asioDriverInfo.past_FramesPerHostBuffer - framesInputHostBuffer,
                                                    asioDriverInfo.pahsc_userInputBufferFrameOffset,
                                                    asioDriverInfo.pahsc_channelInfos[0].type,
                                                    past->past_InputSampleFormat,
                                                    past->past_Flags,
                                                    index);
                        
                        /* Call PortAudio callback */
                        userResult = asioDriverInfo.past->past_Callback(past->past_InputBuffer, past->past_OutputBuffer,
                                past->past_FramesPerUserBuffer,past->past_FrameCount,past->past_UserData );
               
                        /* User callback has asked us to stop in the middle of the host buffer  */
                        if( userResult != 0) {
		            
                            /* Put 0 in the end of the output buffer */
                             Pa_ASIO_Clear_Output(asioDriverInfo.bufferInfos, 
                                        asioDriverInfo.pahsc_channelInfos[0].type,
                                        asioDriverInfo.pahsc_NumInputChannels ,
                                        asioDriverInfo.pahsc_NumOutputChannels,
                                        index, 
                                        asioDriverInfo.pahsc_hostOutputBufferFrameOffset, 
                                        asioDriverInfo.past_FramesPerHostBuffer - asioDriverInfo.pahsc_hostOutputBufferFrameOffset);
		                    
                            past->past_StopSoon = 1; 
                            return;
                        }
		                
		                
                        /* Full user ouput buffer : write offset */
                        asioDriverInfo.pahsc_userOutputBufferFrameOffset = 0;
		                
                        /*  Empty user input buffer : read offset */
                        asioDriverInfo.pahsc_userInputBufferFrameOffset = 0;
		                
                        /*  Fill host ASIO output  */
                        tmp = min (past->past_FramesPerUserBuffer,framesOutputHostBuffer);
                        Pa_ASIO_Callback_Output(index,tmp);
                        
                        framesOutputHostBuffer -= tmp;
                        framesInputHostBuffer -= framesInputUserBuffer;
                
                }else {
                
                        /* Convert ASIO input to user input */
                        Pa_ASIO_Convert_Inter_Input (asioDriverInfo.bufferInfos, 
                                                    past->past_InputBuffer, 
                                                    asioDriverInfo.pahsc_NumInputChannels ,
                                                    asioDriverInfo.pahsc_NumOutputChannels,
                                                    framesInputHostBuffer,
                                                    asioDriverInfo.past_FramesPerHostBuffer - framesInputHostBuffer,
                                                    asioDriverInfo.pahsc_userInputBufferFrameOffset,
                                                    asioDriverInfo.pahsc_channelInfos[0].type,
                                                    past->past_InputSampleFormat,
                                                    past->past_Flags,
                                                    index);
                        
                        /* Update pahsc_userInputBufferFrameOffset */
                        asioDriverInfo.pahsc_userInputBufferFrameOffset += framesInputHostBuffer;
                        
                        /* Update framesInputHostBuffer */
                        framesInputHostBuffer = 0; 
                }               
        }

}

//-------------------------------------------------------------------------------------------------------------------------------------------------------
static void Pa_ASIO_Callback_Output(long index, long framePerBuffer)
{
        internalPortAudioStream *past = asioDriverInfo.past;
        
        if (framePerBuffer > 0) {
                
                /* Convert user output to ASIO ouput */
                Pa_ASIO_Convert_Inter_Output (asioDriverInfo.bufferInfos, 
                                            past->past_OutputBuffer,
                                            asioDriverInfo.pahsc_NumInputChannels,
                                            asioDriverInfo.pahsc_NumOutputChannels,
                                            framePerBuffer,
                                            asioDriverInfo.pahsc_hostOutputBufferFrameOffset,
                                            asioDriverInfo.pahsc_userOutputBufferFrameOffset,
                                            asioDriverInfo.pahsc_channelInfos[0].type,
                                            past->past_InputSampleFormat,
                                            past->past_Flags,
                                            index);
                
                /* Update hostOuputFrameOffset */
                asioDriverInfo.pahsc_hostOutputBufferFrameOffset += framePerBuffer;

                /* Update userOutputFrameOffset */
                asioDriverInfo.pahsc_userOutputBufferFrameOffset += framePerBuffer;
        }
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------
static void Pa_ASIO_Callback_End()
 {
     /* Empty ASIO ouput : write offset */
     asioDriverInfo.pahsc_hostOutputBufferFrameOffset = 0;
 }

//-------------------------------------------------------------------------------------------------------------------------------------------------------
static void Pa_ASIO_Clear_User_Buffers()
{
    if( asioDriverInfo.past->past_InputBuffer != NULL )
    {
        memset( asioDriverInfo.past->past_InputBuffer, 0, asioDriverInfo.past->past_InputBufferSize );
    }
    if( asioDriverInfo.past->past_OutputBuffer != NULL )
    {
        memset( asioDriverInfo.past->past_OutputBuffer, 0, asioDriverInfo.past->past_OutputBufferSize );
    }
}

//-------------------------------------------------------------------------------------------------------------------------------------------------------
/*
 static void Pa_ASIO_Clear_Output(ASIOBufferInfo* nativeBuffer,
        ASIOSampleType nativeFormat,
        long NumInputChannels, 
        long NumOuputChannels,
        long index, 
        long hostFrameOffset,
        long frames)
{

        switch (nativeFormat) {
        
                case ASIOSTInt16MSB:
                case ASIOSTInt16LSB:
                case ASIOSTInt32MSB16:
                case ASIOSTInt32LSB16:
                        Pa_ASIO_Clear_Output_16(nativeBuffer, frames,  NumInputChannels, NumOuputChannels, index, hostFrameOffset);
                        break;
                        
                case ASIOSTFloat64MSB:
                case ASIOSTFloat64LSB:
                        break;
                        
                case ASIOSTFloat32MSB:
                case ASIOSTFloat32LSB:
                case ASIOSTInt32MSB:
                case ASIOSTInt32LSB:
                case ASIOSTInt32MSB18:          
                case ASIOSTInt32MSB20:          
                case ASIOSTInt32MSB24:          
                case ASIOSTInt32LSB18:          
                case ASIOSTInt32LSB20:          
                case ASIOSTInt32LSB24:          
                        Pa_ASIO_Clear_Output_32(nativeBuffer, frames,  NumInputChannels, NumOuputChannels, index, hostFrameOffset);
                        break;
                        
                case ASIOSTInt24MSB:
                case ASIOSTInt24LSB:
                        break;
                        
                default:
                        break;
        }
}


//---------------------------------------------------------------------------------------
static void Pa_ASIO_Convert_Inter_Input(
                ASIOBufferInfo* nativeBuffer, 
                void* inputBuffer,
                long NumInputChannels, 
                long NumOuputChannels,
                long framePerBuffer,
                long hostFrameOffset,
                long userFrameOffset,
                ASIOSampleType nativeFormat, 
                PaSampleFormat paFormat, 
                PaStreamFlags flags,
                long index)
{
                
        if((NumInputChannels > 0) && (nativeBuffer != NULL))
        {
                // Convert from native format to PA format.
                switch(paFormat)
                {
                                case paFloat32:
                        {
                                float *inBufPtr = (float *) inputBuffer;
                             
                                switch (nativeFormat) {
                                        case ASIOSTInt16LSB:
                                                Input_Int16_Float32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset, userFrameOffset, swap);
                                                break;  
                                        case ASIOSTInt16MSB:
                                                Input_Int16_Float32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset, userFrameOffset,!swap);
                                                break;  
                                        case ASIOSTInt32LSB:
                                                Input_Int32_Float32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset, userFrameOffset,swap);
                                                break;
                                        case ASIOSTInt32MSB:
                                                Input_Int32_Float32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset, userFrameOffset,!swap);
                                                break;  
                                        case ASIOSTFloat32LSB:          // IEEE 754 32 bit float, as found on Intel x86 architecture
                                                Input_Float32_Float32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset, userFrameOffset,swap);
                                                break;  
                                        case ASIOSTFloat32MSB:          // IEEE 754 32 bit float, as found on Intel x86 architecture
                                                Input_Float32_Float32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset, userFrameOffset,!swap);
                                                break;  
                                        
                                        case ASIOSTInt24LSB:            // used for 20 bits as well
                                        case ASIOSTInt24MSB:            // used for 20 bits as well
                                                
                                        case ASIOSTFloat64LSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture
                                        case ASIOSTFloat64MSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture

                                        // these are used for 32 bit data buffer, with different alignment of the data inside
                                        // 32 bit PCI bus systems can more easily used with these

                                        case ASIOSTInt32LSB16:          // 32 bit data with 16 bit alignment
                                        case ASIOSTInt32LSB18:          // 32 bit data with 18 bit alignment
                                        case ASIOSTInt32LSB20:          // 32 bit data with 20 bit alignment
                                        case ASIOSTInt32LSB24:          // 32 bit data with 24 bit alignment
                                                                                                                                                                
                                
                                        case ASIOSTInt32MSB16:          // 32 bit data with 16 bit alignment
                                        case ASIOSTInt32MSB18:          // 32 bit data with 18 bit alignment
                                        case ASIOSTInt32MSB20:          // 32 bit data with 20 bit alignment
                                        case ASIOSTInt32MSB24:          // 32 bit data with 24 bit alignment
                                                DBUG(("Not yet implemented : please report the problem\n"));
                                                break;
                                }       
                                
                                break;
                        }
                        
                case paInt32:
                        {
                                long *inBufPtr = (long *)inputBuffer;
                                 
                                switch (nativeFormat) {
                                        case ASIOSTInt16LSB:
                                                Input_Int16_Int32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                break;
                                        case ASIOSTInt16MSB:
                                                Input_Int16_Int32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                break;
                                        case ASIOSTInt32LSB:
                                                Input_Int32_Int32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                break;
                                        case ASIOSTInt32MSB:
                                                Input_Int32_Int32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                break;
                                        case ASIOSTFloat32LSB:          // IEEE 754 32 bit float, as found on Intel x86 architecture
                                                Input_Float32_Int32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                break;  
                                        case ASIOSTFloat32MSB:          // IEEE 754 32 bit float, as found on Intel x86 architecture
                                                Input_Float32_Int32(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                break;  
                                        
                                        case ASIOSTInt24LSB:            // used for 20 bits as well
                                        case ASIOSTInt24MSB:            // used for 20 bits as well
                                                
                                        case ASIOSTFloat64LSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture
                                        case ASIOSTFloat64MSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture

                                        // these are used for 32 bit data buffer, with different alignment of the data inside
                                        // 32 bit PCI bus systems can more easily used with these

                                        case ASIOSTInt32LSB16:          // 32 bit data with 16 bit alignment
                                        case ASIOSTInt32LSB18:          // 32 bit data with 18 bit alignment
                                        case ASIOSTInt32LSB20:          // 32 bit data with 20 bit alignment
                                        case ASIOSTInt32LSB24:          // 32 bit data with 24 bit alignment
                                                                                                                                                                
                                
                                        case ASIOSTInt32MSB16:          // 32 bit data with 16 bit alignment
                                        case ASIOSTInt32MSB18:          // 32 bit data with 18 bit alignment
                                        case ASIOSTInt32MSB20:          // 32 bit data with 20 bit alignment
                                        case ASIOSTInt32MSB24:          // 32 bit data with 24 bit alignment
                                                DBUG(("Not yet implemented : please report the problem\n"));
                                                break;
                                        
                                }
                                break;
                        }
                        
                case paInt16:
                        {
                                short *inBufPtr = (short *) inputBuffer;
                                 
                                switch (nativeFormat) {
                                        case ASIOSTInt16LSB:
                                                Input_Int16_Int16(nativeBuffer, inBufPtr, framePerBuffer , NumInputChannels, index , hostFrameOffset,userFrameOffset, swap);
                                                break;
                                        case ASIOSTInt16MSB:
                                                Input_Int16_Int16(nativeBuffer, inBufPtr, framePerBuffer , NumInputChannels, index , hostFrameOffset,userFrameOffset, !swap);
                                                break;
                                        case ASIOSTInt32LSB:
                                                Input_Int32_Int16(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,swap);
                                                break;
                                        case ASIOSTInt32MSB:
                                                Input_Int32_Int16(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                break;
                                        case ASIOSTFloat32LSB:          // IEEE 754 32 bit float, as found on Intel x86 architecture
                                                Input_Float32_Int16(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,swap);
                                                break;  
                                        case ASIOSTFloat32MSB:          // IEEE 754 32 bit float, as found on Intel x86 architecture
                                                Input_Float32_Int16(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                break;  
                
                                        case ASIOSTInt24LSB:            // used for 20 bits as well
                                        case ASIOSTInt24MSB:            // used for 20 bits as well
                                                
                                        case ASIOSTFloat64LSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture
                                        case ASIOSTFloat64MSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture

                                        // these are used for 32 bit data buffer, with different alignment of the data inside
                                        // 32 bit PCI bus systems can more easily used with these

                                        case ASIOSTInt32LSB16:          // 32 bit data with 16 bit alignment
                                        case ASIOSTInt32LSB18:          // 32 bit data with 18 bit alignment
                                        case ASIOSTInt32LSB20:          // 32 bit data with 20 bit alignment
                                        case ASIOSTInt32LSB24:          // 32 bit data with 24 bit alignment
                                                                                                                                                                
                                
                                        case ASIOSTInt32MSB16:          // 32 bit data with 16 bit alignment
                                        case ASIOSTInt32MSB18:          // 32 bit data with 18 bit alignment
                                        case ASIOSTInt32MSB20:          // 32 bit data with 20 bit alignment
                                        case ASIOSTInt32MSB24:          // 32 bit data with 24 bit alignment
                                                DBUG(("Not yet implemented : please report the problem\n"));
                                                break;
                        
                                }
                                break;
                        }

                case paInt8:
                        {
                                // Convert 16 bit data to 8 bit chars
                                
                                char *inBufPtr = (char *) inputBuffer;
                                
                                switch (nativeFormat) {
                                        case ASIOSTInt16LSB:
                                                Input_Int16_Int8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset,flags,swap);
                                                break;  
                                        case ASIOSTInt16MSB:
                                                Input_Int16_Int8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                break;  
                                        case ASIOSTInt32LSB:
                                                Input_Int32_Int8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,swap);
                                                break;
                                        case ASIOSTInt32MSB:
                                                Input_Int32_Int8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                break;
                                        case ASIOSTFloat32LSB:          // IEEE 754 32 bit float, as found on Intel x86 architecture
                                                Input_Float32_Int8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,swap);
                                                break;  
                                        case ASIOSTFloat32MSB:          // IEEE 754 32 bit float, as found on Intel x86 architecture
                                                Input_Float32_Int8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                break;  
                                        
                                        case ASIOSTInt24LSB:            // used for 20 bits as well
                                        case ASIOSTInt24MSB:            // used for 20 bits as well
                                                
                                        case ASIOSTFloat64LSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture
                                        case ASIOSTFloat64MSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture

                                        // these are used for 32 bit data buffer, with different alignment of the data inside
                                        // 32 bit PCI bus systems can more easily used with these

                                        case ASIOSTInt32LSB16:          // 32 bit data with 16 bit alignment
                                        case ASIOSTInt32LSB18:          // 32 bit data with 18 bit alignment
                                        case ASIOSTInt32LSB20:          // 32 bit data with 20 bit alignment
                                        case ASIOSTInt32LSB24:          // 32 bit data with 24 bit alignment
                                                                                                                                                                
                                
                                        case ASIOSTInt32MSB16:          // 32 bit data with 16 bit alignment
                                        case ASIOSTInt32MSB18:          // 32 bit data with 18 bit alignment
                                        case ASIOSTInt32MSB20:          // 32 bit data with 20 bit alignment
                                        case ASIOSTInt32MSB24:          // 32 bit data with 24 bit alignment
                                                DBUG(("Not yet implemented : please report the problem\n"));
                                                break;  
                                }       
                                break;
                        }

                case paUInt8:
                        {
                                // Convert 16 bit data to 8 bit unsigned chars
                                
                                unsigned char *inBufPtr = (unsigned char *)inputBuffer;
                                 
                                switch (nativeFormat) {
                                        case ASIOSTInt16LSB:
                                                Input_Int16_IntU8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,swap);
                                                break;  
                                        case ASIOSTInt16MSB:
                                                Input_Int16_IntU8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                break;  
                                        case ASIOSTInt32LSB:
                                                Input_Int32_IntU8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset,flags,swap);
                                                break;
                                        case ASIOSTInt32MSB:
                                                Input_Int32_IntU8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                break;
                                        case ASIOSTFloat32LSB:          // IEEE 754 32 bit float, as found on Intel x86 architecture
                                                Input_Float32_IntU8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset,flags,swap);
                                                break;  
                                        case ASIOSTFloat32MSB:          // IEEE 754 32 bit float, as found on Intel x86 architecture
                                                Input_Float32_IntU8(nativeBuffer, inBufPtr, framePerBuffer, NumInputChannels, index, hostFrameOffset,userFrameOffset,flags,!swap);
                                                break;  
                                        
                                        case ASIOSTInt24LSB:            // used for 20 bits as well
                                        case ASIOSTInt24MSB:            // used for 20 bits as well
                                                
                                        case ASIOSTFloat64LSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture
                                        case ASIOSTFloat64MSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture

                                        // these are used for 32 bit data buffer, with different alignment of the data inside
                                        // 32 bit PCI bus systems can more easily used with these

                                        case ASIOSTInt32LSB16:          // 32 bit data with 16 bit alignment
                                        case ASIOSTInt32LSB18:          // 32 bit data with 18 bit alignment
                                        case ASIOSTInt32LSB20:          // 32 bit data with 20 bit alignment
                                        case ASIOSTInt32LSB24:          // 32 bit data with 24 bit alignment
                                                                                                                                                                
                                
                                        case ASIOSTInt32MSB16:          // 32 bit data with 16 bit alignment
                                        case ASIOSTInt32MSB18:          // 32 bit data with 18 bit alignment
                                        case ASIOSTInt32MSB20:          // 32 bit data with 20 bit alignment
                                        case ASIOSTInt32MSB24:          // 32 bit data with 24 bit alignment
                                                DBUG(("Not yet implemented : please report the problem\n"));
                                                break;  
                                
                                }       
                                break;
                        }
                        
                default:
                        break;
                }
        }
}


//---------------------------------------------------------------------------------------
static void Pa_ASIO_Convert_Inter_Output(ASIOBufferInfo* nativeBuffer, 
                void* outputBuffer,
                long NumInputChannels, 
                long NumOuputChannels,
                long framePerBuffer,
                long hostFrameOffset,
                long userFrameOffset,
                ASIOSampleType nativeFormat, 
                PaSampleFormat paFormat, 
                PaStreamFlags flags,
                long index)
{
   
        if((NumOuputChannels > 0) && (nativeBuffer != NULL)) 
        {
                // Convert from PA format to native format
                
                switch(paFormat)
                {
                        case paFloat32:
                                {
                                        float *outBufPtr = (float *) outputBuffer;
                                        
                                        switch (nativeFormat) {
                                                case ASIOSTInt16LSB:
                                                        Output_Float32_Int16(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset, userFrameOffset, flags, swap);
                                                        break;  
                                                case ASIOSTInt16MSB:
                                                        Output_Float32_Int16(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset, userFrameOffset, flags,!swap);
                                                        break;  
                                                case ASIOSTInt32LSB:
                                                        Output_Float32_Int32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset, userFrameOffset, flags,swap);
                                                        break;
                                                case ASIOSTInt32MSB:
                                                        Output_Float32_Int32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                        break;  
                                                case ASIOSTFloat32LSB:
                                                        Output_Float32_Float32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset,flags,swap);
                                                        break;
                                                case ASIOSTFloat32MSB:
                                                        Output_Float32_Float32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                        break;  
                                                
                                                case ASIOSTInt24LSB:            // used for 20 bits as well
                                                case ASIOSTInt24MSB:            // used for 20 bits as well
                                                        
                                                case ASIOSTFloat64LSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture
                                                case ASIOSTFloat64MSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture

                                                // these are used for 32 bit data buffer, with different alignment of the data inside
                                                // 32 bit PCI bus systems can more easily used with these

                                                case ASIOSTInt32LSB16:          // 32 bit data with 16 bit alignment
                                                case ASIOSTInt32LSB18:          // 32 bit data with 18 bit alignment
                                                case ASIOSTInt32LSB20:          // 32 bit data with 20 bit alignment
                                                case ASIOSTInt32LSB24:          // 32 bit data with 24 bit alignment
                                                                                                                                                                        
                                        
                                                case ASIOSTInt32MSB16:          // 32 bit data with 16 bit alignment
                                                case ASIOSTInt32MSB18:          // 32 bit data with 18 bit alignment
                                                case ASIOSTInt32MSB20:          // 32 bit data with 20 bit alignment
                                                case ASIOSTInt32MSB24:          // 32 bit data with 24 bit alignment
                                                        DBUG(("Not yet implemented : please report the problem\n"));
                                                        break;
                                        }       
                                        break;
                                }
                                
                        case paInt32:
                                {
                                        long *outBufPtr = (long *) outputBuffer;
                                        
                                        switch (nativeFormat) {
                                                case ASIOSTInt16LSB:
                                                        Output_Int32_Int16(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, flags,swap);
                                                        break;  
                                                case ASIOSTInt16MSB:
                                                        Output_Int32_Int16(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                        break;  
                                                case ASIOSTInt32LSB:
                                                        Output_Int32_Int32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, flags,swap);
                                                        break;
                                                case ASIOSTInt32MSB:
                                                        Output_Int32_Int32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                        break;  
                                                case ASIOSTFloat32LSB:
                                                        Output_Int32_Float32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, flags,swap);
                                                        break;
                                                case ASIOSTFloat32MSB:
                                                        Output_Int32_Float32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, flags,!swap);
                                                        break;  
                                                
                                                case ASIOSTInt24LSB:            // used for 20 bits as well
                                                case ASIOSTInt24MSB:            // used for 20 bits as well
                                                        
                                                case ASIOSTFloat64LSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture
                                                case ASIOSTFloat64MSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture

                                                // these are used for 32 bit data buffer, with different alignment of the data inside
                                                // 32 bit PCI bus systems can more easily used with these

                                                case ASIOSTInt32LSB16:          // 32 bit data with 16 bit alignment
                                                case ASIOSTInt32LSB18:          // 32 bit data with 18 bit alignment
                                                case ASIOSTInt32LSB20:          // 32 bit data with 20 bit alignment
                                                case ASIOSTInt32LSB24:          // 32 bit data with 24 bit alignment
                                                                                                                                                                        
                                        
                                                case ASIOSTInt32MSB16:          // 32 bit data with 16 bit alignment
                                                case ASIOSTInt32MSB18:          // 32 bit data with 18 bit alignment
                                                case ASIOSTInt32MSB20:          // 32 bit data with 20 bit alignment
                                                case ASIOSTInt32MSB24:          // 32 bit data with 24 bit alignment
                                                        DBUG(("Not yet implemented : please report the problem\n"));
                                                        break;
                                        }       
                                        break;
                                }
                                
                        case paInt16:
                                {
                                        short *outBufPtr = (short *) outputBuffer;
                                        
                                        switch (nativeFormat) {
                                                case ASIOSTInt16LSB:
                                                        Output_Int16_Int16(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                        break;  
                                                case ASIOSTInt16MSB:
                                                        Output_Int16_Int16(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                        break;  
                                                case ASIOSTInt32LSB:
                                                        Output_Int16_Int32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                        break;
                                                case ASIOSTInt32MSB:
                                                        Output_Int16_Int32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                        break;  
                                                case ASIOSTFloat32LSB:
                                                        Output_Int16_Float32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                        break;
                                                case ASIOSTFloat32MSB:
                                                        Output_Int16_Float32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                        break;  
                                                
                                                case ASIOSTInt24LSB:            // used for 20 bits as well
                                                case ASIOSTInt24MSB:            // used for 20 bits as well
                                                        
                                                case ASIOSTFloat64LSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture
                                                case ASIOSTFloat64MSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture

                                                // these are used for 32 bit data buffer, with different alignment of the data inside
                                                // 32 bit PCI bus systems can more easily used with these

                                                case ASIOSTInt32LSB16:          // 32 bit data with 16 bit alignment
                                                case ASIOSTInt32LSB18:          // 32 bit data with 18 bit alignment
                                                case ASIOSTInt32LSB20:          // 32 bit data with 20 bit alignment
                                                case ASIOSTInt32LSB24:          // 32 bit data with 24 bit alignment
                                                                                                                                                                        
                                        
                                                case ASIOSTInt32MSB16:          // 32 bit data with 16 bit alignment
                                                case ASIOSTInt32MSB18:          // 32 bit data with 18 bit alignment
                                                case ASIOSTInt32MSB20:          // 32 bit data with 20 bit alignment
                                                case ASIOSTInt32MSB24:          // 32 bit data with 24 bit alignment
                                                        DBUG(("Not yet implemented : please report the problem\n"));
                                                        break;
                                
                                        }       
                                        break;
                                }


                        case paInt8:
                                {
                                        char *outBufPtr = (char *) outputBuffer;
                                        
                                        switch (nativeFormat) {
                                                case ASIOSTInt16LSB:
                                                        Output_Int8_Int16(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                        break;  
                                                case ASIOSTInt16MSB:
                                                        Output_Int8_Int16(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                        break;  
                                                case ASIOSTInt32LSB:
                                                        Output_Int8_Int32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                        break;
                                                case ASIOSTInt32MSB:
                                                        Output_Int8_Int32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                        break;  
                                                case ASIOSTFloat32LSB:
                                                        Output_Int8_Float32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                        break;
                                                case ASIOSTFloat32MSB:
                                                        Output_Int8_Float32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                        break;  
                                                
                                                case ASIOSTInt24LSB:            // used for 20 bits as well
                                                case ASIOSTInt24MSB:            // used for 20 bits as well
                                                        
                                                case ASIOSTFloat64LSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture
                                                case ASIOSTFloat64MSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture

                                                // these are used for 32 bit data buffer, with different alignment of the data inside
                                                // 32 bit PCI bus systems can more easily used with these

                                                case ASIOSTInt32LSB16:          // 32 bit data with 16 bit alignment
                                                case ASIOSTInt32LSB18:          // 32 bit data with 18 bit alignment
                                                case ASIOSTInt32LSB20:          // 32 bit data with 20 bit alignment
                                                case ASIOSTInt32LSB24:          // 32 bit data with 24 bit alignment
                                                                                                                                                                        
                                        
                                                case ASIOSTInt32MSB16:          // 32 bit data with 16 bit alignment
                                                case ASIOSTInt32MSB18:          // 32 bit data with 18 bit alignment
                                                case ASIOSTInt32MSB20:          // 32 bit data with 20 bit alignment
                                                case ASIOSTInt32MSB24:          // 32 bit data with 24 bit alignment
                                                        DBUG(("Not yet implemented : please report the problem\n"));
                                                        break;
                                        }       
                                        break;
                                }

                        case paUInt8:
                                {
                                        unsigned char *outBufPtr = (unsigned char *) outputBuffer;
                                        
                                        switch (nativeFormat) {
                                                case ASIOSTInt16LSB:
                                                        Output_IntU8_Int16(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                        break;  
                                                case ASIOSTInt16MSB:
                                                        Output_IntU8_Int16(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                        break;  
                                                case ASIOSTInt32LSB:
                                                        Output_IntU8_Int32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                        break;
                                                case ASIOSTInt32MSB:
                                                        Output_IntU8_Int32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                        break;  
                                                case ASIOSTFloat32LSB:
                                                        Output_IntU8_Float32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, swap);
                                                        break;
                                                case ASIOSTFloat32MSB:
                                                        Output_IntU8_Float32(nativeBuffer, outBufPtr, framePerBuffer, NumInputChannels, NumOuputChannels, index, hostFrameOffset,userFrameOffset, !swap);
                                                        break;  
                                                
                                                case ASIOSTInt24LSB:            // used for 20 bits as well
                                                case ASIOSTInt24MSB:            // used for 20 bits as well
                                                        
                                                case ASIOSTFloat64LSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture
                                                case ASIOSTFloat64MSB:          // IEEE 754 64 bit double float, as found on Intel x86 architecture

                                                // these are used for 32 bit data buffer, with different alignment of the data inside
                                                // 32 bit PCI bus systems can more easily used with these

                                                case ASIOSTInt32LSB16:          // 32 bit data with 16 bit alignment
                                                case ASIOSTInt32LSB18:          // 32 bit data with 18 bit alignment
                                                case ASIOSTInt32LSB20:          // 32 bit data with 20 bit alignment
                                                case ASIOSTInt32LSB24:          // 32 bit data with 24 bit alignment
                                                                                                                                                                        
                                        
                                                case ASIOSTInt32MSB16:          // 32 bit data with 16 bit alignment
                                                case ASIOSTInt32MSB18:          // 32 bit data with 18 bit alignment
                                                case ASIOSTInt32MSB20:          // 32 bit data with 20 bit alignment
                                                case ASIOSTInt32MSB24:          // 32 bit data with 24 bit alignment
                                                        DBUG(("Not yet implemented : please report the problem\n"));
                                                        break;
                                        }       
                                        break;
                                }

                        default:
                                break;
                        }               
        }

}
*/



/* Load a ASIO driver corresponding to the required device */
static PaError Pa_ASIO_loadDevice (long device)
{
        PaDeviceInfo * dev = &(sDevices[device].pad_Info);

        if (!Pa_ASIO_loadAsioDriver((char *) dev->name)) return paHostError;
        if (ASIOInit(&asioDriverInfo.pahsc_driverInfo) != ASE_OK) return paHostError;
        if (ASIOGetChannels(&asioDriverInfo.pahsc_NumInputChannels, &asioDriverInfo.pahsc_NumOutputChannels) != ASE_OK) return paHostError;
        if (ASIOGetBufferSize(&asioDriverInfo.pahsc_minSize, &asioDriverInfo.pahsc_maxSize, &asioDriverInfo.pahsc_preferredSize, &asioDriverInfo.pahsc_granularity) != ASE_OK) return paHostError;

        if(ASIOOutputReady() == ASE_OK)
                asioDriverInfo.pahsc_postOutput = true;
        else
                asioDriverInfo.pahsc_postOutput = false;

        return paNoError;
}

//---------------------------------------------------
static int GetHighestBitPosition (unsigned long n)
{
        int pos = -1;
        while( n != 0 )
        {
                pos++;
                n = n >> 1;
        }
        return pos;
}

//------------------------------------------------------------------------------------------
static int GetFirstMultiple(long min, long val ){  return ((min + val - 1) / val) * val; }

//------------------------------------------------------------------------------------------
static int GetFirstPossibleDivisor(long max, long val )
{ 
    for (int i = 2; i < 20; i++) {if (((val%i) == 0) && ((val/i) <= max)) return (val/i); }
    return val;
}

//------------------------------------------------------------------------
static int IsPowerOfTwo( unsigned long n ) { return ((n & (n-1)) == 0); }


/*******************************************************************
* Determine size of native ASIO audio buffer size
* Input parameters : FramesPerUserBuffer, NumUserBuffers 
* Output values : FramesPerHostBuffer, OutputBufferOffset or InputtBufferOffset
*/

static PaError PaHost_CalcNumHostBuffers( internalPortAudioStream *past )
{
        PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
        long requestedBufferSize;
        long firstMultiple, firstDivisor;
        
        // Compute requestedBufferSize 
        if( past->past_NumUserBuffers < 1 ){
                requestedBufferSize = past->past_FramesPerUserBuffer;           
        }else{
                requestedBufferSize = past->past_NumUserBuffers * past->past_FramesPerUserBuffer;
        }
        
        // Adjust FramesPerHostBuffer using requestedBufferSize, ASIO minSize and maxSize, 
        if (requestedBufferSize < asioDriverInfo.pahsc_minSize){
        
                firstMultiple = GetFirstMultiple(asioDriverInfo.pahsc_minSize, requestedBufferSize);
        		
                if (firstMultiple <= asioDriverInfo.pahsc_maxSize)
                        asioDriverInfo.past_FramesPerHostBuffer = firstMultiple;
                else
                        asioDriverInfo.past_FramesPerHostBuffer = asioDriverInfo.pahsc_minSize;
        				
        }else if (requestedBufferSize > asioDriverInfo.pahsc_maxSize){
        	
                firstDivisor = GetFirstPossibleDivisor(asioDriverInfo.pahsc_maxSize, requestedBufferSize);
        	   	
                if ((firstDivisor >= asioDriverInfo.pahsc_minSize) && (firstDivisor <= asioDriverInfo.pahsc_maxSize))
                        asioDriverInfo.past_FramesPerHostBuffer = firstDivisor;
                else
                        asioDriverInfo.past_FramesPerHostBuffer = asioDriverInfo.pahsc_maxSize;
        }else{
                asioDriverInfo.past_FramesPerHostBuffer = requestedBufferSize;
        }
        
        // If ASIO buffer size needs to be a power of two
        if( asioDriverInfo.pahsc_granularity < 0 ){
                // Needs to be a power of two.
                
                if( !IsPowerOfTwo( asioDriverInfo.past_FramesPerHostBuffer ) )
                {
                        int highestBit = GetHighestBitPosition(asioDriverInfo.past_FramesPerHostBuffer);
                        asioDriverInfo.past_FramesPerHostBuffer = 1 << (highestBit + 1);
                }
        }
        
        DBUG(("----------------------------------\n"));
        DBUG(("PortAudio : minSize = %ld \n",asioDriverInfo.pahsc_minSize));
        DBUG(("PortAudio : preferredSize = %ld \n",asioDriverInfo.pahsc_preferredSize));
        DBUG(("PortAudio : maxSize = %ld \n",asioDriverInfo.pahsc_maxSize));
        DBUG(("PortAudio : granularity = %ld \n",asioDriverInfo.pahsc_granularity));
        DBUG(("PortAudio : User buffer size = %d\n", asioDriverInfo.past->past_FramesPerUserBuffer ));
        DBUG(("PortAudio : ASIO buffer size = %d\n", asioDriverInfo.past_FramesPerHostBuffer ));
        
        if (asioDriverInfo.past_FramesPerHostBuffer > past->past_FramesPerUserBuffer){
        
                // Computes the MINIMUM value of null frames shift for the output buffer alignement
                asioDriverInfo.pahsc_OutputBufferOffset = Pa_ASIO_CalcFrameShift (asioDriverInfo.past_FramesPerHostBuffer,past->past_FramesPerUserBuffer);
                asioDriverInfo.pahsc_InputBufferOffset = 0;
                DBUG(("PortAudio : Minimum BufferOffset for Output = %d\n", asioDriverInfo.pahsc_OutputBufferOffset));
        }else{
        
                //Computes the MINIMUM value of null frames shift for the input buffer alignement
                asioDriverInfo.pahsc_InputBufferOffset = Pa_ASIO_CalcFrameShift (asioDriverInfo.past_FramesPerHostBuffer,past->past_FramesPerUserBuffer);
                asioDriverInfo.pahsc_OutputBufferOffset = 0;
                DBUG(("PortAudio : Minimum BufferOffset for Input = %d\n", asioDriverInfo.pahsc_InputBufferOffset));
        }
        
        return paNoError;
}


/***********************************************************************/
int Pa_CountDevices()
{
        PaError err ;
        
        if( sNumDevices <= 0 ) 
        {
                /* Force loading of ASIO drivers  */
                err = Pa_ASIO_QueryDeviceInfo(sDevices);
                if( err != paNoError ) goto error;
        }
        
        return sNumDevices;
        
error:
        PaHost_Term();
        DBUG(("Pa_CountDevices: returns %d\n", err ));
        return err;
}

/***********************************************************************/
PaError PaHost_Init( void )
{
       /* Have we already initialized the device info? */
        PaError err = (PaError) Pa_CountDevices();
        return ( err < 0 ) ? err : paNoError;
}

/***********************************************************************/
PaError PaHost_Term( void )
{       
        int           i;
        PaDeviceInfo *dev;
        double       *rates;
        PaError      result = paNoError;
         
        if (sNumDevices > 0) {
	        
            /* Free allocated sample rate arrays  and names*/
            for( i=0; i<sNumDevices; i++ ){
                    dev =  &sDevices[i].pad_Info;
                    rates = (double *) dev->sampleRates;
                    if ((rates != NULL)) PaHost_FreeFastMemory(rates, MAX_NUMSAMPLINGRATES * sizeof(double)); 
                    dev->sampleRates = NULL;
                   if(dev->name != NULL) PaHost_FreeFastMemory((void *) dev->name, 32);
                    dev->name = NULL;
	                
            }
	        
            sNumDevices = 0;
	        
             /* Dispose : if not done by Pa_CloseStream	*/
            if(ASIODisposeBuffers() != ASE_OK) result = paHostError;        
            if(ASIOExit() != ASE_OK) result = paHostError;
	        
            /* remove the loaded ASIO driver */
            asioDrivers->removeCurrentDriver();
        }

        return result;
}

/***********************************************************************/
PaError PaHost_OpenStream( internalPortAudioStream   *past )
{
        PaError             result = paNoError;
        ASIOError                       err;
        int32                           device;
        
        /*  Check if a stream already runs */
        if (asioDriverInfo.past != NULL) return paHostError;
                        
        /* Check the device number */
        if ((past->past_InputDeviceID != paNoDevice)
                &&(past->past_OutputDeviceID != paNoDevice)
                &&(past->past_InputDeviceID != past->past_OutputDeviceID))
        {
                return paInvalidDeviceId;
        }

        /* Allocation */        
        memset(&asioDriverInfo, 0, sizeof(PaHostSoundControl));
        past->past_DeviceData = (void*) &asioDriverInfo;
        

        /* FIXME */
        asioDriverInfo.past = past;
        
        /* load the ASIO device */
        device = (past->past_InputDeviceID < 0) ? past->past_OutputDeviceID : past->past_InputDeviceID;
        result = Pa_ASIO_loadDevice(device);
        if (result != paNoError) goto error;
                
        /* Check ASIO parameters and input parameters */
        if ((past->past_NumInputChannels > asioDriverInfo.pahsc_NumInputChannels) 
                || (past->past_NumOutputChannels > asioDriverInfo.pahsc_NumOutputChannels)) {
                result = paInvalidChannelCount;
                goto error;
        }
        
        /* Set sample rate */
        if (ASIOSetSampleRate(past->past_SampleRate) != ASE_OK) {
                result = paInvalidSampleRate;
                goto error;
        }
        
        /* if OK calc buffer size */
        result = PaHost_CalcNumHostBuffers( past );
        if (result != paNoError) goto error;
        
           
        /* 
        Allocating input and output buffers number for the real past_NumInputChannels and past_NumOutputChannels
        optimize the data transfer.
        */      
        
        asioDriverInfo.pahsc_NumInputChannels = past->past_NumInputChannels;
        asioDriverInfo.pahsc_NumOutputChannels = past->past_NumOutputChannels;
        
        /* Allocate ASIO buffers and callback*/
        err = Pa_ASIO_CreateBuffers(&asioDriverInfo,
                asioDriverInfo.pahsc_NumInputChannels,
                asioDriverInfo.pahsc_NumOutputChannels,
                asioDriverInfo.past_FramesPerHostBuffer);
    
        if (err == ASE_OK)
                return paNoError;
        else if (err == ASE_NoMemory) 
                result = paInsufficientMemory;
        else if (err == ASE_InvalidParameter) 
                result = paInvalidChannelCount;
        else if (err == ASE_InvalidMode) 
                result = paBufferTooBig;
        else 
                result = paHostError;
                
error:
                ASIOExit();
                return result;

}

/***********************************************************************/
PaError PaHost_CloseStream( internalPortAudioStream   *past )
{
        PaHostSoundControl *pahsc;
        PaError             result = paNoError;

        if( past == NULL ) return paBadStreamPtr;
        pahsc = (PaHostSoundControl *) past->past_DeviceData;
        if( pahsc == NULL ) return paNoError;

        #if PA_TRACE_START_STOP
         AddTraceMessage( "PaHost_CloseStream: pahsc_HWaveOut ", (int) pahsc->pahsc_HWaveOut );
        #endif

        /* Dispose */
        if(ASIODisposeBuffers() != ASE_OK) result = paHostError;
        if(ASIOExit() != ASE_OK) result = paHostError;

        /* Free data and device for output. */
        past->past_DeviceData = NULL;
        asioDriverInfo.past = NULL;

        return result;
}

/***********************************************************************/
PaError PaHost_StartOutput( internalPortAudioStream   *past )
{
        /* Clear the index 0 host output buffer */
        Pa_ASIO_Clear_Output(asioDriverInfo.bufferInfos, 
                asioDriverInfo.pahsc_channelInfos[0].type,
                asioDriverInfo.pahsc_NumInputChannels,
                asioDriverInfo.pahsc_NumOutputChannels,
                0, 
                0, 
                asioDriverInfo.past_FramesPerHostBuffer);

        /* Clear the index 1 host output buffer */
        Pa_ASIO_Clear_Output(asioDriverInfo.bufferInfos, 
                asioDriverInfo.pahsc_channelInfos[0].type,
                asioDriverInfo.pahsc_NumInputChannels,
                asioDriverInfo.pahsc_NumOutputChannels,
                1, 
                0, 
                asioDriverInfo.past_FramesPerHostBuffer);
            	
        Pa_ASIO_Clear_User_Buffers();
        
        Pa_ASIO_Adaptor_Init();

        return paNoError;
}

/***********************************************************************/
PaError PaHost_StopOutput( internalPortAudioStream   *past, int abort )
{
        /* Nothing to do ?? */
        return paNoError;
}

/***********************************************************************/
PaError PaHost_StartInput( internalPortAudioStream   *past )
{
        /* Nothing to do ?? */
        return paNoError;
}

/***********************************************************************/
PaError PaHost_StopInput( internalPortAudioStream   *past, int abort )
{
        /* Nothing to do */
        return paNoError;
}

/***********************************************************************/
PaError PaHost_StartEngine( internalPortAudioStream   *past )
{
        // TO DO : count of samples
        past->past_IsActive = 1;
        return (ASIOStart() == ASE_OK) ? paNoError : paHostError;
}

/***********************************************************************/
PaError PaHost_StopEngine( internalPortAudioStream *past, int abort )
{
        // TO DO :  count of samples
        past->past_IsActive = 0;
        return (ASIOStop() == ASE_OK) ? paNoError : paHostError;
}

/***********************************************************************/
// TO BE CHECKED 
PaError PaHost_StreamActive( internalPortAudioStream   *past )
{
        PaHostSoundControl *pahsc;
        if( past == NULL ) return paBadStreamPtr;
        pahsc = (PaHostSoundControl *) past->past_DeviceData;
        if( pahsc == NULL ) return paInternalError;
        return (PaError) past->past_IsActive;
}

/*************************************************************************/
PaTimestamp Pa_StreamTime( PortAudioStream *stream )
{
        PaHostSoundControl *pahsc;
        internalPortAudioStream   *past = (internalPortAudioStream *) stream;
        if( past == NULL ) return paBadStreamPtr;
        pahsc = (PaHostSoundControl *) past->past_DeviceData;
        return pahsc->pahsc_NumFramesDone;
}

#endif /* OLD CODE IFDEFED OUT */





