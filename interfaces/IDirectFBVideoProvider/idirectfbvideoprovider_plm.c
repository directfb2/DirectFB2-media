/*
   This file is part of DirectFB.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

#include <config.h>
#include <core/layers.h>
#include <direct/thread.h>
#include <display/idirectfbsurface.h>
#ifdef HAVE_FUSIONSOUND
#include <fusionsound.h>
#endif
#define PL_MPEG_IMPLEMENTATION
#include <pl_mpeg.h>
#include <media/idirectfbvideoprovider.h>
#include <media/idirectfbdatabuffer.h>

D_DEBUG_DOMAIN( VideoProvider_PLM, "VideoProvider/PLM", "PL_MPEG Video Provider" );

static DFBResult Probe    ( IDirectFBVideoProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBVideoProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBVideoProvider, PLM )

/**********************************************************************************************************************/

typedef struct {
     DirectLink            link;
     IDirectFBEventBuffer *buffer;
} EventLink;

typedef struct {
     int                        ref;                    /* reference counter */

     IDirectFB                 *idirectfb;

     plm_t                     *plm;

     DFBSurfaceDescription      desc;

     double                     rate;
     DFBVideoProviderStatus     status;

     DirectThread              *thread;
     DirectMutex                lock;
     DirectWaitQueue            cond;

     bool                       seeked;
     double                     seek_time;

     plm_frame_t               *frame;

     IDirectFBSurface          *video_dest;
     DFBRectangle               video_rect;

#ifdef HAVE_FUSIONSOUND
     IFusionSound              *audio_sound;
     IFusionSoundStream        *audio_stream;
     IFusionSoundPlayback      *audio_playback;

     float                      audio_volume;
#endif

     DVFrameCallback            frame_callback;
     void                      *frame_callback_context;

     DirectLink                *events;
     DFBVideoProviderEventType  events_mask;
     DirectMutex                events_lock;
} IDirectFBVideoProvider_PLM_data;

/**********************************************************************************************************************/

static void video_decode_callback( plm_t       *plm,
                                   plm_frame_t *frame,
                                   void        *user )
{
     IDirectFBVideoProvider_PLM_data *data = user;

     data->frame = frame;
}

#ifdef HAVE_FUSIONSOUND
static void audio_decode_callback( plm_t         *plm,
                                   plm_samples_t *samples,
                                   void          *user )
{
     IDirectFBVideoProvider_PLM_data *data = user;

     data->audio_stream->Write( data->audio_stream, samples->interleaved, samples->count );
}
#endif

static void
dispatch_event( IDirectFBVideoProvider_PLM_data *data,
                DFBVideoProviderEventType        type )
{
     EventLink             *link;
     DFBVideoProviderEvent  event;

     if (!data->events || !(data->events_mask & type))
          return;

     event.clazz = DFEC_VIDEOPROVIDER;
     event.type  = type;

     direct_mutex_lock( &data->events_lock );

     direct_list_foreach (link, data->events) {
          link->buffer->PostEvent( link->buffer, DFB_EVENT(&event) );
     }

     direct_mutex_unlock( &data->events_lock );
}

static void *
PLMDecode( DirectThread *thread,
           void         *arg )
{
     DFBResult                        ret;
     long                             duration;
     int                              pitch;
     void                            *ptr;
     IDirectFBSurface                *source;
     double                           time = 0;
     IDirectFBVideoProvider_PLM_data *data = arg;

     ret = data->idirectfb->CreateSurface( data->idirectfb, &data->desc, &source );
     if (ret)
          return NULL;

     source->Lock( source, DSLF_WRITE, &ptr, &pitch );
     source->Unlock( source );

     duration = 1000000 / data->rate;

     dispatch_event( data, DVPET_STARTED );

     while (data->status != DVSTATE_STOP) {
          long   delay;
          double current_time = direct_clock_get_abs_millis() / 1000.0;
          double elapsed_time = current_time - time;

          time = current_time;

          if (elapsed_time > 1 / data->rate)
               elapsed_time = 1 / data->rate;

          direct_mutex_lock( &data->lock );

          if (data->seeked) {
               plm_seek( data->plm, data->seek_time, TRUE );

               if (data->status == DVSTATE_FINISHED)
                    data->status = DVSTATE_PLAY;

               data->seeked = false;
          }
          else
               plm_decode( data->plm, elapsed_time );

          if (plm_has_ended( data->plm )) {
               data->status = DVSTATE_FINISHED;
               dispatch_event( data, DVPET_FINISHED );
               direct_waitqueue_wait( &data->cond, &data->lock );
          }

          plm_frame_to_rgb( data->frame, ptr, pitch );

          data->video_dest->StretchBlit( data->video_dest, source, NULL, &data->video_rect );

          if (data->frame_callback)
               data->frame_callback( data->frame_callback_context );

          delay = direct_clock_get_abs_micros() - current_time * 1000000;
          if (delay > duration) {
               direct_mutex_unlock( &data->lock );
               continue;
          }

          direct_waitqueue_wait_timeout( &data->cond, &data->lock, duration - delay );

          direct_mutex_unlock( &data->lock );
     }

     source->Release( source );

     return NULL;
}

/**********************************************************************************************************************/

static void
IDirectFBVideoProvider_PLM_Destruct( IDirectFBVideoProvider *thiz )
{
     EventLink                       *link, *tmp;
     IDirectFBVideoProvider_PLM_data *data = thiz->priv;

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     thiz->Stop( thiz );

#ifdef HAVE_FUSIONSOUND
     if (data->audio_playback)
          data->audio_playback->Release( data->audio_playback );

     if (data->audio_stream)
          data->audio_stream->Release( data->audio_stream );

     if (data->audio_sound)
          data->audio_sound->Release( data->audio_sound );
#endif

     direct_waitqueue_deinit( &data->cond );
     direct_mutex_deinit( &data->lock );

     direct_list_foreach_safe (link, tmp, data->events) {
          direct_list_remove( &data->events, &link->link );
          link->buffer->Release( link->buffer );
          D_FREE( link );
     }

     direct_mutex_deinit( &data->events_lock );

     plm_destroy( data->plm );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBVideoProvider_PLM_AddRef( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DR_OK;
}

static DirectResult
IDirectFBVideoProvider_PLM_Release( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBVideoProvider_PLM_Destruct( thiz );

     return DR_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_GetCapabilities( IDirectFBVideoProvider       *thiz,
                                            DFBVideoProviderCapabilities *ret_caps )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_caps)
          return DFB_INVARG;

     *ret_caps = DVCAPS_BASIC | DVCAPS_SEEK | DVCAPS_SCALE;
#ifdef HAVE_FUSIONSOUND
     if (data->audio_playback)
          *ret_caps |= DVCAPS_VOLUME;
#endif

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_GetSurfaceDescription( IDirectFBVideoProvider *thiz,
                                                  DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM , "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_GetStreamDescription( IDirectFBVideoProvider *thiz,
                                                 DFBStreamDescription   *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     memset( ret_desc, 0, sizeof(DFBStreamDescription) );

     ret_desc->caps |= DVSCAPS_VIDEO;

     snprintf( ret_desc->video.encoding, DFB_STREAM_DESC_ENCODING_LENGTH, "MPEG-1 Video" );

     ret_desc->video.framerate = data->rate;
     ret_desc->video.aspect    = (double) data->desc.width / data->desc.height;

#ifdef HAVE_FUSIONSOUND
     if (data->audio_stream) {
          ret_desc->caps |= DVSCAPS_AUDIO;

          snprintf( ret_desc->audio.encoding, DFB_STREAM_DESC_ENCODING_LENGTH, "MPEG-1 Audio Layer II" );
          ret_desc->audio.samplerate = plm_get_samplerate( data->plm );
          ret_desc->audio.channels   = 2;
     }
#endif

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_PlayTo( IDirectFBVideoProvider *thiz,
                                   IDirectFBSurface       *destination,
                                   const DFBRectangle     *dest_rect,
                                   DVFrameCallback         callback,
                                   void                   *ctx )
{
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (!destination)
          return DFB_INVARG;

     dst_data = destination->priv;
     if (!dst_data)
          return DFB_DEAD;

     if (dest_rect) {
          if (dest_rect->w < 1 || dest_rect->h < 1)
               return DFB_INVARG;

          rect = *dest_rect;
          rect.x += dst_data->area.wanted.x;
          rect.y += dst_data->area.wanted.y;
     }
     else
          rect = dst_data->area.wanted;

     if (data->thread)
          return DFB_OK;

     direct_mutex_lock( &data->lock );

     data->video_dest             = destination;
     data->video_rect             = rect;
     data->frame_callback         = callback;
     data->frame_callback_context = ctx;

     data->status = DVSTATE_PLAY;

     data->thread = direct_thread_create( DTT_DEFAULT, PLMDecode, data, "PLM Decode" );

     direct_mutex_unlock( &data->lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_Stop( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->status == DVSTATE_STOP)
          return DFB_OK;

     data->status = DVSTATE_STOP;

     if (data->thread) {
          direct_waitqueue_signal( &data->cond );
          direct_thread_join( data->thread );
          direct_thread_destroy( data->thread );
          data->thread = NULL;
     }

     dispatch_event( data, DVPET_STOPPED );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_GetStatus( IDirectFBVideoProvider *thiz,
                                      DFBVideoProviderStatus *ret_status )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_status)
          return DFB_INVARG;

     *ret_status = data->status;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_SeekTo( IDirectFBVideoProvider *thiz,
                                   double                  seconds )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (seconds < 0.0)
          return DFB_INVARG;

     direct_mutex_lock( &data->lock );

     data->seeked    = true;
     data->seek_time = seconds;

     direct_waitqueue_signal( &data->cond );

     direct_mutex_unlock( &data->lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_GetPos( IDirectFBVideoProvider *thiz,
                                   double                 *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DFB_INVARG;

     *ret_seconds = plm_get_time( data->plm );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_GetLength( IDirectFBVideoProvider *thiz,
                                      double                 *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DFB_INVARG;

     *ret_seconds = plm_get_duration( data->plm );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_SetPlaybackFlags( IDirectFBVideoProvider        *thiz,
                                             DFBVideoProviderPlaybackFlags  flags )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (flags & ~DVPLAY_LOOPING)
          return DFB_UNSUPPORTED;

     plm_set_loop( data->plm, flags & DVPLAY_LOOPING );

     return DFB_OK;
}

#ifdef HAVE_FUSIONSOUND
static DFBResult
IDirectFBVideoProvider_PLM_SetVolume( IDirectFBVideoProvider *thiz,
                                      float                   level )
{
     DFBResult ret = DFB_UNSUPPORTED;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (level < 0.0)
          return DFB_INVARG;

     if (data->audio_playback) {
          ret = data->audio_playback->SetVolume( data->audio_playback, level );
          if (ret == DFB_OK)
               data->audio_volume = level;
     }

     return ret;
}

static DFBResult
IDirectFBVideoProvider_PLM_GetVolume( IDirectFBVideoProvider *thiz,
                                      float                  *ret_level )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_level)
          return DFB_INVARG;

     *ret_level = data->audio_volume;

     return DFB_OK;
}
#endif

static DFBResult
IDirectFBVideoProvider_PLM_CreateEventBuffer( IDirectFBVideoProvider  *thiz,
                                              IDirectFBEventBuffer   **ret_interface )
{
     DFBResult             ret;
     IDirectFBEventBuffer *buffer;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_interface)
          return DFB_INVARG;

     ret = data->idirectfb->CreateEventBuffer( data->idirectfb, &buffer );
     if (ret)
          return ret;

     ret = thiz->AttachEventBuffer( thiz, buffer );

     buffer->Release( buffer );

     *ret_interface = (ret == DFB_OK) ? buffer : NULL;

     return ret;
}

static DFBResult
IDirectFBVideoProvider_PLM_AttachEventBuffer( IDirectFBVideoProvider *thiz,
                                              IDirectFBEventBuffer   *buffer )
{
     DFBResult  ret;
     EventLink *link;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (!buffer)
          return DFB_INVARG;

     ret = buffer->AddRef( buffer );
     if (ret)
          return ret;

     link = D_MALLOC( sizeof(EventLink) );
     if (!link) {
          buffer->Release( buffer );
          return D_OOM();
     }

     link->buffer = buffer;

     direct_mutex_lock( &data->events_lock );

     direct_list_append( &data->events, &link->link );

     direct_mutex_unlock( &data->events_lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_EnableEvents( IDirectFBVideoProvider    *thiz,
                                         DFBVideoProviderEventType  mask )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (mask & ~DVPET_ALL)
          return DFB_INVARG;

     data->events_mask |= mask;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_DisableEvents( IDirectFBVideoProvider    *thiz,
                                          DFBVideoProviderEventType  mask )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (mask & ~DVPET_ALL)
          return DFB_INVARG;

     data->events_mask &= ~mask;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_PLM_DetachEventBuffer( IDirectFBVideoProvider *thiz,
                                              IDirectFBEventBuffer   *buffer )
{
     DFBResult  ret = DFB_ITEMNOTFOUND;
     EventLink *link;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     if (!buffer)
          return DFB_INVARG;

     direct_mutex_lock( &data->events_lock );

     direct_list_foreach (link, data->events) {
          if (link->buffer == buffer) {
               direct_list_remove( &data->events, &link->link );
               link->buffer->Release( link->buffer );
               D_FREE( link );
               ret = DFB_OK;
               break;
          }
     }

     direct_mutex_unlock( &data->events_lock );

     return ret;
}

static DFBResult
IDirectFBVideoProvider_PLM_SetDestination( IDirectFBVideoProvider *thiz,
                                           IDirectFBSurface       *destination,
                                           const DFBRectangle     *dest_rect )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p, %4d,%4d-%4dx%4d )\n", __FUNCTION__,
                 thiz, dest_rect->x, dest_rect->y, dest_rect->w, dest_rect->h );

     if (!dest_rect)
          return DFB_INVARG;

     if (dest_rect->w < 1 || dest_rect->h < 1)
          return DFB_INVARG;

     data->video_rect = *dest_rect;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBVideoProvider_ProbeContext *ctx )
{
     int           result;
     plm_buffer_t *buf   = plm_buffer_create_with_memory( ctx->header, sizeof(ctx->header), 0 );
     plm_demux_t  *demux = plm_demux_create( buf, 1 );

     result = plm_demux_has_headers( demux );

     plm_demux_destroy( demux );

     return result ? DFB_OK : DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBVideoProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     uint8_t                  *chunk       = NULL;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBVideoProvider_PLM )

     D_DEBUG_AT( VideoProvider_PLM, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     if (buffer_data->buffer) {
          data->plm = plm_create_with_memory( buffer_data->buffer, buffer_data->length, 0 );
     }
     else if (buffer_data->filename) {
          data->plm = plm_create_with_filename( buffer_data->filename );
     }
     else {
          unsigned int size = 0;

          while (1) {
               unsigned int bytes;

               chunk = D_REALLOC( chunk, size + 4096 );
               if (!chunk) {
                    ret = D_OOM();
                    goto error;
               }

               buffer->WaitForData( buffer, 4096 );
               if (buffer->GetData( buffer, 4096, chunk + size, &bytes ))
                    break;

               size += bytes;
          }

          if (!size) {
               ret = DFB_IO;
               goto error;
          }

          data->plm = plm_create_with_memory( chunk, size, 1 );
     }

     if (!data->plm) {
          D_ERROR( "VideoProvider/PLM: Failed to create stream!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     plm_set_video_decode_callback( data->plm, video_decode_callback, data );
#ifdef HAVE_FUSIONSOUND
     plm_set_audio_decode_callback( data->plm, audio_decode_callback, data );
#endif

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = plm_get_width( data->plm );
     data->desc.height      = plm_get_height( data->plm );
#ifdef WORDS_BIGENDIAN
     data->desc.pixelformat = DSPF_RGB24;
#else
     data->desc.pixelformat = DSPF_BGR24;
#endif

     data->rate = plm_get_framerate( data->plm) ;

#ifdef HAVE_FUSIONSOUND
     if (plm_get_num_audio_streams( data->plm ) &&
         FusionSoundInit( NULL, NULL ) == DR_OK && FusionSoundCreate( &data->audio_sound ) == DR_OK) {
          FSStreamDescription  dsc;

          dsc.flags        = FSSDF_BUFFERSIZE | FSSDF_CHANNELS | FSSDF_SAMPLEFORMAT | FSSDF_SAMPLERATE;
          dsc.channels     = 2;
          dsc.samplerate   = plm_get_samplerate( data->plm );
          dsc.buffersize   = dsc.samplerate / 10;
          dsc.sampleformat = FSSF_FLOAT;

          ret = data->audio_sound->CreateStream( data->audio_sound, &dsc, &data->audio_stream );
          if (ret) {
               D_ERROR( "VideoProvider/PLM: Failed to create FusionSound stream!\n" );
               goto error;
          }
          else {
               data->audio_stream->GetPlayback( data->audio_stream, &data->audio_playback );
          }
     }
     else if (plm_get_num_audio_streams( data->plm )) {
          D_ERROR( "VideoProvider/PLM: Failed to initialize/create FusionSound!\n" );
          goto error;
     }
#endif

     data->status = DVSTATE_STOP;

     data->events_mask = DVPET_ALL;

     direct_mutex_init( &data->events_lock );

     direct_mutex_init( &data->lock );
     direct_waitqueue_init( &data->cond );

#ifdef HAVE_FUSIONSOUND
     data->audio_volume = 1.0;
#endif

     thiz->AddRef                = IDirectFBVideoProvider_PLM_AddRef;
     thiz->Release               = IDirectFBVideoProvider_PLM_Release;
     thiz->GetCapabilities       = IDirectFBVideoProvider_PLM_GetCapabilities;
     thiz->GetSurfaceDescription = IDirectFBVideoProvider_PLM_GetSurfaceDescription;
     thiz->GetStreamDescription  = IDirectFBVideoProvider_PLM_GetStreamDescription;
     thiz->PlayTo                = IDirectFBVideoProvider_PLM_PlayTo;
     thiz->Stop                  = IDirectFBVideoProvider_PLM_Stop;
     thiz->GetStatus             = IDirectFBVideoProvider_PLM_GetStatus;
     thiz->SeekTo                = IDirectFBVideoProvider_PLM_SeekTo;
     thiz->GetPos                = IDirectFBVideoProvider_PLM_GetPos;
     thiz->GetLength             = IDirectFBVideoProvider_PLM_GetLength;
     thiz->SetPlaybackFlags      = IDirectFBVideoProvider_PLM_SetPlaybackFlags;
#ifdef HAVE_FUSIONSOUND
     thiz->SetVolume             = IDirectFBVideoProvider_PLM_SetVolume;
     thiz->GetVolume             = IDirectFBVideoProvider_PLM_GetVolume;
#endif
     thiz->CreateEventBuffer     = IDirectFBVideoProvider_PLM_CreateEventBuffer;
     thiz->AttachEventBuffer     = IDirectFBVideoProvider_PLM_AttachEventBuffer;
     thiz->EnableEvents          = IDirectFBVideoProvider_PLM_EnableEvents;
     thiz->DisableEvents         = IDirectFBVideoProvider_PLM_DisableEvents;
     thiz->DetachEventBuffer     = IDirectFBVideoProvider_PLM_DetachEventBuffer;
     thiz->SetDestination        = IDirectFBVideoProvider_PLM_SetDestination;

     return DFB_OK;

error:
#ifdef HAVE_FUSIONSOUND
     if (data->audio_playback)
          data->audio_playback->Release( data->audio_playback );

     if (data->audio_stream)
          data->audio_stream->Release( data->audio_stream );

     if (data->audio_sound)
          data->audio_sound->Release( data->audio_sound );
#endif

     if (data->plm)
          plm_destroy( data->plm );

     if (chunk)
          D_FREE( chunk );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
