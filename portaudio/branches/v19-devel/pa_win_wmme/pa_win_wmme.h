#ifndef PA_WIN_WMME_H
#define PA_WIN_WMME_H

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/*
 *
 * PortAudio Portable Real-Time Audio Library
 * MME specific extensions
 *
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


#include "portaudio.h"

#define PaWinMmeUseLowLevelLatencyParameters (0x01)
#define PaWinMmeUseMultipleDevices  (0x02)  /* use mme specific multiple device feature */


typedef struct PaWinMmeDeviceAndNumChannels{
    PaDeviceIndex device;
    int numChannels;
}PaWinMmeDeviceAndNumChannels;


typedef struct PaWinMmeStreamInfo{
    PaHostApiSpecificStreamInfo header;

    unsigned long flags;

    /* low-level latency setting support
        These settings control the number and size of host buffers in order
        to set latency. They will be used instead of the generic parameters
        to Pa_OpenStream() if flags contains the PaWinMmeUseLowLevelLatencyParameters
        flag.
    */
    unsigned long framesPerBuffer;
    unsigned long numBuffers;  

    /* multiple devices per direction support
        The total number of channels must agree with the numChannels parameters
        to OpenStream. If flags contains the PaWinMmeUseMultipleDevices flag,
        this functionality will be used, otherwise the device parameter to
        Pa_OpenStream() will be used instead.
    */
    PaWinMmeDeviceAndNumChannels *devices;
    unsigned long numDevices;

}PaWinMmeStreamInfo;


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PA_WIN_WMME_H */
