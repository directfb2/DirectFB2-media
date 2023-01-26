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
#include <libavformat/avformat.h>
#ifdef HAVE_FUSIONSOUND
#include <libswresample/swresample.h>
#endif
#include <libswscale/swscale.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbvideoprovider.h>

D_DEBUG_DOMAIN( VideoProvider_FFMPEG, "VideoProvider/FFMPEG", "FFmpeg Video Provider" );

static DFBResult Probe    ( IDirectFBVideoProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBVideoProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );


#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBVideoProvider, FFmpeg )

/**********************************************************************************************************************/

typedef struct {
     DirectLink link;
     AVPacket   packet;
} PacketLink;

typedef struct {
     DirectLink  *list;
     int          size;
     s64          max_len;
     int          max_size;
     DirectMutex  lock;
} PacketQueue;

typedef struct {
     DirectLink            link;
     IDirectFBEventBuffer *buffer;
} EventLink;

typedef struct {
     int                            ref;                    /* reference counter */

     IDirectFBDataBuffer           *buffer;
     IDirectFB                     *idirectfb;

     bool                           seekable;

     unsigned char                 *io_buf;
     AVIOContext                   *io_ctx;
     AVFormatContext               *fmt_ctx;

     DFBSurfaceDescription          desc;

     double                         rate;
     DFBVideoProviderStatus         status;
     double                         speed;
     DFBVideoProviderPlaybackFlags  flags;
     s64                            start_time;

     struct {
          DirectThread             *thread;
          DirectMutex               lock;

          bool                      buffering;

          bool                      seeked;
          s64                       seek_time;
          int                       seek_flag;
     } input;

     struct {
          DirectThread             *thread;
          DirectMutex               lock;
          DirectWaitQueue           cond;

          AVStream                 *st;
          AVCodecContext           *ctx;
          AVCodec                  *codec;

          PacketQueue               queue;

          s64                       pts;

          bool                      seeked;

          AVFrame                  *src_frame;

          IDirectFBSurface         *dest;
          DFBRectangle              rect;
     } video;

#ifdef HAVE_FUSIONSOUND
     struct {
          DirectThread             *thread;
          DirectMutex               lock;
          DirectWaitQueue           cond;

          AVStream                 *st;
          AVCodecContext           *ctx;
          AVCodec                  *codec;

          PacketQueue               queue;

          s64                       pts;

          bool                      seeked;

          AVFrame                  *src_frame;

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
} IDirectFBVideoProvider_FFmpeg_data;

#define IO_BUFFER_SIZE      8 /* in kilobytes */

#define MAX_QUEUE_LEN       3 /* in seconds */

#define GAP_TOLERANCE   15000 /* in microseconds */

#define GAP_THRESHOLD  250000 /* in microseconds */

/**********************************************************************************************************************/

static __inline__ s64
get_stream_clock( IDirectFBVideoProvider_FFmpeg_data *data )
{
#ifdef HAVE_FUSIONSOUND
     if (data->audio.stream && data->audio.pts != -1) {
          int delay = 0;

          data->audio.stream->GetPresentationDelay( data->audio.stream, &delay );

          return data->audio.pts - delay * 1000ll;
     }
#endif

     return data->video.pts;
}

static int
av_read_callback( void    *opaque,
                  uint8_t *buf,
                  int      size )
{
     DFBResult            ret;
     unsigned int         len    = 0;
     IDirectFBDataBuffer *buffer = opaque;

     if (!buf || size < 0)
          return -1;

     if (size) {
          buffer->WaitForData( buffer, size );
          ret = buffer->GetData( buffer, size, buf, &len );
          if (ret && ret != DFB_EOF)
               return -1;
     }

     return len;
}

static int64_t
av_seek_callback( void    *opaque,
                  int64_t  offset,
                  int      whence )
{
     DFBResult            ret;
     unsigned int         pos;
     IDirectFBDataBuffer *buffer = opaque;

     switch (whence) {
          case SEEK_SET:
               ret = buffer->SeekTo( buffer, offset );
               break;
          case SEEK_CUR:
               ret = buffer->GetPosition( buffer, &pos );
               if (ret == DFB_OK) {
                    if (!offset)
                         return pos;
                    ret = buffer->SeekTo( buffer, pos + offset );
               }
               break;
          case SEEK_END:
               ret = buffer->GetLength( buffer, &pos );
               if (ret == DFB_OK) {
                    if (offset < 0)
                         return pos;
                    ret = buffer->SeekTo( buffer, pos - offset );
               }
               break;
          default:
               ret = DFB_UNSUPPORTED;
               break;
     }

     if (ret != DFB_OK)
          return -1;

     buffer->GetPosition( buffer, &pos );

     return pos;
}

static bool
put_packet( PacketQueue *queue,
            AVPacket    *pkt )
{
     PacketLink *p;

     p = D_MALLOC( sizeof(PacketLink) );
     if (!p) {
          D_OOM();
          return false;
     }

     av_dup_packet( pkt );
     p->packet = *pkt;

     direct_mutex_lock( &queue->lock );
     direct_list_append( &queue->list, &p->link );
     queue->size += pkt->size;
     direct_mutex_unlock( &queue->lock );

     return true;
}

static bool
get_packet( PacketQueue *queue,
            AVPacket    *pkt )
{
     PacketLink *p;

     direct_mutex_lock( &queue->lock );
     p = (PacketLink*) queue->list;
     if (p) {
          direct_list_remove( &queue->list, &p->link );
          queue->size -= p->packet.size;
          *pkt = p->packet;
          D_FREE( p );
     }
     direct_mutex_unlock( &queue->lock );

     return (p != NULL);
}

static void
flush_packets( PacketQueue *queue )
{
     PacketLink *p;

     for (p = (PacketLink*) queue->list; p;) {
          PacketLink *next = (PacketLink*) p->link.next;
          direct_list_remove( &queue->list, &p->link );
          av_free_packet( &p->packet );
          D_FREE( p );
          p = next;
     }

     queue->list = NULL;
     queue->size = 0;
}

static bool
queue_is_full( PacketQueue *queue )
{
     PacketLink *first, *last;

     if (!queue->list)
          return false;

     first = (PacketLink*) queue->list;
     last  = (PacketLink*) first->link.prev;

     if (first->packet.dts != AV_NOPTS_VALUE && last->packet.dts != AV_NOPTS_VALUE) {
          if ((last->packet.dts - first->packet.dts) >= queue->max_len)
               return true;
     }

     return (queue->size >= queue->max_size);
}

static void
dispatch_event( IDirectFBVideoProvider_FFmpeg_data *data,
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
FFmpegInput( DirectThread *thread,
             void         *arg )
{
     IDirectFBVideoProvider_FFmpeg_data *data = arg;

     if (!data->io_ctx->seekable) {
          data->input.buffering = true;
          direct_mutex_lock( &data->video.queue.lock );
#ifdef HAVE_FUSIONSOUND
          direct_mutex_lock( &data->audio.queue.lock );
#endif
     }

#ifdef HAVE_FUSIONSOUND
     data->audio.pts = -1;
#endif

     dispatch_event( data, DVPET_STARTED );

     while (data->status != DVSTATE_STOP) {
          AVPacket pkt;

          direct_mutex_lock( &data->input.lock );

          if (data->input.seeked) {
               if (av_seek_frame( data->fmt_ctx, -1, data->input.seek_time, data->input.seek_flag ) >= 0) {
                    direct_mutex_lock( &data->video.lock );
#ifdef HAVE_FUSIONSOUND
                    direct_mutex_lock( &data->audio.lock );
#endif

                    flush_packets( &data->video.queue );
#ifdef HAVE_FUSIONSOUND
                    flush_packets( &data->audio.queue );
#endif

                    if (!data->input.buffering && !data->io_ctx->seekable) {
                         data->input.buffering = true;
                         direct_mutex_lock( &data->video.queue.lock );
#ifdef HAVE_FUSIONSOUND
                         direct_mutex_lock( &data->audio.queue.lock );
#endif
                    }

                    if (data->status == DVSTATE_FINISHED)
                         data->status = DVSTATE_PLAY;

                    data->video.seeked = true;

#ifdef HAVE_FUSIONSOUND
                    data->audio.pts = -1;
                    data->audio.seeked = true;
#endif

                    if (data->video.thread)
                         direct_waitqueue_signal( &data->video.cond );

#ifdef HAVE_FUSIONSOUND
                    direct_mutex_unlock( &data->audio.lock );
#endif
                    direct_mutex_unlock( &data->video.lock );
               }

               data->input.seeked = false;
          }

#ifdef HAVE_FUSIONSOUND
          if (queue_is_full( &data->video.queue ) || queue_is_full( &data->audio.queue )) {
#else
          if (queue_is_full( &data->video.queue )) {
#endif
               if (data->input.buffering) {
#ifdef HAVE_FUSIONSOUND
                    direct_mutex_unlock( &data->audio.queue.lock );
#endif
                    direct_mutex_unlock( &data->video.queue.lock );
                    data->input.buffering = false;
               }

               direct_mutex_unlock( &data->input.lock );
               usleep( 20000 );
               continue;
          }
#ifdef HAVE_FUSIONSOUND
          else if (data->video.queue.size == 0 || data->audio.queue.size == 0) {
#else
          else if (data->video.queue.size == 0) {
#endif
               if (!data->input.buffering && !data->io_ctx->seekable) {
                    data->input.buffering = true;
                    direct_mutex_lock( &data->video.queue.lock );
#ifdef HAVE_FUSIONSOUND
                    direct_mutex_lock( &data->audio.queue.lock );
#endif
               }
          }

          if (av_read_frame( data->fmt_ctx, &pkt ) < 0) {
               if (avio_feof( data->io_ctx )) {
                    if (data->input.buffering) {
#ifdef HAVE_FUSIONSOUND
                         direct_mutex_unlock( &data->audio.queue.lock );
#endif
                         direct_mutex_unlock( &data->video.queue.lock );
                         data->input.buffering = false;
                    }
#ifdef HAVE_FUSIONSOUND
                    if (data->video.queue.size == 0 && data->audio.queue.size == 0) {
#else
                    if (data->video.queue.size == 0) {
#endif
                         if (data->flags & DVPLAY_LOOPING) {
                              data->input.seeked    = true;
                              data->input.seek_time = 0;
                              data->input.seek_flag = 0;
                         }
                         else {
                              if (data->status != DVSTATE_FINISHED) {
                                   data->status = DVSTATE_FINISHED;
                                   dispatch_event( data, DVPET_FINISHED );
                              }
                         }
                    }
               }

               direct_mutex_unlock( &data->input.lock );
               usleep( 100 );
               continue;
          }

          if (pkt.stream_index == data->video.st->index) {
               put_packet( &data->video.queue, &pkt );
          }
#ifdef HAVE_FUSIONSOUND
          else if (data->audio.stream && pkt.stream_index == data->audio.st->index) {
               put_packet( &data->audio.queue, &pkt );
          }
#endif
          else {
               av_free_packet( &pkt );
          }

          direct_mutex_unlock( &data->input.lock );
     }

     if (data->input.buffering) {
#ifdef HAVE_FUSIONSOUND
          direct_mutex_unlock( &data->audio.queue.lock );
#endif
          direct_mutex_unlock( &data->video.queue.lock );
          data->input.buffering = false;
     }

     return NULL;
}

static void *
FFmpegVideo( DirectThread *thread,
             void         *arg )
{
     DFBSurfacePixelFormat               pixelformat;
     enum AVPixelFormat                  pix_fmt;
     struct SwsContext                  *sws_ctx;
     AVFrame                            *dst_frame;
     void                               *dest_ptr;
     int                                 dest_pitch;
     long                                duration;
     s64                                 firtspts = 0;
     unsigned int                        framecnt = 0;
     int                                 drop     = 0;
     IDirectFBVideoProvider_FFmpeg_data *data     = arg;

     data->video.dest->GetPixelFormat( data->video.dest, &pixelformat );
     switch (pixelformat) {
          case DSPF_ARGB1555:
               pix_fmt = AV_PIX_FMT_RGB555;
               break;
          case DSPF_RGB16:
               pix_fmt = AV_PIX_FMT_RGB565;
               break;
          case DSPF_RGB24:
#ifdef WORDS_BIGENDIAN
               pix_fmt = AV_PIX_FMT_RGB24;
#else
               pix_fmt = AV_PIX_FMT_BGR24;
#endif
               break;
          case DSPF_RGB32:
          case DSPF_ARGB:
               pix_fmt = AV_PIX_FMT_RGB32;
               break;
          case DSPF_ABGR:
               pix_fmt = AV_PIX_FMT_BGR32;
               break;
          default:
               return NULL;
     }

     sws_ctx = sws_getContext( data->video.ctx->width, data->video.ctx->height, data->video.ctx->pix_fmt,
                               data->video.rect.w, data->video.rect.h, pix_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL );

     dst_frame = av_frame_alloc();

     data->video.dest->Lock( data->video.dest, DSLF_WRITE, &dest_ptr, &dest_pitch );
     avpicture_fill( (AVPicture*) dst_frame, dest_ptr, pix_fmt, data->video.ctx->width, data->video.ctx->height );
     data->video.dest->Unlock( data->video.dest );

     duration = 1000000.0 / data->rate;

     while (data->status != DVSTATE_STOP) {
          AVPacket  pkt;
          long long time, now;
          int       got_picture = 0;

          time = direct_clock_get_abs_micros();

          direct_mutex_lock( &data->video.lock );

          if (data->input.buffering || !get_packet( &data->video.queue, &pkt )) {
               direct_mutex_unlock( &data->video.lock );
               usleep( 100 );
               continue;
          }

          if (data->video.seeked) {
               avcodec_flush_buffers( data->video.ctx );
               data->video.seeked = false;
               framecnt = 0;
          }

          avcodec_decode_video2( data->video.ctx, data->video.src_frame, &got_picture, &pkt );

          if (got_picture && !drop) {
               sws_scale( sws_ctx, (void*) data->video.src_frame->data, data->video.src_frame->linesize,
                          0, data->video.ctx->height, dst_frame->data, dst_frame->linesize );

               if (data->frame_callback)
                    data->frame_callback( data->frame_callback_context );
          }

          if (pkt.dts != AV_NOPTS_VALUE)
               data->video.pts = av_rescale_q( pkt.dts, data->video.st->time_base, AV_TIME_BASE_Q );
          else
               data->video.pts += duration;

          av_free_packet( &pkt );

          if (!data->speed) {
               direct_waitqueue_wait( &data->video.cond, &data->video.lock );
          }
          else {
               long length, delay;

               if (framecnt)
                    duration = (data->video.pts - firtspts) / framecnt;

               length = duration;

               if (data->speed != 1.0)
                    length = length / data->speed;

               delay = data->video.pts - get_stream_clock(data);

               if (delay > -GAP_THRESHOLD && delay < GAP_THRESHOLD)
                    delay = CLAMP( delay, -GAP_TOLERANCE, GAP_TOLERANCE );

               length += delay;

               time += length;

               now = direct_clock_get_abs_micros();
               if (time > now) {
                    delay = time - now;
                    direct_waitqueue_wait_timeout( &data->video.cond, &data->video.lock, delay );
                    drop = false;
               }
               else {
                    delay = now - time;
                    drop = (delay >= duration);
               }
          }

          direct_mutex_unlock( &data->video.lock );

          if (framecnt++ == 0)
               firtspts = data->video.pts;
     }

     av_free( dst_frame );
     sws_freeContext( sws_ctx );

     return NULL;
}

#ifdef HAVE_FUSIONSOUND
static void *
FFmpegAudio( DirectThread *thread,
             void         *arg )
{
     struct SwrContext                  *swr_ctx;
     IDirectFBVideoProvider_FFmpeg_data *data           = arg;
     int                                 bytespersample = av_get_bytes_per_sample( AV_SAMPLE_FMT_S16 ) *
                                                          av_get_channel_layout_nb_channels( AV_CH_LAYOUT_STEREO );
     u8                                  buf[bytespersample * data->audio.ctx->sample_rate];

     swr_ctx = swr_alloc_set_opts( NULL,
                                   AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, data->audio.ctx->sample_rate,
                                   data->audio.ctx->channel_layout, data->audio.ctx->sample_fmt, data->audio.ctx->sample_rate,
                                   0, NULL );

     swr_init( swr_ctx );

     while (data->status != DVSTATE_STOP) {
          AVPacket  pkt;
          u8       *pkt_data;
          int       pkt_size;
          int       got_frame;
          int       length = 0;

          direct_mutex_lock( &data->audio.lock );

          if (!data->speed) {
               direct_waitqueue_wait( &data->audio.cond, &data->audio.lock );
               direct_mutex_unlock( &data->audio.lock );
               continue;
          }

          if (data->input.buffering || !get_packet( &data->audio.queue, &pkt )) {
               direct_mutex_unlock( &data->audio.lock );
               usleep( 100 );
               continue;
          }

          if (data->audio.seeked) {
               data->audio.stream->Flush( data->audio.stream );
               avcodec_flush_buffers( data->audio.ctx );
               data->audio.seeked = false;
          }

          for (pkt_data = pkt.data, pkt_size = pkt.size; pkt_size > 0;) {
               int decoded = avcodec_decode_audio4( data->audio.ctx, data->audio.src_frame, &got_frame, &pkt );

               if (decoded < 0)
                    break;

               pkt_data += decoded;
               pkt_size -= decoded;

               length += data->audio.src_frame->nb_samples;
          }

          if (pkt.pts != AV_NOPTS_VALUE) {
               data->audio.pts = av_rescale_q( pkt.pts, data->audio.st->time_base, AV_TIME_BASE_Q );
          }
          else if (length && data->audio.pts != -1) {
               data->audio.pts += (s64) length * AV_TIME_BASE / data->audio.ctx->sample_rate;
          }

          av_free_packet( &pkt );

          direct_mutex_unlock( &data->audio.lock );

          if (length) {
               uint8_t *out[] = { buf };

               swr_convert( swr_ctx, out, data->audio.ctx->sample_rate, (void*) data->audio.src_frame->data, length );

               data->audio.stream->Write( data->audio.stream, buf, length );
          }
          else
               usleep( 1000 );
     }

     swr_free( &swr_ctx );

     return NULL;
}
#endif

/**********************************************************************************************************************/

static void
IDirectFBVideoProvider_FFmpeg_Destruct( IDirectFBVideoProvider *thiz )
{
     EventLink                          *link, *tmp;
     IDirectFBVideoProvider_FFmpeg_data *data = thiz->priv;

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     thiz->Stop( thiz );

#ifdef HAVE_FUSIONSOUND
     if (data->audio.src_frame)
          av_free( data->audio.src_frame );

     if (data->audio.playback)
          data->audio.playback->Release( data->audio.playback );

     if (data->audio.stream)
          data->audio.stream->Release( data->audio.stream );

     if (data->audio.sound)
          data->audio.sound->Release( data->audio.sound );

     if (data->audio.ctx)
          avcodec_close( data->audio.ctx );
#endif

     av_free( data->video.src_frame );

     avcodec_close( data->video.ctx );

#ifdef HAVE_FUSIONSOUND
     flush_packets( &data->audio.queue );
     direct_mutex_deinit( &data->audio.queue.lock );
     direct_waitqueue_deinit( &data->audio.cond );
     direct_mutex_deinit( &data->audio.lock );
#endif

     flush_packets( &data->video.queue );
     direct_mutex_deinit( &data->video.queue.lock );
     direct_waitqueue_deinit( &data->video.cond );
     direct_mutex_deinit( &data->video.lock );

     direct_mutex_deinit( &data->input.lock );

     direct_list_foreach_safe (link, tmp, data->events) {
          direct_list_remove( &data->events, &link->link );
          link->buffer->Release( link->buffer );
          D_FREE( link );
     }

     direct_mutex_deinit( &data->events_lock );

     avformat_close_input( &data->fmt_ctx );

     /* Decrease the data buffer reference counter. */
     if (data->buffer)
          data->buffer->Release( data->buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBVideoProvider_FFmpeg_AddRef( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DR_OK;
}

static DirectResult
IDirectFBVideoProvider_FFmpeg_Release( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBVideoProvider_FFmpeg_Destruct( thiz );

     return DR_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_GetCapabilities( IDirectFBVideoProvider       *thiz,
                                               DFBVideoProviderCapabilities *ret_caps )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_caps)
          return DFB_INVARG;

     *ret_caps = DVCAPS_BASIC | DVCAPS_SCALE | DVCAPS_SPEED;
     if (data->seekable)
          *ret_caps |= DVCAPS_SEEK;
     if (data->video.src_frame->interlaced_frame)
          *ret_caps |= DVCAPS_INTERLACED;
#ifdef HAVE_FUSIONSOUND
     if (data->audio.playback)
          *ret_caps |= DVCAPS_VOLUME;
#endif

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_GetSurfaceDescription( IDirectFBVideoProvider *thiz,
                                                     DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     if (data->video.src_frame->interlaced_frame) {
          data->desc.flags |= DSDESC_CAPS;
          data->desc.caps   = DSCAPS_INTERLACED;
     }

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_GetStreamDescription( IDirectFBVideoProvider *thiz,
                                                    DFBStreamDescription   *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     memset( ret_desc, 0, sizeof(DFBStreamDescription) );

     ret_desc->caps = DVSCAPS_VIDEO;

     snprintf( ret_desc->video.encoding, DFB_STREAM_DESC_ENCODING_LENGTH, data->video.codec->name );

     ret_desc->video.framerate  = data->rate;
     ret_desc->video.aspect     = av_q2d( data->video.ctx->sample_aspect_ratio );
     ret_desc->video.aspect    *= (double) data->desc.width / data->desc.height;
     ret_desc->video.bitrate    = data->video.ctx->bit_rate;

#ifdef HAVE_FUSIONSOUND
     if (data->audio.stream) {
          ret_desc->caps |= DVSCAPS_AUDIO;

          snprintf( ret_desc->audio.encoding, DFB_STREAM_DESC_ENCODING_LENGTH, data->audio.codec->name );

          ret_desc->audio.samplerate = data->audio.ctx->sample_rate;
          ret_desc->audio.channels   = data->audio.ctx->channels;
          ret_desc->audio.bitrate    = data->audio.ctx->bit_rate;
     }
#endif

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_PlayTo( IDirectFBVideoProvider *thiz,
                                      IDirectFBSurface       *destination,
                                      const DFBRectangle     *dest_rect,
                                      DVFrameCallback         callback,
                                      void                   *ctx )
{
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!destination)
          return DFB_INVARG;

     dst_data = destination->priv;
     if (!dst_data)
          return DFB_DEAD;

     if (dest_rect) {
          if (dest_rect->w < 1 || dest_rect->h < 1)
               return DFB_INVARG;

          rect = *dest_rect;
     }
     else {
          rect.x = rect.y = 0;
          rect.w = data->video.ctx->width;
          rect.h = data->video.ctx->height;
     }

     if (data->video.thread)
          return DFB_OK;

     direct_mutex_lock( &data->input.lock );
     direct_mutex_lock( &data->video.lock );
#ifdef HAVE_FUSIONSOUND
     direct_mutex_lock( &data->audio.lock );
#endif

     data->video.dest             = destination;
     data->video.rect             = rect;
     data->frame_callback         = callback;
     data->frame_callback_context = ctx;

     data->status = DVSTATE_PLAY;

     data->input.thread = direct_thread_create( DTT_DEFAULT, FFmpegInput, data, "FFmpeg Input" );

     data->video.thread = direct_thread_create( DTT_DEFAULT, FFmpegVideo, data, "FFmpeg Video" );

#ifdef HAVE_FUSIONSOUND
     if (data->audio.stream) {
          data->audio.thread = direct_thread_create( DTT_DEFAULT, FFmpegAudio, data, "FFmpeg Audio" );
     }
#endif

#ifdef HAVE_FUSIONSOUND
     direct_mutex_unlock( &data->audio.lock );
#endif
     direct_mutex_unlock( &data->video.lock );
     direct_mutex_unlock( &data->input.lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_Stop( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->status == DVSTATE_STOP)
          return DFB_OK;

     direct_mutex_lock( &data->input.lock );

     data->status = DVSTATE_STOP;

     if (data->input.thread) {
          direct_thread_join( data->input.thread );
          direct_thread_destroy( data->input.thread );
          data->input.thread = NULL;
     }

     if (data->video.thread) {
          direct_waitqueue_signal( &data->video.cond );
          direct_thread_join( data->video.thread );
          direct_thread_destroy( data->video.thread );
          data->video.thread = NULL;
     }

     data->video.pts = 0;

     if (data->seekable) {
          av_seek_frame( data->fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD );
          flush_packets( &data->video.queue );
     }

#ifdef HAVE_FUSIONSOUND
     if (data->audio.thread) {
          direct_waitqueue_signal( &data->audio.cond );
          direct_thread_join( data->audio.thread );
          direct_thread_destroy( data->audio.thread );
          data->audio.thread = NULL;
     }

     data->audio.pts = 0;
#endif

     dispatch_event( data, DVPET_STOPPED );

     direct_mutex_unlock( &data->input.lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_GetStatus( IDirectFBVideoProvider *thiz,
                                         DFBVideoProviderStatus *ret_status )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_status)
          return DFB_INVARG;

     if (data->status == DVSTATE_PLAY && data->input.buffering)
          *ret_status = DVSTATE_BUFFERING;
     else
          *ret_status = data->status;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_SeekTo( IDirectFBVideoProvider *thiz,
                                      double                  seconds )
{
     s64    time;
     double pos = 0.0;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (seconds < 0.0)
          return DFB_INVARG;

     if (!data->seekable)
          return DFB_UNSUPPORTED;

     direct_mutex_lock( &data->input.lock );

     time = get_stream_clock( data ) - data->start_time;
     pos  = (time < 0) ? 0.0 : (double) time / AV_TIME_BASE;

     time = seconds * AV_TIME_BASE;

     if (data->fmt_ctx->duration != AV_NOPTS_VALUE && time > data->fmt_ctx->duration)
          return DFB_OK;

     data->input.seeked    = true;
     data->input.seek_time = time;
     data->input.seek_flag = (seconds < pos) ? AVSEEK_FLAG_BACKWARD : 0;

     direct_mutex_unlock( &data->input.lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_GetPos( IDirectFBVideoProvider *thiz,
                                      double                 *ret_seconds )
{
     s64 position;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DFB_INVARG;

     position = get_stream_clock( data ) - data->start_time;

     *ret_seconds = (position < 0) ? 0.0 : (double) position / AV_TIME_BASE;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_GetLength( IDirectFBVideoProvider *thiz,
                                         double                 *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DFB_INVARG;

     if (data->fmt_ctx->duration != AV_NOPTS_VALUE) {
          *ret_seconds = (double) data->fmt_ctx->duration / AV_TIME_BASE;
          return DFB_OK;
     }

     *ret_seconds = 0.0;

     return DFB_UNSUPPORTED;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_SetPlaybackFlags( IDirectFBVideoProvider        *thiz,
                                                DFBVideoProviderPlaybackFlags  flags )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (flags & ~DVPLAY_LOOPING)
          return DFB_UNSUPPORTED;

     if (flags & DVPLAY_LOOPING && !data->seekable)
          return DFB_UNSUPPORTED;

     data->flags = flags;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_SetSpeed( IDirectFBVideoProvider *thiz,
                                        double                  multiplier )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (multiplier < 0.0 || multiplier > 32.0)
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

     if (multiplier && !data->speed) {
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
IDirectFBVideoProvider_FFmpeg_GetSpeed( IDirectFBVideoProvider *thiz,
                                        double                 *ret_multiplier )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_multiplier)
          return DFB_INVARG;

     *ret_multiplier = data->speed;

     return DFB_OK;
}

#ifdef HAVE_FUSIONSOUND
static DFBResult
IDirectFBVideoProvider_FFmpeg_SetVolume( IDirectFBVideoProvider *thiz,
                                         float                   level )
{
     DFBResult ret = DFB_UNSUPPORTED;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_FFmpeg_GetVolume( IDirectFBVideoProvider *thiz,
                                         float                  *ret_level )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_level)
          return DFB_INVARG;

     *ret_level = data->audio.volume;

     return DFB_OK;
}
#endif

static DFBResult
IDirectFBVideoProvider_FFmpeg_CreateEventBuffer( IDirectFBVideoProvider  *thiz,
                                                 IDirectFBEventBuffer   **ret_interface )
{
     DFBResult             ret;
     IDirectFBEventBuffer *buffer;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_FFmpeg_AttachEventBuffer( IDirectFBVideoProvider *thiz,
                                                 IDirectFBEventBuffer   *buffer )
{
     DFBResult  ret;
     EventLink *link;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_FFmpeg_EnableEvents( IDirectFBVideoProvider    *thiz,
                                            DFBVideoProviderEventType  mask )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (mask & ~DVPET_ALL)
          return DFB_INVARG;

     data->events_mask |= mask;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_DisableEvents( IDirectFBVideoProvider    *thiz,
                                             DFBVideoProviderEventType  mask )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (mask & ~DVPET_ALL)
          return DFB_INVARG;

     data->events_mask &= ~mask;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_FFmpeg_DetachEventBuffer( IDirectFBVideoProvider *thiz,
                                                 IDirectFBEventBuffer   *buffer )
{
     DFBResult  ret = DFB_ITEMNOTFOUND;
     EventLink *link;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBVideoProvider_FFmpeg_SetDestination( IDirectFBVideoProvider *thiz,
                                              IDirectFBSurface       *dest,
                                              const DFBRectangle     *dest_rect )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p, %4d,%4d-%4dx%4d )\n", __FUNCTION__,
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
     DFBResult            ret;
     AVProbeData          pd;
     AVInputFormat       *fmt;
     unsigned char        buf[2048];
     unsigned int         len;
     IDirectFBDataBuffer *buffer = ctx->buffer;

     ret = buffer->WaitForData( buffer, sizeof(buf) );
     if (ret == DFB_OK)
          ret = buffer->PeekData( buffer, sizeof(buf), 0, buf, &len );

     if (ret != DFB_OK)
          return ret;

     av_register_all();

     pd.filename = ctx->filename;
     pd.buf      = &buf[0];
     pd.buf_size = len;

     fmt = av_probe_input_format( &pd, 1 );
     if (fmt) {
          if (fmt->name) {
               /* Ignore formats that are known to not contain video stream. */
               if (!strcmp( fmt->name, "aac" ) ||
                   !strcmp( fmt->name, "ac3" ) ||
                   !strcmp( fmt->name, "au"  ) ||
                   !strcmp( fmt->name, "mp2" ) ||
                   !strcmp( fmt->name, "mp3" ) ||
                   !strcmp( fmt->name, "wav" ))
                    return DFB_UNSUPPORTED;
          }

          return DFB_OK;
     }

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBVideoProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     int                       i;
     AVProbeData               pd;
     AVInputFormat            *fmt;
     unsigned char             buf[2048];
     unsigned int              len;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBVideoProvider_FFmpeg )

     D_DEBUG_AT( VideoProvider_FFMPEG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->buffer = buffer;

     /* Increase the data buffer reference counter. */
     buffer->AddRef( buffer );

     data->idirectfb = idirectfb;

     data->seekable = (buffer->SeekTo( buffer, 0 ) == DFB_OK);

     ret = buffer->PeekData( buffer, sizeof(buf), 0, buf, &len );
     if (ret)
          goto error;

     pd.filename = buffer_data->filename;
     pd.buf      = &buf[0];
     pd.buf_size = len;

     fmt = av_probe_input_format( &pd, 1 );
     if (!fmt) {
          D_ERROR( "VideoProvider/FFMPEG: Failed to guess the file format!\n" );
          ret = DFB_INIT;
          goto error;
     }

     data->io_buf = av_malloc( IO_BUFFER_SIZE * 1024 );
     if (!data->io_buf) {
          ret = D_OOM();
          goto error;
     }

     data->io_ctx = avio_alloc_context( data->io_buf, IO_BUFFER_SIZE * 1024, 0, data->buffer, av_read_callback,
                                        NULL, data->seekable ? av_seek_callback : NULL );
     if (!data->io_ctx) {
          av_free( data->io_buf );
          ret = D_OOM();
          goto error;
     }

     data->fmt_ctx = avformat_alloc_context();
     if (!data->fmt_ctx) {
          avio_close( data->io_ctx );
          ret = D_OOM();
          goto error;
     }

     data->fmt_ctx->pb = data->io_ctx;

     if (avformat_open_input( &data->fmt_ctx, pd.filename, fmt, NULL ) < 0) {
          D_ERROR( "VideoProvider/FFMPEG: Failed to open stream!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     if (avformat_find_stream_info( data->fmt_ctx, NULL ) < 0) {
          D_ERROR( "VideoProvider/FFMPEG: Couldn't find stream info!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     for (i = 0; i < data->fmt_ctx->nb_streams; i++) {
          switch (data->fmt_ctx->streams[i]->codec->codec_type) {
               case AVMEDIA_TYPE_VIDEO:
                    if (!data->video.st || data->video.st->codec->bit_rate < data->fmt_ctx->streams[i]->codec->bit_rate)
                         data->video.st = data->fmt_ctx->streams[i];
                    break;
#ifdef HAVE_FUSIONSOUND
               case AVMEDIA_TYPE_AUDIO:
                    if (!data->audio.st || data->audio.st->codec->bit_rate < data->fmt_ctx->streams[i]->codec->bit_rate)
                         data->audio.st = data->fmt_ctx->streams[i];
                    break;
#endif
               default:
                    break;
          }
     }

     if (!data->video.st) {
          D_ERROR( "VideoProvider/FFMPEG: Couldn't find video stream!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     data->video.ctx = data->video.st->codec;

     data->desc.flags  = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width  = data->video.ctx->width;
     data->desc.height = data->video.ctx->height;
     switch (data->video.ctx->pix_fmt) {
          case AV_PIX_FMT_RGB555:
               data->desc.pixelformat = DSPF_ARGB1555;
               break;
          case AV_PIX_FMT_RGB565:
               data->desc.pixelformat = DSPF_RGB16;
               break;
          case AV_PIX_FMT_RGB24:
          case AV_PIX_FMT_BGR24:
               data->desc.pixelformat = DSPF_RGB24;
               break;
          case AV_PIX_FMT_RGB32:
          case AV_PIX_FMT_BGR32:
               data->desc.pixelformat = DSPF_RGB32;
               break;
          case AV_PIX_FMT_YUYV422:
               data->desc.pixelformat = DSPF_YUY2;
               break;
          case AV_PIX_FMT_UYVY422:
               data->desc.pixelformat = DSPF_UYVY;
               break;
          case AV_PIX_FMT_YUV420P:
               data->desc.pixelformat = DSPF_I420;
               break;
          case AV_PIX_FMT_YUV422P:
               data->desc.pixelformat = DSPF_Y42B;
               break;
          case AV_PIX_FMT_YUV444P:
               data->desc.pixelformat = DSPF_Y444;
               break;
          case AV_PIX_FMT_NV12:
               data->desc.pixelformat = DSPF_NV12;
               break;
          case AV_PIX_FMT_NV21:
               data->desc.pixelformat = DSPF_NV21;
               break;
          default:
               data->desc.pixelformat = dfb_primary_layer_pixelformat();
               break;
     }

     data->rate = av_q2d( data->video.st->r_frame_rate );
     if (!data->rate || !finite( data->rate )) {
          D_INFO( "VideoProvider/FFMPEG: Assuming 25 fps\n" );
          data->rate = 25.0;
     }

     data->video.codec = avcodec_find_decoder( data->video.ctx->codec_id );

     if (!data->video.codec || avcodec_open2( data->video.ctx, data->video.codec, NULL ) < 0) {
          D_ERROR( "VideoProvider/FFMPEG: Failed to open video codec!\n" );
          data->video.ctx = NULL;
          ret = DFB_FAILURE;
          goto error;
     }

     data->video.src_frame = av_frame_alloc();
     if (!data->video.src_frame) {
          ret = D_OOM();
          goto error;
     }

     data->video.queue.max_len = av_rescale_q( MAX_QUEUE_LEN * AV_TIME_BASE, AV_TIME_BASE_Q,
                                               data->video.st->time_base );
     if (data->video.ctx->bit_rate > 0)
          data->video.queue.max_size = MAX_QUEUE_LEN * data->video.ctx->bit_rate / 8;
     else
          data->video.queue.max_size = MAX_QUEUE_LEN * 256 * 1024;

#ifdef HAVE_FUSIONSOUND
     if (data->audio.st) {
          data->audio.ctx   = data->audio.st->codec;
          data->audio.codec = avcodec_find_decoder( data->audio.ctx->codec_id );

          if (!data->audio.codec || avcodec_open2( data->audio.ctx, data->audio.codec, NULL ) < 0) {
               data->audio.st    = NULL;
               data->audio.ctx   = NULL;
               data->audio.codec = NULL;
          }
     }

     if (data->audio.st &&
         FusionSoundInit( NULL, NULL ) == DR_OK && FusionSoundCreate( &data->audio.sound ) == DR_OK) {
          FSStreamDescription dsc;

          if (data->audio.ctx->channels > FS_MAX_CHANNELS)
               data->audio.ctx->channels = FS_MAX_CHANNELS;

          dsc.flags        = FSSDF_BUFFERSIZE | FSSDF_CHANNELS | FSSDF_SAMPLEFORMAT | FSSDF_SAMPLERATE;
          dsc.channels     = data->audio.ctx->channels;
          dsc.samplerate   = data->audio.ctx->sample_rate;
          dsc.buffersize   = dsc.samplerate / 10;
          dsc.sampleformat = FSSF_S16;

          ret = data->audio.sound->CreateStream( data->audio.sound, &dsc, &data->audio.stream );
          if (ret != DFB_OK) {
               D_ERROR( "VideoProvider/FFMPEG: Failed to create FusionSound stream!\n" );
               goto error;
          }
          else {
               data->audio.stream->GetPlayback( data->audio.stream, &data->audio.playback );
          }
     }
     else if (data->audio.st) {
          D_ERROR( "VideoProvider/FFMPEG: Failed to initialize/create FusionSound!\n" );
          goto error;
     }

     if (data->audio.stream) {
          data->audio.src_frame = av_frame_alloc();
          if (!data->audio.src_frame) {
               ret = D_OOM();
               goto error;
          }

          data->audio.queue.max_len = av_rescale_q( MAX_QUEUE_LEN * AV_TIME_BASE, AV_TIME_BASE_Q,
                                                    data->audio.st->time_base );
          if (data->audio.ctx->bit_rate > 0)
               data->audio.queue.max_size = MAX_QUEUE_LEN * data->audio.ctx->bit_rate / 8;
          else
               data->audio.queue.max_size = MAX_QUEUE_LEN * 64 * 1024;
     }
#endif

     data->status = DVSTATE_STOP;
     data->speed  = 1.0;

     if (data->fmt_ctx->start_time != AV_NOPTS_VALUE)
          data->start_time = data->fmt_ctx->start_time;

     data->events_mask = DVPET_ALL;

     direct_mutex_init( &data->events_lock );

     direct_mutex_init( &data->input.lock );

     direct_mutex_init( &data->video.lock );
     direct_waitqueue_init( &data->video.cond );
     direct_mutex_init( &data->video.queue.lock );

#ifdef HAVE_FUSIONSOUND
     data->audio.volume = 1.0;

     direct_mutex_init( &data->audio.lock );
     direct_waitqueue_init( &data->audio.cond );
     direct_mutex_init( &data->audio.queue.lock );
#endif

     thiz->AddRef                = IDirectFBVideoProvider_FFmpeg_AddRef;
     thiz->Release               = IDirectFBVideoProvider_FFmpeg_Release;
     thiz->GetCapabilities       = IDirectFBVideoProvider_FFmpeg_GetCapabilities;
     thiz->GetSurfaceDescription = IDirectFBVideoProvider_FFmpeg_GetSurfaceDescription;
     thiz->GetStreamDescription  = IDirectFBVideoProvider_FFmpeg_GetStreamDescription;
     thiz->PlayTo                = IDirectFBVideoProvider_FFmpeg_PlayTo;
     thiz->Stop                  = IDirectFBVideoProvider_FFmpeg_Stop;
     thiz->GetStatus             = IDirectFBVideoProvider_FFmpeg_GetStatus;
     thiz->SeekTo                = IDirectFBVideoProvider_FFmpeg_SeekTo;
     thiz->GetPos                = IDirectFBVideoProvider_FFmpeg_GetPos;
     thiz->GetLength             = IDirectFBVideoProvider_FFmpeg_GetLength;
     thiz->SetPlaybackFlags      = IDirectFBVideoProvider_FFmpeg_SetPlaybackFlags;
     thiz->SetSpeed              = IDirectFBVideoProvider_FFmpeg_SetSpeed;
     thiz->GetSpeed              = IDirectFBVideoProvider_FFmpeg_GetSpeed;
#ifdef HAVE_FUSIONSOUND
     thiz->SetVolume             = IDirectFBVideoProvider_FFmpeg_SetVolume;
     thiz->GetVolume             = IDirectFBVideoProvider_FFmpeg_GetVolume;
#endif
     thiz->CreateEventBuffer     = IDirectFBVideoProvider_FFmpeg_CreateEventBuffer;
     thiz->AttachEventBuffer     = IDirectFBVideoProvider_FFmpeg_AttachEventBuffer;
     thiz->EnableEvents          = IDirectFBVideoProvider_FFmpeg_EnableEvents;
     thiz->DisableEvents         = IDirectFBVideoProvider_FFmpeg_DisableEvents;
     thiz->DetachEventBuffer     = IDirectFBVideoProvider_FFmpeg_DetachEventBuffer;
     thiz->SetDestination        = IDirectFBVideoProvider_FFmpeg_SetDestination;

     return DFB_OK;

error:
#ifdef HAVE_FUSIONSOUND
     if (data->audio.src_frame)
          av_free( data->audio.src_frame );

     if (data->audio.playback)
          data->audio.playback->Release( data->audio.playback );

     if (data->audio.stream)
          data->audio.stream->Release( data->audio.stream );

     if (data->audio.sound)
          data->audio.sound->Release( data->audio.sound );

     if (data->audio.ctx)
          avcodec_close( data->audio.ctx );
#endif

     if (data->video.src_frame)
          av_free( data->video.src_frame );

     if (data->video.ctx)
          avcodec_close( data->video.ctx );

     if (data->fmt_ctx)
          avformat_close_input( &data->fmt_ctx );

     buffer->Release( buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
