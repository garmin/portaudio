/*
 * $Id$
 * pa_mac_core.c
 * Implementation of PortAudio for Mac OS X Core Audio
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
 * CHANGE HISTORY:
 
 3.29.2001 - Phil Burk - First pass... converted from Window MME code with help from Darren.
 3.30.2001 - Darren Gibbs - Added more support for dynamically querying device info.
 12.7.2001 - Gord Peters - Tweaks to compile on PA V17 and OS X 10.1
 2.7.2002 - Darren and Phil - fixed isInput so GetProperty works better, 
             fixed device queries for numChannels and sampleRates,
            one CoreAudio device now maps to separate input and output PaDevices,
            audio input works if using same CoreAudio device (some HW devices make separate CoreAudio devices).
 2.22.2002 - Stephane Letz - Explicit cast needed for compilation with Code Warrior 7
 3.19.2002 - Phil Burk - Added paInt16, paInt8, format using new "pa_common/pa_convert.c" file.
            Return error if opened in mono mode cuz not supported. [Supported 10.12.2002]
            Add support for Pa_GetCPULoad();
            Fixed timestamp in callback and Pa_StreamTime() (Thanks n++k for the advice!)
            Check for invalid sample rates and return an error.
            Check for getenv("PA_MIN_LATENCY_MSEC") to set latency externally.
            Better error checking for invalid channel counts and invalid devices.
 3.29.2002 - Phil Burk - Fixed Pa_GetCPULoad() for small buffers.
 3.31.2002 - Phil Burk - Use getrusage() instead of gettimeofday() for CPU Load calculation.
 10.12.2002 - Phil Burk - Use AudioConverter to allow wide range of sample rates, and mono.
              Use FIFO (from pablio/rinbuffer.h) so that we can pull data through converter.

TODO:
O- FIFO between input and output callbacks if different devices, like in pa_mac.c
*/

#include <CoreServices/CoreServices.h>
#include <CoreAudio/CoreAudio.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/DefaultAudioOutput.h>
#include <AudioToolbox/AudioConverter.h>


#include "portaudio.h"
#include "pa_host.h"
#include "pa_trace.h"
#include "ringbuffer.h"

/************************************************* Constants ********/

/* To trace program, enable TRACE_REALTIME_EVENTS in pa_trace.h */
#define PA_TRACE_RUN             (0)
#define PA_TRACE_START_STOP      (1)

#define PA_MIN_LATENCY_MSEC      (1)
#define MIN_TIMEOUT_MSEC         (1000)

#define PRINT(x) { printf x; fflush(stdout); }
#define ERR_RPT(x) PRINT(x)
#define DBUG(x)    /* PRINT(x) */
#define DBUGX(x)   /* PRINT(x) */

// define value of isInput passed to CoreAudio routines
#define IS_INPUT    (true)
#define IS_OUTPUT   (false)

/**************************************************************
 * Structure for internal host specific stream data.
 * This is allocated on a per stream basis.
 */
typedef struct PaHostSoundControl
{
    AudioDeviceID      pahsc_AudioDeviceID;  // Must be the same for input and output for now.
    /* Input -------------- */
    int                pahsc_BytesPerUserNativeInputBuffer; /* native buffer size in bytes per user chunk */
    /* Output -------------- */
    int                pahsc_BytesPerUserNativeOutputBuffer; /* native buffer size in bytes per user chunk */
    /* Init Time -------------- */
    int                pahsc_FramesPerHostBuffer;
    int                pahsc_UserBuffersPerHostBuffer;
    /* For sample rate and format conversion. */
    RingBuffer         pahsc_FIFO;
    char              *pahsc_FIFOdata;
    AudioConverterRef  pahsc_InputConverter;
    void              *pahsc_InputConverterBuffer;
    AudioConverterRef  pahsc_OutputConverter;
    void              *pahsc_OutputConverterBuffer;
    /* For measuring CPU utilization. */
    struct rusage      pahsc_EntryRusage;
    double             pahsc_InverseMicrosPerHostBuffer; /* 1/Microseconds of real-time audio per user buffer. */
}
PaHostSoundControl;

/**************************************************************
 * Structure for internal extended device info.
 * There will be one or two PortAudio devices for each Core Audio device:
 *   one input and or one output.
 */
typedef struct PaHostDeviceInfo
{
    PaDeviceInfo      paInfo;
    AudioDeviceID     audioDeviceID;
}
PaHostDeviceInfo;

/************************************************* Shared Data ********/
/* FIXME - put Mutex around this shared data. */
static int sNumPaDevices = 0;   /* Total number of PaDeviceInfos */
static int sNumInputDevices = 0; /* Total number of input PaDeviceInfos */
static int sNumOutputDevices = 0;
static PaHostDeviceInfo *sDeviceInfos = NULL;
static int sDefaultInputDeviceID = paNoDevice;
static int sDefaultOutputDeviceID = paNoDevice;
static int sPaHostError = 0;

static int sNumCoreDevices = 0;
static AudioDeviceID *sCoreDeviceIDs;   // Array of Core AudioDeviceIDs

static const char sMapperSuffixInput[] = " - Input";
static const char sMapperSuffixOutput[] = " - Output";

/* We index the input devices first, then the output devices. */
#define LOWEST_INPUT_DEVID     (0)
#define HIGHEST_INPUT_DEVID    (sNumInputDevices - 1)
#define LOWEST_OUTPUT_DEVID    (sNumInputDevices)
#define HIGHEST_OUTPUT_DEVID   (sNumPaDevices - 1)

/************************************************* Macros ********/

/************************************************* Prototypes **********/

static PaError Pa_QueryDevices( void );
PaError PaHost_GetTotalBufferFrames( internalPortAudioStream   *past );

static int PaHost_ScanDevices( Boolean isInput );
static int PaHost_QueryDeviceInfo( PaHostDeviceInfo *hostDeviceInfo, int coreDeviceIndex, Boolean isInput );

static PaDeviceID Pa_QueryDefaultInputDevice( void );
static PaDeviceID Pa_QueryDefaultOutputDevice( void );
static void PaHost_CalcHostBufferSize( internalPortAudioStream *past );

/************************************************************/
static unsigned long RoundUpToNextPowerOf2( unsigned long n )
{
    long numBits = 0;
    if( ((n-1) & n) == 0) return n; /* Already Power of two. */
    while( n > 0 )
    {
        n= n>>1;
        numBits++;
    }
    return (1<<numBits);
}

/************* DEBUG TOOLS *********************************************/
static int dumpBuffer = 0;

static void DisplayBuffer( void *bufferPtr, int numSamples )
{
    float *ptr = (float *)bufferPtr;
    int i;
    for( i=0; i<numSamples; i++ ) PRINT(("buf[%d] = %f\n", i, *ptr++ ));
}


/********************************* BEGIN CPU UTILIZATION MEASUREMENT ****/
static void Pa_StartUsageCalculation( internalPortAudioStream   *past )
{
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return;
    /* Query user CPU timer for usage analysis and to prevent overuse of CPU. */
    getrusage( RUSAGE_SELF, &pahsc->pahsc_EntryRusage );
}

static long SubtractTime_AminusB( struct timeval *timeA, struct timeval *timeB )
{
    long secs = timeA->tv_sec - timeB->tv_sec;
    long usecs = secs * 1000000;
    usecs += (timeA->tv_usec - timeB->tv_usec);
    return usecs;
}

/******************************************************************************
** Measure fractional CPU load based on real-time it took to calculate
** buffers worth of output.
*/
static void Pa_EndUsageCalculation( internalPortAudioStream   *past )
{
    struct rusage currentRusage;
    long  usecsElapsed;
    double newUsage;

#define LOWPASS_COEFFICIENT_0   (0.95)
#define LOWPASS_COEFFICIENT_1   (0.99999 - LOWPASS_COEFFICIENT_0)

    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return;
    
    if( getrusage( RUSAGE_SELF, &currentRusage ) == 0 )
    {
        usecsElapsed = SubtractTime_AminusB( &currentRusage.ru_utime, &pahsc->pahsc_EntryRusage.ru_utime );
        
        /* Use inverse because it is faster than the divide. */
        newUsage =  usecsElapsed * pahsc->pahsc_InverseMicrosPerHostBuffer;

        past->past_Usage = (LOWPASS_COEFFICIENT_0 * past->past_Usage) +
                           (LOWPASS_COEFFICIENT_1 * newUsage);
    }
}
/****************************************** END CPU UTILIZATION *******/

/************************************************************************/
static PaDeviceID Pa_QueryDefaultInputDevice( void )
{
    OSStatus err = noErr;
    UInt32  count;
    int          i;
    AudioDeviceID tempDeviceID = kAudioDeviceUnknown;
    PaDeviceID  defaultDeviceID = paNoDevice;

    // get the default output device for the HAL
    // it is required to pass the size of the data to be returned
    count = sizeof(tempDeviceID);
    err = AudioHardwareGetProperty( kAudioHardwarePropertyDefaultInputDevice,  &count, (void *) &tempDeviceID);
    if (err != noErr) goto Bail;
    
    // scan input devices to see which one matches this device
    defaultDeviceID = paNoDevice;
    for( i=LOWEST_INPUT_DEVID; i<=HIGHEST_INPUT_DEVID; i++ )
    {
        DBUG(("Pa_QueryDefaultInputDevice: i = %d, aDevId = %d\n", i, sDeviceInfos[i].audioDeviceID ));
        if( sDeviceInfos[i].audioDeviceID == tempDeviceID )
        {
            defaultDeviceID = i;
            break;
        }
    }
Bail:
    return defaultDeviceID;
}

/************************************************************************/
static PaDeviceID Pa_QueryDefaultOutputDevice( void )
{
    OSStatus err = noErr;
    UInt32  count;
    int          i;
    AudioDeviceID tempDeviceID = kAudioDeviceUnknown;
    PaDeviceID  defaultDeviceID = paNoDevice;

    // get the default output device for the HAL
    // it is required to pass the size of the data to be returned
    count = sizeof(tempDeviceID);
    err = AudioHardwareGetProperty( kAudioHardwarePropertyDefaultOutputDevice,  &count, (void *) &tempDeviceID);
    if (err != noErr) goto Bail;
    
    // scan output devices to see which one matches this device
    defaultDeviceID = paNoDevice;
    for( i=LOWEST_OUTPUT_DEVID; i<=HIGHEST_OUTPUT_DEVID; i++ )
    {
        DBUG(("Pa_QueryDefaultOutputDevice: i = %d, aDevId = %d\n", i, sDeviceInfos[i].audioDeviceID ));
        if( sDeviceInfos[i].audioDeviceID == tempDeviceID )
        {
            defaultDeviceID = i;
            break;
        }
    }
Bail:
    return defaultDeviceID;
}

/******************************************************************/
static PaError Pa_QueryDevices( void )
{
    OSStatus err = noErr;
    UInt32   outSize;
    Boolean  outWritable;
    int         numBytes;

    // find out how many Core Audio devices there are, if any
    err = AudioHardwareGetPropertyInfo(kAudioHardwarePropertyDevices, &outSize, &outWritable);
    if (err != noErr)
        ERR_RPT(("Couldn't get info about list of audio devices\n"));

    // calculate the number of device available
    sNumCoreDevices = outSize / sizeof(AudioDeviceID);

    // Bail if there aren't any devices
    if (sNumCoreDevices < 1)
        ERR_RPT(("No Devices Available\n"));

    // make space for the devices we are about to get
    sCoreDeviceIDs =  (AudioDeviceID *)malloc(outSize);

    // get an array of AudioDeviceIDs
    err = AudioHardwareGetProperty(kAudioHardwarePropertyDevices, &outSize, (void *)sCoreDeviceIDs);
    if (err != noErr)
        ERR_RPT(("Couldn't get list of audio device IDs\n"));

    // Allocate structures to hold device info pointers.
    // There will be a maximum of two Pa devices per Core Audio device, input and/or output.
    numBytes = sNumCoreDevices * 2 * sizeof(PaHostDeviceInfo);
    sDeviceInfos = (PaHostDeviceInfo *) PaHost_AllocateFastMemory( numBytes );
    if( sDeviceInfos == NULL ) return paInsufficientMemory;

    // Scan all the Core Audio devices to see which support input and allocate a
    // PaHostDeviceInfo structure for each one.
    PaHost_ScanDevices( IS_INPUT );
    sNumInputDevices = sNumPaDevices;
    // Now scan all the output devices.
    PaHost_ScanDevices( IS_OUTPUT );
    sNumOutputDevices = sNumPaDevices - sNumInputDevices;

    // Figure out which of the devices that we scanned is the default device.
    sDefaultInputDeviceID = Pa_QueryDefaultInputDevice();
    sDefaultOutputDeviceID = Pa_QueryDefaultOutputDevice();

    return paNoError;
}

/************************************************************************************/
long Pa_GetHostError()
{
    return sPaHostError;
}

/*************************************************************************/
int Pa_CountDevices()
{
    if( sNumPaDevices <= 0 ) Pa_Initialize();
    return sNumPaDevices;
}

/*************************************************************************/
/* Allocate a string containing the device name. */
static char *PaHost_DeviceNameFromID(AudioDeviceID deviceID, Boolean isInput )
{
    OSStatus err = noErr;
    UInt32  outSize;
    Boolean  outWritable;
    char     *deviceName = nil;
    
    // query size of name
    err =  AudioDeviceGetPropertyInfo(deviceID, 0, isInput, kAudioDevicePropertyDeviceName, &outSize, &outWritable);
    if (err == noErr)
    {
        deviceName =  (char*)malloc( outSize + 1);
        if( deviceName )
        {
            err = AudioDeviceGetProperty(deviceID, 0, isInput, kAudioDevicePropertyDeviceName, &outSize, deviceName);
            if (err != noErr)
                ERR_RPT(("Couldn't get audio device name.\n"));
        }
    }

    return deviceName;
}

/*************************************************************************/
// An AudioStreamBasicDescription is passed in to query whether or not
// the format is supported. A kAudioDeviceUnsupportedFormatError will
// be returned if the format is not supported and kAudioHardwareNoError
// will be returned if it is supported. AudioStreamBasicDescription
// fields set to 0 will be ignored in the query, but otherwise values
// must match exactly.

Boolean deviceDoesSupportFormat(AudioDeviceID deviceID, AudioStreamBasicDescription *desc, Boolean isInput )
{
    OSStatus err = noErr;
    UInt32  outSize;

    outSize = sizeof(*desc);
    err = AudioDeviceGetProperty(deviceID, 0, isInput, kAudioDevicePropertyStreamFormatSupported, &outSize, desc);

    if (err == kAudioHardwareNoError)
        return true;
    else
        return false;
}

/*************************************************************************/
// return an error string
char* coreAudioErrorString (int errCode )
{
    char *str;

    switch (errCode)
    {
    case kAudioHardwareUnspecifiedError:
        str = "kAudioHardwareUnspecifiedError";
        break;
    case kAudioHardwareNotRunningError:
        str = "kAudioHardwareNotRunningError";
        break;
    case kAudioHardwareUnknownPropertyError:
        str = "kAudioHardwareUnknownPropertyError";
        break;
    case kAudioDeviceUnsupportedFormatError:
        str = "kAudioDeviceUnsupportedFormatError";
        break;
    case kAudioHardwareBadPropertySizeError:
        str = "kAudioHardwareBadPropertySizeError";
        break;
    case kAudioHardwareIllegalOperationError:
        str = "kAudioHardwareIllegalOperationError";
        break;
    default:
        str = "Unknown CoreAudio Error!";
        break;
    }

    return str;
}

/*************************************************************************
** PaDeviceInfo structures have already been created
** so just return the pointer.
**
*/
const PaDeviceInfo* Pa_GetDeviceInfo( PaDeviceID id )
{
    if( id < 0 || id >= sNumPaDevices )
        return NULL;

    return &sDeviceInfos[id].paInfo;
}

/*************************************************************************
** Scan all of the Core Audio devices to see which support input or output.
** Changes sNumDevices, and fills in sDeviceInfos.
*/
static int PaHost_ScanDevices( Boolean isInput )
{
    int coreDeviceIndex;
    int result;
    PaHostDeviceInfo  *hostDeviceInfo;
    int numAdded = 0;

    for(  coreDeviceIndex=0; coreDeviceIndex<sNumCoreDevices; coreDeviceIndex++ )
    {
        // try to fill in next PaHostDeviceInfo
        hostDeviceInfo = &sDeviceInfos[sNumPaDevices];
        result = PaHost_QueryDeviceInfo( hostDeviceInfo, coreDeviceIndex, isInput );
        DBUG(("PaHost_ScanDevices: paDevId = %d, coreDevId = %d\n", sNumPaDevices, hostDeviceInfo->audioDeviceID ));
        if( result > 0 )
        {
            sNumPaDevices += 1;  // bump global counter if we got one
            numAdded += 1;
        }
        else if( result < 0 ) return result;
    }
    return numAdded;
}

static double supportedSampleRateRange[] = { 8000.0, 96000.0 };

/*************************************************************************
** Try to fill in the device info for this device.
** Return 1 if a good device that PA can use.
** Return 0 if not appropriate
** or return negative error.
**
*/
static int PaHost_QueryDeviceInfo( PaHostDeviceInfo *hostDeviceInfo, int coreDeviceIndex, Boolean isInput )
{
    OSErr   err;
    UInt32  outSize;
    AudioStreamBasicDescription formatDesc;
    AudioDeviceID    devID;
    double *sampleRates = NULL; /* non-const ptr */

    PaDeviceInfo *deviceInfo = &hostDeviceInfo->paInfo;

    deviceInfo->structVersion = 1;
    deviceInfo->maxInputChannels = 0;
    deviceInfo->maxOutputChannels = 0;

    deviceInfo->sampleRates = supportedSampleRateRange; // because we use sample rate converter to get continuous rates
    deviceInfo->numSampleRates = -1;

    devID = sCoreDeviceIDs[ coreDeviceIndex ];
    hostDeviceInfo->audioDeviceID = devID;

    // Get data format info from the device.
    outSize = sizeof(formatDesc);
    err = AudioDeviceGetProperty(devID, 0, isInput, kAudioDevicePropertyStreamFormat, &outSize, &formatDesc);

    // If no channels supported, then not a very good device.
    if( (err != noErr) || (formatDesc.mChannelsPerFrame == 0) ) goto error;

    if( isInput )
    {
        deviceInfo->maxInputChannels = formatDesc.mChannelsPerFrame;
    }
    else
    {
        deviceInfo->maxOutputChannels = formatDesc.mChannelsPerFrame;
    }

    // Right now the Core Audio headers only define one formatID: LinearPCM
    // Apparently LinearPCM must be Float32 for now.
    switch (formatDesc.mFormatID)
    {
    case kAudioFormatLinearPCM:
        deviceInfo->nativeSampleFormats = paFloat32;

        // FIXME - details about the format are in these flags.
        // formatDesc.mFormatFlags

        // here are the possibilities
        // kLinearPCMFormatFlagIsFloat   // set for floating point, clear for integer
        // kLinearPCMFormatFlagIsBigEndian  // set for big endian, clear for little
        // kLinearPCMFormatFlagIsSignedInteger // set for signed integer, clear for unsigned integer,
        //    only valid if kLinearPCMFormatFlagIsFloat is clear
        // kLinearPCMFormatFlagIsPacked   // set if the sample bits are packed as closely together as possible,
        //    clear if they are high or low aligned within the channel
        // kLinearPCMFormatFlagIsAlignedHigh  // set if the sample bits are placed
        break;

    default:
        deviceInfo->nativeSampleFormats = paFloat32;  // FIXME
        break;
    }

    // Get the device name
    deviceInfo->name = PaHost_DeviceNameFromID( devID, isInput );
    return 1;

error:
    if( sampleRates != NULL ) free( sampleRates );
    return 0;
}

/*************************************************************************
** Returns recommended device ID.
** On the PC, the recommended device can be specified by the user by
** setting an environment variable. For example, to use device #1.
**
**    set PA_RECOMMENDED_OUTPUT_DEVICE=1
**
** The user should first determine the available device ID by using
** the supplied application "pa_devs".
*/
#define PA_ENV_BUF_SIZE  (32)
#define PA_REC_IN_DEV_ENV_NAME  ("PA_RECOMMENDED_INPUT_DEVICE")
#define PA_REC_OUT_DEV_ENV_NAME  ("PA_RECOMMENDED_OUTPUT_DEVICE")

static PaDeviceID PaHost_GetEnvDefaultDeviceID( char *envName )
{
#if 0
    UInt32   hresult;
    char    envbuf[PA_ENV_BUF_SIZE];
    PaDeviceID recommendedID = paNoDevice;

    /* Let user determine default device by setting environment variable. */
    hresult = GetEnvironmentVariable( envName, envbuf, PA_ENV_BUF_SIZE );
    if( (hresult > 0) && (hresult < PA_ENV_BUF_SIZE) )
    {
        recommendedID = atoi( envbuf );
    }

    return recommendedID;
#endif
    return paNoDevice;
}

static PaError Pa_MaybeQueryDevices( void )
{
    if( sNumPaDevices == 0 )
    {
        return Pa_QueryDevices();
    }
    return 0;
}

/**********************************************************************
** Check for environment variable, else query devices and use result.
*/
PaDeviceID Pa_GetDefaultInputDeviceID( void )
{
    PaError result;
    result = PaHost_GetEnvDefaultDeviceID( PA_REC_IN_DEV_ENV_NAME );
    if( result < 0 )
    {
        result = Pa_MaybeQueryDevices();
        if( result < 0 ) return result;
        result = sDefaultInputDeviceID;
    }
    return result;
}

PaDeviceID Pa_GetDefaultOutputDeviceID( void )
{
    PaError result;
    result = PaHost_GetEnvDefaultDeviceID( PA_REC_OUT_DEV_ENV_NAME );
    if( result < 0 )
    {
        result = Pa_MaybeQueryDevices();
        if( result < 0 ) return result;
        result = sDefaultOutputDeviceID;
    }
    return result;
}

/**********************************************************************
** Initialize Host dependant part of API.
*/
PaError PaHost_Init( void )
{
    return Pa_MaybeQueryDevices();
}


// This is the proc that supplies the data to the AudioConverterFillBuffer call
static OSStatus PaHost_InputConverterCallbackProc (AudioConverterRef			inAudioConverter,
								UInt32*						outDataSize,
								void**						outData,
								void*						inUserData)
{
    internalPortAudioStream   *past = (internalPortAudioStream *) inUserData;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    void *dataPtr1;
    long size1;
    void *dataPtr2;
    long size2;
           
    /* Pass contiguous region from FIFO directly to converter. */
    RingBuffer_GetReadRegions( &pahsc->pahsc_FIFO, *outDataSize,
            &dataPtr1, &size1, &dataPtr2, &size2 );

    if( size1 > 0 )
    {
        *outData = dataPtr1;
        *outDataSize = size1;
        RingBuffer_AdvanceReadIndex( &pahsc->pahsc_FIFO, size1 );
    }
    else
    {
        DBUG(("PaHost_InputConverterCallbackProc: got no data!\n"));
        *outData = pahsc->pahsc_FIFOdata; // FIXME - just give it bogus data to keep it happy.
        // *outDataSize = 0;
    }
	return noErr; // FIXME
}

// This is the proc that supplies the data to the AudioConverterFillBuffer call
static OSStatus PaHost_OutputConverterCallbackProc (AudioConverterRef			inAudioConverter,
								UInt32*						outDataSize,
								void**						outData,
								void*						inUserData)
{
    internalPortAudioStream   *past = (internalPortAudioStream *) inUserData;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    
    /* Do we need data from the converted input? */
    if( pahsc->pahsc_InputConverterBuffer != NULL )
    {
        UInt32 size = pahsc->pahsc_BytesPerUserNativeInputBuffer;
        verify_noerr (AudioConverterFillBuffer(
            pahsc->pahsc_InputConverter,
            PaHost_InputConverterCallbackProc,
            past,
            &size,
            pahsc->pahsc_InputConverterBuffer));

    }
    
    /* Fill part of audio converter buffer by converting input to user format,
    * calling user callback, then converting output to native format. */
    if( PaConvert_Process( past, pahsc->pahsc_InputConverterBuffer, pahsc->pahsc_OutputConverterBuffer ))
    {
        past->past_StopSoon = 1;
    }
    
	*outData = pahsc->pahsc_OutputConverterBuffer;
	*outDataSize = pahsc->pahsc_BytesPerUserNativeOutputBuffer;
	return noErr; // FIXME
}

/**********************************************************************
** Fill any available output buffers and use any available
** input buffers by calling user callback.
** Will set past->past_StopSoon if user callback indicates that it is finished.
*/
static PaError Pa_TimeSlice( internalPortAudioStream   *past, const AudioBufferList*  inInputData,
                             AudioBufferList*  outOutputData )
{
    PaError           result = 0;
    char             *inputNativeBufferfPtr = NULL;
    char             *outputNativeBufferfPtr = NULL;
    int               i;
    int               buffersProcessed = 0;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paInternalError;

    past->past_NumCallbacks += 1;

#if PA_TRACE_RUN
    AddTraceMessage("Pa_TimeSlice: past_NumCallbacks ", past->past_NumCallbacks );
#endif

    Pa_StartUsageCalculation( past );

    /* If we are using output, then we need an empty output buffer. */
    if( past->past_NumOutputChannels > 0 )
    {
        outputNativeBufferfPtr =  (char*)outOutputData->mBuffers[0].mData;
    }

    /* If we are using input, then we need a full input buffer. */
    if(  past->past_NumInputChannels > 0  )
    {
        inputNativeBufferfPtr = (char*)inInputData->mBuffers[0].mData;
    }

    buffersProcessed += 1;
    
    /* If there is a FIFO for input then write to it. */
    if( pahsc->pahsc_FIFOdata != NULL )
    {
        long writeRoom = RingBuffer_GetWriteAvailable( &pahsc->pahsc_FIFO );
        long numBytes = pahsc->pahsc_FramesPerHostBuffer * past->past_NumInputChannels * sizeof(float);
        if( numBytes <= writeRoom )
        {
            RingBuffer_Write(  &pahsc->pahsc_FIFO, inputNativeBufferfPtr, numBytes );
        } // FIXME else ???            
    }

    if( pahsc->pahsc_OutputConverter != NULL )
    {
        UInt32 size = pahsc->pahsc_FramesPerHostBuffer * past->past_NumOutputChannels * sizeof(float);
        verify_noerr (AudioConverterFillBuffer(
            pahsc->pahsc_OutputConverter,
            PaHost_OutputConverterCallbackProc,
            past,
            &size,
            outputNativeBufferfPtr));
    }
    else if( pahsc->pahsc_InputConverter != NULL )
    {
        /* Generate user buffers as long as we have a half full input FIFO. */
        long gotHalf = pahsc->pahsc_FIFO.bufferSize / 2;
        while( (RingBuffer_GetReadAvailable( &pahsc->pahsc_FIFO ) >= gotHalf) &&
            (past->past_StopSoon == 0) )
        {
            UInt32 size = pahsc->pahsc_BytesPerUserNativeInputBuffer;
            verify_noerr (AudioConverterFillBuffer(
                pahsc->pahsc_InputConverter,
                PaHost_InputConverterCallbackProc,
                past,
                &size,
                pahsc->pahsc_InputConverterBuffer));
            /* Convert 32 bit native data to user data and call user routine. */
            if( PaConvert_Process( past, pahsc->pahsc_InputConverterBuffer, NULL ) != 0 )
            {
                past->past_StopSoon = 1;
            }
        }

    }
    else
    {
        /* Each host buffer contains multiple user buffers so do them all now. */
        for( i=0; i<pahsc->pahsc_UserBuffersPerHostBuffer; i++ )
        {
            if( past->past_StopSoon )
            {
                if( outputNativeBufferfPtr )
                {
                    /* Clear remainder of audio buffer if we are waiting for stop. */
                    AddTraceMessage("Pa_TimeSlice: zero rest of wave buffer ", i );
                    memset( outputNativeBufferfPtr, 0, pahsc->pahsc_BytesPerUserNativeOutputBuffer );
                }
            }
            else
            {
                    /* Convert 32 bit native data to user data and call user routine. */
                    if( PaConvert_Process( past, inputNativeBufferfPtr, outputNativeBufferfPtr ) != 0 )
                    {
                        past->past_StopSoon = 1;
                    }
            }
            if( inputNativeBufferfPtr ) inputNativeBufferfPtr += pahsc->pahsc_BytesPerUserNativeInputBuffer;
            if( outputNativeBufferfPtr) outputNativeBufferfPtr += pahsc->pahsc_BytesPerUserNativeOutputBuffer;
        }
    }
    
 
    Pa_EndUsageCalculation( past );

#if PA_TRACE_RUN
    AddTraceMessage("Pa_TimeSlice: buffersProcessed ", buffersProcessed );
#endif

    return result;
}

OSStatus appIOProc (AudioDeviceID  inDevice, const AudioTimeStamp*  inNow,
                    const AudioBufferList*  inInputData, const AudioTimeStamp*  inInputTime,
                    AudioBufferList*  outOutputData, const AudioTimeStamp* inOutputTime,
                    void* contextPtr)
{

    PaError      result = 0;
    internalPortAudioStream *past;
    PaHostSoundControl *pahsc;
    past = (internalPortAudioStream *) contextPtr;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;

// printf("Num input Buffers: %d; Num output Buffers: %d.\n", inInputData->mNumberBuffers,	outOutputData->mNumberBuffers);

    /* Has someone asked us to abort by calling Pa_AbortStream()? */
    if( past->past_StopNow )
    {
        past->past_IsActive = 0; /* Will cause thread to return. */
    }
    /* Has someone asked us to stop by calling Pa_StopStream()
     * OR has a user callback returned '1' to indicate finished.
     */
    else if( past->past_StopSoon )
    {
        // FIXME - pretend all done
        past->past_IsActive = 0; /* Will cause thread to return. */
    }
    else
    {
        /* use time stamp from CoreAudio if valid */
        if( inOutputTime->mFlags & kAudioTimeStampSampleTimeValid) 
        {
            past->past_FrameCount = inOutputTime->mSampleTime;
        }
        
        /* Process full input buffer and fill up empty output buffers. */
        result = Pa_TimeSlice( past, inInputData, outOutputData );
    }

    // FIXME PaHost_UpdateStreamTime( pahsc );

    return result;
}

#if 0
static int PaHost_CalcTimeOut( internalPortAudioStream *past )
{
    /* Calculate timeOut longer than longest time it could take to play all buffers. */
    int timeOut = (UInt32) (1500.0 * PaHost_GetTotalBufferFrames( past ) / past->past_SampleRate);
    if( timeOut < MIN_TIMEOUT_MSEC ) timeOut = MIN_TIMEOUT_MSEC;
    return timeOut;
}
#endif


/*******************************************************************/
/* Attempt to set device sample rate. */
static PaError PaHost_SetSampleRate( AudioDeviceID devID, Boolean isInput, double sampleRate )
{
    AudioStreamBasicDescription formatDesc;
    OSStatus err;
    memset( &formatDesc, 0, sizeof(AudioStreamBasicDescription) );
    formatDesc.mSampleRate = sampleRate;
    err = AudioDeviceSetProperty( devID, 0, 0,
        isInput, kAudioDevicePropertyStreamFormat, sizeof(formatDesc), &formatDesc);

    if (err != kAudioHardwareNoError) return paInvalidSampleRate;
   	else return paNoError;
}

static void DumpDeviceInfo( AudioDeviceID devID, Boolean isInput )
{
    OSStatus err = noErr;
    UInt32    dataSize;
    UInt32    data32;
    AudioValueRange audioRange;
    
    dataSize = sizeof( data32 );
    err = AudioDeviceGetProperty( devID, 0, isInput, 
        kAudioDevicePropertyLatency, &dataSize, &data32 );
    if( err != noErr )
    {
        ERR_RPT(("Error reading latency = %d\n", (int)err));
        return;
    }
    PRINT(("Device latency = %d\n", (int)data32 ));
    
    dataSize = sizeof( data32 );
    err = AudioDeviceGetProperty( devID, 0, isInput, 
        kAudioDevicePropertyBufferSize, &dataSize, &data32 );
    if( err != noErr )
    {
        ERR_RPT(("Error reading buffer size = %d\n", (int)err));
        return;
    }
    PRINT(("Buffer size = %d bytes\n", (int)data32 ));

    dataSize = sizeof( audioRange );
    err = AudioDeviceGetProperty( devID, 0, isInput, 
        kAudioDevicePropertyBufferSizeRange, &dataSize, &audioRange );
    if( err != noErr )
    {
        ERR_RPT(("Error reading buffer size range = %d\n", (int)err));
        return;
    }
    PRINT(("Buffer size range = %g to %g bytes\n", audioRange.mMinimum, audioRange.mMaximum ));
    
    dataSize = sizeof( data32 );
    err = AudioDeviceGetProperty( devID, 0, isInput, 
        kAudioDevicePropertyBufferFrameSize, &dataSize, &data32 );
    if( err != noErr )
    {
        ERR_RPT(("Error reading buffer size = %d\n", (int)err));
        return;
    }
    PRINT(("Buffer size = %d frames\n", (int)data32 ));
    
    dataSize = sizeof( audioRange );
    err = AudioDeviceGetProperty( devID, 0, isInput, 
        kAudioDevicePropertyBufferFrameSizeRange, &dataSize, &audioRange );
    if( err != noErr )
    {
        ERR_RPT(("Error reading buffer size range = %d\n", (int)err));
        return;
    }
    PRINT(("Buffer size range = %g to %g frames\n", audioRange.mMinimum, audioRange.mMaximum ));

    return;
}

/*******************************************************************/
PaError PaHost_OpenInputStream( internalPortAudioStream   *past )
{
    PaHostSoundControl *pahsc;
    const PaHostDeviceInfo *hostDeviceInfo;
    PaError          result = paNoError;
    UInt32           dataSize;
    OSStatus         err = noErr;
    int              needConverter = 0;
    double           deviceRate = past->past_SampleRate;

    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    
    DBUG(("PaHost_OpenStream: deviceID = 0x%x\n", past->past_InputDeviceID));
    if( (past->past_InputDeviceID < LOWEST_INPUT_DEVID) ||
        (past->past_InputDeviceID > HIGHEST_INPUT_DEVID) )
    {
        return paInvalidDeviceId;
    }
    hostDeviceInfo = &sDeviceInfos[past->past_InputDeviceID];
    
/* If output is using a converter then we need an input converter. */
    if( pahsc->pahsc_OutputConverter != NULL )
    {
        DBUG(("Using output converter so also need input converter.\n"));
        needConverter = 1;
    }
    else
    {
        /* Try to set sample rate. */
        result = PaHost_SetSampleRate( hostDeviceInfo->audioDeviceID, IS_INPUT, past->past_SampleRate );
        if( result != paNoError )
        {
            needConverter = 1;
            result = paNoError;
        }
    
        if( past->past_NumInputChannels > hostDeviceInfo->paInfo.maxInputChannels )
        {
            return paInvalidChannelCount; /* Too many channels! */
        }
        else if( past->past_NumInputChannels < hostDeviceInfo->paInfo.maxInputChannels )
        {
        /* Attempt to set number of channels. */ 
            AudioStreamBasicDescription formatDesc;
            OSStatus err;
            memset( &formatDesc, 0, sizeof(AudioStreamBasicDescription) );
            formatDesc.mChannelsPerFrame = past->past_NumInputChannels;
            err = AudioDeviceSetProperty( hostDeviceInfo->audioDeviceID, 0, 0,
                IS_INPUT, kAudioDevicePropertyStreamFormat, sizeof(formatDesc), &formatDesc);
            if (err != kAudioHardwareNoError)
            {
                needConverter = 1;
            }
        }
    
        /* Change the I/O bufferSize of the device. */
        dataSize = sizeof(pahsc->pahsc_FramesPerHostBuffer);
        err = AudioDeviceSetProperty( hostDeviceInfo->audioDeviceID, 0, 0, IS_INPUT,
                                    kAudioDevicePropertyBufferFrameSize, dataSize,
                                    &pahsc->pahsc_FramesPerHostBuffer);
        if( err != noErr )
        {
            DBUG(("Need converter for buffer size = %d\n", pahsc->pahsc_FramesPerHostBuffer));
            needConverter = 1;
        }
    }
    
    // setup PA conversion procedure
    result = PaConvert_SetupInput( past, paFloat32 );
    
    if( needConverter )
    {
        AudioStreamBasicDescription sourceStreamFormat, destStreamFormat;
        
        /* Get source device format */
        dataSize = sizeof(sourceStreamFormat);
        err = AudioDeviceGetProperty(hostDeviceInfo->audioDeviceID, 0, IS_INPUT,
            kAudioDevicePropertyStreamFormat, &dataSize, &sourceStreamFormat);
        if( err != noErr )
        {
            ERR_RPT(("Could not get input device format. Err = %d\n", (int) err));
            return paHostError;
        }
        deviceRate = sourceStreamFormat.mSampleRate;
        
        /* Set target user format. */
        destStreamFormat = sourceStreamFormat;
        destStreamFormat.mSampleRate = past->past_SampleRate;	// sample rate of the user synthesis code
        destStreamFormat.mChannelsPerFrame = past->past_NumInputChannels;	//	the number of channels in each frame
                
        /* Allocate an input buffer because we need it between the user callback and the converter. */
        pahsc->pahsc_InputConverterBuffer = PaHost_AllocateFastMemory( pahsc->pahsc_BytesPerUserNativeInputBuffer );
        err = AudioConverterNew (
            &sourceStreamFormat, 
            &destStreamFormat, 
            &pahsc->pahsc_InputConverter);
        if( err != noErr )
        {
            ERR_RPT(("Could not create input format converter. Err = %d\n", (int) err));
            return paHostError;
        }
    }
    
    /* Allocate FIFO between Device callback and Converter callback so that device can push data
    * and converter can pull data.
    */
    if( pahsc->pahsc_InputConverterBuffer != NULL )
    {
        double sampleRateRatio = deviceRate / past->past_SampleRate;
        long minSize = pahsc->pahsc_BytesPerUserNativeInputBuffer * 2 * sampleRateRatio;
        long numBytes = RoundUpToNextPowerOf2( minSize );
        
        DBUG(("Using input converter so also need ring buffer. Size = %d bytes\n", numBytes));
        pahsc->pahsc_FIFOdata = PaHost_AllocateFastMemory( numBytes );
        if( pahsc->pahsc_FIFOdata == NULL )
        {
            return paInsufficientMemory;
        }
        RingBuffer_Init( &pahsc->pahsc_FIFO, numBytes, pahsc->pahsc_FIFOdata );
    }
    
    return result;
}

/*******************************************************************/
PaError PaHost_OpenOutputStream( internalPortAudioStream *past )
{
    PaHostSoundControl *pahsc;
    const PaHostDeviceInfo *hostDeviceInfo;
    PaError          result = paNoError;
    UInt32           dataSize;
    OSStatus         err = noErr;
    int              needConverter = 0;
    
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    
    DBUG(("PaHost_OpenStream: deviceID = 0x%x\n", past->past_OutputDeviceID));
    if( (past->past_OutputDeviceID < LOWEST_OUTPUT_DEVID) ||
        (past->past_OutputDeviceID > HIGHEST_OUTPUT_DEVID) )
    {
        return paInvalidDeviceId;
    }
    hostDeviceInfo = &sDeviceInfos[past->past_OutputDeviceID];

    //DumpDeviceInfo( hostDeviceInfo->audioDeviceID, IS_OUTPUT );
    
    /* Try to set sample rate. */
    result = PaHost_SetSampleRate( hostDeviceInfo->audioDeviceID, IS_OUTPUT, past->past_SampleRate );
	if( result != paNoError )
    {
        needConverter = 1;
        result = paNoError;
    }

    if( past->past_NumOutputChannels > hostDeviceInfo->paInfo.maxOutputChannels )
    {
        return paInvalidChannelCount; /* Too many channels! */
    }
    else if( past->past_NumOutputChannels < hostDeviceInfo->paInfo.maxOutputChannels )
    {
    /* Attempt to set number of channels. */ 
        AudioStreamBasicDescription formatDesc;
        OSStatus err;
        memset( &formatDesc, 0, sizeof(AudioStreamBasicDescription) );
        formatDesc.mChannelsPerFrame = past->past_NumOutputChannels;
        err = AudioDeviceSetProperty( hostDeviceInfo->audioDeviceID, 0, 0,
            IS_OUTPUT, kAudioDevicePropertyStreamFormat, sizeof(formatDesc), &formatDesc);
        if (err != kAudioHardwareNoError)
        {
            needConverter = 1;
        }
    }

    /* Change the I/O bufferSize of the device. */
    dataSize = sizeof(pahsc->pahsc_FramesPerHostBuffer);
    err = AudioDeviceSetProperty( hostDeviceInfo->audioDeviceID, 0, 0, IS_OUTPUT,
                                  kAudioDevicePropertyBufferFrameSize, dataSize,
                                  &pahsc->pahsc_FramesPerHostBuffer);
    if( err != noErr )
    {
        DBUG(("Need converter for buffer size = %d\n", pahsc->pahsc_FramesPerHostBuffer));
        needConverter = 1;
    }
    
    //DumpDeviceInfo( hostDeviceInfo->audioDeviceID, IS_OUTPUT );
    
    // setup conversion procedure
    result = PaConvert_SetupOutput( past, paFloat32 );
    
    if( needConverter )
    {
        AudioStreamBasicDescription sourceStreamFormat, destStreamFormat;
        
        /* Get target device format */
        dataSize = sizeof(destStreamFormat);
        err = AudioDeviceGetProperty(hostDeviceInfo->audioDeviceID, 0, IS_OUTPUT,
            kAudioDevicePropertyStreamFormat, &dataSize, &destStreamFormat);
        if( err != noErr )
        {
            ERR_RPT(("Could not get output device format. Err = %d\n", (int) err));
            return paHostError;
        }

        /* Set source user format. */
        sourceStreamFormat = destStreamFormat;
        sourceStreamFormat.mSampleRate = past->past_SampleRate;	// sample rate of the user synthesis code
        sourceStreamFormat.mChannelsPerFrame = past->past_NumOutputChannels;	//	the number of channels in each frame
                
        /* Allocate an output buffer because we need it between the user callback and the converter. */
        pahsc->pahsc_OutputConverterBuffer = PaHost_AllocateFastMemory( pahsc->pahsc_BytesPerUserNativeOutputBuffer );
        err = AudioConverterNew (
            &sourceStreamFormat, 
            &destStreamFormat, 
            &pahsc->pahsc_OutputConverter);
        if( err != noErr )
        {
            ERR_RPT(("Could not create output format converter. Err = %d\n", (int) err));
            return paHostError;
        }
    }
    
    return result;
}

/*******************************************************************/
PaError PaHost_GetTotalBufferFrames( internalPortAudioStream   *past )
{
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    return pahsc->pahsc_FramesPerHostBuffer;
}


/*******************************************************************
* Determine how many User Buffers we can put into our CoreAudio stream buffer.
* Uses:
*    past->past_FramesPerUserBuffer, etc.
* Sets:
*    past->past_NumUserBuffers
*    pahsc->pahsc_UserBuffersPerHostBuffer
*    pahsc->pahsc_FramesPerHostBuffer
*    pahsc->pahsc_BytesPerUserNativeOutputBuffer
*/
static void PaHost_CalcHostBufferSize( internalPortAudioStream *past )
{
    PaHostSoundControl *pahsc = ( PaHostSoundControl *)past->past_DeviceData;
    unsigned int  minNumUserBuffers;

    // Determine number of user buffers based on minimum latency.
    minNumUserBuffers = Pa_GetMinNumBuffers( past->past_FramesPerUserBuffer, past->past_SampleRate );
    // Compare to user requested number in case user wants more than the minimum.
    past->past_NumUserBuffers = ( minNumUserBuffers > past->past_NumUserBuffers ) ?
                                minNumUserBuffers : past->past_NumUserBuffers;
    DBUG(("PaHost_CalcNumHostBuffers: min past_NumUserBuffers = %d\n", past->past_NumUserBuffers ));

    // For CoreAudio, we only have one Host buffer, so...
    pahsc->pahsc_UserBuffersPerHostBuffer = past->past_NumUserBuffers;
    // Calculate size of CoreAudio buffer.
    pahsc->pahsc_FramesPerHostBuffer = past->past_FramesPerUserBuffer * past->past_NumUserBuffers;

    // calculate buffer sizes in bytes
    pahsc->pahsc_BytesPerUserNativeInputBuffer = past->past_FramesPerUserBuffer *
        Pa_GetSampleSize(paFloat32) * past->past_NumInputChannels;
    pahsc->pahsc_BytesPerUserNativeOutputBuffer = past->past_FramesPerUserBuffer *
        Pa_GetSampleSize(paFloat32) * past->past_NumOutputChannels;

    DBUG(("PaHost_CalcNumHostBuffers: pahsc_UserBuffersPerHostBuffer = %d\n", pahsc->pahsc_UserBuffersPerHostBuffer ));
    DBUG(("PaHost_CalcNumHostBuffers: pahsc_FramesPerHostBuffer = %d\n", pahsc->pahsc_FramesPerHostBuffer ));
}

/*******************************************************************/
PaError PaHost_OpenStream( internalPortAudioStream   *past )
{
    PaError             result = paNoError;
    PaHostSoundControl *pahsc;

    /* Allocate and initialize host data. */
    pahsc = (PaHostSoundControl *) malloc(sizeof(PaHostSoundControl));
    if( pahsc == NULL )
    {
        result = paInsufficientMemory;
        goto error;
    }
    memset( pahsc, 0, sizeof(PaHostSoundControl) );
    past->past_DeviceData = (void *) pahsc;

    // If we are using both input and out, then they must be on the same CoreAudio device,
    // until we implement a FIFO between two devices.
    if( (past->past_OutputDeviceID != paNoDevice) && (past->past_InputDeviceID != paNoDevice) )
    {
        AudioDeviceID inputID = sDeviceInfos[past->past_InputDeviceID].audioDeviceID;
        AudioDeviceID outputID = sDeviceInfos[past->past_OutputDeviceID].audioDeviceID;
        if( inputID != outputID )
        {
            ERR_RPT(("PortAudio: input and output must use same CoreAudio device!\n"));
            return paInvalidDeviceId;
        }
    }

    PaHost_CalcHostBufferSize( past );
    
    /* Setup constants for CPU load measurement. */
    pahsc->pahsc_InverseMicrosPerHostBuffer = past->past_SampleRate / (1000000.0 * 	pahsc->pahsc_FramesPerHostBuffer);

    /* ------------------ OUTPUT */
    if( (past->past_OutputDeviceID != paNoDevice) && (past->past_NumOutputChannels > 0) )
    {
        pahsc->pahsc_AudioDeviceID = sDeviceInfos[past->past_OutputDeviceID].audioDeviceID;
        result = PaHost_OpenOutputStream( past );
        if( result < 0 ) goto error;
    }

    /* ------------------ INPUT */
    if( (past->past_InputDeviceID != paNoDevice) && (past->past_NumInputChannels > 0) )
    {
        pahsc->pahsc_AudioDeviceID = sDeviceInfos[past->past_InputDeviceID].audioDeviceID;
        result = PaHost_OpenInputStream( past );
        if( result < 0 ) goto error;
    }

    return result;

error:
    PaHost_CloseStream( past );
    return result;
}

/*************************************************************************/
PaError PaHost_StartOutput( internalPortAudioStream *past )
{
    return 0;
}

/*************************************************************************/
PaError PaHost_StartInput( internalPortAudioStream *past )
{
    return 0;
}

/*************************************************************************/
PaError PaHost_StartEngine( internalPortAudioStream *past )
{
    OSStatus            err = noErr;
    PaError             result = paNoError;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;

    past->past_StopSoon = 0;
    past->past_StopNow = 0;
    past->past_IsActive = 1;
    
    // Associate an IO proc with the device and pass a pointer to the audio data context
    err = AudioDeviceAddIOProc(pahsc->pahsc_AudioDeviceID, (AudioDeviceIOProc)appIOProc, past);
    if (err != noErr) goto error;

    // start playing sound through the device
    err = AudioDeviceStart(pahsc->pahsc_AudioDeviceID, (AudioDeviceIOProc)appIOProc);
    if (err != noErr) goto error;
    return result;

#if PA_TRACE_START_STOP
    AddTraceMessage( "PaHost_StartEngine: TimeSlice() returned ", result );
#endif

error:
    return paHostError; // FIXME - save host error
}

/*************************************************************************/
PaError PaHost_StopEngine( internalPortAudioStream *past, int abort )
{
    OSStatus  err = noErr;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paNoError;
    (void) abort;

    /* Tell background thread to stop generating more data and to let current data play out. */
    past->past_StopSoon = 1;
    /* If aborting, tell background thread to stop NOW! */
    if( abort ) past->past_StopNow = 1;
    past->past_IsActive = 0;

#if PA_TRACE_START_STOP
    AddTraceMessage( "PaHost_StopOutput: pahsc_HWaveOut ", (int) pahsc->pahsc_HWaveOut );
#endif

    // FIXME - we should ask proc to stop instead of stopping abruptly
    err = AudioDeviceStop(pahsc->pahsc_AudioDeviceID, (AudioDeviceIOProc)appIOProc);
    if (err != noErr) goto Bail;

    err = AudioDeviceRemoveIOProc(pahsc->pahsc_AudioDeviceID, (AudioDeviceIOProc)appIOProc);
    if (err != noErr) goto Bail;

    return paNoError;

Bail:
    return paHostError; // FIXME - save err
}

/*************************************************************************/
PaError PaHost_StopInput( internalPortAudioStream *past, int abort )
{
    return paNoError;
}

/*************************************************************************/
PaError PaHost_StopOutput( internalPortAudioStream *past, int abort )
{
    return paNoError;
}

/*******************************************************************/
PaError PaHost_CloseStream( internalPortAudioStream   *past )
{
    PaHostSoundControl *pahsc;

    if( past == NULL ) return paBadStreamPtr;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paNoError;
        
    //DumpDeviceInfo( sDeviceInfos[past->past_OutputDeviceID].audioDeviceID, IS_OUTPUT );

    DBUG(("pahsc_BytesPerUserNativeOutputBuffer = %d\n", pahsc->pahsc_BytesPerUserNativeOutputBuffer));

#if PA_TRACE_START_STOP
    AddTraceMessage( "PaHost_CloseStream: pahsc_HWaveOut ", (int) pahsc->pahsc_HWaveOut );
#endif

    if( pahsc->pahsc_OutputConverterBuffer != NULL )
    {
        PaHost_FreeFastMemory( pahsc->pahsc_OutputConverterBuffer, pahsc->pahsc_BytesPerUserNativeOutputBuffer );
    }
    if( pahsc->pahsc_InputConverterBuffer != NULL )
    {
        PaHost_FreeFastMemory( pahsc->pahsc_InputConverterBuffer, pahsc->pahsc_BytesPerUserNativeInputBuffer );
    }
    if( pahsc->pahsc_FIFOdata != NULL )
    {
        PaHost_FreeFastMemory( pahsc->pahsc_FIFOdata, pahsc->pahsc_FIFO.bufferSize );
    }
    if( pahsc->pahsc_OutputConverter != NULL )
    {
        verify_noerr(AudioConverterDispose (pahsc->pahsc_OutputConverter));
    }
    if( pahsc->pahsc_InputConverter != NULL )
    {
        verify_noerr(AudioConverterDispose (pahsc->pahsc_InputConverter));
    }
    
    free( pahsc );
    past->past_DeviceData = NULL;

    return paNoError;
}

/*************************************************************************
** Determine minimum number of buffers required for this host based
** on minimum latency. Latency can be optionally set by user by setting
** an environment variable. For example, to set latency to 20 msec, put:
**
**    set PA_MIN_LATENCY_MSEC=20
**
** in the .cshrc file.
*/
#define PA_LATENCY_ENV_NAME  ("PA_MIN_LATENCY_MSEC")

int Pa_GetMinNumBuffers( int framesPerBuffer, double framesPerSecond )
{
    int minBuffers;
    double denominator;
    int minLatencyMsec = PA_MIN_LATENCY_MSEC;
    char *minLatencyText = getenv(PA_LATENCY_ENV_NAME);
    if( minLatencyText != NULL )
    {
        PRINT(("PA_MIN_LATENCY_MSEC = %s\n", minLatencyText ));
        minLatencyMsec = atoi( minLatencyText );
        if( minLatencyMsec < 1 ) minLatencyMsec = 1;
        else if( minLatencyMsec > 5000 ) minLatencyMsec = 5000;
    }

    denominator =  1000.0 * framesPerBuffer;
    minBuffers = (int) (((minLatencyMsec * framesPerSecond) + denominator - 1) / denominator );
    if( minBuffers < 1 ) minBuffers = 1;
    return minBuffers;
}

/*************************************************************************
** Cleanup device info.
*/
PaError PaHost_Term( void )
{
    int i;

    if( sDeviceInfos != NULL )
    {
        for( i=0; i<sNumPaDevices; i++ )
        {
            if( sDeviceInfos[i].paInfo.name != NULL )
            {
                free( (char*)sDeviceInfos[i].paInfo.name );
            }
        }
        free( sDeviceInfos );
        sDeviceInfos = NULL;
    }

    sNumPaDevices = 0;
    return paNoError;
}

/*************************************************************************/
void Pa_Sleep( long msec )
{
#if 0
    struct timeval timeout;
    timeout.tv_sec = msec / 1000;
	timeout.tv_usec = (msec % 1000) * 1000;
	select( 0, NULL, NULL, NULL, &timeout );
#else
    usleep( msec * 1000 );
#endif
}

/*************************************************************************
 * Allocate memory that can be accessed in real-time.
 * This may need to be held in physical memory so that it is not
 * paged to virtual memory.
 * This call MUST be balanced with a call to PaHost_FreeFastMemory().
 */
void *PaHost_AllocateFastMemory( long numBytes )
{
    void *addr = malloc( numBytes ); /* FIXME - do we need physical memory? */
    if( addr != NULL ) memset( addr, 0, numBytes );
    return addr;
}

/*************************************************************************
 * Free memory that could be accessed in real-time.
 * This call MUST be balanced with a call to PaHost_AllocateFastMemory().
 */
void PaHost_FreeFastMemory( void *addr, long numBytes )
{
    if( addr != NULL ) free( addr );
}


/***********************************************************************/
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
    AudioTimeStamp timeStamp;
    PaTimestamp streamTime;
    PaHostSoundControl *pahsc;
    internalPortAudioStream   *past = (internalPortAudioStream *) stream;
    if( past == NULL ) return paBadStreamPtr;
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
  
    AudioDeviceGetCurrentTime(pahsc->pahsc_AudioDeviceID, &timeStamp);
  
    streamTime = ( timeStamp.mFlags & kAudioTimeStampSampleTimeValid) ?
            timeStamp.mSampleTime : past->past_FrameCount;

    return streamTime;
}
