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
#include <direct/memcpy.h>
#include <direct/stream.h>
#include <direct/thread.h>
#include <media/ifusionsoundmusicprovider.h>
#include <tremor/ivorbisfile.h>

D_DEBUG_DOMAIN( MusicProvider_Tremor, "MusicProvider/Tremor", "Tremor Music Provider" );

static DirectResult Probe    ( IFusionSoundMusicProvider_ProbeContext *ctx );

static DirectResult Construct( IFusionSoundMusicProvider              *thiz,
                               const char                             *filename,
                               DirectStream                           *stream );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IFusionSoundMusicProvider, Tremor )

/**********************************************************************************************************************/

typedef struct {
     int                           ref;                     /* reference counter */

     DirectStream                 *stream;

     OggVorbis_File                vf;

     int                           channels;
     int                           samplerate;

     long                          bitrate_nominal;         /* average bitrate for a VBR bitstream */

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
          FSSampleFormat           sampleformat;
          FSChannelMode            mode;
          int                      buffersize;
     } dest;

     FMBufferCallback              buffer_callback;
     void                         *buffer_callback_context;
} IFusionSoundMusicProvider_Tremor_data;

/**********************************************************************************************************************/

typedef struct {
#ifdef WORDS_BIGENDIAN
     s8 c;
     u8 b;
     u8 a;
#else
     u8 a;
     u8 b;
     s8 c;
#endif
} __attribute__((packed)) s24;

#define VORBIS_MIX_LOOP()                                           \
 do {                                                               \
     int i, n;                                                      \
     if (d_n == s_n) {                                              \
          s16 *s = src + pos * s_n;                                 \
          if (sizeof(TYPE) == sizeof(s16)) {                        \
               direct_memcpy( dst, s, frames * s_n * sizeof(s16) ); \
          }                                                         \
          else {                                                    \
               TYPE *d = (TYPE*) dst;                               \
               for (i = frames * s_n; i; i--) {                     \
                    *d++ = CONV( *s );                              \
                    s++;                                            \
               }                                                    \
          }                                                         \
          break;                                                    \
     }                                                              \
     else if (d_n < s_n) {                                          \
          if (d_n == 1 && s_n == 2) {                               \
               /* Downmix to stereo to mono */                      \
               s16  *s = src + pos * 2;                             \
               TYPE *d = (TYPE*) dst;                               \
               for (i = frames; i; i--) {                           \
                    *d++ = CONV( (s[0] + s[1]) >> 1 );              \
                    s += 2;                                         \
               }                                                    \
               break;                                               \
          }                                                         \
     }                                                              \
     else if (d_n > s_n) {                                          \
          if (d_n == 2 && s_n == 1) {                               \
               /* Upmix stereo to mono */                           \
               s16  *s = src + pos;                                 \
               TYPE *d = (TYPE*) dst;                               \
               for (i = frames; i; i--) {                           \
                    d[0] = d[1] = CONV( *s );                       \
                    d += 2;                                         \
                    s++;                                            \
               }                                                    \
               break;                                               \
          }                                                         \
          memset( dst, 0, frames * d_n * sizeof(TYPE) );            \
     }                                                              \
     for (n = 0; n < MIN( s_n, d_n ); n++) {                        \
          s16  *s = &(src + pos * s_n)[n];                          \
          TYPE *d = &((TYPE*) dst)[n];                              \
          for (i = frames; i; i--) {                                \
               *d = CONV( *s );                                     \
               d += d_n;                                            \
               s += s_n;                                            \
          }                                                         \
     }                                                              \
} while (0)

static void
vorbis_mix_audio( s16            *src,
                  void           *dst,
                  int             pos,
                  int             frames,
                  FSSampleFormat  f,
                  int             channels,
                  FSChannelMode   mode )
{
     int s_n = channels;
     int d_n = FS_CHANNELS_FOR_MODE( mode );

     switch (f) {
          case FSSF_U8:
               #define TYPE u8
               #define CONV(s) (((s) >> 8) + 128)
               VORBIS_MIX_LOOP();
               #undef CONV
               #undef TYPE
               break;

          case FSSF_S16:
               #define TYPE s16
               #define CONV(s) (s)
               VORBIS_MIX_LOOP();
               #undef CONV
               #undef TYPE
               break;

          case FSSF_S24:
               #define TYPE s24
               #define CONV(s) ((s24) { a:0, b:(s), c:(s) >> 8 })
               VORBIS_MIX_LOOP();
               #undef CONV
               #undef TYPE
               break;

          case FSSF_S32:
               #define TYPE s32
               #define CONV(s) ((s) << 8)
               VORBIS_MIX_LOOP();
               #undef CONV
               #undef TYPE
               break;

          case FSSF_FLOAT:
               #define TYPE float
               #define CONV(s) ((s) / 32768.f)
               VORBIS_MIX_LOOP();
               #undef CONV
               #undef TYPE
               break;

          default:
               break;
     }
}

static size_t
ov_read_func( void   *ptr,
              size_t  size,
              size_t  nmemb,
              void   *user )
{
     DirectResult  ret;
     size_t        length = 0;
     size_t        total  = size * nmemb;
     DirectStream *stream = user;

     while (length < total) {
          unsigned int len;

          direct_stream_wait( stream, total - length, NULL );

          ret = direct_stream_read( stream, total - length, ptr + length, &len );
          if (ret) {
               memset( ptr + length, 0, total - length );
               if (!length)
                    return (ret == DR_EOF) ? 0 : -1;
               break;
          }

          length += len;
     }

     return length / size;
}

static int
ov_seek_func( void        *user,
              ogg_int64_t  offset,
              int          whence )
{
     unsigned int  pos;
     DirectStream *stream = user;

     if (!direct_stream_seekable( stream ) || direct_stream_remote( stream ))
          return -1;

     switch (whence) {
          case SEEK_SET:
               break;
          case SEEK_CUR:
               pos = direct_stream_offset( stream );
               offset += pos;
               break;
          case SEEK_END:
               pos = direct_stream_length( stream );
               if (offset < 0)
                    return pos;
               offset = pos - offset;
               break;
          default:
               return -1;
     }

     if (offset >= 0) {
          if (direct_stream_seek( stream, offset ))
               return -1;
     }

     return direct_stream_offset( stream );
}

static int
ov_close_func( void *user )
{
     return 0;
}

static long
ov_tell_func( void *user )
{
     DirectStream *stream = user;

     return direct_stream_offset( stream );
}

/**********************************************************************************************************************/

static void
Tremor_Stop( IFusionSoundMusicProvider_Tremor_data *data,
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
TremorStream( DirectThread *thread,
              void         *arg )
{
     IFusionSoundMusicProvider_Tremor_data *data = arg;

     while (data->status == FMSTATE_PLAY) {
          int  section;
          long length;
          s16  src[2048];
          int  pos = 0;

          direct_mutex_lock( &data->lock );

          if (data->status != FMSTATE_PLAY) {
               direct_mutex_unlock( &data->lock );
               break;
          }

          if (data->seeked) {
               data->dest.stream->Flush( data->dest.stream );
               data->seeked = false;
          }

          length = ov_read( &data->vf, (char*) &src, sizeof(src), &section );
          length = length / (data->channels * 2);

          if (length == 0) {
               if (data->flags & FMPLAY_LOOPING) {
                    if (direct_stream_remote( data->stream ))
                         direct_stream_seek( data->stream, 0 );
                    else
                         ov_time_seek( &data->vf, 0 );
               }
               else {
                    data->finished = true;
                    data->status = FMSTATE_FINISHED;
                    direct_waitqueue_broadcast( &data->cond );
               }
          }

          direct_mutex_unlock( &data->lock );

          /* Converting to output format. */
          while (pos < length) {
               int   frames;
               void *dst;

               if (data->dest.stream->Access( data->dest.stream, &dst, &frames ))
                    break;

               if (frames > length - pos)
                    frames = length - pos;

               vorbis_mix_audio( src, dst, pos, frames, data->dest.sampleformat,
                                 data->channels, data->dest.mode );

               data->dest.stream->Commit( data->dest.stream, frames );

               pos += frames;
          }
     }

     return NULL;
}

static void *
TremorBuffer( DirectThread *thread,
              void         *arg )
{
     IFusionSoundMusicProvider_Tremor_data *data           = arg;
     int                                    bytespersample = FS_CHANNELS_FOR_MODE(data->dest.mode) *
                                                             FS_BYTES_PER_SAMPLE(data->dest.sampleformat);

     while (data->status == FMSTATE_PLAY) {
          DirectResult  ret;
          int           section;
          int           frames;
          char         *dst;
          int           pos = 0;

          direct_mutex_lock( &data->lock );

          if (data->status != FMSTATE_PLAY) {
               direct_mutex_unlock( &data->lock );
               break;
          }

          ret = data->dest.buffer->Lock( data->dest.buffer, (void*) &dst, &frames, NULL );
          if (ret) {
               D_DERROR( ret, "MusicProvider/Tremor: Could not lock buffer!\n" );
               direct_mutex_unlock( &data->lock );
               break;
          }

          do {
               long length;
               s16  src[2048];

               length = ov_read( &data->vf, (char*) &src, frames - pos, &section );
               length = length / (data->channels * 2);

               if (length == 0) {
                    if (data->flags & FMPLAY_LOOPING) {
                         if (direct_stream_remote( data->stream ))
                              direct_stream_seek( data->stream, 0 );
                         else
                              ov_time_seek( &data->vf, 0 );
                    }
                    else {
                         data->finished = true;
                         data->status = FMSTATE_FINISHED;
                         direct_waitqueue_broadcast( &data->cond );
                    }
                    continue;
               }

               /* Converting to output format. */
               if (length > 0) {
                    int len;

                    do {
                         len = MIN( frames - pos, length );

                         vorbis_mix_audio( src, &dst[pos*bytespersample], 0, len, data->dest.sampleformat,
                                           data->channels, data->dest.mode );

                         length -= len;
                         pos    += len;
                    } while (len > 0);
               }
          } while (pos < frames && data->status != FMSTATE_FINISHED);

          data->dest.buffer->Unlock( data->dest.buffer );

          direct_mutex_unlock( &data->lock );

          if (data->buffer_callback) {
               if (data->buffer_callback( pos, data->buffer_callback_context )) {
                    data->status = FMSTATE_STOP;
                    direct_waitqueue_broadcast( &data->cond );
               }
          }
     }

     return NULL;
}

/**********************************************************************************************************************/

static void
IFusionSoundMusicProvider_Tremor_Destruct( IFusionSoundMusicProvider *thiz )
{
     IFusionSoundMusicProvider_Tremor_data *data = thiz->priv;

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     Tremor_Stop( data, true );

     direct_stream_destroy( data->stream );

     direct_waitqueue_deinit( &data->cond );
     direct_mutex_deinit( &data->lock );

     ov_clear( &data->vf );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IFusionSoundMusicProvider_Tremor_AddRef( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_Release( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IFusionSoundMusicProvider_Tremor_Destruct( thiz );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_GetCapabilities( IFusionSoundMusicProvider   *thiz,
                                                  FSMusicProviderCapabilities *ret_caps )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_caps)
          return DR_INVARG;

     *ret_caps = FMCAPS_BASIC;
     if (direct_stream_seekable( data->stream ))
          *ret_caps |= FMCAPS_SEEK;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_GetTrackDescription( IFusionSoundMusicProvider *thiz,
                                                      FSTrackDescription        *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DR_INVARG;

     *ret_desc = data->desc;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_GetStreamDescription( IFusionSoundMusicProvider *thiz,
                                                       FSStreamDescription       *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

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
IFusionSoundMusicProvider_Tremor_GetBufferDescription( IFusionSoundMusicProvider *thiz,
                                                       FSBufferDescription       *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DR_INVARG;

     ret_desc->flags        = FSBDF_LENGTH | FSBDF_CHANNELS | FSBDF_SAMPLEFORMAT | FSBDF_SAMPLERATE;
     ret_desc->length       = MIN( ov_pcm_total( &data->vf, -1 ), FS_MAX_FRAMES );
     ret_desc->channels     = data->channels;
     ret_desc->sampleformat = FSSF_S16;
     ret_desc->samplerate   = data->samplerate;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_PlayToStream( IFusionSoundMusicProvider *thiz,
                                               IFusionSoundStream        *destination )
{
     FSStreamDescription desc;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     if (!destination)
          return DR_INVARG;

     if (destination == data->dest.stream)
          return DR_OK;

     destination->GetDescription( destination, &desc );

     if (desc.samplerate != data->samplerate)
          return DR_UNSUPPORTED;

     switch (desc.sampleformat) {
          case FSSF_U8:
          case FSSF_S16:
          case FSSF_S24:
          case FSSF_S32:
          case FSSF_FLOAT:
               break;
          default:
               return DR_UNSUPPORTED;
     }

     if (desc.channels > 6)
          return DR_UNSUPPORTED;

     switch (desc.channelmode) {
          case FSCM_MONO:
          case FSCM_STEREO:
          case FSCM_STEREO21:
          case FSCM_STEREO30:
          case FSCM_STEREO31:
          case FSCM_SURROUND30:
          case FSCM_SURROUND31:
          case FSCM_SURROUND40_2F2R:
          case FSCM_SURROUND41_2F2R:
          case FSCM_SURROUND40_3F1R:
          case FSCM_SURROUND41_3F1R:
          case FSCM_SURROUND50:
          case FSCM_SURROUND51:
               break;
          default:
               return DR_UNSUPPORTED;
     }

     direct_mutex_lock( &data->lock );

     Tremor_Stop( data, false );

     /* Increase the sound stream reference counter. */
     destination->AddRef( destination );

     data->dest.stream       = destination;
     data->dest.sampleformat = desc.sampleformat;
     data->dest.mode         = desc.channelmode;
     data->dest.buffersize   = desc.buffersize;

     if (data->finished) {
          if (direct_stream_remote( data->stream ))
               direct_stream_seek( data->stream, 0 );
          else
               ov_time_seek( &data->vf, 0 );
          data->finished = false;
     }

     data->status = FMSTATE_PLAY;

     direct_waitqueue_broadcast( &data->cond );

     data->thread = direct_thread_create( DTT_DEFAULT, TremorStream, data, "Tremor Stream" );

     direct_mutex_unlock( &data->lock );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_PlayToBuffer( IFusionSoundMusicProvider *thiz,
                                               IFusionSoundBuffer        *destination,
                                               FMBufferCallback           callback,
                                               void                      *ctx )
{
     FSBufferDescription desc;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     if (!destination)
          return DR_INVARG;

     if (destination == data->dest.buffer)
          return DR_OK;

     destination->GetDescription( destination, &desc );

     if (desc.samplerate != data->samplerate)
          return DR_UNSUPPORTED;

     switch (desc.sampleformat) {
          case FSSF_U8:
          case FSSF_S16:
          case FSSF_S24:
          case FSSF_S32:
          case FSSF_FLOAT:
               break;
          default:
               return DR_UNSUPPORTED;
     }

     if (desc.channels > 6)
          return DR_UNSUPPORTED;

     switch (desc.channelmode) {
          case FSCM_MONO:
          case FSCM_STEREO:
          case FSCM_STEREO21:
          case FSCM_STEREO30:
          case FSCM_STEREO31:
          case FSCM_SURROUND30:
          case FSCM_SURROUND31:
          case FSCM_SURROUND40_2F2R:
          case FSCM_SURROUND41_2F2R:
          case FSCM_SURROUND40_3F1R:
          case FSCM_SURROUND41_3F1R:
          case FSCM_SURROUND50:
          case FSCM_SURROUND51:
               break;
          default:
               return DR_UNSUPPORTED;
     }

     direct_mutex_lock( &data->lock );

     Tremor_Stop( data, false );

     /* Increase the sound buffer reference counter. */
     destination->AddRef( destination );

     data->dest.buffer             = destination;
     data->dest.sampleformat       = desc.sampleformat;
     data->dest.mode               = desc.channelmode;
     data->buffer_callback         = callback;
     data->buffer_callback_context = ctx;

     if (data->finished) {
          if (direct_stream_remote( data->stream ))
               direct_stream_seek( data->stream, 0 );
          else
               ov_time_seek( &data->vf, 0 );
          data->finished = false;
     }

     data->status = FMSTATE_PLAY;

     direct_waitqueue_broadcast( &data->cond );

     data->thread = direct_thread_create( DTT_DEFAULT, TremorBuffer, data, "Tremor Buffer" );

     direct_mutex_unlock( &data->lock );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_Stop( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     direct_mutex_lock( &data->lock );

     Tremor_Stop( data, false );

     direct_waitqueue_broadcast( &data->cond );

     direct_mutex_unlock( &data->lock );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_GetStatus( IFusionSoundMusicProvider *thiz,
                                            FSMusicProviderStatus     *ret_status )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_status)
          return DR_INVARG;

     *ret_status = data->status;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_SeekTo( IFusionSoundMusicProvider *thiz,
                                         double                     seconds )
{
     DirectResult ret = DR_OK;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     if (seconds < 0.0)
          return DR_INVARG;

     direct_mutex_lock( &data->lock );

     if (direct_stream_remote( data->stream )) {
          unsigned int offset;

          if (!data->bitrate_nominal)
               return DR_UNSUPPORTED;

          offset = seconds * (data->bitrate_nominal >> 3);
          ret = direct_stream_seek( data->stream, offset );
     }
     else {
          seconds *= 1000.0;
          if (ov_time_seek( &data->vf, seconds ))
               ret = DR_FAILURE;
     }

     if (ret == DR_OK) {
          data->seeked   = true;
          data->finished = false;
     }

     direct_mutex_unlock( &data->lock );

     return ret;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_GetPos( IFusionSoundMusicProvider *thiz,
                                         double                    *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DR_INVARG;

     *ret_seconds = ov_time_tell( &data->vf ) / 1000.0;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_GetLength( IFusionSoundMusicProvider *thiz,
                                            double                    *ret_seconds )
{
     double seconds;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DR_INVARG;

     seconds = ov_time_total( &data->vf, -1 ) / 1000.0;
     if (seconds < 0) {
          if (data->bitrate_nominal)
               seconds = (double) direct_stream_length( data->stream ) / (data->bitrate_nominal >> 3);
     }

     *ret_seconds = seconds;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_SetPlaybackFlags( IFusionSoundMusicProvider    *thiz,
                                                   FSMusicProviderPlaybackFlags  flags )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     if (flags & ~FMPLAY_LOOPING)
          return DR_UNSUPPORTED;

     if (flags & FMPLAY_LOOPING && !direct_stream_seekable( data->stream ))
          return DR_UNSUPPORTED;

     data->flags = flags;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Tremor_WaitStatus( IFusionSoundMusicProvider *thiz,
                                             FSMusicProviderStatus      mask,
                                             unsigned int               timeout )
{
     DirectResult ret;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Tremor)

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

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
                    return DR_TIMEOUT;
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
     if (!memcmp( &ctx->header[0], "OggS", 4 ) && !memcmp( &ctx->header[29], "vorbis", 6 ))
          return DR_OK;

     return DR_UNSUPPORTED;
}

static DirectResult
Construct( IFusionSoundMusicProvider *thiz,
           const char                *filename,
           DirectStream              *stream )
{
     DirectResult   ret;
     ov_callbacks   callbacks;
     vorbis_info   *info;
     char         **ptr;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IFusionSoundMusicProvider_Tremor )

     D_DEBUG_AT( MusicProvider_Tremor, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->stream = direct_stream_dup( stream );

     callbacks.read_func  = ov_read_func;
     callbacks.seek_func  = ov_seek_func;
     callbacks.close_func = ov_close_func;
     callbacks.tell_func  = ov_tell_func;

     if (ov_open_callbacks( data->stream, &data->vf, NULL, 0, callbacks ) < 0) {
          D_ERROR( "MusicProvider/Tremor: Failed to open stream!\n" );
          direct_stream_destroy( stream );
          DIRECT_DEALLOCATE_INTERFACE( thiz );
          return DR_UNSUPPORTED;
     }

     info = ov_info( &data->vf, -1 );
     if (!info) {
          D_ERROR( "MusicProvider/Tremor: Could not get stream info!\n" );
          ret = DR_FAILURE;
          goto error;
     }

     data->channels   = MIN( info->channels, FS_MAX_CHANNELS );
     data->samplerate = info->rate;

     data->bitrate_nominal = info->bitrate_nominal;

     ptr = ov_comment( &data->vf, -1 )->user_comments;

     while (*ptr) {
          char *comment = *ptr;

          if (!strncasecmp( comment, "ARTIST=", sizeof("ARTIST=") - 1 )) {
               strncpy( data->desc.artist, comment + sizeof("ARTIST=") - 1, FS_TRACK_DESC_ARTIST_LENGTH - 1 );
          }
          else if (!strncasecmp( comment, "TITLE=", sizeof("TITLE=") - 1 )) {
               strncpy( data->desc.title, comment + sizeof("TITLE=") - 1, FS_TRACK_DESC_TITLE_LENGTH - 1 );
          }
          else if (!strncasecmp( comment, "ALBUM=", sizeof("ALBUM=") - 1 )) {
               strncpy( data->desc.album, comment + sizeof("ALBUM=") - 1, FS_TRACK_DESC_ALBUM_LENGTH - 1 );
          }
          else if (!strncasecmp( comment, "DATE=", sizeof("DATE=") - 1 )) {
               data->desc.year = atoi( comment + sizeof("DATE=") );
          }
          else if (!strncasecmp( comment, "GENRE=", sizeof("GENRE=") - 1 )) {
               strncpy( data->desc.genre, comment + sizeof("GENRE=") - 1, FS_TRACK_DESC_GENRE_LENGTH - 1 );
          }

          ptr++;
     }

     snprintf( data->desc.encoding, FS_TRACK_DESC_ENCODING_LENGTH, "vorbis" );

     data->desc.bitrate = ov_bitrate( &data->vf, -1 ) ?: ov_bitrate_instant( &data->vf );

     direct_mutex_init( &data->lock );
     direct_waitqueue_init( &data->cond );

     data->status = FMSTATE_STOP;

     thiz->AddRef               = IFusionSoundMusicProvider_Tremor_AddRef;
     thiz->Release              = IFusionSoundMusicProvider_Tremor_Release;
     thiz->GetCapabilities      = IFusionSoundMusicProvider_Tremor_GetCapabilities;
     thiz->GetTrackDescription  = IFusionSoundMusicProvider_Tremor_GetTrackDescription;
     thiz->GetStreamDescription = IFusionSoundMusicProvider_Tremor_GetStreamDescription;
     thiz->GetBufferDescription = IFusionSoundMusicProvider_Tremor_GetBufferDescription;
     thiz->PlayToStream         = IFusionSoundMusicProvider_Tremor_PlayToStream;
     thiz->PlayToBuffer         = IFusionSoundMusicProvider_Tremor_PlayToBuffer;
     thiz->Stop                 = IFusionSoundMusicProvider_Tremor_Stop;
     thiz->GetStatus            = IFusionSoundMusicProvider_Tremor_GetStatus;
     thiz->SeekTo               = IFusionSoundMusicProvider_Tremor_SeekTo;
     thiz->GetPos               = IFusionSoundMusicProvider_Tremor_GetPos;
     thiz->GetLength            = IFusionSoundMusicProvider_Tremor_GetLength;
     thiz->SetPlaybackFlags     = IFusionSoundMusicProvider_Tremor_SetPlaybackFlags;
     thiz->WaitStatus           = IFusionSoundMusicProvider_Tremor_WaitStatus;

     return DR_OK;

error:
     ov_clear( &data->vf );

     direct_stream_destroy( stream );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
