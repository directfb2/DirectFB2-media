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
#include <libmpeg3.h>
#include <media/idirectfbvideoprovider.h>
#include <media/idirectfbdatabuffer.h>

D_DEBUG_DOMAIN( VideoProvider_Libmpeg3, "VideoProvider/Libmpeg3", "Libmpeg3 Video Provider" );

static DFBResult Probe    ( IDirectFBVideoProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBVideoProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBVideoProvider, Libmpeg3 )

/**********************************************************************************************************************/

typedef struct {
     DirectLink            link;
     IDirectFBEventBuffer *buffer;
} EventLink;

typedef struct {
     int                            ref;                    /* reference counter */

     IDirectFB                     *idirectfb;

     mpeg3_t                       *file;

     DFBSurfaceDescription          desc;

     double                         rate;
     DFBVideoProviderStatus         status;
     DFBVideoProviderPlaybackFlags  flags;

     struct {
          DirectThread             *thread;
          DirectMutex               lock;
          DirectWaitQueue           cond;

          IDirectFBSurface         *dest;
          DFBRectangle              rect;
     } video;

#ifdef HAVE_FUSIONSOUND
     struct {
          DirectThread             *thread;
          DirectMutex               lock;

          IFusionSound             *sound;
          IFusionSoundStream       *stream;
          IFusionSoundPlayback     *playback;

          float                     volume;
     } audio;
#endif

     DVFrameCallback                frame_callback;
     void                          *frame_callback_context;

     DirectLink                    *events;
     DFBVideoProviderEventType      events_mask;
     DirectMutex                    events_lock;
} IDirectFBVideoProvider_Libmpeg3_data;

/**********************************************************************************************************************/

static void
dispatch_event( IDirectFBVideoProvider_Libmpeg3_data *data,
                DFBVideoProviderEventType             type )
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
Libmpeg3Video( DirectThread *thread,
               void         *arg )
{
     DFBResult                             ret;
     long                                  duration;
     int                                   pitch, s;
     void                                 *ptr;
     IDirectFBSurface                     *source;
     int                                   drop = 0;
     IDirectFBVideoProvider_Libmpeg3_data *data = arg;

     ret = data->idirectfb->CreateSurface( data->idirectfb, &data->desc, &source );
     if (ret)
          return NULL;

     source->Lock( source, DSLF_WRITE, &ptr, &pitch );
     source->Unlock( source );

     s = DFB_PLANE_MULTIPLY( data->desc.pixelformat, data->desc.height );

     duration = 1000000 / data->rate;

     dispatch_event( data, DVPET_STARTED );

     while (data->status != DVSTATE_STOP) {
          int       result;
          long      delay;
          long long time;

          time = direct_clock_get_abs_micros();

          direct_mutex_lock( &data->video.lock );

          if (drop) {
               mpeg3_drop_frames( data->file, drop, 0 );
               drop = 0;
          }

          result = mpeg3_read_yuvframe( data->file,
                                        ptr, ptr + pitch * data->desc.height, ptr + pitch * (data->desc.height + s) / 2,
                                        0, 0, data->desc.width, data->desc.height, 0 );
          if (result) {
               if (data->flags & DVPLAY_LOOPING) {
                    mpeg3_seek_byte( data->file, 0 );
                    drop = 1;
                    direct_mutex_unlock( &data->video.lock );
                    continue;
               }
               else {
                    data->status = DVSTATE_FINISHED;
                    dispatch_event( data, DVPET_FINISHED );
                    direct_waitqueue_wait( &data->video.cond, &data->video.lock );
               }
          }

          data->video.dest->StretchBlit( data->video.dest, source, NULL, &data->video.rect );

          if (data->frame_callback)
               data->frame_callback( data->frame_callback_context );

          delay = direct_clock_get_abs_micros() - time;
          if (delay > duration) {
               drop = delay / duration;
               direct_mutex_unlock( &data->video.lock );
               continue;
          }

          direct_waitqueue_wait_timeout( &data->video.cond, &data->video.lock, duration - delay );

          direct_mutex_unlock( &data->video.lock );
     }

     source->Release( source );

     return NULL;
}

#ifdef HAVE_FUSIONSOUND
static void *
Libmpeg3Audio( DirectThread *thread,
               void         *arg )
{
     IDirectFBVideoProvider_Libmpeg3_data *data    = arg;
     long                                  samples = mpeg3_sample_rate( data->file, 0 ) / 5;
     short                                 buf[samples * mpeg3_audio_channels( data->file, 0 )];

     while (data->status != DVSTATE_STOP) {
          direct_mutex_lock( &data->audio.lock );

          if (mpeg3_audio_channels( data->file, 0 ) == 1) {
               mpeg3_read_audio( data->file, NULL, buf, 0, samples, 0 );

               data->audio.stream->Write( data->audio.stream, buf, samples );
          }
          else {
               int   i;
               short left[samples];
               short right[samples];

               mpeg3_read_audio( data->file, NULL, left,  0, samples, 0 );
               mpeg3_reread_audio( data->file, NULL, right, 1, samples, 0 );

               for (i = 0; i < samples; i++) {
                    buf[i*2+0] = left[i];
                    buf[i*2+1] = right[i];
               }

               data->audio.stream->Write( data->audio.stream, buf, samples );
          }

          direct_mutex_unlock( &data->audio.lock );
     }

     return NULL;
}
#endif

/**********************************************************************************************************************/

static void
IDirectFBVideoProvider_Libmpeg3_Destruct( IDirectFBVideoProvider *thiz )
{
     EventLink                            *link, *tmp;
     IDirectFBVideoProvider_Libmpeg3_data *data = thiz->priv;

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     thiz->Stop( thiz );

#ifdef HAVE_FUSIONSOUND
     if (data->audio.playback)
          data->audio.playback->Release( data->audio.playback );

     if (data->audio.stream)
          data->audio.stream->Release( data->audio.stream );

     if (data->audio.sound)
          data->audio.sound->Release( data->audio.sound );

     direct_mutex_deinit( &data->audio.lock );
#endif

     direct_waitqueue_deinit( &data->video.cond );
     direct_mutex_deinit( &data->video.lock );

     direct_list_foreach_safe (link, tmp, data->events) {
          direct_list_remove( &data->events, &link->link );
          link->buffer->Release( link->buffer );
          D_FREE( link );
     }

     direct_mutex_deinit( &data->events_lock );

     mpeg3_close( data->file );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBVideoProvider_Libmpeg3_AddRef( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DR_OK;
}

static DirectResult
IDirectFBVideoProvider_Libmpeg3_Release( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBVideoProvider_Libmpeg3_Destruct( thiz );

     return DR_OK;
}

static DFBResult
IDirectFBVideoProvider_Libmpeg3_GetCapabilities( IDirectFBVideoProvider       *thiz,
                                                 DFBVideoProviderCapabilities *ret_caps )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_caps)
          return DFB_INVARG;

     *ret_caps = DVCAPS_BASIC | DVCAPS_SCALE;
#ifdef HAVE_FUSIONSOUND
     if (data->audio.playback)
          *ret_caps |= DVCAPS_VOLUME;
#endif

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Libmpeg3_GetSurfaceDescription( IDirectFBVideoProvider *thiz,
                                                       DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3 , "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Libmpeg3_GetStreamDescription( IDirectFBVideoProvider *thiz,
                                                      DFBStreamDescription   *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     memset( ret_desc, 0, sizeof(DFBStreamDescription) );

     ret_desc->caps |= DVSCAPS_VIDEO;

     snprintf( ret_desc->video.encoding, DFB_STREAM_DESC_ENCODING_LENGTH, "MPEG" );

     ret_desc->video.framerate = data->rate;
     ret_desc->video.aspect    = mpeg3_aspect_ratio( data->file, 0 ) ?: (double) data->desc.width / data->desc.height;

#ifdef HAVE_FUSIONSOUND
     if (data->audio.stream) {
          ret_desc->caps |= DVSCAPS_AUDIO;

          snprintf( ret_desc->audio.encoding, DFB_STREAM_DESC_ENCODING_LENGTH, "MPEG" );
          ret_desc->audio.samplerate = mpeg3_sample_rate( data->file, 0 );
          ret_desc->audio.channels   = mpeg3_audio_channels( data->file, 0 );
     }
#endif

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Libmpeg3_PlayTo( IDirectFBVideoProvider *thiz,
                                        IDirectFBSurface       *destination,
                                        const DFBRectangle     *dest_rect,
                                        DVFrameCallback         callback,
                                        void                   *ctx )
{
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

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

     if (data->video.thread)
          return DFB_OK;

     direct_mutex_lock( &data->video.lock );
#ifdef HAVE_FUSIONSOUND
     direct_mutex_lock( &data->audio.lock );
#endif

     data->video.dest             = destination;
     data->video.rect             = rect;
     data->frame_callback         = callback;
     data->frame_callback_context = ctx;

     data->status = DVSTATE_PLAY;

     data->video.thread = direct_thread_create( DTT_DEFAULT, Libmpeg3Video, data, "Libmpeg3 Video" );

#ifdef HAVE_FUSIONSOUND
     if (data->audio.stream)
          data->audio.thread = direct_thread_create( DTT_DEFAULT, Libmpeg3Audio, data, "Libmpeg3 Audio" );
#endif

#ifdef HAVE_FUSIONSOUND
     direct_mutex_unlock( &data->audio.lock );
#endif
     direct_mutex_unlock( &data->video.lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Libmpeg3_Stop( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->status == DVSTATE_STOP)
          return DFB_OK;

     data->status = DVSTATE_STOP;

     if (data->video.thread) {
          direct_waitqueue_signal( &data->video.cond );
          direct_thread_join( data->video.thread );
          direct_thread_destroy( data->video.thread );
          data->video.thread = NULL;
     }

#ifdef HAVE_FUSIONSOUND
     if (data->audio.thread) {
          direct_thread_join( data->audio.thread );
          direct_thread_destroy( data->audio.thread );
          data->audio.thread = NULL;
     }
#endif

     dispatch_event( data, DVPET_STOPPED );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Libmpeg3_GetStatus( IDirectFBVideoProvider *thiz,
                                           DFBVideoProviderStatus *ret_status )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_status)
          return DFB_INVARG;

     *ret_status = data->status;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Libmpeg3_GetPos( IDirectFBVideoProvider *thiz,
                                        double                 *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DFB_INVARG;

     *ret_seconds = mpeg3_get_frame( data->file, 0 ) / data->rate;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Libmpeg3_SetPlaybackFlags( IDirectFBVideoProvider        *thiz,
                                                  DFBVideoProviderPlaybackFlags  flags )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     if (flags & ~DVPLAY_LOOPING)
          return DFB_UNSUPPORTED;

     data->flags = flags;

     return DFB_OK;
}

#ifdef HAVE_FUSIONSOUND
static DFBResult
IDirectFBVideoProvider_Libmpeg3_SetVolume( IDirectFBVideoProvider *thiz,
                                           float                   level )
{
     DFBResult ret = DFB_UNSUPPORTED;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     if (level < 0.0)
          return DFB_INVARG;

     if (data->audio.playback) {
          ret = data->audio.playback->SetVolume( data->audio.playback, level );
          if (ret == DFB_OK)
               data->audio.volume = level;
     }

     return ret;
}

static DFBResult
IDirectFBVideoProvider_Libmpeg3_GetVolume( IDirectFBVideoProvider *thiz,
                                           float                  *ret_level )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_level)
          return DFB_INVARG;

     *ret_level = data->audio.volume;

     return DFB_OK;
}
#endif

static DFBResult
IDirectFBVideoProvider_Libmpeg3_CreateEventBuffer( IDirectFBVideoProvider  *thiz,
                                                   IDirectFBEventBuffer   **ret_interface )
{
     DFBResult             ret;
     IDirectFBEventBuffer *buffer;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_Libmpeg3_AttachEventBuffer( IDirectFBVideoProvider *thiz,
                                                   IDirectFBEventBuffer   *buffer )
{
     DFBResult  ret;
     EventLink *link;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_Libmpeg3_EnableEvents( IDirectFBVideoProvider    *thiz,
                                              DFBVideoProviderEventType  mask )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     if (mask & ~DVPET_ALL)
          return DFB_INVARG;

     data->events_mask |= mask;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Libmpeg3_DisableEvents( IDirectFBVideoProvider    *thiz,
                                               DFBVideoProviderEventType  mask )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     if (mask & ~DVPET_ALL)
          return DFB_INVARG;

     data->events_mask &= ~mask;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Libmpeg3_DetachEventBuffer( IDirectFBVideoProvider *thiz,
                                                   IDirectFBEventBuffer   *buffer )
{
     DFBResult  ret = DFB_ITEMNOTFOUND;
     EventLink *link;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_Libmpeg3_SetDestination( IDirectFBVideoProvider *thiz,
                                                IDirectFBSurface       *destination,
                                                const DFBRectangle     *dest_rect )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p, %4d,%4d-%4dx%4d )\n", __FUNCTION__,
                 thiz, dest_rect->x, dest_rect->y, dest_rect->w, dest_rect->h );

     if (!dest_rect)
          return DFB_INVARG;

     if (dest_rect->w < 1 || dest_rect->h < 1)
          return DFB_INVARG;

     data->video.rect = *dest_rect;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBVideoProvider_ProbeContext *ctx )
{
     mpeg3_t *file;

     /* Check for valid filename. */
     if (!ctx->filename)
          return DFB_UNSUPPORTED;

     /* Check the signature. */
     if (!mpeg3_check_sig( (char*) ctx->filename ))
          return DFB_UNSUPPORTED;

     /* Check for video stream. */
     file = mpeg3_open( (char*) ctx->filename, NULL );
     if (!file)
          return DFB_UNSUPPORTED;

     if (!mpeg3_has_video( file )) {
          mpeg3_close( file );
          return DFB_UNSUPPORTED;
     }

     mpeg3_close( file );

     return DFB_OK;
}

static DFBResult
Construct( IDirectFBVideoProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBVideoProvider_Libmpeg3 )

     D_DEBUG_AT( VideoProvider_Libmpeg3, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     data->file = mpeg3_open( buffer_data->filename, NULL );
     if (!data->file) {
          D_ERROR( "VideoProvider/Libmpeg3: Failed to open MPEG stream!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     data->desc.flags  = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width  = mpeg3_video_width( data->file, 0 );
     data->desc.height = mpeg3_video_height( data->file, 0 );
     switch (mpeg3_colormodel( data->file, 0 )) {
          case MPEG3_YUV420P:
               data->desc.pixelformat = DSPF_I420;
               break;
          case MPEG3_YUV422P:
               data->desc.pixelformat = DSPF_Y42B;
               break;
          default:
               data->desc.pixelformat = dfb_primary_layer_pixelformat();
               break;
     }

     data->rate = mpeg3_frame_rate( data->file, 0 );

#ifdef HAVE_FUSIONSOUND
     if (mpeg3_has_audio( data->file ) &&
         FusionSoundInit( NULL, NULL ) == DR_OK && FusionSoundCreate( &data->audio.sound ) == DR_OK) {
          FSStreamDescription  dsc;

          dsc.flags        = FSSDF_BUFFERSIZE | FSSDF_CHANNELS | FSSDF_SAMPLEFORMAT | FSSDF_SAMPLERATE;
          dsc.channels     = mpeg3_audio_channels( data->file, 0 );
          dsc.samplerate   = mpeg3_sample_rate( data->file, 0 );
          dsc.buffersize   = dsc.samplerate / 10;
          dsc.sampleformat = FSSF_S16;

          ret = data->audio.sound->CreateStream( data->audio.sound, &dsc, &data->audio.stream );
          if (ret) {
               D_ERROR( "VideoProvider/Libmpeg3: Failed to create FusionSound stream!\n" );
               goto error;
          }
          else {
               data->audio.stream->GetPlayback( data->audio.stream, &data->audio.playback );
          }
     }
     else if (mpeg3_has_audio( data->file )) {
          D_ERROR( "VideoProvider/Libmpeg3: Failed to initialize/create FusionSound!\n" );
          goto error;
     }
#endif

     data->status = DVSTATE_STOP;

     data->events_mask = DVPET_ALL;

     direct_mutex_init( &data->events_lock );

     direct_mutex_init( &data->video.lock );
     direct_waitqueue_init( &data->video.cond );

#ifdef HAVE_FUSIONSOUND
     data->audio.volume = 1.0;

     direct_mutex_init( &data->audio.lock );
#endif

     thiz->AddRef                = IDirectFBVideoProvider_Libmpeg3_AddRef;
     thiz->Release               = IDirectFBVideoProvider_Libmpeg3_Release;
     thiz->GetCapabilities       = IDirectFBVideoProvider_Libmpeg3_GetCapabilities;
     thiz->GetSurfaceDescription = IDirectFBVideoProvider_Libmpeg3_GetSurfaceDescription;
     thiz->GetStreamDescription  = IDirectFBVideoProvider_Libmpeg3_GetStreamDescription;
     thiz->PlayTo                = IDirectFBVideoProvider_Libmpeg3_PlayTo;
     thiz->Stop                  = IDirectFBVideoProvider_Libmpeg3_Stop;
     thiz->GetStatus             = IDirectFBVideoProvider_Libmpeg3_GetStatus;
     thiz->GetPos                = IDirectFBVideoProvider_Libmpeg3_GetPos;
     thiz->SetPlaybackFlags      = IDirectFBVideoProvider_Libmpeg3_SetPlaybackFlags;
#ifdef HAVE_FUSIONSOUND
     thiz->SetVolume             = IDirectFBVideoProvider_Libmpeg3_SetVolume;
     thiz->GetVolume             = IDirectFBVideoProvider_Libmpeg3_GetVolume;
#endif
     thiz->CreateEventBuffer     = IDirectFBVideoProvider_Libmpeg3_CreateEventBuffer;
     thiz->AttachEventBuffer     = IDirectFBVideoProvider_Libmpeg3_AttachEventBuffer;
     thiz->EnableEvents          = IDirectFBVideoProvider_Libmpeg3_EnableEvents;
     thiz->DisableEvents         = IDirectFBVideoProvider_Libmpeg3_DisableEvents;
     thiz->DetachEventBuffer     = IDirectFBVideoProvider_Libmpeg3_DetachEventBuffer;
     thiz->SetDestination        = IDirectFBVideoProvider_Libmpeg3_SetDestination;

     return DFB_OK;

error:
#ifdef HAVE_FUSIONSOUND
     if (data->audio.playback)
          data->audio.playback->Release( data->audio.playback );

     if (data->audio.stream)
          data->audio.stream->Release( data->audio.stream );

     if (data->audio.sound)
          data->audio.sound->Release( data->audio.sound );
#endif

     if (data->file)
          mpeg3_close( data->file );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
