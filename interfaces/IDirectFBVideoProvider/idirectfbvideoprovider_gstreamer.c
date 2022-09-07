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
#include <fusionsound_limits.h>
#endif
#include <gst/app/gstappsink.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbvideoprovider.h>

D_DEBUG_DOMAIN( VideoProvider_GST, "VideoProvider/GST", "GStreamer Video Provider" );

static DFBResult Probe    ( IDirectFBVideoProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBVideoProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBVideoProvider, GSTREAMER )

/**********************************************************************************************************************/

typedef struct {
     DirectLink            link;
     IDirectFBEventBuffer *buffer;
} EventLink;

typedef struct {
     int                            ref;                    /* reference counter */

     IDirectFB                     *idirectfb;

     gboolean                       seekable;

     GstElement                    *pipeline;
     GstElement                    *decode;
     int                            error;
     int                            parsed_video;
     GstElement                    *convert_video;
     GstElement                    *decode_video;
     GstElement                    *queue_video;
     GstElement                    *appsink_video;
#ifdef HAVE_FUSIONSOUND
     int                            parsed_audio;
     GstElement                    *convert_audio;
     GstElement                    *decode_audio;
     GstElement                    *queue_audio;
     GstElement                    *appsink_audio;
     GstElement                    *appsrc_audio;
#endif

     DFBSurfaceDescription          desc;

     double                         rate;
     DFBVideoProviderStatus         status;
     double                         speed;
     DFBVideoProviderPlaybackFlags  flags;

     DirectThread                  *video_thread;
     DirectMutex                    video_lock;
     DirectWaitQueue                video_cond;

     bool                           seeked;
     gint64                         seek_time;

     IDirectFBSurface              *video_dest;

#ifdef HAVE_FUSIONSOUND
     gulong                         audio_id;

     gint                           audio_channels;
     gint                           audio_rate;

     IFusionSound                  *audio_sound;
     IFusionSoundStream            *audio_stream;
     IFusionSoundPlayback          *audio_playback;

     float                          audio_volume;
#endif

     DVFrameCallback                frame_callback;
     void                          *frame_callback_context;

     DirectLink                    *events;
     DFBVideoProviderEventType      events_mask;
     DirectMutex                    events_lock;
} IDirectFBVideoProvider_GSTREAMER_data;

/**********************************************************************************************************************/

static void
decode_unknown_type( GstBin   *bin,
                     GstPad   *pad,
                     GstCaps  *caps,
                     gpointer  ptr )
{
     D_DEBUG_AT( VideoProvider_GST, "%s( caps %s )\n", __FUNCTION__, gst_caps_to_string( caps ) );
}

static void
decode_pad_added( GstElement *element,
                  GstPad     *srcpad,
                  gpointer    ptr )
{
     GstPadLinkReturn                       ret;
     GstPad                                *sinkpad;
     GstCaps                               *caps = gst_pad_query_caps( srcpad, NULL );
     IDirectFBVideoProvider_GSTREAMER_data *data = ptr;
     int                                    err  = 1;

     D_DEBUG_AT( VideoProvider_GST, "%s( caps %s )\n", __FUNCTION__, gst_caps_to_string( caps ) );

     if (strstr( gst_caps_to_string( caps ), "video" )) {
          D_DEBUG_AT( VideoProvider_GST, "  -> linking video pad\n" );
          sinkpad = gst_element_get_static_pad( data->convert_video, "sink" );
          ret = gst_pad_link( srcpad, sinkpad );
          gst_object_unref( sinkpad );
     }
#ifdef HAVE_FUSIONSOUND
     else if (strstr( gst_caps_to_string( caps ), "audio" )) {
          D_DEBUG_AT( VideoProvider_GST, "  -> linking audio pad\n" );
          sinkpad = gst_element_get_static_pad( data->convert_audio, "sink" );
          ret = gst_pad_link( srcpad, sinkpad );
          gst_object_unref( sinkpad );
     }
#endif
     else {
          D_DEBUG_AT( VideoProvider_GST, "  -> unhandled caps\n" );
          return;
     }

     switch (ret) {
          case GST_PAD_LINK_OK:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> OK\n" );
               err = 0;
               break;
          case GST_PAD_LINK_WRONG_HIERARCHY:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> WRONG_HIERARCHY\n" );
               break;
          case GST_PAD_LINK_WAS_LINKED:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> WAS_LINKED\n" );
               break;
          case GST_PAD_LINK_WRONG_DIRECTION:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> WRONG_DIRECTION\n" );
               break;
          case GST_PAD_LINK_NOFORMAT:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> NOFORMAT\n" );
               break;
          case GST_PAD_LINK_NOSCHED:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> NOSCHED\n" );
               break;
          case GST_PAD_LINK_REFUSED:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> REFUSED\n" );
               break;
          default:
               break;
     }

     direct_mutex_lock( &data->video_lock );

     if (err)
          data->error = 1;

     direct_waitqueue_signal( &data->video_cond );

     direct_mutex_unlock( &data->video_lock );
}

static void
decode_video_pad_added( GstElement *element,
                        GstPad     *srcpad,
                        gpointer    ptr )
{
     GstPadLinkReturn                       ret;
     GstPad                                *sinkpad;
     GstCaps                               *caps = gst_pad_query_caps( srcpad, NULL );
     GstStructure                          *str  = gst_caps_get_structure( caps, 0 );
     IDirectFBVideoProvider_GSTREAMER_data *data = ptr;
     int                                    err  = 1;

     D_DEBUG_AT( VideoProvider_GST, "%s( caps %s )\n", __FUNCTION__, gst_caps_to_string( caps ) );

     data->desc.flags |= DSDESC_WIDTH | DSDESC_HEIGHT;
     gst_structure_get_int( str, "width", &data->desc.width );
     gst_structure_get_int( str, "height", &data->desc.height );

     gint framerate_num, framerate_den;
     gst_structure_get_fraction( str, "framerate", &framerate_num, &framerate_den );
     data->rate = (double) framerate_num / framerate_den;

     sinkpad = gst_element_get_static_pad( data->queue_video, "sink" );
     ret = gst_pad_link( srcpad, sinkpad );
     gst_object_unref( sinkpad );

     switch (ret) {
          case GST_PAD_LINK_OK:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> OK\n" );
               err = 0;
               break;
          case GST_PAD_LINK_WRONG_HIERARCHY:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> WRONG_HIERARCHY\n" );
               break;
          case GST_PAD_LINK_WAS_LINKED:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> WAS_LINKED\n" );
               break;
          case GST_PAD_LINK_WRONG_DIRECTION:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> WRONG_DIRECTION\n" );
               break;
          case GST_PAD_LINK_NOFORMAT:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> NOFORMAT\n" );
               break;
          case GST_PAD_LINK_NOSCHED:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> NOSCHED\n" );
               break;
          case GST_PAD_LINK_REFUSED:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> REFUSED\n" );
               break;
          default:
               break;
     }

     direct_mutex_lock( &data->video_lock );

     if (err)
          data->error = 1;
     else
          data->parsed_video = 1;

     direct_waitqueue_signal( &data->video_cond );

     direct_mutex_unlock( &data->video_lock );
}

#ifdef HAVE_FUSIONSOUND
static void
decode_audio_pad_added( GstElement *element,
                        GstPad     *srcpad,
                        gpointer    ptr )
{
     GstPadLinkReturn                       ret;
     GstPad                                *sinkpad;
     GstCaps                               *caps = gst_pad_query_caps( srcpad, NULL );
     GstStructure                          *str  = gst_caps_get_structure( caps, 0 );
     IDirectFBVideoProvider_GSTREAMER_data *data = ptr;
     int                                    err  = 1;

     D_DEBUG_AT( VideoProvider_GST, "%s( caps %s )\n", __FUNCTION__, gst_caps_to_string( caps ) );

     gst_structure_get_int( str, "rate", &data->audio_rate );
     gst_structure_get_int( str, "channels", &data->audio_channels );

     sinkpad = gst_element_get_static_pad( data->queue_audio, "sink" );
     ret = gst_pad_link( srcpad, sinkpad );
     gst_object_unref( sinkpad );

     switch (ret) {
          case GST_PAD_LINK_OK:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> OK\n" );
               err = 0;
               break;
          case GST_PAD_LINK_WRONG_HIERARCHY:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> WRONG_HIERARCHY\n" );
               break;
          case GST_PAD_LINK_WAS_LINKED:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> WAS_LINKED\n" );
               break;
          case GST_PAD_LINK_WRONG_DIRECTION:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> WRONG_DIRECTION\n" );
               break;
          case GST_PAD_LINK_NOFORMAT:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> NOFORMAT\n" );
               break;
          case GST_PAD_LINK_NOSCHED:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> NOSCHED\n" );
               break;
          case GST_PAD_LINK_REFUSED:
               D_DEBUG_AT( VideoProvider_GST, "  -> gst_pad_link --> REFUSED\n" );
               break;
          default:
               break;
     }

     direct_mutex_lock( &data->video_lock );

     if (err)
          data->error = 1;
     else
          data->parsed_audio = 1;

     direct_waitqueue_signal( &data->video_cond );

     direct_mutex_unlock( &data->video_lock );
}
#endif

static void
dispatch_event( IDirectFBVideoProvider_GSTREAMER_data *data,
                DFBVideoProviderEventType              type )
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
GStreamerVideo( DirectThread *self,
                void         *arg )
{
     GstSample                             *sample;
     GstBuffer                             *buffer;
     IDirectFBSurface_data                 *dst_data;
     CoreSurfaceBufferLock                  lock;
     IDirectFBVideoProvider_GSTREAMER_data *data = arg;

     dst_data = data->video_dest->priv;
     if (!dst_data)
          return NULL;

     if (!dst_data->surface)
          return NULL;

     dispatch_event( data, DVPET_STARTED );

     while (data->status != DVSTATE_STOP) {
          sample = gst_app_sink_pull_sample( GST_APP_SINK( data->appsink_video) );

          if (sample)
               buffer = gst_sample_get_buffer( sample );

          direct_mutex_lock( &data->video_lock );

          if (data->seeked) {
               gst_element_seek_simple( data->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, data->seek_time);

               if (data->status == DVSTATE_FINISHED)
                    data->status = DVSTATE_PLAY;

               data->seeked = false;

               direct_mutex_unlock( &data->video_lock );
               continue;
          }

          if (!buffer) {
               if (data->flags & DVPLAY_LOOPING) {
                    data->seeked    = true;
                    data->seek_time = 0;
               }
               else {
                    if (data->status != DVSTATE_FINISHED && data->status != DVSTATE_STOP) {
                         data->status = DVSTATE_FINISHED;
                         dispatch_event( data, DVPET_FINISHED );
                    }
               }

               direct_mutex_unlock( &data->video_lock );
               usleep( 100 );
               continue;
          }

          dfb_surface_lock_buffer( dst_data->surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
          gst_buffer_extract( buffer, 0, lock.addr, gst_buffer_get_size( buffer ) );
          gst_buffer_unref( buffer );
          dfb_surface_unlock_buffer( dst_data->surface, &lock );

          if (data->frame_callback)
               data->frame_callback( data->frame_callback_context );

          direct_mutex_unlock( &data->video_lock );
     }

     return NULL;
}

#ifdef HAVE_FUSIONSOUND
static GstFlowReturn
GStreamerAudio( GstAppSink *appsink,
                gpointer    arg )
{
  GstSample                             *sample;
  GstBuffer                             *buffer;
  int                                    size;
  IDirectFBVideoProvider_GSTREAMER_data *data           = arg;
  int                                    bytespersample = 2 * data->audio_channels;
  u8                                     buf[bytespersample * data->audio_rate];

  sample = gst_app_sink_pull_sample( appsink );
  buffer = gst_sample_get_buffer( sample );
  size   = gst_buffer_extract( buffer, 0, buf, gst_buffer_get_size( buffer ) ) / bytespersample;

  data->audio_stream->Write( data->audio_stream, buf, size );

  return GST_FLOW_OK;
}
#endif

/**********************************************************************************************************************/

static void
IDirectFBVideoProvider_GSTREAMER_Destruct( IDirectFBVideoProvider *thiz )
{
     EventLink                             *link, *tmp;
     IDirectFBVideoProvider_GSTREAMER_data *data = thiz->priv;

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     thiz->Stop( thiz );

#ifdef HAVE_FUSIONSOUND
     if (data->audio_playback)
          data->audio_playback->Release( data->audio_playback );

     if (data->audio_stream)
          data->audio_stream->Release( data->audio_stream );

     if (data->audio_sound)
          data->audio_sound->Release( data->audio_sound );
#endif

     gst_object_unref( data->pipeline );

     direct_waitqueue_deinit( &data->video_cond );
     direct_mutex_deinit( &data->video_lock );

     direct_list_foreach_safe (link, tmp, data->events) {
          direct_list_remove( &data->events, &link->link );
          link->buffer->Release( link->buffer );
          D_FREE( link );
     }

     direct_mutex_deinit( &data->events_lock );

     gst_deinit();

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBVideoProvider_GSTREAMER_AddRef( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DR_OK;
}

static DirectResult
IDirectFBVideoProvider_GSTREAMER_Release( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBVideoProvider_GSTREAMER_Destruct( thiz );

     return DR_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_GetCapabilities( IDirectFBVideoProvider       *thiz,
                                                  DFBVideoProviderCapabilities *ret_caps )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_caps)
          return DFB_INVARG;

     *ret_caps = DVCAPS_BASIC | DVCAPS_SPEED;
     if (data->seekable)
          *ret_caps |= DVCAPS_SEEK;
#ifdef HAVE_FUSIONSOUND
     if (data->audio_playback)
          *ret_caps |= DVCAPS_VOLUME;
#endif

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_GetSurfaceDescription( IDirectFBVideoProvider *thiz,
                                                        DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_GetStreamDescription( IDirectFBVideoProvider *thiz,
                                                       DFBStreamDescription   *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DVSCAPS_VIDEO;

     ret_desc->video.framerate = data->rate;
     ret_desc->video.aspect    = (double) data->desc.width / data->desc.height;

#ifdef HAVE_FUSIONSOUND
     if (data->audio_stream) {
          ret_desc->caps |= DVSCAPS_AUDIO;

          ret_desc->audio.samplerate = data->audio_rate;
          ret_desc->audio.channels   = data->audio_channels;
     }
#endif

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_PlayTo( IDirectFBVideoProvider *thiz,
                                         IDirectFBSurface       *destination,
                                         const DFBRectangle     *dest_rect,
                                         DVFrameCallback         callback,
                                         void                   *ctx )
{
     IDirectFBSurface_data *dst_data;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (!destination)
          return DFB_INVARG;

     dst_data = destination->priv;
     if (!dst_data)
          return DFB_DEAD;

     if (data->video_thread)
          return DFB_OK;

     direct_mutex_lock( &data->video_lock );

     data->video_dest             = destination;
     data->frame_callback         = callback;
     data->frame_callback_context = ctx;

     data->status = DVSTATE_PLAY;

     data->video_thread = direct_thread_create( DTT_DEFAULT, GStreamerVideo, data, "GStreamer Video" );

#ifdef HAVE_FUSIONSOUND
     if (data->audio_stream) {
          data->audio_id = g_signal_connect( data->appsink_audio, "new-sample", G_CALLBACK( GStreamerAudio ), data );
     }
#endif

     gst_element_set_state( data->pipeline, GST_STATE_PLAYING );

     direct_mutex_unlock( &data->video_lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_Stop( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->status == DVSTATE_STOP)
          return DFB_OK;

     data->status = DVSTATE_STOP;

     if (data->video_thread) {
          gst_element_set_state( data->pipeline, GST_STATE_NULL );
          direct_thread_join( data->video_thread );
          direct_thread_destroy( data->video_thread );
          data->video_thread = NULL;
     }

#ifdef HAVE_FUSIONSOUND
     if (data->audio_id) {
          g_signal_handler_disconnect( data->appsink_audio, data->audio_id );
          data->audio_id = 0;
     }
#endif

     dispatch_event( data, DVPET_STOPPED );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_GetStatus( IDirectFBVideoProvider *thiz,
                                            DFBVideoProviderStatus *ret_status )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_status)
          return DFB_INVARG;

     *ret_status = data->status;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_SeekTo( IDirectFBVideoProvider *thiz,
                                         double                  seconds )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (seconds < 0.0)
          return DFB_INVARG;

     if (!data->seekable)
          return DFB_UNSUPPORTED;

     direct_mutex_lock( &data->video_lock );

     data->seeked    = true;
     data->seek_time = seconds * GST_SECOND;

     direct_mutex_unlock( &data->video_lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_GetPos( IDirectFBVideoProvider *thiz,
                                         double                 *ret_seconds )
{
     gint64 position;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DFB_INVARG;

     if (gst_element_query_position( data->appsink_video, GST_FORMAT_TIME, &position )) {
          *ret_seconds = position / 1000000000.0;
          return DFB_OK;
     }

     *ret_seconds = 0.0;

     return DFB_UNSUPPORTED;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_GetLength( IDirectFBVideoProvider *thiz,
                                            double                 *ret_seconds )
{
     gint64 duration;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DFB_INVARG;

     if (gst_element_query_duration( data->pipeline, GST_FORMAT_TIME, &duration )) {
          *ret_seconds = duration / 1000000000.0;
          return DFB_OK;
     }

     *ret_seconds = 0.0;

     return DFB_UNSUPPORTED;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_SetPlaybackFlags( IDirectFBVideoProvider        *thiz,
                                                   DFBVideoProviderPlaybackFlags  flags )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (flags & ~DVPLAY_LOOPING)
          return DFB_UNSUPPORTED;

     if (flags & DVPLAY_LOOPING && !data->seekable)
          return DFB_UNSUPPORTED;

     data->flags = flags;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_SetSpeed( IDirectFBVideoProvider *thiz,
                                           double                  multiplier )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (multiplier != 0.0 && multiplier != 1.0)
          return DFB_UNSUPPORTED;

     if (multiplier == data->speed)
          return DFB_OK;

     direct_mutex_lock( &data->video_lock );

     gst_element_set_state( data->pipeline, multiplier == 0.0 ? GST_STATE_PAUSED : GST_STATE_PLAYING );

     data->speed = multiplier;

     dispatch_event( data, DVPET_SPEEDCHANGE );

     direct_mutex_unlock( &data->video_lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_GetSpeed( IDirectFBVideoProvider *thiz,
                                           double                 *ret_multiplier )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_multiplier)
          return DFB_INVARG;

     *ret_multiplier = data->speed;

     return DFB_OK;
}

#ifdef HAVE_FUSIONSOUND
static DFBResult
IDirectFBVideoProvider_GSTREAMER_SetVolume( IDirectFBVideoProvider *thiz,
                                            float                   level )
{
     DFBResult ret = DFB_UNSUPPORTED;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_GSTREAMER_GetVolume( IDirectFBVideoProvider *thiz,
                                            float                  *ret_level )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_level)
          return DFB_INVARG;

     *ret_level = data->audio_volume;

     return DFB_OK;
}
#endif

static DFBResult
IDirectFBVideoProvider_GSTREAMER_CreateEventBuffer( IDirectFBVideoProvider  *thiz,
                                                    IDirectFBEventBuffer   **ret_interface )
{
     DFBResult             ret;
     IDirectFBEventBuffer *buffer;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_GSTREAMER_AttachEventBuffer( IDirectFBVideoProvider *thiz,
                                                    IDirectFBEventBuffer   *buffer )
{
     DFBResult  ret;
     EventLink *link;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_GSTREAMER_EnableEvents( IDirectFBVideoProvider    *thiz,
                                               DFBVideoProviderEventType  mask )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (mask & ~DVPET_ALL)
          return DFB_INVARG;

     data->events_mask |= mask;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_DisableEvents( IDirectFBVideoProvider    *thiz,
                                                DFBVideoProviderEventType  mask )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     if (mask & ~DVPET_ALL)
          return DFB_INVARG;

     data->events_mask &= ~mask;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_GSTREAMER_DetachEventBuffer( IDirectFBVideoProvider *thiz,
                                                    IDirectFBEventBuffer   *buffer )
{
     DFBResult  ret = DFB_ITEMNOTFOUND;
     EventLink *link;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

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

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBVideoProvider_ProbeContext *ctx )
{
     return DFB_OK;
}

static DFBResult
Construct( IDirectFBVideoProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     char                      uri[PATH_MAX];
     GstBin                   *bin;
     GstCaps                  *video_caps;
#ifdef HAVE_FUSIONSOUND
     GstCaps                  *audio_caps;
#endif
     GError                   *err = NULL;
     int                       max_signals = 5;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBVideoProvider_GSTREAMER )

     D_DEBUG_AT( VideoProvider_GST, "%s( %p )\n", __FUNCTION__, thiz );

     /* Check for valid filename. */
     if (!buffer_data->filename) {
          DIRECT_DEALLOCATE_INTERFACE( thiz );
          return DFB_UNSUPPORTED;
     }

     if (g_strstr_len( buffer_data->filename, -1, "://" )) {
          g_strlcpy( uri, buffer_data->filename, sizeof(uri) );
     }
     else {
          if (*buffer_data->filename == '/')
               g_snprintf( uri, sizeof(uri), "file://%s", buffer_data->filename );
          else
               g_snprintf( uri, sizeof(uri), "file://%s/%s", get_current_dir_name(), buffer_data->filename );
     }

     data->ref       = 1;
     data->idirectfb = idirectfb;

     if (!gst_init_check( NULL, NULL, &err )) {
          D_ERROR( "VideoProvider/GST: Failed to initialize GStreamer!\n" );
          if (err) {
               g_error_free( err );
          }
          DIRECT_DEALLOCATE_INTERFACE( thiz );
          return DFB_INIT;
     }

     data->pipeline = gst_pipeline_new( "uri-decode-pipeline" );

     data->decode        = gst_element_factory_make( "uridecodebin", "uri-decode-bin" );
     data->convert_video = gst_element_factory_make( "videoconvert", "convert-video" );
     data->decode_video  = gst_element_factory_make( "decodebin",    "decode-video" );
     data->queue_video   = gst_element_factory_make( "queue",        "queue-video" );
     data->appsink_video = gst_element_factory_make( "appsink",      "sink-buffer-video" );
#ifdef HAVE_FUSIONSOUND
     data->convert_audio = gst_element_factory_make( "audioconvert", "convert-audio");
     data->decode_audio  = gst_element_factory_make( "decodebin",    "decode-audio" );
     data->queue_audio   = gst_element_factory_make( "queue",        "queue-audio" );
     data->appsink_audio = gst_element_factory_make( "appsink",      "sink-buffer-audio" );
#endif

     if (!data->convert_video || !data->decode_video || !data->queue_video || !data->appsink_video ||
#ifdef HAVE_FUSIONSOUND
         !data->convert_audio || !data->decode_audio || !data->queue_audio || !data->appsink_audio ||
#endif
         !data->pipeline || !data->decode) {
          D_DEBUG_AT( VideoProvider_GST, "Failed to create some GStreamer elements\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     g_object_set( data->decode, "uri", uri, NULL );

     g_signal_connect( data->decode,       "pad-added",    G_CALLBACK(decode_pad_added), data );
     g_signal_connect( data->decode,       "unknown-type", G_CALLBACK(decode_unknown_type), data );
     g_signal_connect( data->decode_video, "pad-added",    G_CALLBACK(decode_video_pad_added), data );
#ifdef HAVE_FUSIONSOUND
     g_signal_connect( data->decode_audio, "pad-added",    G_CALLBACK(decode_audio_pad_added), data );
#endif

     bin = GST_BIN( data->pipeline );
     gst_bin_add( bin, data->decode );

     gst_bin_add_many( bin, data->convert_video, data->decode_video, data->queue_video, data->appsink_video, NULL );
#ifdef HAVE_FUSIONSOUND
     gst_bin_add_many( bin, data->convert_audio, data->decode_audio, data->queue_audio, data->appsink_audio, NULL );
#endif

     data->desc.flags       = DSDESC_PIXELFORMAT;
     data->desc.pixelformat = dfb_primary_layer_pixelformat();

     switch (data->desc.pixelformat) {
          case DSPF_ARGB1555:
               video_caps = gst_caps_new_simple( "video/x-raw", "format", G_TYPE_STRING, "RGB15", NULL );
               break;
          case DSPF_RGB16:
               video_caps = gst_caps_new_simple( "video/x-raw", "format", G_TYPE_STRING, "RGB16", NULL );
               break;
          case DSPF_RGB24:
#ifdef WORDS_BIGENDIAN
               video_caps = gst_caps_new_simple( "video/x-raw", "format", G_TYPE_STRING, "RGB", NULL );
#else
               video_caps = gst_caps_new_simple( "video/x-raw", "format", G_TYPE_STRING, "BGR", NULL );
#endif
               break;
          case DSPF_RGB32:
          case DSPF_ARGB:
#ifdef WORDS_BIGENDIAN
               video_caps = gst_caps_new_simple( "video/x-raw", "format", G_TYPE_STRING, "ARGB", NULL );
#else
               video_caps = gst_caps_new_simple( "video/x-raw", "format", G_TYPE_STRING, "BGRA", NULL );
#endif
               break;
          case DSPF_ABGR:
#ifdef WORDS_BIGENDIAN
               video_caps = gst_caps_new_simple( "video/x-raw", "format", G_TYPE_STRING, "ABGR", NULL );
#else
               video_caps = gst_caps_new_simple( "video/x-raw", "format", G_TYPE_STRING, "RGBA", NULL );
#endif
               break;
          default:
               D_ERROR( "VideoProvider/GST: Unknown pixel format!\n" );
               ret = DFB_FAILURE;
               goto error;
     }

     gst_element_link_filtered( data->convert_video, data->decode_video, video_caps );
     gst_element_link( data->queue_video, data->appsink_video );

#ifdef HAVE_FUSIONSOUND
     audio_caps = gst_caps_new_simple( "audio/x-raw", "format", G_TYPE_STRING, "S16LE", NULL );
     gst_element_link_filtered( data->convert_audio, data->decode_audio, audio_caps );
     gst_element_link( data->queue_audio, data->appsink_audio );
#endif

     direct_mutex_init( &data->video_lock );
     direct_waitqueue_init( &data->video_cond );

     direct_mutex_lock( &data->video_lock );

     gst_element_set_state( data->pipeline, GST_STATE_PAUSED );

     while (1) {
          direct_waitqueue_wait_timeout( &data->video_cond, &data->video_lock, 1000000 );

          if (data->error || --max_signals < 0)
               break;

          if (data->parsed_video) {
#ifdef HAVE_FUSIONSOUND
               if (!data->parsed_audio) {
                    max_signals = 0;
                    continue;
               }
#endif
               break;
          }
     }

#ifdef HAVE_FUSIONSOUND
     if (!data->parsed_audio) {
          gst_element_set_state( data->queue_audio,   GST_STATE_NULL );
          gst_element_set_state( data->decode_audio,  GST_STATE_NULL );
          gst_element_set_state( data->convert_audio, GST_STATE_NULL );
          gst_element_set_state( data->appsink_audio, GST_STATE_NULL );
          gst_bin_remove_many( bin,
                               data->convert_audio, data->decode_audio, data->queue_audio, data->appsink_audio, NULL );

     }
#endif

     direct_mutex_unlock( &data->video_lock );

     if (data->error) {
          D_DEBUG_AT( VideoProvider_GST, "VideoProvider/GST: Failed to prepare pipeline\n" );
          ret = DFB_FAILURE;
          goto error;
     }

#ifdef HAVE_FUSIONSOUND
     if (data->parsed_audio &&
         FusionSoundInit( NULL, NULL ) == DR_OK && FusionSoundCreate( &data->audio_sound ) == DR_OK) {
          FSStreamDescription dsc;

          if (data->audio_channels > FS_MAX_CHANNELS)
               data->audio_channels = FS_MAX_CHANNELS;

          dsc.flags        = FSSDF_BUFFERSIZE | FSSDF_CHANNELS | FSSDF_SAMPLEFORMAT | FSSDF_SAMPLERATE;
          dsc.channels     = data->audio_channels;
          dsc.samplerate   = data->audio_rate;
          dsc.buffersize   = dsc.samplerate / 10;
          dsc.sampleformat = FSSF_S16;

          ret = data->audio_sound->CreateStream( data->audio_sound, &dsc, &data->audio_stream );
          if (ret != DFB_OK) {
               D_ERROR( "VideoProvider/GST: Failed to create FusionSound stream!\n" );
               goto error;
          }
          else {
               data->audio_stream->GetPlayback( data->audio_stream, &data->audio_playback );
          }
     }
     else if (data->parsed_audio) {
          D_ERROR( "VideoProvider/GST: Failed to initialize/create FusionSound!\n" );
          goto error;
     }

     if (data->parsed_audio)
          g_object_set( data->appsink_audio, "emit-signals", TRUE, "sync", FALSE, NULL );
#endif

     data->status = DVSTATE_STOP;
     data->speed  = 1.0;

     GstQuery *query = gst_query_new_seeking( GST_FORMAT_TIME );
     if (gst_element_query( data->pipeline, query )) {
          gint64 start, end;
          gst_query_parse_seeking( query, NULL, &data->seekable, &start, &end );
     }
     gst_query_unref( query );

     data->events_mask = DVPET_ALL;

#ifdef HAVE_FUSIONSOUND
     data->audio_volume = 1.0;
#endif

     thiz->AddRef                = IDirectFBVideoProvider_GSTREAMER_AddRef;
     thiz->Release               = IDirectFBVideoProvider_GSTREAMER_Release;
     thiz->GetCapabilities       = IDirectFBVideoProvider_GSTREAMER_GetCapabilities;
     thiz->GetSurfaceDescription = IDirectFBVideoProvider_GSTREAMER_GetSurfaceDescription;
     thiz->GetStreamDescription  = IDirectFBVideoProvider_GSTREAMER_GetStreamDescription;
     thiz->PlayTo                = IDirectFBVideoProvider_GSTREAMER_PlayTo;
     thiz->Stop                  = IDirectFBVideoProvider_GSTREAMER_Stop;
     thiz->GetStatus             = IDirectFBVideoProvider_GSTREAMER_GetStatus;
     thiz->SeekTo                = IDirectFBVideoProvider_GSTREAMER_SeekTo;
     thiz->GetPos                = IDirectFBVideoProvider_GSTREAMER_GetPos;
     thiz->GetLength             = IDirectFBVideoProvider_GSTREAMER_GetLength;
     thiz->SetPlaybackFlags      = IDirectFBVideoProvider_GSTREAMER_SetPlaybackFlags;
     thiz->SetSpeed              = IDirectFBVideoProvider_GSTREAMER_SetSpeed;
     thiz->GetSpeed              = IDirectFBVideoProvider_GSTREAMER_GetSpeed;
#ifdef HAVE_FUSIONSOUND
     thiz->SetVolume             = IDirectFBVideoProvider_GSTREAMER_SetVolume;
     thiz->GetVolume             = IDirectFBVideoProvider_GSTREAMER_GetVolume;
#endif
     thiz->CreateEventBuffer     = IDirectFBVideoProvider_GSTREAMER_CreateEventBuffer;
     thiz->AttachEventBuffer     = IDirectFBVideoProvider_GSTREAMER_AttachEventBuffer;
     thiz->EnableEvents          = IDirectFBVideoProvider_GSTREAMER_EnableEvents;
     thiz->DisableEvents         = IDirectFBVideoProvider_GSTREAMER_DisableEvents;
     thiz->DetachEventBuffer     = IDirectFBVideoProvider_GSTREAMER_DetachEventBuffer;

     return DFB_OK;

error:
#ifdef HAVE_FUSIONSOUND
     if (data->audio_playback)
          data->audio_playback->Release( data->audio_playback );

     if (data->audio_stream)
          data->audio_stream->Release( data->audio_stream );

     if (data->audio_sound)
          data->audio_sound->Release( data->audio_sound );

     if (data->parsed_audio) {
          gst_element_set_state( data->queue_audio,   GST_STATE_NULL );
          gst_element_set_state( data->decode_audio,  GST_STATE_NULL );
          gst_element_set_state( data->convert_audio, GST_STATE_NULL );
          gst_element_set_state( data->appsink_audio, GST_STATE_NULL );
          gst_bin_remove_many( bin,
                               data->convert_audio, data->decode_audio, data->queue_audio, data->appsink_audio, NULL );

     }
#endif

     if (data->pipeline)
          gst_object_unref( data->pipeline );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
