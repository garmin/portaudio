
#include <sys/poll.h>
#include <limits.h>
#include <math.h>  /* abs() */

#include <alsa/asoundlib.h>

#include "pa_linux_alsa.h"

#define MIN(x,y) ( (x) < (y) ? (x) : (y) )

void Stop( void *data );
void *ExtractAddress( const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset );

static int wait( PaAlsaStream *stream )
{
    int need_capture;
    int need_playback;
    int capture_avail = INT_MAX;
    int playback_avail = INT_MAX;
    int common_avail;

    if( stream->pcm_capture )
        need_capture = 1;
    else
        need_capture = 0;

    if( stream->pcm_playback )
        need_playback = 1;
    else
        need_playback = 0;

    while( need_capture || need_playback )
    {
        int playback_pfd_offset;
        int total_fds = 0;

        /* if the main thread has requested that we stop, do so now */
        pthread_testcancel();

        /*PA_DEBUG(("still polling...\n"));
        if( need_capture )
            PA_DEBUG(("need capture.\n"));
        if( need_playback )
            PA_DEBUG(("need playback.\n")); */

        /* get the fds, packing all applicable fds into a single array,
         * so we can check them all with a single poll() call */

        if( need_capture )
        {
            snd_pcm_poll_descriptors( stream->pcm_capture, stream->pfds,
                                      stream->capture_nfds );
            total_fds += stream->capture_nfds;
        }

        if( need_playback )
        {
            playback_pfd_offset = total_fds;
            snd_pcm_poll_descriptors( stream->pcm_playback,
                                      stream->pfds + playback_pfd_offset,
                                      stream->playback_nfds );
            total_fds += stream->playback_nfds;
        }

        /* now poll on the combination of playback and capture fds.
         * TODO: handle interrupt and/or failure */
        poll( stream->pfds, total_fds, 1000 );

        /* check the return status of our pfds */
        if( need_capture )
        {
            short revents;
            snd_pcm_poll_descriptors_revents( stream->pcm_capture, stream->pfds,
                                              stream->capture_nfds, &revents );
            if( revents == POLLIN )
                need_capture = 0;
        }

        if( need_playback )
        {
            short revents;
            snd_pcm_poll_descriptors_revents( stream->pcm_playback,
                                              stream->pfds + playback_pfd_offset,
                                              stream->playback_nfds, &revents );
            //if( revents & POLLOUT )
            //if( revents & POLLERR )
            //    PA_DEBUG(("polling error!"));
            if( revents == POLLOUT )
                need_playback = 0;
        }
    }

    /* we have now established that there are buffers ready to be
     * operated on.  Now determine how many frames are available. */
    if( stream->pcm_capture )
        capture_avail = snd_pcm_avail_update( stream->pcm_capture );

    if( stream->pcm_playback )
        playback_avail = snd_pcm_avail_update( stream->pcm_playback );

    common_avail = MIN(capture_avail, playback_avail);
    common_avail -= common_avail % stream->frames_per_period;

    return common_avail;
}

static int setup_buffers( PaAlsaStream *stream, int frames_avail )
{
    int i;
    int capture_frames_avail = INT_MAX;
    int playback_frames_avail = INT_MAX;
    int common_frames_avail;

    if( stream->pcm_capture )
    {
        const snd_pcm_channel_area_t *capture_areas;
        const snd_pcm_channel_area_t *area;
        snd_pcm_uframes_t frames = frames_avail;

        /* I do not understand this code fragment yet, it is copied out of the
         * alsa-devel archives... */
        snd_pcm_mmap_begin( stream->pcm_capture, &capture_areas,
                            &stream->capture_offset, &frames);

        if( stream->capture_interleaved )
        {
            void *interleaved_capture_buffer;
            area = &capture_areas[0];
            interleaved_capture_buffer = ExtractAddress( area, stream->capture_offset );
            PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor,
                                                0, /* starting at channel 0 */
                                                interleaved_capture_buffer,
                                                0  /* default numInputChannels */
                                              );
        }
        else
        {
            /* noninterleaved */
            for( i = 0; i < stream->capture_channels; i++ )
            {
                void *noninterleaved_capture_buffer;
                area = &capture_areas[i];
                noninterleaved_capture_buffer = ExtractAddress( area, stream->capture_offset );
                PaUtil_SetNonInterleavedInputChannel( &stream->bufferProcessor,
                                                      i,
                                                      noninterleaved_capture_buffer);
            }
        }

        capture_frames_avail = frames;
    }

    if( stream->pcm_playback )
    {
        const snd_pcm_channel_area_t *playback_areas;
        const snd_pcm_channel_area_t *area;
        snd_pcm_uframes_t frames = frames_avail;

        snd_pcm_mmap_begin( stream->pcm_playback, &playback_areas, 
                            &stream->playback_offset, &frames);

        if( stream->playback_interleaved )
        {
            void *interleaved_playback_buffer;
            area = &playback_areas[0];
            interleaved_playback_buffer = ExtractAddress( area, stream->playback_offset );
            PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor,
                                                 0, /* starting at channel 0 */
                                                 interleaved_playback_buffer,
                                                 0  /* default numInputChannels */
                                               );
        }
        else
        {
            /* noninterleaved */
            for( i = 0; i < stream->playback_channels; i++ )
            {
                void *noninterleaved_playback_buffer;
                area = &playback_areas[i];
                noninterleaved_playback_buffer = ExtractAddress( area, stream->playback_offset );
                PaUtil_SetNonInterleavedOutputChannel( &stream->bufferProcessor,
                                                      i,
                                                      noninterleaved_playback_buffer);
            }
        }

        playback_frames_avail = frames;
    }


    common_frames_avail = MIN(capture_frames_avail, playback_frames_avail);
    common_frames_avail -= common_frames_avail % stream->frames_per_period;
    //PA_DEBUG(( "%d capture frames available\n", capture_frames_avail ));
    //PA_DEBUG(( "%d frames playback available\n", playback_frames_avail ));
    //PA_DEBUG(( "%d frames available\n", common_frames_avail ));

    if( stream->pcm_capture )
        PaUtil_SetInputFrameCount( &stream->bufferProcessor, common_frames_avail );

    if( stream->pcm_playback )
        PaUtil_SetOutputFrameCount( &stream->bufferProcessor, common_frames_avail );

    return common_frames_avail;
}

void *CallbackThread( void *userData )
{
    PaAlsaStream *stream = (PaAlsaStream*)userData;
    pthread_cleanup_push( &Stop, stream );   // Execute Stop on exit

    if( stream->pcm_playback )
        snd_pcm_start( stream->pcm_playback );
    else if( stream->pcm_capture )
        snd_pcm_start( stream->pcm_capture );

    while(1)
    {
        int frames_avail;
        int frames_got;

        PaStreamCallbackTimeInfo timeInfo = {0,0,0}; /* IMPLEMENT ME */
        int callbackResult;
        int framesProcessed;

        pthread_testcancel();
        {
            /* calculate time info */
            snd_timestamp_t capture_timestamp;
            snd_timestamp_t playback_timestamp;
            snd_pcm_status_t *capture_status;
            snd_pcm_status_t *playback_status;
            snd_pcm_status_alloca( &capture_status );
            snd_pcm_status_alloca( &playback_status );

            if( stream->pcm_capture )
            {
                snd_pcm_status( stream->pcm_capture, capture_status );
                snd_pcm_status_get_tstamp( capture_status, &capture_timestamp );
            }
            if( stream->pcm_playback )
            {
                snd_pcm_status( stream->pcm_playback, playback_status );
                snd_pcm_status_get_tstamp( playback_status, &playback_timestamp );
            }

            /* Hmm, we potentially have both a playback and a capture timestamp.
             * Hopefully they are the same... */
            if( stream->pcm_capture && stream->pcm_playback )
            {
                float capture_time = capture_timestamp.tv_sec +
                                     ((float)capture_timestamp.tv_usec/1000000);
                float playback_time= playback_timestamp.tv_sec +
                                     ((float)playback_timestamp.tv_usec/1000000);
                if( fabsf(capture_time-playback_time) > 0.01 )
                    PA_DEBUG(("Capture time and playback time differ by %f\n", fabsf(capture_time-playback_time)));
                timeInfo.currentTime = capture_time;
            }
            else if( stream->pcm_playback )
            {
                timeInfo.currentTime = playback_timestamp.tv_sec +
                                       ((float)playback_timestamp.tv_usec/1000000);
            }
            else
            {
                timeInfo.currentTime = capture_timestamp.tv_sec +
                                       ((float)capture_timestamp.tv_usec/1000000);
            }

            if( stream->pcm_capture )
            {
                snd_pcm_sframes_t capture_delay = snd_pcm_status_get_delay( capture_status );
                timeInfo.inputBufferAdcTime = timeInfo.currentTime -
                    (float)capture_delay / stream->streamRepresentation.streamInfo.sampleRate;
            }

            if( stream->pcm_playback )
            {
                snd_pcm_sframes_t playback_delay = snd_pcm_status_get_delay( playback_status );
                timeInfo.outputBufferDacTime = timeInfo.currentTime +
                    (float)playback_delay / stream->streamRepresentation.streamInfo.sampleRate;
            }
        }


        /*
            IMPLEMENT ME:
                - handle buffer slips
        */

        /*
            depending on whether the host buffers are interleaved, non-interleaved
            or a mixture, you will want to call PaUtil_ProcessInterleavedBuffers(),
            PaUtil_ProcessNonInterleavedBuffers() or PaUtil_ProcessBuffers() here.
        */

        framesProcessed = frames_avail = wait( stream );

        while( frames_avail > 0 )
        {
            //PA_DEBUG(( "%d frames available\n", frames_avail ));

            /* Now we know the soundcard is ready to produce/receive at least
             * one period.  We just need to get the buffers for the client
             * to read/write. */
            PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo,
                    0 /* @todo pass underflow/overflow flags when necessary */ );

            frames_got = setup_buffers( stream, frames_avail );


            PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

            callbackResult = paContinue;

            /* this calls the callback */

            framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor,
                                                          &callbackResult );

            PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

            /* inform ALSA how many frames we wrote */

            if( stream->pcm_capture )
                snd_pcm_mmap_commit( stream->pcm_capture, stream->capture_offset, frames_got );

            if( stream->pcm_playback )
                snd_pcm_mmap_commit( stream->pcm_playback, stream->playback_offset, frames_got );

            if( callbackResult != paContinue )
                break;

            frames_avail -= frames_got;
        }


        /*
            If you need to byte swap outputBuffer, you can do it here using
            routines in pa_byteswappers.h
        */

        if( callbackResult != paContinue )
        {
            stream->callback_finished = 1;
            stream->callbackAbort = (callbackResult == paAbort);

            pthread_exit( NULL );
        }
    }

    /* This code is unreachable, but important to include regardless because it
     * is possibly a macro with a closing brace to match the opening brace in
     * pthread_cleanup_push() above.  The documentation states that they must
     * always occur in pairs. */

    pthread_cleanup_pop( 1 );
}

void Stop( void *data )
{
    PaAlsaStream *stream = (PaAlsaStream *) data;

    if( stream->callbackAbort )
    {
        if( stream->pcm_playback )
            snd_pcm_drop( stream->pcm_playback );
        if( stream->pcm_capture && !stream->pcmsSynced )
            snd_pcm_drop( stream->pcm_capture );

        PA_DEBUG(( "Dropped frames\n" ));
        stream->callbackAbort = 0;
    }
    else
    {
        if( stream->pcm_playback )
            snd_pcm_drain( stream->pcm_playback );
        if( stream->pcm_capture && !stream->pcmsSynced )
            snd_pcm_drain( stream->pcm_capture );
    }

    PA_DEBUG(( "Stoppage\n" ));
}

void *ExtractAddress( const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset )
{
    return area->addr + (area->first + offset * area->step) / 8;
}
