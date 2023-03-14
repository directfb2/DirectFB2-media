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

#include <direct/stream.h>
#include <direct/thread.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <media/ifusionsoundmusicprovider.h>

D_DEBUG_DOMAIN( MusicProvider_FFmpeg, "MusicProvider/FFmpeg", "FFmpeg Music Provider" );

static DirectResult Probe    ( IFusionSoundMusicProvider_ProbeContext *ctx );

static DirectResult Construct( IFusionSoundMusicProvider              *thiz,
                               const char                             *filename,
                               DirectStream                           *stream );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IFusionSoundMusicProvider, FFmpeg )

/**********************************************************************************************************************/

typedef struct {
     int                           ref;                     /* reference counter */

     DirectStream                 *stream;

     unsigned char                *io_buf;
     AVIOContext                  *io_ctx;
     AVFormatContext              *fmt_ctx;
     AVStream                     *st;
     AVFrame                      *frame;
     AVCodecContext               *codec_ctx;

     int                           channels;
     int                           samplerate;

     s64                           pts;                     /* presentation timestamp */

     FSTrackDescription            desc;

     FSMusicProviderPlaybackFlags  flags;

     DirectThread                 *thread;
     DirectMutex                   lock;
     DirectWaitQueue               cond;

     FSMusicProviderStatus         status;
     int                           finished;
     int                           seeked;

     struct {
          IFusionSoundStream      *stream;
          IFusionSoundBuffer      *buffer;
          enum AVSampleFormat      sample_fmt;
          int64_t                  ch_layout;
     } dest;

     FMBufferCallback              buffer_callback;
     void                         *buffer_callback_context;
} IFusionSoundMusicProvider_FFmpeg_data;

#define IO_BUFFER_SIZE 8 /* in kilobytes */

/**********************************************************************************************************************/

static int
av_read_callback( void    *opaque,
                  uint8_t *buf,
                  int      size )
{
     DirectResult  ret;
     unsigned int  length = 0;
     DirectStream *stream = opaque;

     if (!buf || size < 0)
          return -1;

     while (size) {
          unsigned int len;

          direct_stream_wait( stream, size, NULL );

          ret = direct_stream_read( stream, size, buf + length, &len );
          if (ret) {
               if (!length)
                    return (ret == DR_EOF) ? 0 : -1;
               break;
          }

          length += len;
          size   -= len;
     }

     return length;
}

static int64_t
av_seek_callback( void    *opaque,
                  int64_t  offset,
                  int      whence )
{
     DirectResult  ret;
     unsigned int  pos    = 0;
     DirectStream *stream = opaque;

     switch (whence) {
          case SEEK_SET:
               ret = direct_stream_seek( stream, offset );
               break;
          case SEEK_CUR:
               pos = direct_stream_offset( stream );
               if (!offset)
                    return pos;
               ret = direct_stream_seek( stream, pos + offset );
               break;
          case SEEK_END:
               pos = direct_stream_length( stream );
               if (offset < 0)
                    return pos;
               ret = direct_stream_seek( stream, pos - offset );
               break;
          default:
               ret = DR_UNSUPPORTED;
               break;
     }

     if (ret != DR_OK)
          return -1;

     pos = direct_stream_offset( stream );

     return pos;
}

/**********************************************************************************************************************/

static void
FFmpeg_Stop( IFusionSoundMusicProvider_FFmpeg_data *data,
             bool                                   now )
{
     data->status = FMSTATE_STOP;

     if (data->thread) {
          if (!direct_thread_is_joined( data->thread )) {
               if (now) {
                    direct_thread_cancel( data->thread );
                    direct_thread_join( data->thread );
               }
               else {
                    /* Mutex must already be locked. */
                    direct_mutex_unlock( &data->lock );
                    direct_thread_join( data->thread );
                    direct_mutex_lock( &data->lock );
               }
          }
          direct_thread_destroy( data->thread );
          data->thread = NULL;
     }

     if (data->dest.stream) {
          data->dest.stream->Release( data->dest.stream );
          data->dest.stream = NULL;
     }

     if (data->dest.buffer) {
          data->dest.buffer->Release( data->dest.buffer );
          data->dest.buffer = NULL;
     }
}

static void *
FFmpegStream( DirectThread *thread,
              void         *arg )
{
     struct SwrContext                     *swr_ctx;
     AVPacket                               pkt;
     int                                    pkt_size       = 0;
     IFusionSoundMusicProvider_FFmpeg_data *data           = arg;
     int                                    bytespersample = av_get_bytes_per_sample( data->dest.sample_fmt ) *
                                                             av_get_channel_layout_nb_channels( data->dest.ch_layout );
     u8                                     buf[bytespersample * data->samplerate];

     swr_ctx = swr_alloc_set_opts( NULL,
                                   data->dest.ch_layout, data->dest.sample_fmt,
                                   data->samplerate,
                                   data->codec_ctx->channel_layout, data->codec_ctx->sample_fmt,
                                   data->samplerate, 0, NULL );

     swr_init( swr_ctx );

     while (data->status == FMSTATE_PLAY) {
          int decoded;
          int got_frame;
          int length = 0;

          direct_mutex_lock( &data->lock );

          if (data->status != FMSTATE_PLAY) {
               direct_mutex_unlock( &data->lock );
               break;
          }

          if (data->seeked) {
               data->dest.stream->Flush( data->dest.stream );
               if (pkt_size > 0) {
                    av_free_packet( &pkt );
                    pkt_size = 0;
               }
               avcodec_flush_buffers( data->codec_ctx );
               data->seeked = false;
          }

          if (pkt_size <= 0) {
               s64 pkt_pts;

               if (av_read_frame( data->fmt_ctx, &pkt ) < 0) {
                    if (!(data->flags & FMPLAY_LOOPING) || av_seek_frame( data->fmt_ctx, -1, 0, 0 ) < 0) {
                         data->finished = true;
                         data->status = FMSTATE_FINISHED;
                         direct_waitqueue_broadcast( &data->cond );
                    }
                    direct_mutex_unlock( &data->lock );
                    continue;
               }

               if (pkt.stream_index != data->st->index) {
                    av_free_packet( &pkt );
                    direct_mutex_unlock( &data->lock );
                    continue;
               }

               pkt_size = pkt.size;
               pkt_pts  = pkt.pts;
               if (pkt_pts != AV_NOPTS_VALUE) {
                    if (data->st->start_time != AV_NOPTS_VALUE)
                         pkt_pts -= data->st->start_time;
                    data->pts = av_rescale_q( pkt_pts, data->st->time_base, AV_TIME_BASE_Q );
               }
          }

          decoded = avcodec_decode_audio4( data->codec_ctx, data->frame, &got_frame, &pkt );

          if (decoded < 0) {
               av_free_packet( &pkt );
               pkt_size = 0;
          }
          else {
               pkt_size -= decoded;
               if (pkt_size <= 0)
                    av_free_packet( &pkt );

               length = data->frame->nb_samples;
               data->pts += (s64) length * AV_TIME_BASE / data->samplerate;
          }

          direct_mutex_unlock( &data->lock );

          /* Converting to output format. */
          if (length) {
               uint8_t *out[] = { buf };

               swr_convert( swr_ctx, out, data->samplerate, (void*) data->frame->data, length );

               data->dest.stream->Write( data->dest.stream, buf, length );
          }
     }

     if (pkt_size > 0)
          av_free_packet( &pkt );

     swr_free( &swr_ctx );

     return NULL;
}

static void *
FFmpegBuffer( DirectThread *thread,
              void         *arg )
{
     struct SwrContext                     *swr_ctx;
     AVPacket                               pkt;
     int                                    pkt_size       = 0;
     IFusionSoundMusicProvider_FFmpeg_data *data           = arg;
     int                                    pos            = 0;
     int                                    bytespersample = av_get_bytes_per_sample( data->dest.sample_fmt ) *
                                                             av_get_channel_layout_nb_channels( data->dest.ch_layout );

     swr_ctx = swr_alloc_set_opts( NULL,
                                   data->dest.ch_layout, data->dest.sample_fmt, data->samplerate,
                                   data->codec_ctx->channel_layout, data->codec_ctx->sample_fmt, data->samplerate,
                                   0, NULL );

     swr_init( swr_ctx );

     while (data->status == FMSTATE_PLAY) {
          int decoded;
          int got_frame;
          int length = 0;

          direct_mutex_lock( &data->lock );

          if (data->status != FMSTATE_PLAY) {
               direct_mutex_unlock( &data->lock );
               break;
          }

          if (data->seeked) {
               if (pkt_size > 0) {
                    av_free_packet( &pkt );
                    pkt_size = 0;
               }
               avcodec_flush_buffers( data->codec_ctx );
               data->seeked = false;
          }

          if (pkt_size <= 0) {
               s64 pkt_pts;

               if (av_read_frame( data->fmt_ctx, &pkt ) < 0) {
                    if (!(data->flags & FMPLAY_LOOPING) || av_seek_frame( data->fmt_ctx, -1, 0, 0 ) < 0) {
                         data->finished = true;
                         data->status = FMSTATE_FINISHED;
                         if (data->buffer_callback && pos) {
                              if (data->buffer_callback( pos, data->buffer_callback_context ))
                                   data->status = FMSTATE_STOP;
                         }
                         direct_waitqueue_broadcast( &data->cond );
                    }
                    direct_mutex_unlock( &data->lock );
                    continue;
               }

               if (pkt.stream_index != data->st->index) {
                    av_free_packet( &pkt );
                    direct_mutex_unlock( &data->lock );
                    continue;
               }

               pkt_size = pkt.size;
               pkt_pts  = pkt.pts;
               if (pkt_pts != AV_NOPTS_VALUE) {
                    if (data->st->start_time != AV_NOPTS_VALUE)
                         pkt_pts -= data->st->start_time;
                    data->pts = av_rescale_q( pkt_pts, data->st->time_base, AV_TIME_BASE_Q );
               }
          }

          decoded = avcodec_decode_audio4( data->codec_ctx, data->frame, &got_frame, &pkt );

          if (decoded < 0) {
               av_free_packet( &pkt );
               pkt_size = 0;
          }
          else {
               pkt_size -= decoded;
               if (pkt_size <= 0)
                    av_free_packet( &pkt );

               length = data->frame->nb_samples;
               data->pts += (s64) length * AV_TIME_BASE / data->samplerate;
          }

          /* Converting to output format. */
          while (length) {
               DirectResult  ret;
               int           len;
               int           frames;
               void         *dst;
               uint8_t      *out[1];

               ret = data->dest.buffer->Lock( data->dest.buffer, &dst, &frames, NULL );
               if (ret) {
                    D_DERROR( ret, "MusicProvider/FFmpeg: Could not lock buffer!\n" );
                    break;
               }

               len = MIN( frames - pos, length );

               dst += pos * bytespersample;
               *out =dst;
               swr_convert( swr_ctx, out, data->samplerate, (void*) data->frame->data, len );

               length -= len;
               pos    += len;

               data->dest.buffer->Unlock( data->dest.buffer );

               if (pos >= frames) {
                    if (data->buffer_callback) {
                         if (data->buffer_callback( pos, data->buffer_callback_context )) {
                              data->status = FMSTATE_STOP;
                              direct_waitqueue_broadcast( &data->cond );
                              break;
                         }
                    }
                    pos = 0;
               }
          }

          direct_mutex_unlock( &data->lock );
     }

     if (pkt_size > 0)
          av_free_packet( &pkt );

     swr_free( &swr_ctx );

     return NULL;
}

/**********************************************************************************************************************/

static void
IFusionSoundMusicProvider_FFmpeg_Destruct( IFusionSoundMusicProvider *thiz )
{
     IFusionSoundMusicProvider_FFmpeg_data *data = thiz->priv;

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     FFmpeg_Stop( data, true );

     direct_stream_destroy( data->stream );

     direct_waitqueue_deinit( &data->cond );
     direct_mutex_deinit( &data->lock );

     avcodec_close( data->codec_ctx );
     av_free( data->frame );
     avformat_close_input( &data->fmt_ctx );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_AddRef( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     data->ref++;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_Release( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IFusionSoundMusicProvider_FFmpeg_Destruct( thiz );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_GetCapabilities( IFusionSoundMusicProvider   *thiz,
                                                  FSMusicProviderCapabilities *ret_caps )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (!ret_caps)
          return DR_INVARG;

     *ret_caps = FMCAPS_BASIC;
     if (direct_stream_seekable( data->stream ))
          *ret_caps |= FMCAPS_SEEK;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_GetTrackDescription( IFusionSoundMusicProvider *thiz,
                                                      FSTrackDescription        *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DR_INVARG;

     *ret_desc = data->desc;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_GetStreamDescription( IFusionSoundMusicProvider *thiz,
                                                       FSStreamDescription       *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DR_INVARG;

     ret_desc->flags        = FSSDF_BUFFERSIZE | FSSDF_CHANNELS | FSSDF_SAMPLEFORMAT | FSSDF_SAMPLERATE;
     ret_desc->buffersize   = data->samplerate / 8;
     ret_desc->channels     = data->channels;
     ret_desc->sampleformat = FSSF_S16;
     ret_desc->samplerate   = data->samplerate;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_GetBufferDescription( IFusionSoundMusicProvider *thiz,
                                                       FSBufferDescription       *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DR_INVARG;

     ret_desc->flags        = FSBDF_LENGTH | FSBDF_CHANNELS | FSBDF_SAMPLEFORMAT | FSBDF_SAMPLERATE;
     if (data->st->nb_frames)
          ret_desc->length  = MIN( data->st->nb_frames, FS_MAX_FRAMES);
     else
          ret_desc->length  = MIN( data->fmt_ctx->duration * data->samplerate / AV_TIME_BASE, FS_MAX_FRAMES );
     ret_desc->channels     = data->channels;
     ret_desc->sampleformat = FSSF_S16;
     ret_desc->samplerate   = data->samplerate;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_PlayToStream( IFusionSoundMusicProvider *thiz,
                                               IFusionSoundStream        *destination )
{
     FSStreamDescription desc;
     enum AVSampleFormat sample_fmt;
     int64_t             ch_layout;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (!destination)
          return DR_INVARG;

     if (destination == data->dest.stream)
          return DR_OK;

     destination->GetDescription( destination, &desc );

     if (desc.samplerate != data->samplerate)
          return DR_UNSUPPORTED;

     switch (desc.sampleformat) {
          case FSSF_U8:
               sample_fmt = AV_SAMPLE_FMT_U8;
               break;
          case FSSF_S16:
               sample_fmt = AV_SAMPLE_FMT_S16;
               break;
          case FSSF_S32:
               sample_fmt = AV_SAMPLE_FMT_S32;
               break;
          case FSSF_FLOAT:
               sample_fmt = AV_SAMPLE_FMT_FLT;
               break;
          case FSSF_S24:
          default:
               return DR_UNSUPPORTED;
     }

     if (desc.channels > 6)
          return DR_UNSUPPORTED;

     switch (desc.channelmode) {
          case FSCM_MONO:
               ch_layout = AV_CH_LAYOUT_MONO;
               break;
          case FSCM_STEREO:
               ch_layout = AV_CH_LAYOUT_STEREO;
               break;
          case FSCM_STEREO21:
               ch_layout = AV_CH_LAYOUT_2POINT1;
               break;
          case FSCM_STEREO30:
               ch_layout = AV_CH_LAYOUT_SURROUND;
               break;
          case FSCM_STEREO31:
               ch_layout = AV_CH_LAYOUT_3POINT1;
               break;
          case FSCM_SURROUND30:
               ch_layout = AV_CH_LAYOUT_2_1;
               break;
          case FSCM_SURROUND40_2F2R:
               ch_layout = AV_CH_LAYOUT_QUAD;
               break;
          case FSCM_SURROUND40_3F1R:
               ch_layout = AV_CH_LAYOUT_4POINT0;
               break;
          case FSCM_SURROUND41_3F1R:
               ch_layout = AV_CH_LAYOUT_4POINT1;
               break;
          case FSCM_SURROUND50:
               ch_layout = AV_CH_LAYOUT_5POINT0_BACK;
               break;
          case FSCM_SURROUND51:
               ch_layout = AV_CH_LAYOUT_5POINT1_BACK;
               break;
          case FSCM_SURROUND31:
          case FSCM_SURROUND41_2F2R:
          default:
               return DR_UNSUPPORTED;
     }

     direct_mutex_lock( &data->lock );

     FFmpeg_Stop( data, false );

     /* Increase the sound stream reference counter. */
     destination->AddRef( destination );

     data->dest.stream     = destination;
     data->dest.sample_fmt = sample_fmt;
     data->dest.ch_layout  = ch_layout;

     if (data->finished) {
          if (av_seek_frame( data->fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD ) < 0) {
               direct_mutex_unlock( &data->lock );
               return DR_UNSUPPORTED;
          }
          data->finished = false;
     }

     data->status = FMSTATE_PLAY;

     direct_waitqueue_broadcast( &data->cond );

     data->thread = direct_thread_create( DTT_DEFAULT, FFmpegStream, data, "FFmpeg Stream" );

     direct_mutex_unlock( &data->lock );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_PlayToBuffer( IFusionSoundMusicProvider *thiz,
                                               IFusionSoundBuffer        *destination,
                                               FMBufferCallback           callback,
                                               void                      *ctx )
{
     FSBufferDescription desc;
     enum AVSampleFormat sample_fmt;
     int64_t             ch_layout;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (!destination)
          return DR_INVARG;

     if (destination == data->dest.buffer)
          return DR_OK;

     destination->GetDescription( destination, &desc );

     if (desc.samplerate != data->samplerate)
          return DR_UNSUPPORTED;

     switch (desc.sampleformat) {
          case FSSF_U8:
               sample_fmt = AV_SAMPLE_FMT_U8;
               break;
          case FSSF_S16:
               sample_fmt = AV_SAMPLE_FMT_S16;
               break;
          case FSSF_S32:
               sample_fmt = AV_SAMPLE_FMT_S32;
               break;
          case FSSF_FLOAT:
               sample_fmt = AV_SAMPLE_FMT_FLT;
               break;
          case FSSF_S24:
          default:
               return DR_UNSUPPORTED;
     }

     if (desc.channels > 6)
          return DR_UNSUPPORTED;

     switch (desc.channelmode) {
          case FSCM_MONO:
               ch_layout = AV_CH_LAYOUT_MONO;
               break;
          case FSCM_STEREO:
               ch_layout = AV_CH_LAYOUT_STEREO;
               break;
          case FSCM_STEREO21:
               ch_layout = AV_CH_LAYOUT_2POINT1;
               break;
          case FSCM_STEREO30:
               ch_layout = AV_CH_LAYOUT_SURROUND;
               break;
          case FSCM_STEREO31:
               ch_layout = AV_CH_LAYOUT_3POINT1;
               break;
          case FSCM_SURROUND30:
               ch_layout = AV_CH_LAYOUT_2_1;
               break;
          case FSCM_SURROUND40_2F2R:
               ch_layout = AV_CH_LAYOUT_QUAD;
               break;
          case FSCM_SURROUND40_3F1R:
               ch_layout = AV_CH_LAYOUT_4POINT0;
               break;
          case FSCM_SURROUND41_3F1R:
               ch_layout = AV_CH_LAYOUT_4POINT1;
               break;
          case FSCM_SURROUND50:
               ch_layout = AV_CH_LAYOUT_5POINT0_BACK;
               break;
          case FSCM_SURROUND51:
               ch_layout = AV_CH_LAYOUT_5POINT1_BACK;
               break;
          case FSCM_SURROUND31:
          case FSCM_SURROUND41_2F2R:
          default:
               return DR_UNSUPPORTED;
     }

     direct_mutex_lock( &data->lock );

     FFmpeg_Stop( data, false );

     /* Increase the sound buffer reference counter. */
     destination->AddRef( destination );

     data->dest.buffer             = destination;
     data->dest.sample_fmt         = sample_fmt;
     data->dest.ch_layout          = ch_layout;
     data->buffer_callback         = callback;
     data->buffer_callback_context = ctx;

     if (data->finished) {
          if (av_seek_frame( data->fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD ) < 0) {
               direct_mutex_unlock( &data->lock );
               return DR_UNSUPPORTED;
          }
          data->finished = false;
     }

     data->status = FMSTATE_PLAY;

     direct_waitqueue_broadcast( &data->cond );

     data->thread = direct_thread_create( DTT_DEFAULT, FFmpegBuffer, data, "FFmpeg Buffer" );

     direct_mutex_unlock( &data->lock );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_Stop( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     direct_mutex_lock( &data->lock );

     FFmpeg_Stop( data, false );

     direct_waitqueue_broadcast( &data->cond );

     direct_mutex_unlock( &data->lock );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_GetStatus( IFusionSoundMusicProvider *thiz,
                                            FSMusicProviderStatus     *status )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     if (!status)
          return DR_INVARG;

     *status = data->status;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_SeekTo( IFusionSoundMusicProvider *thiz,
                                         double                     seconds )
{
     DirectResult ret;
     s64          time;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (seconds < 0.0)
          return DR_INVARG;

     if (!direct_stream_seekable( data->stream ))
          return DR_UNSUPPORTED;

     time = seconds * AV_TIME_BASE;

     if (data->fmt_ctx->duration != AV_NOPTS_VALUE && time > data->fmt_ctx->duration)
          return DR_OK;

     direct_mutex_lock( &data->lock );

     if (av_seek_frame( data->fmt_ctx, -1, time, (time < data->pts) ? AVSEEK_FLAG_BACKWARD : 0 ) >= 0) {
          data->seeked = true;
          data->finished = false;
          data->pts = time;
          ret = DR_OK;
     }
     else {
          ret = DR_FAILURE;
     }

     direct_mutex_unlock( &data->lock );

     return ret;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_GetPos( IFusionSoundMusicProvider *thiz,
                                         double                    *ret_seconds )
{
     s64 position;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DR_INVARG;

     position = data->pts;

     if (data->dest.stream) {
          int delay = 0;

          data->dest.stream->GetPresentationDelay( data->dest.stream, &delay );

          position -= delay * 1000ll;
     }

     *ret_seconds = (position < 0) ? 0.0 : (double) position / AV_TIME_BASE;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_GetLength( IFusionSoundMusicProvider *thiz,
                                            double                    *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DR_INVARG;

     if (data->fmt_ctx->duration > 0) {
          *ret_seconds = (double) data->fmt_ctx->duration / AV_TIME_BASE;
          return DR_OK;
     }

     *ret_seconds = 0.0;

     return DR_UNSUPPORTED;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_SetPlaybackFlags( IFusionSoundMusicProvider    *thiz,
                                                   FSMusicProviderPlaybackFlags  flags )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (flags & ~FMPLAY_LOOPING)
          return DR_UNSUPPORTED;

     if (flags & FMPLAY_LOOPING && !direct_stream_seekable( data->stream ))
          return DR_UNSUPPORTED;

     data->flags = flags;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_FFmpeg_WaitStatus( IFusionSoundMusicProvider *thiz,
                                             FSMusicProviderStatus      mask,
                                             unsigned int               timeout )
{
     DirectResult ret;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     if (!mask || mask & ~FMSTATE_ALL)
          return DR_INVARG;

     if (timeout) {
          long long s;

          s = direct_clock_get_abs_micros() + timeout * 1000ll;

          while (direct_mutex_trylock( &data->lock )) {
               usleep( 1000 );
               if (direct_clock_get_abs_micros() >= s)
                    return DR_TIMEOUT;
          }

          while (!(data->status & mask)) {
               ret = direct_waitqueue_wait_timeout( &data->cond, &data->lock, timeout * 1000ll );
               if (ret) {
                    direct_mutex_unlock( &data->lock );
                    return ret;
               }
          }
     }
     else {
          direct_mutex_lock( &data->lock );

          while (!(data->status & mask))
               direct_waitqueue_wait( &data->cond, &data->lock );
     }

     direct_mutex_unlock( &data->lock );

     return DR_OK;
}

/**********************************************************************************************************************/

static DirectResult
Probe( IFusionSoundMusicProvider_ProbeContext *ctx )
{
     AVProbeData    pd;
     AVInputFormat *fmt;

     av_register_all();

     memset( &pd, 0, sizeof(AVProbeData) );
     pd.filename = ctx->filename;
     pd.buf      = ctx->header;
     pd.buf_size = sizeof(ctx->header);

     fmt = av_probe_input_format( &pd, 1 );
     if (fmt && fmt->name) {
          if (!strcmp( fmt->name, "ac3" ) ||
              !strcmp( fmt->name, "mp3" ))
               return DR_OK;
     }

     return DR_UNSUPPORTED;
}

static DirectResult
Construct( IFusionSoundMusicProvider *thiz,
           const char                *filename,
           DirectStream              *stream )
{
     DirectResult   ret;
     int            i;
     AVProbeData    pd;
     unsigned int   len;
     unsigned char  buf[64];
     AVInputFormat *fmt;
     AVCodec       *codec;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IFusionSoundMusicProvider_FFmpeg )

     D_DEBUG_AT( MusicProvider_FFmpeg, "%s( %p )n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->stream = direct_stream_dup( stream );

     direct_stream_peek( stream, sizeof(buf), 0, &buf[0], &len );

     memset( &pd, 0, sizeof(AVProbeData) );
     pd.filename = filename;
     pd.buf      = &buf[0];
     pd.buf_size = len;

     fmt = av_probe_input_format( &pd, 1 );
     if (!fmt) {
          D_ERROR( "MusicProvider/FFmpeg: Failed to guess the file format!\n" );
          ret = DR_INIT;
          goto error;
     }

     data->io_buf = av_malloc( IO_BUFFER_SIZE * 1024 );
     if (!data->io_buf) {
          ret = D_OOM();
          goto error;
     }

     data->io_ctx = avio_alloc_context( data->io_buf, IO_BUFFER_SIZE * 1024, 0, data->stream, av_read_callback,
                                        NULL, direct_stream_seekable( stream ) ? av_seek_callback : NULL );
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
          D_ERROR( "MusicProvider/FFmpeg: Failed to open stream!\n" );
          ret = DR_FAILURE;
          goto error;
     }

     if (avformat_find_stream_info( data->fmt_ctx, NULL ) < 0) {
          D_ERROR( "MusicProvider/FFmpeg: Couldn't find stream info!\n" );
          ret = DR_FAILURE;
          goto error;
     }

     for (i = 0; i < data->fmt_ctx->nb_streams; i++) {
          if (data->fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
               if (!data->st || data->st->codec->bit_rate < data->fmt_ctx->streams[i]->codec->bit_rate)
                    data->st = data->fmt_ctx->streams[i];
          }
     }

     if (!data->st) {
          D_ERROR( "MusicProvider/FFmpeg: Couldn't find audio stream!\n" );
          ret = DR_FAILURE;
          goto error;
     }

     data->codec_ctx = data->st->codec;

     codec = avcodec_find_decoder( data->codec_ctx->codec_id );

     if (!codec || avcodec_open2( data->codec_ctx, codec, NULL ) < 0) {
          D_ERROR( "MusicProvider/FFmpeg: Failed to open audio codec!\n" );
          data->codec_ctx = NULL;
          ret = DR_FAILURE;
          goto error;
     }

     data->frame = av_frame_alloc();

     data->channels   = MIN( data->codec_ctx->channels, FS_MAX_CHANNELS );
     data->samplerate = data->codec_ctx->sample_rate;

     snprintf( data->desc.encoding, FS_TRACK_DESC_ENCODING_LENGTH, "%s", data->codec_ctx->codec->name );

     data->desc.bitrate = data->codec_ctx->bit_rate;

     direct_mutex_init( &data->lock );
     direct_waitqueue_init( &data->cond );

     data->status = FMSTATE_STOP;

     thiz->AddRef               = IFusionSoundMusicProvider_FFmpeg_AddRef;
     thiz->Release              = IFusionSoundMusicProvider_FFmpeg_Release;
     thiz->GetCapabilities      = IFusionSoundMusicProvider_FFmpeg_GetCapabilities;
     thiz->GetTrackDescription  = IFusionSoundMusicProvider_FFmpeg_GetTrackDescription;
     thiz->GetStreamDescription = IFusionSoundMusicProvider_FFmpeg_GetStreamDescription;
     thiz->GetBufferDescription = IFusionSoundMusicProvider_FFmpeg_GetBufferDescription;
     thiz->PlayToStream         = IFusionSoundMusicProvider_FFmpeg_PlayToStream;
     thiz->PlayToBuffer         = IFusionSoundMusicProvider_FFmpeg_PlayToBuffer;
     thiz->Stop                 = IFusionSoundMusicProvider_FFmpeg_Stop;
     thiz->GetStatus            = IFusionSoundMusicProvider_FFmpeg_GetStatus;
     thiz->SeekTo               = IFusionSoundMusicProvider_FFmpeg_SeekTo;
     thiz->GetPos               = IFusionSoundMusicProvider_FFmpeg_GetPos;
     thiz->GetLength            = IFusionSoundMusicProvider_FFmpeg_GetLength;
     thiz->SetPlaybackFlags     = IFusionSoundMusicProvider_FFmpeg_SetPlaybackFlags;
     thiz->WaitStatus           = IFusionSoundMusicProvider_FFmpeg_WaitStatus;

     return DR_OK;

error:
     if (data->frame)
          av_free( data->frame );

     if (data->codec_ctx)
          avcodec_close( data->codec_ctx );

     if (data->fmt_ctx)
          avformat_close_input( &data->fmt_ctx );

     direct_stream_destroy( stream );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
