#ifndef PORTAUDIO_H
#define PORTAUDIO_H

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library
 * PortAudio API Header File
 * Latest version available at: http://www.portaudio.com/
 *
 * Copyright (c) 1999-2002 Ross Bencina and Phil Burk
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

/** Error codes returned by PortAudio functions. */

typedef int PaError;
typedef enum PaErrorNum
{
    paNoError = 0,

    paNotInitialized = -10000,
    paHostError,
    paInvalidChannelCount,
    paInvalidSampleRate,
    paInvalidDevice,
    paInvalidFlag,
    paSampleFormatNotSupported,
    paBadIODeviceCombination,
    paInsufficientMemory,
    paBufferTooBig,
    paBufferTooSmall,
    paNullCallback,
    paBadStreamPtr,
    paTimedOut,
    paInternalError,
    paDeviceUnavailable,
    paIncompatibleStreamInfo,
    paStreamIsStopped,
    paStreamIsNotStopped
} PaErrorNum;


/** Library initialization function - call this before using the PortAudio.
*/
PaError Pa_Initialize( void );


/** Library termination function - call this after using the PortAudio.
*/
PaError Pa_Terminate( void );


/** Retrieve a host specific error code.
 Can be called after receiving a PortAudio error number of paHostError.
*/
long Pa_GetHostError( void );


/** Translate the supplied PortAudio error number into a human readable
 message.
*/
const char *Pa_GetErrorText( PaError errnum );


/** The type used to refer to audio devices. Values of this type usually
 range from 0 to (Pa_DeviceCount-1), and may also take on the PaNoDevice
 and paUseHostApiSpecificDeviceSpecification values.

 @see Pa_DeviceCount, paNoDevice, paUseHostApiSpecificDeviceSpecification
*/
typedef int PaDeviceIndex;


/** A special PaDeviceIndex value indicating that no device is available,
 or should be used.

 @see PaDeviceIndex
*/
#define paNoDevice (-1)


/** A special PaDeviceIndex value indicating that the device(s) to be used
 are specified in the host api specific stream info structure.

 @see PaDeviceIndex
*/
#define paUseHostApiSpecificDeviceSpecification (-2)


/* Host API enumeration mechanism */

/** The type used to enumerate to host APIs at runtime. Values of this type
 range from 0 to (Pa_CountHostApis()-1).

 @see Pa_CountHostApis
*/
typedef int PaHostApiIndex;


/** Unchanging unique identifiers for each supported host API. This type
    is used in the PaHostApiInfo structure. The values are guaranteed to be
    unique and to never change, thus allowing code to be written that
    conditionally uses host API specific extensions.

    New type ids will be allocated when support for a host API reaches
    "public alpha" status, prior to that developers should use the
    paInDevelopment type id.

    @see PaHostApiInfo
*/
typedef enum PaHostApiTypeId
{
    paInDevelopment=0, /* use while developing support for a new host API */
    paDirectSound=1,
    paMME=2,
    paASIO=3,
    paSoundManager=4,
    paCoreAudio=5,
    paOSS=7,
    paALSA=8,
    paAL=9,
    paBeOS=10
} PaHostApiTypeId;


/** Convert a static host API unique identifier, into a runtime
 host API index.

 @param type A unique host API identifier belonging to the PaHostApiTypeId
 enumeration.

 @return A valid PaHostApiIndex ranging from 0 to (Pa_CountHostApis()-1), or
 -1 if the host API specified by the type parameter is not available.
 
 @see PaHostApiTypeId
*/
PaHostApiIndex Pa_HostApiTypeIdToHostApiIndex( PaHostApiTypeId type );


/** Retrieve the number of available host APIs. Even if a host API is
 available it may have no devices available.

 @return The number of available host APIs. May return 0 if PortAudio is
 not initialized or an error has occured.

 @see PaHostApiIndex
*/
PaHostApiIndex Pa_CountHostApis( void );


/** Retrieve the index of the defualt hostAPI. The default host API will be
 the lowest common denominator host API on the current platform and is
 unlikely to provide the best performance.

 @return The default host API index.
*/
PaHostApiIndex Pa_GetDefaultHostApi( void );


/** A structure containing information about a particular host API. */

typedef struct PaHostApiInfo
{
    int structVersion;
    /** The well known unique identifier of this host API @see PaHostApiTypeId */
    PaHostApiTypeId type;
    /** a textual description of the host API for display on user interfaces */
    const char *name;
} PaHostApiInfo;


/** Retrieve a pointer to a structure containing information about a specific
 host Api.

 @param hostApi A valid host API index ranging from 0 to (Pa_CountHostApis()-1)

 @return A pointer to an immutable PaHostApiInfo structure describing
 a specific host API. If the hostApi parameter is out of range or an error
 is encountered, the function returns NULL.

 The returned structure is owned by the PortAudio implementation and must not
 be manipulated or freed. The pointer is only guaranteed to be valid between
 calls to Pa_Initialize() and Pa_Terminate().
*/
const PaHostApiInfo * Pa_GetHostApiInfo( PaHostApiIndex hostApi );


/** Retrieve the default input device for the specified host API

 @param hostApi A valid host API index ranging from 0 to (Pa_CountHostApis()-1)

 @return A device index ranging from 0 to (Pa_CountDevices()-1), or paNoDevice
 if there is no default input device available for the specified host API.
*/
PaDeviceIndex Pa_HostApiDefaultInputDevice( PaHostApiIndex hostApi );


/** Retrieve the default output device for the specified host API

 @param hostApi A valid host API index ranging from 0 to (Pa_CountHostApis()-1)

 @return A device index ranging from 0 to (Pa_CountDevices()-1), or paNoDevice
 if there is no default output device available for the specified host API.
*/
PaDeviceIndex Pa_HostApiDefaultOutputDevice( PaHostApiIndex hostApi );


/** Retrieve the number of devices belonging to a specific host API.
 This function may be used in conjunction with Pa_HostApiDeviceIndexToDeviceIndex()
 to enumerate all devices for a specific host API.

 @param hostApi A valid host API index ranging from 0 to (Pa_CountHostApis()-1)

 @return The number of devices belonging to the specified host API.

 @see Pa_HostApiDeviceIndexToDeviceIndex
*/
int Pa_HostApiCountDevices( PaHostApiIndex hostApi );


/** Convert a host-API-specific device index to standard PortAudio device index.
 This function may be used in conjunction with Pa_HostApiCountDevices() to
 enumerate all devices for a specific host API.

 @param hostApi A valid host API index ranging from 0 to (Pa_CountHostApis()-1)

 @param hostApiDeviceIndex A valid per-host device index in the range
 0 to (Pa_HostApiCountDevices(hostApi)-1)

 @see Pa_HostApiCountDevices
*/
PaDeviceIndex Pa_HostApiDeviceIndexToDeviceIndex( PaHostApiIndex hostApi,
        int hostApiDeviceIndex );




/* Device enumeration and capabilities */

/** Retrieve the number of available devices.
 @return The number of available devices. May return 0 if PortAudio is
 not initialized or an error has occured.
*/
PaDeviceIndex Pa_CountDevices( void );


/** Retrieve the index of the default input device. The result can be
 used in the inputDevice parameter to Pa_OpenStream().

 @return The default input device index for the defualt host API, or paNoDevice
 if not input device is available.
*/
PaDeviceIndex Pa_GetDefaultInputDevice( void );


/** Retrieve the index of the default output device. The result can be
 used in the outputDevice parameter to Pa_OpenStream().

 @return The default output device index for the defualt host API, or paNoDevice
 if not output device is available.

 @note
 On the PC, the user can specify a default device by
 setting an environment variable. For example, to use device #1.
<pre>
 set PA_RECOMMENDED_OUTPUT_DEVICE=1
</pre>
 The user should first determine the available device ids by using
 the supplied application "pa_devs".
*/
PaDeviceIndex Pa_GetDefaultOutputDevice( void );


/** A type used to specify one or more sample formats. They indicate
 the formats used to pass sound data between the callback and the
 stream. Each device has one or more "native" formats which may be used when
 optimum efficiency or control over conversion is required.

 Formats marked "always available" are supported (emulated) by all
 PortAudio implementations.

 The floating point representation (paFloat32) uses +1.0 and -1.0 as the
 maximum and minimum respectively.

 paUInt8 is an unsigned 8 bit format where 128 is considered "ground"

 The paNonInterleaved flag indicates that a multichannel buffer is passed
 as a set of non-interleaved pointers.

 @see Pa_OpenStream, Pa_OpenDefaultStream, PaDeviceInfo
 @see paFloat32, paInt16, paInt32, paInt24, paInt8
 @see paUInt8, paCustomFormat, paNonInterleaved
*/
typedef unsigned long PaSampleFormat;


#define paFloat32      ((PaSampleFormat) (1<<0)) /** @see PaSampleFormat */
#define paInt32        ((PaSampleFormat) (1<<1)) /** @see PaSampleFormat */
#define paInt24        ((PaSampleFormat) (1<<2)) /** Packed 24 bit format. @see PaSampleFormat */
#define paInt16        ((PaSampleFormat) (1<<3)) /** @see PaSampleFormat */
#define paInt8         ((PaSampleFormat) (1<<4)) /** @see PaSampleFormat */
#define paUInt8        ((PaSampleFormat) (1<<5)) /** @see PaSampleFormat */
#define paCustomFormat ((PaSampleFormat) (1<<16))/** @see PaSampleFormat */

#define paNonInterleaved ((PaSampleFormat) (1<<31))

/** A structure providing information and capabilities of PortAudio devices.
 Devices may support input, output or both input and output.
*/
typedef struct PaDeviceInfo
{
    int structVersion;  /* this is struct version 2 */
    const char *name;
    PaHostApiIndex hostApi; /* note this is a host API index, not a type id*/
    int maxInputChannels;
    int maxOutputChannels;

    /* THE FOLLOWING FIELDS WILL BE REMOVED in favour of Pa_IsFormatSupported() */

    /* Number of discrete rates, or -1 if range supported. */
    int numSampleRates;
    /* Array of supported sample rates, or {min,max} if range supported. */
    const double *sampleRates;
    PaSampleFormat nativeSampleFormats;
} PaDeviceInfo;


/** Retrieve a pointer to a PaDeviceInfo structure containing information
 about the specified device.
 @return A pointer to an immutable PaDeviceInfo structure. If the device
 parameter is out of range the function returns NULL.

 @param device A valid device index in the range 0 to (Pa_CountDevices()-1)

 @note PortAudio manages the memory referenced by the returned pointer,
 the client must not manipulate or free the memory. The pointer is only
 guaranteed to be valid between calls to Pa_Initialize() and Pa_Terminate().

 @see PaDeviceInfo, PaDeviceIndex
*/
const PaDeviceInfo* Pa_GetDeviceInfo( PaDeviceIndex device );


/** The type used to represent monotonic time in seconds that can be used
 for syncronisation. The type is used for the outTime argument to the
 PaStreamCallback and as the result of Pa_GetStreamTime().
     
 @see PaStreamCallback, Pa_GetStreamTime
*/
typedef double PaTime;


/** REVIEW: perhaps defaultLowInputLatency etc should be
    fields in PaDeviceInfo */
    
/** Return the recommended default input latency for interactive performance.
 The latency is expressed in seconds and can be used in the suggestedLatency
 field of the PaStreamParameters structure.
*/
PaTime Pa_GetDefaultLowInputLatency( PaDeviceIndex deviceID );


/** Return the recommended default input latency for playing soundfiles and
 similar tasks. The latency is expressed in seconds and can be used in the
 suggestedLatency field of the PaStreamParameters structure.
*/
PaTime Pa_GetDefaultHighInputLatency( PaDeviceIndex deviceID );


/** Return the recommended default output latency for interactive performance.
 The latency is expressed in seconds and can be used in the suggestedLatency
 field of the PaStreamParameters structure.
*/
PaTime Pa_GetDefaultLowOutputLatency( PaDeviceIndex deviceID );


/** Return the recommended default input latency for playing soundfiles and
 similar tasks. The latency is expressed in seconds and can be used in the
 suggestedLatency field of the PaStreamParameters structure.
*/
PaTime Pa_GetDefaultHighOutputLatency( PaDeviceIndex deviceID );



#define paNullHostApiSpecificStreamInfo ((void*)0)

/** Parameters for one direction (input or output) of a stream.
*/
typedef struct PaStreamParameters
{
    /** A valid device index in the range 0 to (Pa_CountDevices()-1)
     specifying the device to be used or the special constant
     paUseHostApiSpecificDeviceSpecification which indicates that the actual
     device(s) to use are specified in hostApiSpecificStreamInfo.
     This field must not be set to paNoDevice.
    */
    PaDeviceIndex device;
    
    /** The number of channels of sound to be delivered to the
     stream callback or accessed by Pa_ReadStream() or Pa_WriteStream().
     It can range from 1 to the value of maxInputChannels in the
     PaDeviceInfo record for the device specified by the device parameter.
    */
    int numChannels;

    /** The sample format of the buffer provided to the stream callback,
     a_ReadStream() or Pa_WriteStream(). It may be any of the formats described
     by the PaSampleFormat enumeration.
     FIXME: wrt below, what are we guaranteeing these days, if anything?
     PortAudio guarantees support for
     the device's native formats (nativeSampleFormats in the device info record)
     and additionally 16 and 32 bit integer and 32 bit floating point formats.
     Support for other formats is implementation defined.
    */
    PaSampleFormat sampleFormat;

    /** The desired latency in seconds. Where practical PortAudio implementations
     will select configure internal buffer sizes based on this parameters,
     otherwise they may choose the closest viable buffer size and latency instead.
     In such cases the PortAudio implementations will round-up
     (ie always provide an equal or higher latency than requested.)
     Actual latency values for an open stream may be retrieved using
     Pa_GetStreamInputLatency and Pa_GetStreamInputLatency
     @see Pa_GetDefaultLowInputLatency, Pa_GetDefaultHighInputLatency,
     Pa_GetDefaultLowOutputLatency, Pa_GetDefaultHighOutputLatency
    */
    PaTime suggestedLatency;

    /** An optional pointer to a host api specific data structure
     containing additional information for device setup and/or stream processing.
     hostApiSpecificStreamInfo is never required for correct operation.
     If not used it should be set to paNullHostApiSpecificStreamInfo (aka NULL)
     FIXME: redocument this based on new changes:
     If hostApiSpecificStreamInfo is supplied, it's
     size and hostApi fields must be compatible with the input devices host api.
    */
    void *hostApiSpecificStreamInfo;

} PaStreamParameters;



// Pa_IsFormatSupported goes here


/* Streaming types and functions */


/**
 A single PaStream can provide multiple channels of real-time
 streaming audio input and output to a client application.
 Pointers to PaStream objects are passed between PortAudio functions that
 operate on streams.

 @see Pa_OpenStream, Pa_OpenDefaultStream, Pa_OpenDefaultStream, Pa_CloseStream,
 Pa_StartStream, Pa_StopStream, Pa_AbortStream, Pa_IsStreamActive,
 Pa_GetStreamTime, Pa_GetStreamCpuLoad

*/
typedef void PaStream;


/** For backwards compatibility only. TODO: should be removed before V19-final*/
#define PortAudioStream PaStream;


/** Can be passed as the framesPerBuffer parameter to Pa_OpenStream()
 or Pa_OpenDefaultStream() to indicate that the stream callback will
 accept buffers of any size.
*/
#define paFramesPerBufferUnspecified  (0)


/** Flags used to control the behavior of a stream. They are passed as
 parameters to Pa_OpenStream or Pa_OpenDefaultStream. Multiple flags may be
 ORed together.

 @see Pa_OpenStream, Pa_OpenDefaultStream
 @see paNoFlag, paClipOff, paDitherOff, paPlatformSpecificFlags
*/
typedef unsigned long PaStreamFlags;

#define   paNoFlag      (0)      /**< @see PaStreamFlags */
#define   paClipOff     (1<<0)   /**< Disable default clipping of out of range samples. @see PaStreamFlags */
#define   paDitherOff   (1<<1)   /**< Disable default dithering. @see PaStreamFlags */
#define   paNeverDropInput (1<<2)/**< A full duplex stream will not discard overflowed input samples without calling the callback */


#define   paPlatformSpecificFlags (0xFFFF0000) /** A mask specifying the platform specific bits. @see PaStreamFlags */


/**
 Timing information for the buffers passed to the stream callback.
*/
typedef struct PaStreamCallbackTimeInfo{
    PaTime inputBufferAdcTime;
    PaTime currentTime;
    PaTime outputBufferDacTime;
} PaStreamCallbackTimeInfo;


/**
 Flag bit constants for the statusFlags to PaStreamCallback.
*/
#define paInputUnderflow   (1<<0) /**< Input data is all zeros because no real data is available. */
#define paInputOverflow    (1<<1) /**< Input data was discarded by PortAudio */
#define paOutputUnderflow  (1<<2) /**< Output data was inserted by PortAudio because the callback is using too much CPU */
#define paOutputOverflow   (1<<3) /**< Output data will be discarded because no room is available. */


/**
 Allowable return values for the PaStreamCallback.
*/
typedef enum PaStreamCallbackResult
{
    paContinue=0,
    paComplete=1,
    paAbort=2
} PaStreamCallbackResult;


/**
 Functions of type PaStreamCallback are implemented by PortAudio clients.
 They consume, process or generate audio in response to requests from an
 active PortAudio stream.
     
 @param input and @param output are arrays of interleaved samples,
 the format, packing and number of channels used by the buffers are
 determined by parameters to Pa_OpenStream().
     
 @param frameCount The number of sample frames to be processed by
 the callback.

 @param timeInfo The time in seconds when the first sample of the input
 buffer was received at the audio input, the time in seconds when the first
 sample of the output buffer will begin being played at the audio output, and
 the time in seconds when the callback was called. See also Pa_GetStreamTime()

 @param statusFlags Flags indicating whether input and/or output buffers
 have been inserted or will be dropped to overcome underflow or overflow
 conditions.

 @param userData The value of a user supplied pointer passed to
 Pa_OpenStream() intended for storing synthesis data etc.

 @return
 The callback should return one of the values in the PaStreamCallbackResult
 enumeration. To ensure that the callback is continues to be called, it
 should return paContinue (0). Either paComplete or paAbort can be returned
 to stop the stream, after either of these values is returned with callback will
 not be called again. If paAbort is returned the stream will stop as soon
 as possible. If paComplete is returned, the stream will continue until all
 buffers generated by the callback have been played. This may be useful in
 applications such as soundfile players where a specific duration of output
 is required. However, it is not necessary to utilise this mechanism
 as Pa_StopStream(), Pa_AbortStream() or Pa_CloseStream() can also be used to
 terminate the stream. The callback must always fill the entire output buffer
 irrespective of its return value.

 @see Pa_OpenStream, Pa_OpenDefaultStream

 @note With the exception of Pa_StreamCPULoad() it is not permissable to call
 PortAudio API functions from within the callback.
*/
typedef PaStreamCallbackResult PaStreamCallback(
    void *input, void *output,
    unsigned long frameCount,
    const PaStreamCallbackTimeInfo* timeInfo,
    unsigned long statusFlags,
    void *userData );


/** Opens a stream for either input, output or both.
     
 @param stream The address of a PaStream pointer which will receive
 a pointer to the newly opened stream.
     
 @param inputParameters A structure that describes the input parameters used by
 the opened stream. See PaStreamParameters for a description of these parameters.
 inputParameters must be NULL for output-only streams.

 @param outputParameters A structure that describes the output parameters used by
 the opened stream. See PaStreamParameters for a description of these parameters.
 outputParameters must be NULL for input-only streams.
 
 @param sampleRate The desired sampleRate. For full-duplex streams it is the
 sample rate for both input and output
     
 @param framesPerBuffer The number of frames passed to the callback function,
 or the preferred block granularity for a blocking read/write stream. The
 special value paFramesPerBufferUnspecified (0) may be used to request that
 the callback will recieve an optimal (and possibly varying) number of frames
 based on host requirements and the requested latency settings.
 Note: With some host APIs, the use of non-zero framesPerBuffer for a callback
 stream may introduce an additional layer of buffering which could introduce
 additional latency. PortAudio guarantees that the additional latency
 will be kept to the theoretical minimum however, it is strongly recommended
 that a non-zero framesPerBuffer value only be used when your algorithm
 requires a fixed number of frames per callback.
 
 @param streamFlags Flags which modify the behaviour of the streaming process.
 This parameter may contain a combination of flags ORed together. Some flags may
 only be relevant to certain buffer formats.
     
 @param callback A pointer to a client supplied function that is responsible
 for processing and filling input and output buffers.
     
 @param userData A client supplied pointer which is passed to the callback
 function. It could for example, contain a pointer to instance data necessary
 for processing the audio buffers.
     
 @return
 Upon success Pa_OpenStream() returns paNoError and places a pointer to a
 valid PaStream in the stream argument. The stream is inactive (stopped).
 If a call to Pa_OpenStream() fails, a non-zero error code is returned (see
 PaError for possible error codes) and the value of stream is invalid.

 @see PaStreamParameters, PaStreamCallback
*/
PaError Pa_OpenStream( PaStream** stream,
                       const PaStreamParameters *inputParameters,
                       const PaStreamParameters *outputParameters,
                       double sampleRate,
                       unsigned long framesPerBuffer,
                       PaStreamFlags streamFlags,
                       PaStreamCallback *callback,
                       void *userData );


/** A simplified version of Pa_OpenStream() that opens the default input
 and/or output devices.

 @param stream The address of a PaStream pointer which will receive
 a pointer to the newly opened stream.
 
 @param numInputChannels  The number of channels of sound that will be supplied
 to the stream callback or returned by Pa_ReadStream. It can range from 1 to
 the value of maxInputChannels in the PaDeviceInfo record for the default input
 device. If 0 the stream is opened as an output-only stream.

 @param numOutputChannels The number of channels of sound to be delivered to the
 stream callback or passed to Pa_WriteStream. It can range from 1 to the value
 of maxOutputChannels in the PaDeviceInfo record for the default output dvice.
 If 0 the stream is opened as an output-only stream.

 @param sampleFormat The sample format of both the input and output buffers
 provided to the callback or passed to and from Pa_ReadStream and Pa_WriteStream.
 sampleFormat may be any of the formats described by the PaSampleFormat enumeration
 (see above).
 FIXME: the following may need to be rewritten - PortAudio guarantees support for
 the device's native formats (nativeSampleFormats in the device info record)
 and additionally 16 and 32 bit integer and 32 bit float
 
 @param sampleRate Same as Pa_OpenStream parameter of the same name.
 @param framesPerBuffer Same as Pa_OpenStream parameter of the same name.
 @param callback Same as Pa_OpenStream parameter of the same name.
 @param userData Same as Pa_OpenStream parameter of the same name.

 @return As for Pa_OpenStream

 @see Pa_OpenStream, PaStreamCallback
*/
PaError Pa_OpenDefaultStream( PaStream** stream,
                              int numInputChannels,
                              int numOutputChannels,
                              PaSampleFormat sampleFormat,
                              double sampleRate,
                              unsigned long framesPerBuffer,
                              PaStreamCallback *callback,
                              void *userData );


/** Closes an audio stream. If the audio stream is active it
 discards any pending buffers as if Pa_AbortStream() had been called.
*/
PaError Pa_CloseStream( PaStream *stream );


/** Commences audio processing.
*/
PaError Pa_StartStream( PaStream *stream );


/** Terminates audio processing. It waits until all pending
 audio buffers have been played before it returns.
*/
PaError Pa_StopStream( PaStream *stream );


/** Terminates audio processing immediately without waiting for pending
 buffers to complete.
*/
PaError Pa_AbortStream( PaStream *stream );


/** @return Returns one (1) when the stream is stopped, zero (0) when
    the stream is running, or a negative error number if the stream
    is invalid.
    FIXME: update this to reflect new state machine
*/
PaError Pa_IsStreamStopped( PaStream *stream );


/** @return Returns one (1) when the stream is active (ie playing
 or recording audio), zero (0) when not playing, or a negative error number
 if the stream is invalid.

 A stream is active after a successful call to Pa_StartStream(), until it
 becomes inactive either as a result of a call to Pa_StopStream() or
 Pa_AbortStream(), or as a result of a non-zero return value from the
 user callback. In the latter case, the stream is considered inactive after
 the last buffer has finished playing.

 FIXME: update this to reflect new state machine

 @see Pa_StopStream, Pa_AbortStream
*/
PaError Pa_IsStreamActive( PaStream *stream );


/**
 @return The input latency of the stream in seconds.

 @see PaTime, PaStreamCallback
*/
PaTime Pa_GetStreamInputLatency( PaStream *stream );


/**
 @return The output latency of the stream in seconds.

 @see PaTime, PaStreamCallback
*/
PaTime Pa_GetStreamOutputLatency( PaStream *stream );


/**
 @return The current time (in seconds) according to the same clock used to
 generate buffer timestamps for stream.
 This time may be used for syncronising other events to the audio stream,
 for example synchronizing audio to MIDI.

 @see PaTime, PaStreamCallback
*/
PaTime Pa_GetStreamTime( PaStream *stream );


/** Retrieve CPU usage information for the specified stream.
 The "CPU Load" is a fraction of total CPU time consumed by the stream's
 audio processing routines including, but not limited to the client supplied
 callback.
     
 This function may be called from the callback function or the application.
     
 @return
 A floating point value, typically between 0.0 and 1.0, where 1.0 indicates
 that the callback is consuming the maximum number of CPU cycles possible to
 maintain real-time operation. A value of 0.5 would imply that PortAudio and
 the sound generating callback was consuming roughly 50% of the available CPU
 time. The return value may exceed 1.0.
*/
double Pa_GetStreamCpuLoad( PaStream* stream );


/**
 FIXME: write documentation here
 @see http://www.portaudio.com/docs/proposals.html#Blocking
*/
PaError Pa_ReadStream( PaStream* stream,
                       void *buffer,
                       unsigned long frames );


/**
 FIXME: write documentation here
 @see http://www.portaudio.com/docs/proposals.html#Blocking
*/
PaError Pa_WriteStream( PaStream* stream,
                        void *buffer,
                        unsigned long frames );


/**
 FIXME: write documentation here
 @see http://www.portaudio.com/docs/proposals.html#Blocking
*/
unsigned long Pa_GetStreamReadAvailable( PaStream* stream );


/**
 FIXME: write documentation here
 @see http://www.portaudio.com/docs/proposals.html#Blocking
*/
unsigned long Pa_GetStreamWriteAvailable( PaStream* stream );


/* Miscellaneous utilities */


/**
 @return The size in bytes of a single sample in the specified format,
 or paSampleFormatNotSupported if the format is not supported.
*/
PaError Pa_GetSampleSize( PaSampleFormat format );


/** Puts the caller to sleep for at least 'msec' milliseconds.
 It may sleep longer than requested so don't rely on this for accurate
 musical timing.

 This function is provided only as a convenience for authors of portable code
 (such as the tests and examples in the PortAudio distribution.)
*/
void Pa_Sleep( long msec );



#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PORTAUDIO_H */
