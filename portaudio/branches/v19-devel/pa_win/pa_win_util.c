/*
 * $Id$
 * Portable Audio I/O Library
 * Win32 platform-specific support functions
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2000 Ross Bencina
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

 
#include <windows.h>
#include <mmsystem.h> /* for timeGetTime() */

#include "pa_util.h"


/*
   Track memory allocations to avoid leaks.
 */

#if PA_TRACK_MEMORY
static int numAllocations_ = 0;
#endif


void *PaUtil_AllocateMemory( long size )
{
    void *result = GlobalAlloc( GPTR, size );

#if PA_TRACK_MEMORY
    if( result != NULL ) numAllocations_ += 1;
#endif
    return result;
}


void PaUtil_FreeMemory( void *block )
{
    if( block != NULL )
    {
        GlobalFree( block );
#if PA_TRACK_MEMORY
        numAllocations_ -= 1;
#endif

    }
}


int PaUtil_CountMemoryLeaks( void )
{
#if PA_TRACK_MEMORY
    return numAllocations_;
#else
    return 0;
#endif
}


void Pa_Sleep( long msec )
{
    Sleep( msec );
}


static int usePerformanceCounter_;
static double microsecondsPerTick_;

void PaUtil_InitializeMicrosecondClock( void )
{
    LARGE_INTEGER frequency;

    if( QueryPerformanceFrequency( &frequency ) != 0 )
    {
        usePerformanceCounter_ = 1;
        microsecondsPerTick_ = (double)frequency.QuadPart * 0.000001;
    }
    else
    {
        usePerformanceCounter_ = 0;
    }
}


double PaUtil_MicrosecondTime( void )
{
    LARGE_INTEGER time;

    if( usePerformanceCounter_ )
    {
        QueryPerformanceCounter( &time );
        return time.QuadPart * microsecondsPerTick_;
    }
    else
    {
        return timeGetTime() * 1000.;
    }
}
