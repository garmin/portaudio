/*
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 * Linux OSS Implementation by douglas repetto and Phil Burk
 *
 * Copyright (c) 1999-2000 Phil Burk
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
/*
Modfication History
  1/2001 - Phil Burk - initial hack for Linux
  2/2001 - Douglas Repetto - many improvements, initial query support
  4/2/2001 - Phil - stop/abort thread control, separate in/out native buffers
  5/28/2001 - Phil - use pthread_create() instead of clone(). Thanks Stephen Brandon!
       use pthread_join() after thread shutdown.
  5/29/2001 - Phil - query for multiple devices, multiple formats,
                     input mode and input+output mode working,
       Pa_GetCPULoad() implemented.
  PLB20010817 - Phil & Janos Haber - don't halt if test of sample rate fails.
  SB20010904 - Stephen Brandon - mods needed for GNUSTEP and SndKit
  JH20010905 - Janos Haber - FreeBSD mods
  2001-09-22 - Heiko - (i.e. Heiko Purnhagen <purnhage@tnt.uni-hannover.de> ;-)
                       added 24k and 16k to ratesToTry[]
         fixed Pa_GetInternalDevice()
         changed DEVICE_NAME_BASE from /dev/audio to /dev/dsp
         handled SNDCTL_DSP_SPEED in Pq_QueryDevice() more graceful
         fixed Pa_StreamTime() for paqa_errs.c
         fixed numCannel=2 oddity and error handling in Pa_SetupDeviceFormat()
         grep also for HP20010922 ...
  PLB20010924 - Phil - merged Heiko's changes
                       removed sNumDevices and potential related bugs,
         use getenv("PA_MIN_LATENCY_MSEC") to set desired latency,
         simplify CPU Load calculation by comparing real-time to framesPerBuffer,
         always close device when querying even if error occurs,
  PLB20010927 - Phil - Improved negotiation for numChannels.
  SG20011005 - Stewart Greenhill - set numChannels back to reasonable value after query.
  DH20010115 - David Herring - fixed uninitialized handle.

  DM20020218 - Dominic Mazzoni - Try to open in nonblocking mode first, in case
                                 the device is already open.  New implementation of
                                 Pa_StreamTime that uses SNDCTL_DSP_GETOPTR but
                                 uses our own counter to avoid wraparound.
  PLB20020222 - Phil Burk - Added WatchDog proc if audio running at high priority.
                      Check error return from read() and write().
                      Check CPU endianness instead of assuming Little Endian.
  
TODO
O- put semaphore lock around shared data?
O- handle native formats better
O- handle stereo-only device better ???
O- what if input and output of a device capabilities differ (e.g. es1371) ???
*/
/*
 PROPOSED - should we add this to "portaudio.h". Problem with 
 Pa_QueryDevice() not having same driver name os Pa_OpenStream().
 
 A PaDriverInfo structure can be passed to the underlying device
 on the Pa_OpenStream() call. The contents and interpretation of
 the structure is determined by the PA implementation.
*/
typedef struct PaDriverInfo /* PROPOSED */
{
    /* Size of structure. Allows driver to extend the structure without breaking existing applications. */
    int           size;
    /* Can be used to request a specific device name. */
    const char   *name;
    unsigned long data;
}
PaDriverInfo;



#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <errno.h>

#ifdef __linux__
#include <linux/soundcard.h>
#else
#include <machine/soundcard.h> /* JH20010905 */
#endif

#include "portaudio.h"
#include "pa_host.h"
#include "pa_trace.h"

#define PRINT(x)   { printf x; fflush(stdout); }
#define ERR_RPT(x) PRINT(x)
#define DBUG(x)    /* PRINT(x) */
#define DBUGX(x)   /* PRINT(x) */

#define BAD_DEVICE_ID (-1)

#define MIN_LATENCY_MSEC   (100)
#define MIN_TIMEOUT_MSEC   (100)
#define MAX_TIMEOUT_MSEC   (1000)

/************************************************* Definitions ********/
#ifdef __linux__
 #define DEVICE_NAME_BASE            "/dev/dsp"
#else
 #define DEVICE_NAME_BASE            "/dev/audio"
#endif

/* What is a pthread_t anyway? Here is a guess at a value that will never happen in real life. */ 
#define INVALID_THREAD              (-1)

#define MAX_CHARS_DEVNAME           (32)
#define MAX_SAMPLE_RATES            (10)
typedef struct internalPortAudioDevice
{
    struct internalPortAudioDevice *pad_Next; /* Singly linked list. */
    double          pad_SampleRates[MAX_SAMPLE_RATES]; /* for pointing to from pad_Info */
    char            pad_DeviceName[MAX_CHARS_DEVNAME];
    PaDeviceInfo    pad_Info;
}
internalPortAudioDevice;

/* Define structure to contain all OSS and Linux specific data. */
typedef struct PaHostSoundControl
{
    int              pahsc_OutputHandle;
    int              pahsc_InputHandle;
    int              pahsc_AudioPriority;          /* priority of background audio thread */
    pthread_t        pahsc_AudioThread;            /* background audio thread */
    pid_t            pahsc_AudioThreadPID;         /* background audio thread */
    pthread_t        pahsc_WatchDogThread;         /* highest priority thread that protects system */
    int              pahsc_WatchDogRun;           /* Ask WatchDog to stop. */
    pthread_t        pahsc_CanaryThread;           /* low priority thread that detects abuse by audio */
    struct timeval   pahsc_CanaryTime;
    int              pahsc_CanaryRun;             /* Ask Canary to stop. */
    short           *pahsc_NativeInputBuffer;
    short           *pahsc_NativeOutputBuffer;
    unsigned int     pahsc_BytesPerInputBuffer;    /* native buffer size in bytes */
    unsigned int     pahsc_BytesPerOutputBuffer;   /* native buffer size in bytes */
    /* For measuring CPU utilization. */
    struct timeval   pahsc_EntryTime;
    double           pahsc_InverseMicrosPerBuffer; /* 1/Microseconds of real-time audio per user buffer. */

   /* For calculating stream time */
    int              pahsc_LastPosPtr;
    double           pahsc_LastStreamBytes;
}
PaHostSoundControl;

/************************************************* Shared Data ********/
/* FIXME - put Mutex around this shared data. */
static internalPortAudioDevice *sDeviceList = NULL;
static int sDefaultInputDeviceID = paNoDevice;
static int sDefaultOutputDeviceID = paNoDevice;
static int sPaHostError = 0;

/************************************************* Prototypes **********/

static internalPortAudioDevice *Pa_GetInternalDevice( PaDeviceID id );
static PaError Pa_QueryDevices( void );
static PaError Pa_QueryDevice( const char *deviceName, internalPortAudioDevice *pad );
static PaError Pa_SetupDeviceFormat( int devHandle, int numChannels, int sampleRate );

/********************************* BEGIN CPU UTILIZATION MEASUREMENT ****/
static void Pa_StartUsageCalculation( internalPortAudioStream   *past )
{
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return;
    /* Query system timer for usage analysis and to prevent overuse of CPU. */
    gettimeofday( &pahsc->pahsc_EntryTime, NULL );
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
    struct timeval currentTime;
    long  usecsElapsed;
    double newUsage;

#define LOWPASS_COEFFICIENT_0   (0.95)
#define LOWPASS_COEFFICIENT_1   (0.99999 - LOWPASS_COEFFICIENT_0)

    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return;

    if( gettimeofday( &currentTime, NULL ) == 0 )
    {
        usecsElapsed = SubtractTime_AminusB( &currentTime, &pahsc->pahsc_EntryTime );
        /* Use inverse because it is faster than the divide. */
        newUsage =  usecsElapsed * pahsc->pahsc_InverseMicrosPerBuffer;

        past->past_Usage = (LOWPASS_COEFFICIENT_0 * past->past_Usage) +
                           (LOWPASS_COEFFICIENT_1 * newUsage);
    }
}
/****************************************** END CPU UTILIZATION *******/

#ifndef AFMT_S16_NE
#define AFMT_S16_NE  Get_AFMT_S16_NE()
/*********************************************************************
 * Some versions of OSS do not define AFMT_S16_NE. So check CPU.
 * PowerPC is Big Endian. X86 is Little Endian.
 */
static int Get_AFMT_S16_NE( void )
{
    long testData = 1; 
    char *ptr = (char *) &testData;
    int isLittle = ( *ptr == 1 ); /* Does address point to least significant byte? */
    return isLittle ? AFMT_S16_LE : AFMT_S16_BE;
}
#endif

/*********************************************************************
 * Try to open the named device.
 * If it opens, try to set various rates and formats and fill in 
 * the device info structure.
 */
static PaError Pa_QueryDevice( const char *deviceName, internalPortAudioDevice *pad )
{
    int result = paHostError;
    int tempDevHandle;
    int numChannels, maxNumChannels;
    int format;
    int numSampleRates;
    int sampleRate;
    int numRatesToTry;
    int ratesToTry[9] = {96000, 48000, 44100, 32000, 24000, 22050, 16000, 11025, 8000};
    int i;

    /* douglas:
     we have to do this querying in a slightly different order. apparently
     some sound cards will give you different info based on their settins. 
     e.g. a card might give you stereo at 22kHz but only mono at 44kHz.
     the correct order for OSS is: format, channels, sample rate
     
    */
    if ( (tempDevHandle = open(deviceName,O_WRONLY|O_NONBLOCK))  == -1 )
    {
        DBUG(("Pa_QueryDevice: could not open %s\n", deviceName ));
        return paHostError;
    }

    /*  Ask OSS what formats are supported by the hardware. */
    pad->pad_Info.nativeSampleFormats = 0;
    if (ioctl(tempDevHandle, SNDCTL_DSP_GETFMTS, &format) == -1)
    {
        ERR_RPT(("Pa_QueryDevice: could not get format info\n" ));
        goto error;
    }
    if( format & AFMT_U8 )     pad->pad_Info.nativeSampleFormats |= paUInt8;
    if( format & AFMT_S16_NE ) pad->pad_Info.nativeSampleFormats |= paInt16;

    /* Negotiate for the maximum number of channels for this device. PLB20010927
     * Consider up to 16 as the upper number of channels.
     * Variable numChannels should contain the actual upper limit after the call.
     * Thanks to John Lazzaro and Heiko Purnhagen for suggestions.
     */
    maxNumChannels = 0;
    for( numChannels = 1; numChannels <= 16; numChannels++ )
    {
        int temp = numChannels;
        DBUG(("Pa_QueryDevice: use SNDCTL_DSP_CHANNELS, numChannels = %d\n", numChannels ))
        if(ioctl(tempDevHandle, SNDCTL_DSP_CHANNELS, &temp) < 0 )
        {
            /* ioctl() failed so bail out if we already have stereo */
            if( numChannels > 2 ) break;
        }
        else
        {
            /* ioctl() worked but bail out if it does not support numChannels.
             * We don't want to leave gaps in the numChannels supported.
             */
            if( (numChannels > 2) && (temp != numChannels) ) break;
            DBUG(("Pa_QueryDevice: temp = %d\n", temp ))
            if( temp > maxNumChannels ) maxNumChannels = temp; /* Save maximum. */
        }
    }

    /* The above negotiation may fail for an old driver so try this older technique. */
    if( maxNumChannels < 1 )
    {
        int stereo = 1;
        if(ioctl(tempDevHandle, SNDCTL_DSP_STEREO, &stereo) < 0)
        {
            maxNumChannels = 1;
        }
        else
        {
            maxNumChannels = (stereo) ? 2 : 1;
        }
        DBUG(("Pa_QueryDevice: use SNDCTL_DSP_STEREO, maxNumChannels = %d\n", maxNumChannels ))
    }

    pad->pad_Info.maxOutputChannels = maxNumChannels;
    DBUG(("Pa_QueryDevice: maxNumChannels = %d\n", maxNumChannels))

    /* During channel negotiation, the last ioctl() may have failed. This can
     * also cause sample rate negotiation to fail. Hence the following, to return
     * to a supported number of channels. SG20011005 */
    {
        int temp = maxNumChannels;
        if( temp > 2 ) temp = 2; /* use most reasonable default value */
        ioctl(tempDevHandle, SNDCTL_DSP_CHANNELS, &temp);
    }

    /* FIXME - for now, assume maxInputChannels = maxOutputChannels.
     *    Eventually do separate queries for O_WRONLY and O_RDONLY
    */
    pad->pad_Info.maxInputChannels = pad->pad_Info.maxOutputChannels;

    DBUG(("Pa_QueryDevice: maxInputChannels = %d\n",
          pad->pad_Info.maxInputChannels))


    /* Determine available sample rates by trying each one and seeing result.
     */
    numSampleRates = 0;
    numRatesToTry = sizeof(ratesToTry)/sizeof(int);
    for (i = 0; i < numRatesToTry; i++)
    {
        sampleRate = ratesToTry[i];

        if (ioctl(tempDevHandle, SNDCTL_DSP_SPEED, &sampleRate) >= 0 ) /* PLB20010817 */
        {
            if (sampleRate == ratesToTry[i])
            {
                DBUG(("Pa_QueryDevice: got sample rate: %d\n", sampleRate))
                pad->pad_SampleRates[numSampleRates] = (float)ratesToTry[i];
                numSampleRates++;
            }
        }
    }

    DBUG(("Pa_QueryDevice: final numSampleRates = %d\n", numSampleRates))
    if (numSampleRates==0)   /* HP20010922 */
    {
        ERR_RPT(("Pa_QueryDevice: no supported sample rate (or SNDCTL_DSP_SPEED ioctl call failed).\n" ));
        goto error;
    }

    pad->pad_Info.numSampleRates = numSampleRates;
    pad->pad_Info.sampleRates = pad->pad_SampleRates;

    pad->pad_Info.name = deviceName;

    result = paNoError;

error:
    /* We MUST close the handle here or we won't be able to reopen it later!!!  */
    close(tempDevHandle);

    return result;
}

/*********************************************************************
 * Determines the number of available devices by trying to open
 * each "/dev/dsp#" or "/dsp/audio#" in order until it fails.
 * Add each working device to a singly linked list of devices.
 */
static PaError Pa_QueryDevices( void )
{
    internalPortAudioDevice *pad, *lastPad;
    int      go = 1;
    int      numDevices = 0;
    PaError  testResult;
    PaError  result = paNoError;

    sDefaultInputDeviceID = paNoDevice;
    sDefaultOutputDeviceID = paNoDevice;

    lastPad = NULL;

    while( go )
    {
        /* Allocate structure to hold device info. */
        pad = PaHost_AllocateFastMemory( sizeof(internalPortAudioDevice) );
        if( pad == NULL ) return paInsufficientMemory;
        memset( pad, 0, sizeof(internalPortAudioDevice) );

        /* Build name for device. */
        if( numDevices == 0 )
        {
            sprintf( pad->pad_DeviceName, DEVICE_NAME_BASE);
        }
        else
        {
            sprintf( pad->pad_DeviceName, DEVICE_NAME_BASE "%d", numDevices );
        }

        DBUG(("Try device %s\n", pad->pad_DeviceName ));
        testResult = Pa_QueryDevice( pad->pad_DeviceName, pad );
        DBUG(("Pa_QueryDevice returned %d\n", testResult ));
        if( testResult != paNoError )
        {
            if( lastPad == NULL )
            {
                result = testResult; /* No good devices! */
            }
            go = 0;
            PaHost_FreeFastMemory( pad, sizeof(internalPortAudioDevice) );
        }
        else
        {
            numDevices += 1;
            /* Add to linked list of devices. */
            if( lastPad )
            {
                lastPad->pad_Next = pad;
            }
            else
            {
                sDeviceList = pad; /* First element in linked list. */
            }
            lastPad = pad;
        }
    }

    return result;

}

/*************************************************************************/
int Pa_CountDevices()
{
    int numDevices = 0;
    internalPortAudioDevice *pad;

    if( sDeviceList == NULL ) Pa_Initialize();
    /* Count devices in list. */
    pad = sDeviceList;
    while( pad != NULL )
    {
        pad = pad->pad_Next;
        numDevices++;
    }

    return numDevices;
}

/*************************************************************************/
static internalPortAudioDevice *Pa_GetInternalDevice( PaDeviceID id )
{
    internalPortAudioDevice *pad;
    if( (id < 0) || ( id >= Pa_CountDevices()) ) return NULL;
    pad = sDeviceList;
    while( id > 0 )
    {
        pad = pad->pad_Next;
        id--;
    }
    return pad;
}

/*************************************************************************/
const PaDeviceInfo* Pa_GetDeviceInfo( PaDeviceID id )
{
    internalPortAudioDevice *pad;
    if( (id < 0) || ( id >= Pa_CountDevices()) ) return NULL;
    pad = Pa_GetInternalDevice( id );
    return  &pad->pad_Info ;
}

static PaError Pa_MaybeQueryDevices( void )
{
    if( sDeviceList == NULL )
    {
        return Pa_QueryDevices();
    }
    return 0;
}

PaDeviceID Pa_GetDefaultInputDeviceID( void )
{
    /* return paNoDevice; */
    return 0;
}

PaDeviceID Pa_GetDefaultOutputDeviceID( void )
{
    return 0;
}

/**********************************************************************
** Make sure that we have queried the device capabilities.
*/

PaError PaHost_Init( void )
{
    return Pa_MaybeQueryDevices();
}

/*******************************************************************************************
 * The ol' Canary in a Coal Mine trick.
 * Just update the time periodically.
 * Runs at low priority so if audio thread runs wild, this thread will get starved
 * and the watchdog will detect it.
 */
 
#define SCHEDULER_POLICY         SCHED_RR
#define WATCHDOG_MAX_SECONDS    (3)
#define WATCHDOG_INTERVAL_USEC  (1000000)

static int PaHost_CanaryProc( PaHostSoundControl   *pahsc )
{
    int   result = 0;

#ifdef GNUSTEP
    GSRegisterCurrentThread(); /* SB20010904 */
#endif

    while( pahsc->pahsc_CanaryRun && ((result = usleep( WATCHDOG_INTERVAL_USEC )) == 0) )
    { 
        gettimeofday( &pahsc->pahsc_CanaryTime, NULL );
    }

    DBUG(("PaHost_CanaryProc: exiting.\n"));
    
#ifdef GNUSTEP
    GSUnregisterCurrentThread();  /* SB20010904 */
#endif

    return result;
}

/*******************************************************************************************
 * Monitor audio thread and lower its it if it hogs the CPU.
 * To prevent getting killed, the audio thread must update a
 * variable with a timer value.
 * If the value is not recent enough, then the
 * thread will get killed. 
 */

static PaError PaHost_WatchDogProc( PaHostSoundControl   *pahsc )
{
    PaError               result = 0;
    struct sched_param    schp = { 0 };
    int                   maxPri;

#ifdef GNUSTEP
    GSRegisterCurrentThread(); /* SB20010904 */
#endif
    
/* Run at a priority level above audio thread so we can still run if it hangs. */
/* Rise more than 1 because of rumored off-by-one scheduler bugs. */
    schp.sched_priority = pahsc->pahsc_AudioPriority + 4;
    maxPri = sched_get_priority_max(SCHEDULER_POLICY);
    if( schp.sched_priority > maxPri ) schp.sched_priority = maxPri;

    if (sched_setscheduler(0, SCHEDULER_POLICY, &schp) != 0)
    {
        ERR_RPT(("PaHost_WatchDogProc: cannot set watch dog priority!\n"));
        goto killAudio;
    }
    
    /* Compare watchdog time with audio and canary thread times. */
    /* Sleep for a while or until thread cancelled. */
    while( pahsc->pahsc_WatchDogRun && ((result = usleep( WATCHDOG_INTERVAL_USEC )) == 0) )
    {
        int              delta;
        struct timeval   currentTime;
        
        gettimeofday( &currentTime, NULL );

        /* If audio thread is not advancing, then lower its priority. */
        delta = currentTime.tv_sec - pahsc->pahsc_EntryTime.tv_sec;
        DBUG(("PaHost_WatchDogProc: audio delta = %d\n", delta ));
        if( delta > WATCHDOG_MAX_SECONDS )
        {
            goto killAudio;
        }
        
        /* If canary died, then kill audio and canary. */
        delta = currentTime.tv_sec - pahsc->pahsc_CanaryTime.tv_sec;
        if( delta > WATCHDOG_MAX_SECONDS )
        {
            ERR_RPT(("PaHost_WatchDogProc: canary died!\n"));
            goto lowerAudio;
        }
    }

    DBUG(("PaHost_WatchDogProc: exiting.\n"));
#ifdef GNUSTEP
    GSUnregisterCurrentThread();  /* SB20010904 */
#endif
    return 0;
    
lowerAudio:
    {
        struct sched_param    schat = { 0 };
        if( sched_setscheduler(pahsc->pahsc_AudioThreadPID, SCHED_OTHER, &schat) != 0)
        {
            ERR_RPT(("PaHost_WatchDogProc: failed to lower audio priority. errno = %d\n", errno ));
            /* Fall through into killing audio thread. */
        }
        else
        {
            ERR_RPT(("PaHost_WatchDogProc: lowered audio priority to prevent hogging of CPU.\n"));
            goto cleanup;
        }
    }
    
killAudio:
    ERR_RPT(("PaHost_WatchDogProc: killing hung audio thread!\n"));
    pthread_kill( pahsc->pahsc_AudioThread, SIGKILL );

cleanup:
    pahsc->pahsc_CanaryRun = 0;
    DBUG(("PaHost_WatchDogProc: cancel Canary\n"));
    pthread_cancel( pahsc->pahsc_CanaryThread );
    DBUG(("PaHost_WatchDogProc: join Canary\n"));
    pthread_join( pahsc->pahsc_CanaryThread, NULL );
    DBUG(("PaHost_WatchDogProc: forget Canary\n"));
    pahsc->pahsc_CanaryThread = INVALID_THREAD;
    
#ifdef GNUSTEP
    GSUnregisterCurrentThread();  /* SB20010904 */
#endif
    return 0;
}

/*******************************************************************************************/
static void PaHost_StopWatchDog( PaHostSoundControl   *pahsc )
{    
/* Cancel WatchDog thread if there is one. */
    if( pahsc->pahsc_WatchDogThread != INVALID_THREAD )
    {
        pahsc->pahsc_WatchDogRun = 0;
        DBUG(("PaHost_StopWatchDog: cancel WatchDog\n"));
        pthread_cancel( pahsc->pahsc_WatchDogThread );
        pthread_join( pahsc->pahsc_WatchDogThread, NULL );
        pahsc->pahsc_WatchDogThread = INVALID_THREAD;
    }
/* Cancel Canary thread if there is one. */
    if( pahsc->pahsc_CanaryThread != INVALID_THREAD )
    {
        pahsc->pahsc_CanaryRun = 0;
        DBUG(("PaHost_StopWatchDog: cancel Canary\n"));
        pthread_cancel( pahsc->pahsc_CanaryThread );
        DBUG(("PaHost_StopWatchDog: join Canary\n"));
        pthread_join( pahsc->pahsc_CanaryThread, NULL );
        pahsc->pahsc_CanaryThread = INVALID_THREAD;
    }
}

/*******************************************************************************************/
static PaError PaHost_StartWatchDog( PaHostSoundControl   *pahsc )
{
    int      hres;
    PaError  result = 0;
    
    /* The watch dog watches for these timer updates */
    gettimeofday( &pahsc->pahsc_EntryTime, NULL );
    gettimeofday( &pahsc->pahsc_CanaryTime, NULL );

    /* Launch a canary thread to detect priority abuse. */
    pahsc->pahsc_CanaryRun = 1;
    hres = pthread_create(&(pahsc->pahsc_CanaryThread),
                      NULL /*pthread_attr_t * attr*/,
                      (void*)PaHost_CanaryProc, pahsc);
    if( hres != 0 )
    {
        pahsc->pahsc_CanaryThread = INVALID_THREAD;
        result = paHostError;
        sPaHostError = hres;
        goto error;
    }

    /* Launch a watchdog thread to prevent runaway audio thread. */
    pahsc->pahsc_WatchDogRun = 1;
    hres = pthread_create(&(pahsc->pahsc_WatchDogThread),
                      NULL /*pthread_attr_t * attr*/,
                      (void*)PaHost_WatchDogProc, pahsc);
    if( hres != 0 )
    {
        pahsc->pahsc_WatchDogThread = INVALID_THREAD;
        result = paHostError;
        sPaHostError = hres;
        goto error;
    }
    return result;
    
error:
    PaHost_StopWatchDog( pahsc );
    return result;
}

/*******************************************************************************************
 * Bump priority of audio thread if running with superuser priveledges.
 * if priority bumped then launch a watchdog.
 */
static PaError PaHost_BoostPriority( internalPortAudioStream *past )
{
    PaHostSoundControl  *pahsc;
    PaError              result = paNoError;
    struct sched_param   schp = { 0 };
    
    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paInternalError;
        
    pahsc->pahsc_AudioThreadPID = getpid();
    DBUG(("PaHost_BoostPriority: audio PID = %d\n", pahsc->pahsc_AudioThreadPID ));
    
    /* Choose a priority in the middle of the range. */
    pahsc->pahsc_AudioPriority = (sched_get_priority_max(SCHEDULER_POLICY) -
                                  sched_get_priority_min(SCHEDULER_POLICY)) / 2;
    schp.sched_priority = pahsc->pahsc_AudioPriority;

    if (sched_setscheduler(0, SCHEDULER_POLICY, &schp) != 0)
    {
        PRINT(("PortAudio: only superuser can use real-time priority.\n"));
    }
    else
    {
        PRINT(("PortAudio: audio callback priority set to level %d!\n", schp.sched_priority));        
        /* We are running at high priority so we should have a watchdog in case audio goes wild. */
        result = PaHost_StartWatchDog( pahsc );
    }
    
    return result;
}

/*******************************************************************************************/
static PaError Pa_AudioThreadProc( internalPortAudioStream   *past )
{
    PaError      result;
    PaHostSoundControl             *pahsc;
    ssize_t      bytes_read, bytes_written;
    count_info   info;
    int          delta;

    pahsc = (PaHostSoundControl *) past->past_DeviceData;
    if( pahsc == NULL ) return paInternalError;
    
#ifdef GNUSTEP
    GSRegisterCurrentThread(); /* SB20010904 */
#endif

    result = PaHost_BoostPriority( past );
    if( result < 0 ) goto error;

    past->past_IsActive = 1;
    DBUG(("entering thread.\n"));

    while( (past->past_StopNow == 0) && (past->past_StopSoon == 0) )
    {
        /* Read data from device */
        if(pahsc->pahsc_NativeInputBuffer)
        {
            DBUG(("Pa_AudioThreadProc: attempt to read %d bytes\n", pahsc->pahsc_BytesPerInputBuffer));
            bytes_read = read(pahsc->pahsc_InputHandle,
                              (void *)pahsc->pahsc_NativeInputBuffer,
                              pahsc->pahsc_BytesPerInputBuffer);
            DBUG(("Pa_AudioThreadProc: bytes_read = %d\n", bytes_read));
            if( bytes_read < pahsc->pahsc_BytesPerInputBuffer )
            {
                ERR_RPT(("PortAudio: read interrupted! Only got %d bytes.\n", bytes_read ));
                break;
            }
        }

        /* Convert 16 bit native data to user data and call user routine. */
        DBUG(("converting...\n"));
        Pa_StartUsageCalculation( past );
        result = Pa_CallConvertInt16( past,
                                      pahsc->pahsc_NativeInputBuffer,
                                      pahsc->pahsc_NativeOutputBuffer );
        Pa_EndUsageCalculation( past );
        if( result != 0)
        {
            DBUG(("hmm, Pa_CallConvertInt16() says: %d. i'm bailing.\n",
                  result));
            break;
        }

        /* Write data to device. */
        if( pahsc->pahsc_NativeOutputBuffer )
        {

            bytes_written = write(pahsc->pahsc_OutputHandle,
                  (void *)pahsc->pahsc_NativeOutputBuffer,
                  pahsc->pahsc_BytesPerOutputBuffer);
            if( bytes_written < pahsc->pahsc_BytesPerOutputBuffer )
            {
                ERR_RPT(("PortAudio: write interrupted! Only got %d bytes.\n", bytes_written ));
                break;
            }
        }

        /* Update current stream time (using a double so that
           we don't wrap around like info.bytes does) */
        if( pahsc->pahsc_NativeOutputBuffer )
           ioctl(pahsc->pahsc_OutputHandle, SNDCTL_DSP_GETOPTR, &info);
        else
           ioctl(pahsc->pahsc_InputHandle, SNDCTL_DSP_GETIPTR, &info);
        delta = (info.bytes - pahsc->pahsc_LastPosPtr) & 0x000FFFFF;
        pahsc->pahsc_LastStreamBytes += delta;
        pahsc->pahsc_LastPosPtr = info.bytes;
    }
    DBUG(("Pa_AudioThreadProc: left audio loop.\n"));
    
    past->past_IsActive = 0;
    PaHost_StopWatchDog( pahsc );
    
error:
    DBUG(("leaving audio thread.\n"));
#ifdef GNUSTEP
    GSUnregisterCurrentThread();  /* SB20010904 */
#endif
    return result;
}

/*******************************************************************************************/
static PaError Pa_SetupDeviceFormat( int devHandle, int numChannels, int sampleRate )
{
    PaError result = paNoError;
    int     tmp;

    /* Set format, channels, and rate in this order to keep OSS happy. */
    /* Set data format. FIXME - handle more native formats. */
    tmp = AFMT_S16_NE;
    if( ioctl(devHandle,SNDCTL_DSP_SETFMT,&tmp) == -1)
    {
        ERR_RPT(("Pa_SetupDeviceFormat: could not SNDCTL_DSP_SETFMT\n" ));
        return paHostError;
    }
    if( tmp != AFMT_S16_NE )
    {
        ERR_RPT(("Pa_SetupDeviceFormat: HW does not support AFMT_S16_NE\n" ));
        return paHostError;
    }


    /* Set number of channels. */
    tmp = numChannels;
    if (ioctl(devHandle, SNDCTL_DSP_CHANNELS, &numChannels) == -1)
    {
        ERR_RPT(("Pa_SetupDeviceFormat: could not SNDCTL_DSP_CHANNELS\n" ));
        return paHostError;
    }
    if( tmp != numChannels)
    {
        ERR_RPT(("Pa_SetupDeviceFormat: HW does not support %d channels\n", numChannels ));
        return paHostError;
    }

    /* Set playing frequency. 44100, 22050 and 11025 are safe bets. */
    tmp = sampleRate;
    if( ioctl(devHandle,SNDCTL_DSP_SPEED,&tmp) == -1)
    {
        ERR_RPT(("Pa_SetupDeviceFormat: could not SNDCTL_DSP_SPEED\n" ));
        return paHostError;
    }
    if( tmp != sampleRate)
    {
        ERR_RPT(("Pa_SetupDeviceFormat: HW does not support %d Hz sample rate\n",sampleRate ));
        return paHostError;
    }

    return result;
}

static int CalcHigherLogTwo( int n )
{
    int log2 = 0;
    while( (1<<log2) < n ) log2++;
    return log2;
}

/*******************************************************************************************
** Set number of fragments and size of fragments to achieve desired latency.
*/
static void Pa_SetLatency( int devHandle, int numBuffers, int framesPerBuffer, int channelsPerFrame  )
{
    int     tmp;
    int     bufferSize, powerOfTwo;

    /* Increase size of buffers and reduce number of buffers to reduce latency inside driver. */
    while( numBuffers > 8 )
    {
        numBuffers = (numBuffers + 1) >> 1;
        framesPerBuffer = framesPerBuffer << 1;
    }

    /* calculate size of buffers in bytes */
    bufferSize = framesPerBuffer * channelsPerFrame * sizeof(short); /* FIXME - other sizes? */

    /* Calculate next largest power of two */
    powerOfTwo = CalcHigherLogTwo( bufferSize );
    DBUG(("Pa_SetLatency: numBuffers = %d, framesPerBuffer = %d, powerOfTwo = %d\n",
          numBuffers, framesPerBuffer, powerOfTwo ));

    /* Encode info into a single int */
    tmp=(numBuffers<<16) + powerOfTwo;

    if(ioctl(devHandle,SNDCTL_DSP_SETFRAGMENT,&tmp) == -1)
    {
        ERR_RPT(("Pa_SetLatency: could not SNDCTL_DSP_SETFRAGMENT\n" ));
        /* Don't return an error. Best to just continue and hope for the best. */
        ERR_RPT(("Pa_SetLatency: numBuffers = %d, framesPerBuffer = %d, powerOfTwo = %d\n",
                 numBuffers, framesPerBuffer, powerOfTwo ));
    }
}

/*************************************************************************
** Determine minimum number of buffers required for this host based
** on minimum latency. Latency can be optionally set by user by setting
** an environment variable. For example, to set latency to 200 msec, put:
**
**    set PA_MIN_LATENCY_MSEC=200
**
** in the cshrc file.
*/
#define PA_LATENCY_ENV_NAME  ("PA_MIN_LATENCY_MSEC")

int Pa_GetMinNumBuffers( int framesPerBuffer, double framesPerSecond )
{
    int minBuffers;
    int minLatencyMsec = MIN_LATENCY_MSEC;
    char *minLatencyText = getenv(PA_LATENCY_ENV_NAME);
    if( minLatencyText != NULL )
    {
        PRINT(("PA_MIN_LATENCY_MSEC = %s\n", minLatencyText ));
        minLatencyMsec = atoi( minLatencyText );
        if( minLatencyMsec < 1 ) minLatencyMsec = 1;
        else if( minLatencyMsec > 5000 ) minLatencyMsec = 5000;
    }

    minBuffers = (int) ((minLatencyMsec * framesPerSecond) / ( 1000.0 * framesPerBuffer ));
    if( minBuffers < 2 ) minBuffers = 2;
    return minBuffers;
}

/*******************************************************************/
PaError PaHost_OpenStream( internalPortAudioStream   *past )
{
    PaError          result = paNoError;
    PaHostSoundControl *pahsc;
    unsigned int     minNumBuffers;
    internalPortAudioDevice *pad;
    DBUG(("PaHost_OpenStream() called.\n" ));

    /* Allocate and initialize host data. */
    pahsc = (PaHostSoundControl *) malloc(sizeof(PaHostSoundControl));
    if( pahsc == NULL )
    {
        result = paInsufficientMemory;
        goto error;
    }
    memset( pahsc, 0, sizeof(PaHostSoundControl) );
    past->past_DeviceData = (void *) pahsc;

    pahsc->pahsc_OutputHandle = BAD_DEVICE_ID; /* No device currently opened. */
    pahsc->pahsc_InputHandle = BAD_DEVICE_ID;
    pahsc->pahsc_AudioThread = INVALID_THREAD;
    pahsc->pahsc_WatchDogThread = INVALID_THREAD;

    /* Allocate native buffers. */
    pahsc->pahsc_BytesPerInputBuffer = past->past_FramesPerUserBuffer *
                                       past->past_NumInputChannels * sizeof(short);
    if( past->past_NumInputChannels > 0)
    {
        pahsc->pahsc_NativeInputBuffer = (short *) malloc(pahsc->pahsc_BytesPerInputBuffer);
        if( pahsc->pahsc_NativeInputBuffer == NULL )
        {
            result = paInsufficientMemory;
            goto error;
        }
    }
    pahsc->pahsc_BytesPerOutputBuffer = past->past_FramesPerUserBuffer *
                                        past->past_NumOutputChannels * sizeof(short);
    if( past->past_NumOutputChannels > 0)
    {
        pahsc->pahsc_NativeOutputBuffer = (short *) malloc(pahsc->pahsc_BytesPerOutputBuffer);
        if( pahsc->pahsc_NativeOutputBuffer == NULL )
        {
            result = paInsufficientMemory;
            goto error;
        }
    }

    /* DBUG(("PaHost_OpenStream: pahsc_MinFramesPerHostBuffer = %d\n", pahsc->pahsc_MinFramesPerHostBuffer )); */
    minNumBuffers = Pa_GetMinNumBuffers( past->past_FramesPerUserBuffer, past->past_SampleRate );
    past->past_NumUserBuffers = ( minNumBuffers > past->past_NumUserBuffers ) ? minNumBuffers : past->past_NumUserBuffers;

    pahsc->pahsc_InverseMicrosPerBuffer = past->past_SampleRate / (1000000.0 * past->past_FramesPerUserBuffer);
    DBUG(("pahsc_InverseMicrosPerBuffer = %g\n", pahsc->pahsc_InverseMicrosPerBuffer ));

    /* ------------------------- OPEN DEVICE -----------------------*/

    /* just output */
    if (past->past_OutputDeviceID == past->past_InputDeviceID)
    {

        if ((past->past_NumOutputChannels > 0) && (past->past_NumInputChannels > 0) )
        {
            pad = Pa_GetInternalDevice( past->past_OutputDeviceID );
            DBUG(("PaHost_OpenStream: attempt to open %s for O_RDWR\n", pad->pad_DeviceName ));

            /* dmazzoni: test it first in nonblocking mode to
               make sure the device is not busy */
            pahsc->pahsc_InputHandle = open(pad->pad_DeviceName,O_RDWR|O_NONBLOCK);
            if(pahsc->pahsc_InputHandle==-1)
            {
                ERR_RPT(("PaHost_OpenStream: could not open %s for O_RDWR\n", pad->pad_DeviceName ));
                result = paHostError;
                goto error;
            }
            close(pahsc->pahsc_InputHandle);

            pahsc->pahsc_OutputHandle = pahsc->pahsc_InputHandle =
                                            open(pad->pad_DeviceName,O_RDWR);
            if(pahsc->pahsc_InputHandle==-1)
            {
                ERR_RPT(("PaHost_OpenStream: could not open %s for O_RDWR\n", pad->pad_DeviceName ));
                result = paHostError;
                goto error;
            }
            Pa_SetLatency( pahsc->pahsc_OutputHandle,
                           past->past_NumUserBuffers, past->past_FramesPerUserBuffer,
                           past->past_NumOutputChannels );
            result = Pa_SetupDeviceFormat( pahsc->pahsc_OutputHandle,
                                           past->past_NumOutputChannels, (int)past->past_SampleRate );
        }
    }
    else
    {
        if (past->past_NumOutputChannels > 0)
        {
            pad = Pa_GetInternalDevice( past->past_OutputDeviceID );
            DBUG(("PaHost_OpenStream: attempt to open %s for O_WRONLY\n", pad->pad_DeviceName ));
            /* dmazzoni: test it first in nonblocking mode to
               make sure the device is not busy */
            pahsc->pahsc_OutputHandle = open(pad->pad_DeviceName,O_WRONLY|O_NONBLOCK);
            if(pahsc->pahsc_OutputHandle==-1)
            {
                ERR_RPT(("PaHost_OpenStream: could not open %s for O_WRONLY\n", pad->pad_DeviceName ));
                result = paHostError;
                goto error;
            }
            close(pahsc->pahsc_OutputHandle);

            pahsc->pahsc_OutputHandle = open(pad->pad_DeviceName,O_WRONLY);
            if(pahsc->pahsc_OutputHandle==-1)
            {
                ERR_RPT(("PaHost_OpenStream: could not open %s for O_WRONLY\n", pad->pad_DeviceName ));
                result = paHostError;
                goto error;
            }
            Pa_SetLatency( pahsc->pahsc_OutputHandle,
                           past->past_NumUserBuffers, past->past_FramesPerUserBuffer,
                           past->past_NumOutputChannels );
            result = Pa_SetupDeviceFormat( pahsc->pahsc_OutputHandle,
                                           past->past_NumOutputChannels, (int)past->past_SampleRate );
        }

        if (past->past_NumInputChannels > 0)
        {
            pad = Pa_GetInternalDevice( past->past_InputDeviceID );
            DBUG(("PaHost_OpenStream: attempt to open %s for O_RDONLY\n", pad->pad_DeviceName ));
            /* dmazzoni: test it first in nonblocking mode to
               make sure the device is not busy */ 
            pahsc->pahsc_InputHandle = open(pad->pad_DeviceName,O_RDONLY|O_NONBLOCK);
            if(pahsc->pahsc_InputHandle==-1)
            {
                ERR_RPT(("PaHost_OpenStream: could not open %s for O_RDONLY\n", pad->pad_DeviceName ));
                result = paHostError;
                goto error;
            }
            close(pahsc->pahsc_InputHandle);

            pahsc->pahsc_InputHandle = open(pad->pad_DeviceName,O_RDONLY);
            if(pahsc->pahsc_InputHandle==-1)
            {
                ERR_RPT(("PaHost_OpenStream: could not open %s for O_RDONLY\n", pad->pad_DeviceName ));
                result = paHostError;
                goto error;
            }
            Pa_SetLatency( pahsc->pahsc_InputHandle, /* DH20010115 - was OutputHandle! */
                           past->past_NumUserBuffers, past->past_FramesPerUserBuffer,
                           past->past_NumInputChannels );
            result = Pa_SetupDeviceFormat( pahsc->pahsc_InputHandle,
                                           past->past_NumInputChannels, (int)past->past_SampleRate );
        }
    }


    DBUG(("PaHost_OpenStream: SUCCESS - result = %d\n", result ));
    return result;

error:
    ERR_RPT(("PaHost_OpenStream: ERROR - result = %d\n", result ));
    PaHost_CloseStream( past );
    return result;
}

/*************************************************************************/
PaError PaHost_StartOutput( internalPortAudioStream *past )
{
    return paNoError;
}

/*************************************************************************/
PaError PaHost_StartInput( internalPortAudioStream *past )
{
    return paNoError;
}

/*************************************************************************/
PaError PaHost_StartEngine( internalPortAudioStream *past )
{
    PaHostSoundControl *pahsc;
    PaError             result = paNoError;
    int                 hres;

    pahsc = (PaHostSoundControl *) past->past_DeviceData;

    past->past_StopSoon = 0;
    past->past_StopNow = 0;
    past->past_IsActive = 1;

    /* Use pthread_create() instead of __clone() because:
     *   - pthread_create also works for other UNIX systems like Solaris,
     *   - the Java HotSpot VM crashes in pthread_setcanceltype() when using __clone()
     */
    hres = pthread_create(&(pahsc->pahsc_AudioThread),
                          NULL /*pthread_attr_t * attr*/,
                          (void*)Pa_AudioThreadProc, past);
    if( hres != 0 )
    {
        result = paHostError;
        sPaHostError = hres;
        pahsc->pahsc_AudioThread = INVALID_THREAD;
        goto error;
    }

error:
    return result;
}

/*************************************************************************/
PaError PaHost_StopEngine( internalPortAudioStream *past, int abort )
{
    int                 hres;
    PaError             result = paNoError;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;

    if( pahsc == NULL ) return paNoError;

    /* Tell background thread to stop generating more data and to let current data play out. */
    past->past_StopSoon = 1;
    /* If aborting, tell background thread to stop NOW! */
    if( abort ) past->past_StopNow = 1;

    /* Join thread to recover memory resources. */
    if( pahsc->pahsc_AudioThread != INVALID_THREAD )
    {
        /* This check is needed for GNUSTEP - SB20010904 */
        if ( !pthread_equal( pahsc->pahsc_AudioThread, pthread_self() ) )
        {
            hres = pthread_join( pahsc->pahsc_AudioThread, NULL );
        }
        else
        {
            DBUG(("Play thread was stopped from itself - can't do pthread_join()\n"));
            hres = 0;
        }

        if( hres != 0 )
        {
            result = paHostError;
            sPaHostError = hres;
        }
        pahsc->pahsc_AudioThread = INVALID_THREAD;
    }

    past->past_IsActive = 0;

    return result;
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

    if( pahsc->pahsc_OutputHandle != BAD_DEVICE_ID )
    {
        int err;
        DBUG(("PaHost_CloseStream: attempt to close output device handle = %d\n",
              pahsc->pahsc_OutputHandle ));
        err = close(pahsc->pahsc_OutputHandle);
        if( err < 0 )
        {
            ERR_RPT(("PaHost_CloseStream: warning, closing output device failed.\n"));
        }
    }

    if( (pahsc->pahsc_InputHandle != BAD_DEVICE_ID) &&
            (pahsc->pahsc_InputHandle != pahsc->pahsc_OutputHandle) )
    {
        int err;
        DBUG(("PaHost_CloseStream: attempt to close input device handle = %d\n",
              pahsc->pahsc_InputHandle ));
        err = close(pahsc->pahsc_InputHandle);
        if( err < 0 )
        {
            ERR_RPT(("PaHost_CloseStream: warning, closing input device failed.\n"));
        }
    }
    pahsc->pahsc_OutputHandle = BAD_DEVICE_ID;
    pahsc->pahsc_InputHandle = BAD_DEVICE_ID;

    if( pahsc->pahsc_NativeInputBuffer )
    {
        free( pahsc->pahsc_NativeInputBuffer );
        pahsc->pahsc_NativeInputBuffer = NULL;
    }
    if( pahsc->pahsc_NativeOutputBuffer )
    {
        free( pahsc->pahsc_NativeOutputBuffer );
        pahsc->pahsc_NativeOutputBuffer = NULL;
    }

    free( pahsc );
    past->past_DeviceData = NULL;
    return paNoError;
}

/*************************************************************************/
PaError PaHost_Term( void )
{
    /* Free all of the linked devices. */
    internalPortAudioDevice *pad, *nextPad;
    pad = sDeviceList;
    while( pad != NULL )
    {
        nextPad = pad->pad_Next;
        DBUG(("PaHost_Term: freeing %s\n", pad->pad_DeviceName ));
        PaHost_FreeFastMemory( pad, sizeof(internalPortAudioDevice) );
        pad = nextPad;
    }
    sDeviceList = NULL;
    return 0;
}

/*************************************************************************
 * Sleep for the requested number of milliseconds.
 */
void Pa_Sleep( long msec )
{
#if 0
    struct timeval timeout;
    timeout.tv_sec = msec / 1000;
    timeout.tv_usec = (msec % 1000) * 1000;
    select( 0, NULL, NULL, NULL, &timeout );
#else
    long usecs = msec * 1000;
    usleep( usecs );
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
    void *addr = malloc( numBytes ); /* FIXME - do we need physical, wired, non-virtual memory? */
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
    return (PaError) (past->past_IsActive != 0);
}

/***********************************************************************/
PaTimestamp Pa_StreamTime( PortAudioStream *stream )
{
    internalPortAudioStream *past = (internalPortAudioStream *) stream;
    PaHostSoundControl *pahsc = (PaHostSoundControl *) past->past_DeviceData;
    count_info    info;
    int           delta;

    if( past == NULL ) return paBadStreamPtr;

    if( pahsc->pahsc_NativeOutputBuffer ) {
       ioctl(pahsc->pahsc_OutputHandle, SNDCTL_DSP_GETOPTR, &info);
       delta = (info.bytes - pahsc->pahsc_LastPosPtr) & 0x000FFFFF;
       return (pahsc->pahsc_LastStreamBytes + delta) / (past->past_NumOutputChannels * sizeof(short));
    }
    else {
       ioctl(pahsc->pahsc_InputHandle, SNDCTL_DSP_GETIPTR, &info);
       delta = (info.bytes - pahsc->pahsc_LastPosPtr) & 0x000FFFFF;
       return (pahsc->pahsc_LastStreamBytes + delta) / (past->past_NumInputChannels * sizeof(short));

    }
}

/***********************************************************************/
long Pa_GetHostError( void )
{
    return (long) sPaHostError;
}
