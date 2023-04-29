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
#include <direct/filesystem.h>
#include <direct/memcpy.h>
#include <direct/thread.h>
#include <display/idirectfbsurface.h>
#ifdef HAVE_FUSIONSOUND
#include <fusionsound.h>
#endif
#include <libswfdec/swfdec.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbvideoprovider.h>

D_DEBUG_DOMAIN( VideoProvider_Swfdec, "VideoProvider/Swfdec", "Swfdec Video Provider" );

static DFBResult Probe    ( IDirectFBVideoProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBVideoProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBVideoProvider, Swfdec )

/**********************************************************************************************************************/

typedef struct {
     DirectLink   link;
     SwfdecAudio *audio;
} AudioLink;

typedef struct {
     DirectLink            link;
     IDirectFBEventBuffer *buffer;
} EventLink;

typedef struct {
     int                        ref;                    /* reference counter */

     IDirectFB                 *idirectfb;

     DFBBoolean                 seekable;

     SwfdecPlayer              *player;
     DirectMutex                player_lock;

     int                        mouse_x;
     int                        mouse_y;

     DFBSurfaceDescription      desc;

     double                     rate;
     DFBVideoProviderStatus     status;
     double                     speed;

     struct {
          DirectThread         *thread;
          DirectMutex           lock;
          DirectWaitQueue       cond;

          long                  pos;

          long                  seek;

          IDirectFBSurface     *dest;
          DFBRectangle          rect;
     } video;

#ifdef HAVE_FUSIONSOUND
     struct {
          DirectThread         *thread;
          DirectMutex           lock;
          DirectWaitQueue       cond;

          DirectLink           *st;

          unsigned int          offset;

          IFusionSound         *sound;
          IFusionSoundStream   *stream;
          IFusionSoundPlayback *playback;

          float                 volume;
     } audio;
#endif

     DVFrameCallback            frame_callback;
     void                      *frame_callback_context;

     DirectLink                *events;
     DFBVideoProviderEventType  events_mask;
     DirectMutex                events_lock;
} IDirectFBVideoProvider_Swfdec_data;

/**********************************************************************************************************************/

typedef struct {
     SwfdecLoader         loader;

     IDirectFBDataBuffer *buffer;

     DirectThread        *thread;
} DataBufferLoader;

typedef struct {
     SwfdecLoaderClass class;
} DataBufferLoaderClass;

G_DEFINE_TYPE (DataBufferLoader, databuffer_loader, SWFDEC_TYPE_LOADER)

static void *
SwfLoader( DirectThread *thread,
           void         *arg )
{
     DFBResult         ret;
     DataBufferLoader *databuffer_loader = arg;

     while (1) {
          char          buf[4096];
          unsigned int  len;
          SwfdecBuffer *buffer;

          ret = databuffer_loader->buffer->WaitForData( databuffer_loader->buffer, sizeof(buf) );
          if (ret == DFB_OK)
               databuffer_loader->buffer->GetData( databuffer_loader->buffer, sizeof(buf), buf, &len );

          if (ret)
               break;

          buffer = swfdec_buffer_new_and_alloc( len );
          direct_memcpy( buffer->data, buf, len );
          swfdec_loader_push( &databuffer_loader->loader, buffer );
     }

     swfdec_loader_eof( &databuffer_loader->loader );

     return NULL;
}

static void
databuffer_loader_load( SwfdecLoader        *loader,
                        SwfdecLoader        *parent,
                        SwfdecLoaderRequest  request,
                        const char          *data,
                        gsize                data_len )
{
     DFBResult                 ret;
     SwfdecBuffer             *buffer;
     unsigned int              length            = 0;
     DataBufferLoader         *databuffer_loader = (DataBufferLoader*) loader;
     IDirectFBDataBuffer_data *buffer_data       = databuffer_loader->buffer->priv;

     if (request == SWFDEC_LOADER_REQUEST_POST)
          return;

     ret = databuffer_loader->buffer->GetLength( databuffer_loader->buffer, &length );
     if (ret == DFB_OK)
          swfdec_loader_set_size( loader, length );

     swfdec_loader_open( loader, NULL );

     if (buffer_data->buffer) {
          buffer = swfdec_buffer_new_for_data( buffer_data->buffer, buffer_data->length );
          swfdec_loader_push( loader, buffer );
          swfdec_loader_eof( loader );
     }
     else {
          char         buf[4096];
          unsigned int len;

          ret = databuffer_loader->buffer->WaitForData( databuffer_loader->buffer, sizeof(buf) );
          if (ret == DFB_OK)
               ret = databuffer_loader->buffer->GetData( databuffer_loader->buffer, sizeof(buf), buf, &len );

          if (ret)
               return;

          buffer = swfdec_buffer_new_and_alloc( len );
          direct_memcpy( buffer->data, buf, len );
          swfdec_loader_push( &databuffer_loader->loader, buffer );

          if (!length || length > len)
               databuffer_loader->thread = direct_thread_create( DTT_DEFAULT, SwfLoader, databuffer_loader,
                                                                 "Swf Loader" );
          else
               swfdec_loader_eof( &databuffer_loader->loader );
     }
}

static void
databuffer_loader_close( SwfdecLoader *loader )
{
     DataBufferLoader *databuffer_loader = (DataBufferLoader*) loader;

     if (databuffer_loader->thread) {
          direct_thread_cancel( databuffer_loader->thread );
          direct_thread_join( databuffer_loader->thread );
          direct_thread_destroy( databuffer_loader->thread );
     }

     /* Decrease the data buffer reference counter. */
     if (databuffer_loader->buffer)
          databuffer_loader->buffer->Release( databuffer_loader->buffer );
}

static void
databuffer_loader_class_init( DataBufferLoaderClass *klass )
{
     SwfdecLoaderClass *loader_class = SWFDEC_LOADER_CLASS (klass);

     loader_class->load  = databuffer_loader_load;
     loader_class->close = databuffer_loader_close;
}

static void
databuffer_loader_init( DataBufferLoader *loader )
{
}

static SwfdecKey
symbol_translate( DFBInputDeviceKeySymbol symbol )
{
     switch (symbol) {
          case DIKS_BACKSPACE:
               return SWFDEC_KEY_BACKSPACE;
          case DIKS_TAB:
               return SWFDEC_KEY_TAB;
          case DIKS_CLEAR:
               return SWFDEC_KEY_CLEAR;
          case DIKS_ENTER:
               return SWFDEC_KEY_ENTER;
          case DIKS_SHIFT:
               return SWFDEC_KEY_SHIFT;
          case DIKS_CONTROL:
               return SWFDEC_KEY_CONTROL;
          case DIKS_ALT:
               return SWFDEC_KEY_ALT;
          case DIKS_CAPS_LOCK:
               return SWFDEC_KEY_CAPS_LOCK;
          case DIKS_ESCAPE:
               return SWFDEC_KEY_ESCAPE;
          case DIKS_SPACE:
               return SWFDEC_KEY_SPACE;
          case DIKS_PAGE_UP:
               return SWFDEC_KEY_PAGE_UP;
          case DIKS_PAGE_DOWN:
               return SWFDEC_KEY_PAGE_DOWN;
          case DIKS_END:
               return SWFDEC_KEY_END;
          case DIKS_HOME:
               return SWFDEC_KEY_HOME;
          case DIKS_CURSOR_LEFT:
               return SWFDEC_KEY_LEFT;
          case DIKS_CURSOR_UP:
               return SWFDEC_KEY_UP;
          case DIKS_CURSOR_RIGHT:
               return SWFDEC_KEY_RIGHT;
          case DIKS_CURSOR_DOWN:
               return SWFDEC_KEY_DOWN;
          case DIKS_INSERT:
               return SWFDEC_KEY_INSERT;
          case DIKS_DELETE:
               return SWFDEC_KEY_DELETE;
          case DIKS_HELP:
               return SWFDEC_KEY_HELP;
          case DIKS_0:
               return SWFDEC_KEY_0;
          case DIKS_1:
               return SWFDEC_KEY_1;
          case DIKS_2:
               return SWFDEC_KEY_2;
          case DIKS_3:
               return SWFDEC_KEY_3;
          case DIKS_4:
               return SWFDEC_KEY_4;
          case DIKS_5:
               return SWFDEC_KEY_5;
          case DIKS_6:
               return SWFDEC_KEY_6;
          case DIKS_7:
               return SWFDEC_KEY_7;
          case DIKS_8:
               return SWFDEC_KEY_8;
          case DIKS_9:
               return SWFDEC_KEY_9;
          case DIKS_SMALL_A:
               return SWFDEC_KEY_A;
          case DIKS_SMALL_B:
               return SWFDEC_KEY_B;
          case DIKS_SMALL_C:
               return SWFDEC_KEY_C;
          case DIKS_SMALL_D:
               return SWFDEC_KEY_D;
          case DIKS_SMALL_E:
               return SWFDEC_KEY_E;
          case DIKS_SMALL_F:
               return SWFDEC_KEY_F;
          case DIKS_SMALL_G:
               return SWFDEC_KEY_G;
          case DIKS_SMALL_H:
               return SWFDEC_KEY_H;
          case DIKS_SMALL_I:
               return SWFDEC_KEY_I;
          case DIKS_SMALL_J:
               return SWFDEC_KEY_J;
          case DIKS_SMALL_K:
               return SWFDEC_KEY_K;
          case DIKS_SMALL_L:
               return SWFDEC_KEY_L;
          case DIKS_SMALL_M:
               return SWFDEC_KEY_M;
          case DIKS_SMALL_N:
               return SWFDEC_KEY_N;
          case DIKS_SMALL_O:
               return SWFDEC_KEY_O;
          case DIKS_SMALL_P:
               return SWFDEC_KEY_P;
          case DIKS_SMALL_Q:
               return SWFDEC_KEY_Q;
          case DIKS_SMALL_R:
               return SWFDEC_KEY_R;
          case DIKS_SMALL_S:
               return SWFDEC_KEY_S;
          case DIKS_SMALL_T:
               return SWFDEC_KEY_T;
          case DIKS_SMALL_U:
               return SWFDEC_KEY_U;
          case DIKS_SMALL_V:
               return SWFDEC_KEY_V;
          case DIKS_SMALL_W:
               return SWFDEC_KEY_W;
          case DIKS_SMALL_X:
               return SWFDEC_KEY_X;
          case DIKS_SMALL_Y:
               return SWFDEC_KEY_Y;
          case DIKS_SMALL_Z:
               return SWFDEC_KEY_Z;
          case DIKS_F1:
               return SWFDEC_KEY_F1;
          case DIKS_F2:
               return SWFDEC_KEY_F2;
          case DIKS_F3:
               return SWFDEC_KEY_F3;
          case DIKS_F4:
               return SWFDEC_KEY_F4;
          case DIKS_F5:
               return SWFDEC_KEY_F5;
          case DIKS_F6:
               return SWFDEC_KEY_F6;
          case DIKS_F7:
               return SWFDEC_KEY_F7;
          case DIKS_F8:
               return SWFDEC_KEY_F8;
          case DIKS_F9:
               return SWFDEC_KEY_F9;
          case DIKS_F10:
               return SWFDEC_KEY_F10;
          case DIKS_F11:
               return SWFDEC_KEY_F11;
          case DIKS_F12:
               return SWFDEC_KEY_F12;
          case DIKS_NUM_LOCK:
               return SWFDEC_KEY_NUM_LOCK;
          case DIKS_SEMICOLON:
               return SWFDEC_KEY_SEMICOLON;
          case DIKS_EQUALS_SIGN:
               return SWFDEC_KEY_EQUAL;
          case DIKS_MINUS_SIGN:
               return SWFDEC_KEY_MINUS;
          case DIKS_SLASH:
               return SWFDEC_KEY_SLASH;
          case DIKS_GRAVE_ACCENT:
               return SWFDEC_KEY_GRAVE;
          case DIKS_PARENTHESIS_LEFT:
               return SWFDEC_KEY_LEFT_BRACKET;
          case DIKS_BACKSLASH:
               return SWFDEC_KEY_BACKSLASH;
          case DIKS_PARENTHESIS_RIGHT:
               return SWFDEC_KEY_RIGHT_BRACKET;
          case DIKS_APOSTROPHE:
               return SWFDEC_KEY_APOSTROPHE;
          default:
               return 0;
     }
}

#ifdef HAVE_FUSIONSOUND
static void
audio_advance( SwfdecPlayer                       *player,
               guint                               msecs,
               guint                               samples,
               IDirectFBVideoProvider_Swfdec_data *data )
{
     if (samples >= data->audio.offset)
          data->audio.offset = 0;
     else
          data->audio.offset -= samples;
}

static void
audio_added( SwfdecPlayer                       *player,
             SwfdecAudio                        *audio,
             IDirectFBVideoProvider_Swfdec_data *data )
{
     AudioLink *link;

     link = D_MALLOC( sizeof(AudioLink) );
     if (!link) {
          D_OOM();
          return;
     }

     g_object_ref( audio );

     link->audio = audio;

     direct_mutex_lock( &data->audio.lock );

     direct_list_append( &data->audio.st, &link->link );

     direct_waitqueue_signal( &data->audio.cond );

     direct_mutex_unlock( &data->audio.lock );
}

static void
audio_removed( SwfdecPlayer                       *player,
               SwfdecAudio                        *audio,
               IDirectFBVideoProvider_Swfdec_data *data )
{
     AudioLink *link, *tmp;

     direct_mutex_lock( &data->audio.lock );

     direct_list_foreach_safe (link, tmp, data->audio.st) {
          if (link->audio == audio) {
               direct_list_remove( &data->audio.st, &link->link );
               g_object_unref( link->audio );
               D_FREE( link );
               break;
          }
     }

     direct_mutex_unlock( &data->audio.lock );
}
#endif

static void
dispatch_event( IDirectFBVideoProvider_Swfdec_data *data,
                DFBVideoProviderEventType           type )
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
SwfVideo( DirectThread *thread,
          void         *arg )
{
     DFBResult                           ret;
     int                                 pitch;
     void                               *ptr;
     IDirectFBSurface                   *source;
     cairo_surface_t                    *cairo_surface;
     long                                next = 0;
     IDirectFBVideoProvider_Swfdec_data *data = arg;

     ret = data->idirectfb->CreateSurface( data->idirectfb, &data->desc, &source );
     if (ret)
          return NULL;

     source->Lock( source, DSLF_WRITE, &ptr, &pitch );
     source->Unlock( source );

     cairo_surface = cairo_image_surface_create_for_data( ptr, CAIRO_FORMAT_ARGB32,
                                                          data->desc.width, data->desc.height, pitch );

     dispatch_event( data, DVPET_STARTED );

     while (data->status != DVSTATE_STOP) {
          long long  time;
          guint      bgcolor;
          cairo_t   *cairo;

          time = direct_clock_get_abs_micros();

          direct_mutex_lock( &data->video.lock );

          if (data->video.seek > data->video.pos) {
               next = data->video.seek - data->video.pos;
               data->video.seek = 0;
          }

          direct_mutex_lock( &data->player_lock );

          swfdec_player_advance( data->player, next );
          next = swfdec_player_get_next_event( data->player );

          direct_mutex_unlock( &data->player_lock );

          bgcolor = swfdec_player_get_background_color( data->player );
          source->Clear( source, bgcolor >> 16, bgcolor >> 8, bgcolor, bgcolor >> 24 );

          cairo = cairo_create( cairo_surface );
          swfdec_player_render( data->player, cairo, 0, 0, data->desc.width, data->desc.height );
          cairo_destroy( cairo );

          data->video.dest->StretchBlit( data->video.dest, source, NULL, NULL );

          data->video.pos += next;

          if (data->frame_callback)
               data->frame_callback( data->frame_callback_context );

          if (next < 0) {
               data->status = DVSTATE_FINISHED;
               dispatch_event( data, DVPET_FINISHED );
               direct_waitqueue_wait( &data->video.cond, &data->video.lock );
               next = 0;
          }
          else {
               if (!data->speed) {
                    direct_waitqueue_wait( &data->video.cond, &data->video.lock );
               }
               else {
                    if (data->speed != 1.0)
                         next = next / data->speed;

                    direct_waitqueue_wait_timeout( &data->video.cond, &data->video.lock, next * 1000 );

                    next = (direct_clock_get_abs_micros() - time + 500) / 1000;
                    if (data->speed != 1.0)
                         next = next * data->speed;
               }
          }

          direct_mutex_unlock( &data->video.lock );
     }

     cairo_surface_destroy( cairo_surface );

     source->Release( source );

     return NULL;
}

#ifdef HAVE_FUSIONSOUND
static void *
SwfAudio( DirectThread *thread,
          void         *arg )
{
     gint16                              buf[1152*2];
     IDirectFBVideoProvider_Swfdec_data *data = arg;

     while (data->status != DVSTATE_STOP) {
          AudioLink *link;

          direct_mutex_lock( &data->audio.lock );

          if (!data->speed || !data->audio.st) {
               direct_waitqueue_wait( &data->audio.cond, &data->audio.lock );
               direct_mutex_unlock( &data->audio.lock );
               continue;
          }

          direct_list_foreach (link, data->audio.st) {
               swfdec_audio_render( link->audio, buf, data->audio.offset, sizeof(buf) / 4 );
          }

          data->audio.offset += sizeof(buf) / 4;

          direct_mutex_unlock( &data->audio.lock );

          data->audio.stream->Write( data->audio.stream, buf, sizeof(buf) / 4 );
     }

     return NULL;
}
#endif

/**********************************************************************************************************************/

static void
IDirectFBVideoProvider_Swfdec_Destruct( IDirectFBVideoProvider *thiz )
{
     DirectLink                         *link, *tmp;
     IDirectFBVideoProvider_Swfdec_data *data = thiz->priv;

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     thiz->Stop( thiz );

#ifdef HAVE_FUSIONSOUND
     if (data->audio.playback)
          data->audio.playback->Release( data->audio.playback );

     if (data->audio.stream)
          data->audio.stream->Release( data->audio.stream );

     if (data->audio.sound)
          data->audio.sound->Release( data->audio.sound );

     direct_list_foreach_safe (link, tmp, data->audio.st) {
          direct_list_remove( &data->audio.st, &((AudioLink*) link)->link );
          g_object_unref( ((AudioLink*) link)->audio );
          D_FREE( link );
     }

     direct_waitqueue_deinit( &data->audio.cond );
     direct_mutex_deinit( &data->audio.lock );
#endif

     direct_waitqueue_deinit( &data->video.cond );
     direct_mutex_deinit( &data->video.lock );

     direct_mutex_deinit( &data->player_lock );

     direct_list_foreach_safe (link, tmp, data->events) {
          direct_list_remove( &data->events, &((EventLink*) link)->link );
          ((EventLink*) link)->buffer->Release( ((EventLink*) link)->buffer );
          D_FREE( link );
     }

     direct_mutex_deinit( &data->events_lock );

     g_object_unref( data->player );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBVideoProvider_Swfdec_AddRef( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DR_OK;
}

static DirectResult
IDirectFBVideoProvider_Swfdec_Release( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBVideoProvider_Swfdec_Destruct( thiz );

     return DR_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_GetCapabilities( IDirectFBVideoProvider       *thiz,
                                               DFBVideoProviderCapabilities *ret_caps )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_caps)
          return DFB_INVARG;

     *ret_caps = DVCAPS_BASIC | DVCAPS_INTERACTIVE | DVCAPS_SPEED;
     if (data->seekable)
          *ret_caps |= DVCAPS_SEEK;
#ifdef HAVE_FUSIONSOUND
     if (data->audio.playback)
          *ret_caps |= DVCAPS_VOLUME;
#endif

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_GetSurfaceDescription( IDirectFBVideoProvider *thiz,
                                                     DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec , "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_GetStreamDescription( IDirectFBVideoProvider *thiz,
                                                    DFBStreamDescription   *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     memset( ret_desc, 0, sizeof(DFBStreamDescription) );

     ret_desc->caps = DVSCAPS_VIDEO;

     snprintf( ret_desc->video.encoding, DFB_STREAM_DESC_ENCODING_LENGTH, "swf" );

     ret_desc->video.framerate = data->rate;
     ret_desc->video.aspect    = (double) data->desc.width / data->desc.height;

#ifdef HAVE_FUSIONSOUND
     if (data->audio.stream) {
          ret_desc->caps |= DVSCAPS_AUDIO;

          snprintf( ret_desc->audio.encoding, DFB_STREAM_DESC_ENCODING_LENGTH, "mp3" );

          ret_desc->audio.samplerate = 44100;
          ret_desc->audio.channels   = 2;
     }
#endif

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_PlayTo( IDirectFBVideoProvider *thiz,
                                      IDirectFBSurface       *destination,
                                      const DFBRectangle     *dest_rect,
                                      DVFrameCallback         callback,
                                      void                   *ctx )
{
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

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

     data->video.thread = direct_thread_create( DTT_DEFAULT, SwfVideo, data, "Swf Video" );

#ifdef HAVE_FUSIONSOUND
     if (data->audio.stream)
          data->audio.thread = direct_thread_create( DTT_DEFAULT, SwfAudio, data, "Swf Audio" );
#endif

#ifdef HAVE_FUSIONSOUND
     direct_mutex_unlock( &data->audio.lock );
#endif
     direct_mutex_unlock( &data->video.lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_Stop( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

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
          direct_waitqueue_signal( &data->audio.cond );
          direct_thread_join( data->audio.thread );
          direct_thread_destroy( data->audio.thread );
          data->video.thread = NULL;
     }
#endif

     dispatch_event( data, DVPET_STOPPED );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_GetStatus( IDirectFBVideoProvider *thiz,
                                         DFBVideoProviderStatus *ret_status )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_status)
          return DFB_INVARG;

     *ret_status = data->status;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_SeekTo( IDirectFBVideoProvider *thiz,
                                      double                  seconds )
{
     long msecs;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (seconds < 0.0)
          return DFB_INVARG;

     if (!data->seekable)
          return DFB_UNSUPPORTED;

     msecs = seconds * 1000;

     direct_mutex_lock( &data->video.lock );

     if (data->video.pos > msecs) {
          direct_mutex_unlock( &data->video.lock );
          return DFB_UNSUPPORTED;
     }

     data->video.seek = msecs;

     direct_waitqueue_signal( &data->video.cond );

     direct_mutex_unlock( &data->video.lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_GetPos( IDirectFBVideoProvider *thiz,
                                      double                 *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DFB_INVARG;

     *ret_seconds = data->video.pos / 1000.0;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_GetLength( IDirectFBVideoProvider *thiz,
                                         double                 *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DFB_INVARG;

     *ret_seconds = 0.0;

     return DFB_UNIMPLEMENTED;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_SendEvent( IDirectFBVideoProvider *thiz,
                                         const DFBEvent         *event )
{
     int    width, height;
     double x, y;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (!event)
          return DFB_INVARG;

     data->video.dest->GetSize( data->video.dest, &width, &height );

     x = (double) data->desc.width / width;
     y = (double) data->desc.height / height;

     direct_mutex_lock( &data->player_lock );

     switch (event->clazz) {
          case DFEC_INPUT:
               switch (event->input.type) {
                    case DIET_KEYPRESS:
                         swfdec_player_key_press( data->player,
                                                  symbol_translate( event->input.key_symbol ),
                                                  event->input.key_symbol );
                         break;

                    case DIET_KEYRELEASE:
                         swfdec_player_key_release( data->player,
                                                    symbol_translate( event->input.key_symbol ),
                                                    event->input.key_symbol );
                         break;

                    case DIET_BUTTONPRESS:
                         swfdec_player_mouse_press( data->player,
                                                    data->mouse_x * x, data->mouse_y * y,
                                                    event->input.button + 1 );
                         break;

                    case DIET_BUTTONRELEASE:
                         swfdec_player_mouse_release( data->player,
                                                      data->mouse_x * x, data->mouse_y * y,
                                                      event->input.button + 1 );
                         break;

                    case DIET_AXISMOTION:
                         switch (event->input.axis) {
                              case DIAI_X:
                                   if (event->input.flags & DIEF_AXISREL)
                                        data->mouse_x += event->input.axisrel;
                                   if (event->input.flags & DIEF_AXISABS)
                                        data->mouse_x = event->input.axisabs;

                                   swfdec_player_mouse_move( data->player, data->mouse_x * x, data->mouse_y * y );
                                   break;

                              case DIAI_Y:
                                   if (event->input.flags & DIEF_AXISREL)
                                        data->mouse_y += event->input.axisrel;
                                   if (event->input.flags & DIEF_AXISABS)
                                        data->mouse_y = event->input.axisabs;

                                   swfdec_player_mouse_move( data->player, data->mouse_x * x, data->mouse_y * y );
                                   break;

                              default:
                                   break;
                         }
                         break;

                    default:
                         break;
               }
               break;

          case DFEC_WINDOW:
               switch (event->window.type) {
                    case DWET_KEYDOWN:
                         swfdec_player_key_press( data->player,
                                                  symbol_translate( event->window.key_symbol ),
                                                  event->window.key_symbol );
                         break;

                    case DWET_KEYUP:
                         swfdec_player_key_release( data->player,
                                                    symbol_translate( event->window.key_symbol ),
                                                    event->window.key_symbol );
                         break;
                    case DWET_BUTTONDOWN:
                         swfdec_player_mouse_press( data->player,
                                                    event->window.x * x, event->window.y * y,
                                                    event->window.button + 1 );
                         break;
                    case DWET_BUTTONUP:
                         swfdec_player_mouse_release( data->player,
                                                      event->window.x*x, event->window.y*y,
                                                      event->window.button + 1 );
                         break;

                    case DWET_ENTER:
                    case DWET_MOTION:
                         swfdec_player_mouse_move( data->player, event->window.x * x, event->window.y * y );
                         break;

                    case DWET_LEAVE:
                         swfdec_player_mouse_move( data->player, -1, -1 );
                         break;

                    default:
                         break;
               }
               break;

          default:
               break;
     }

     direct_mutex_unlock( &data->player_lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_SetSpeed( IDirectFBVideoProvider *thiz,
                                        double                  multiplier )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (multiplier < 0.0 || multiplier > 64.0)
          return DFB_UNSUPPORTED;

     if (multiplier == data->speed)
          return DFB_OK;

     direct_mutex_lock( &data->video.lock );
#ifdef HAVE_FUSIONSOUND
     direct_mutex_lock( &data->audio.lock );
#endif

     if (multiplier) {
          multiplier = MAX( multiplier, 0.01 );
#ifdef HAVE_FUSIONSOUND
          if (data->audio.playback)
               data->audio.playback->SetPitch( data->audio.playback, multiplier );
#endif
     }

     if (multiplier > data->speed) {
          direct_waitqueue_signal( &data->video.cond );
#ifdef HAVE_FUSIONSOUND
          direct_waitqueue_signal( &data->audio.cond );
#endif
     }

     data->speed = multiplier;

     dispatch_event( data, DVPET_SPEEDCHANGE );

#ifdef HAVE_FUSIONSOUND
     direct_mutex_unlock( &data->audio.lock );
#endif
     direct_mutex_unlock( &data->video.lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_GetSpeed( IDirectFBVideoProvider *thiz,
                                        double                 *ret_multiplier )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_multiplier)
          return DFB_INVARG;

     *ret_multiplier = data->speed;

     return DFB_OK;
}

#ifdef HAVE_FUSIONSOUND
static DFBResult
IDirectFBVideoProvider_Swfdec_SetVolume( IDirectFBVideoProvider *thiz,
                                         float                   level )
{
     DFBResult ret = DFB_UNSUPPORTED;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_Swfdec_GetVolume( IDirectFBVideoProvider *thiz,
                                         float                  *ret_level )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_level)
          return DFB_INVARG;

     *ret_level = data->audio.volume;

     return DFB_OK;
}
#endif

static DFBResult
IDirectFBVideoProvider_Swfdec_CreateEventBuffer( IDirectFBVideoProvider  *thiz,
                                                 IDirectFBEventBuffer   **ret_interface )
{
     DFBResult             ret;
     IDirectFBEventBuffer *buffer;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_Swfdec_AttachEventBuffer( IDirectFBVideoProvider *thiz,
                                                 IDirectFBEventBuffer   *buffer )
{
     DFBResult  ret;
     EventLink *link;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_Swfdec_EnableEvents( IDirectFBVideoProvider    *thiz,
                                            DFBVideoProviderEventType  mask )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (mask & ~DVPET_ALL)
          return DFB_INVARG;

     data->events_mask |= mask;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_DisableEvents( IDirectFBVideoProvider    *thiz,
                                             DFBVideoProviderEventType  mask )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     if (mask & ~DVPET_ALL)
          return DFB_INVARG;

     data->events_mask &= ~mask;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_Swfdec_DetachEventBuffer( IDirectFBVideoProvider *thiz,
                                                 IDirectFBEventBuffer   *buffer )
{
     DFBResult  ret = DFB_ITEMNOTFOUND;
     EventLink *link;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_Swfdec_SetDestination( IDirectFBVideoProvider *thiz,
                                              IDirectFBSurface       *destination,
                                              const DFBRectangle     *dest_rect )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p, %4d,%4d-%4dx%4d )\n", __FUNCTION__,
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
     /* Check for valid filename. */
     if (!ctx->filename)
          return DFB_UNSUPPORTED;

     /* Check the magic. */
     if ((ctx->header[0] == 'F' || ctx->header[0] == 'C') && ctx->header[1] == 'W' && ctx->header[2] == 'S')
          return DFB_OK;

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBVideoProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     SwfdecURL                *url;
     SwfdecLoader             *loader;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBVideoProvider_Swfdec )

     D_DEBUG_AT( VideoProvider_Swfdec, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref = 1;

     swfdec_init();

     if (g_strstr_len( buffer_data->filename, -1, "://" )) {
          url = swfdec_url_new( buffer_data->filename );
     }
     else {
          char *uri;

          if (*buffer_data->filename == '/')
               uri = g_strconcat( "file://", buffer_data->filename, NULL );
          else
               uri = g_strconcat( "file://", g_get_current_dir(), "/", buffer_data->filename, NULL );

          url = swfdec_url_new( uri );

          g_free( uri );
     }

     loader = g_object_new( databuffer_loader_get_type(), "url", url, NULL );
     if (!loader) {
        D_ERROR( "VideoProvider/Swfdec: Failed to create loader!\n" );
        swfdec_url_free( url );
        DIRECT_DEALLOCATE_INTERFACE( thiz );
        return DFB_FAILURE;
     }

     ((DataBufferLoader*) loader)->buffer = buffer;

     swfdec_url_free( url );

     /* Increase the data buffer reference counter. */
     buffer->AddRef( buffer );

     data->seekable = (buffer->SeekTo( buffer, 0 ) == DFB_OK);

     data->idirectfb = idirectfb;

     databuffer_loader_load( loader, NULL, SWFDEC_LOADER_REQUEST_DEFAULT, NULL, 0 );

     data->player = swfdec_player_new( NULL );
     if (!data->player) {
          D_ERROR( "VideoProvider/Swfdec: Failed to create player!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     swfdec_player_set_loader( data->player, loader );

     swfdec_player_advance( data->player, 0 );

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     swfdec_player_get_default_size( data->player, (guint*) &data->desc.width, (guint*) &data->desc.height );
     data->desc.pixelformat = DSPF_ARGB;

     data->rate = swfdec_player_get_rate( data->player );

#ifdef HAVE_FUSIONSOUND
     if (FusionSoundInit( NULL, NULL ) == DR_OK && FusionSoundCreate( &data->audio.sound ) == DR_OK) {
          FSStreamDescription dsc;

          dsc.flags        = FSSDF_BUFFERSIZE | FSSDF_CHANNELS | FSSDF_SAMPLEFORMAT | FSSDF_SAMPLERATE;
          dsc.channels     = 2;
          dsc.samplerate   = 44100;
          dsc.buffersize   = 4410;
          dsc.sampleformat = FSSF_S16;

          ret = data->audio.sound->CreateStream( data->audio.sound, &dsc, &data->audio.stream );
          if (ret) {
               D_ERROR( "VideoProvider/Swfdec: Failed to create FusionSound stream!\n" );
               goto error;
          }
          else {
               data->audio.stream->GetPlayback( data->audio.stream, &data->audio.playback );

               g_signal_connect( data->player, "advance", G_CALLBACK( audio_advance ), data );
               g_signal_connect( data->player, "audio-added", G_CALLBACK( audio_added ), data );
               g_signal_connect( data->player, "audio-removed", G_CALLBACK( audio_removed ), data );
          }
     }
     else {
          D_ERROR( "VideoProvider/Swfdec: Failed to initialize/create FusionSound!\n" );
          goto error;
     }
#endif

     data->status = DVSTATE_STOP;
     data->speed  = 1.0;

     data->events_mask = DVPET_ALL;

     direct_mutex_init( &data->events_lock );

     direct_mutex_init( &data->player_lock );

     direct_mutex_init( &data->video.lock );
     direct_waitqueue_init( &data->video.cond );

#ifdef HAVE_FUSIONSOUND
     data->audio.volume = 1.0;

     direct_mutex_init( &data->audio.lock );
     direct_waitqueue_init( &data->audio.cond );
#endif

     thiz->AddRef                = IDirectFBVideoProvider_Swfdec_AddRef;
     thiz->Release               = IDirectFBVideoProvider_Swfdec_Release;
     thiz->GetCapabilities       = IDirectFBVideoProvider_Swfdec_GetCapabilities;
     thiz->GetSurfaceDescription = IDirectFBVideoProvider_Swfdec_GetSurfaceDescription;
     thiz->GetStreamDescription  = IDirectFBVideoProvider_Swfdec_GetStreamDescription;
     thiz->PlayTo                = IDirectFBVideoProvider_Swfdec_PlayTo;
     thiz->Stop                  = IDirectFBVideoProvider_Swfdec_Stop;
     thiz->GetStatus             = IDirectFBVideoProvider_Swfdec_GetStatus;
     thiz->SeekTo                = IDirectFBVideoProvider_Swfdec_SeekTo;
     thiz->GetPos                = IDirectFBVideoProvider_Swfdec_GetPos;
     thiz->GetLength             = IDirectFBVideoProvider_Swfdec_GetLength;
     thiz->SendEvent             = IDirectFBVideoProvider_Swfdec_SendEvent;
     thiz->SetSpeed              = IDirectFBVideoProvider_Swfdec_SetSpeed;
     thiz->GetSpeed              = IDirectFBVideoProvider_Swfdec_GetSpeed;
#ifdef HAVE_FUSIONSOUND
     thiz->SetVolume             = IDirectFBVideoProvider_Swfdec_SetVolume;
     thiz->GetVolume             = IDirectFBVideoProvider_Swfdec_GetVolume;
#endif
     thiz->CreateEventBuffer     = IDirectFBVideoProvider_Swfdec_CreateEventBuffer;
     thiz->AttachEventBuffer     = IDirectFBVideoProvider_Swfdec_AttachEventBuffer;
     thiz->EnableEvents          = IDirectFBVideoProvider_Swfdec_EnableEvents;
     thiz->DisableEvents         = IDirectFBVideoProvider_Swfdec_DisableEvents;
     thiz->DetachEventBuffer     = IDirectFBVideoProvider_Swfdec_DetachEventBuffer;
     thiz->SetDestination        = IDirectFBVideoProvider_Swfdec_SetDestination;

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

     if (data->player)
          g_object_unref( data->player );

     g_object_unref( loader );

     buffer->Release( buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
