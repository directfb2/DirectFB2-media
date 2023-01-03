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
#include <mad.h>
#include <media/ifusionsoundmusicprovider.h>

D_DEBUG_DOMAIN( MusicProvider_MAD, "MusicProvider/MAD", "MAD Music Provider" );

static DirectResult Probe    ( IFusionSoundMusicProvider_ProbeContext *ctx );

static DirectResult Construct( IFusionSoundMusicProvider              *thiz,
                               const char                             *filename,
                               DirectStream                           *stream );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IFusionSoundMusicProvider, MAD )

/**********************************************************************************************************************/

typedef struct {
     int                           ref;                     /* reference counter */

     DirectStream                 *stream;

     struct mad_stream             st;
     struct mad_frame              frame;
     struct mad_synth              synth;

     int                           channels;
     int                           samplerate;

     unsigned int                  frames;                  /* number of frames */

     FSTrackDescription            desc;

     FSMusicProviderPlaybackFlags  flags;

     DirectThread                 *thread;
     DirectMutex                   lock;
     DirectWaitQueue               cond;

     FSMusicProviderStatus         status;
     int                           finished;
     int                           seeked;

     void                         *buf;
     int                           len;

     struct {
          IFusionSoundStream      *stream;
          IFusionSoundBuffer      *buffer;
          FSSampleFormat           sampleformat;
          FSChannelMode            mode;
          int                      length;
     } dest;

     FMBufferCallback              buffer_callback;
     void                         *buffer_callback_context;
} IFusionSoundMusicProvider_MAD_data;

/**********************************************************************************************************************/

#define PREBUFFER_SIZE 1 /* seconds */

#define XING_MAGIC (('X' << 24) | ('i' << 16) | ('n' << 8) | 'g')

struct id3_tag {
     s8 tag[3];
     s8 title[30];
     s8 artist[30];
     s8 album[30];
     s8 year[4];
     s8 comment[30];
     u8 genre;
};

static const char *id3_genres[] = {
     "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
     "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
     "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
     "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient",
     "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical",
     "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
     "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
     "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
     "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
     "Southern Rock", "Comedy", "Cult", "Gangsta Rap", "Top 40",
     "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret",
     "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi",
     "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical",
     "Rock & Roll", "Hard Rock", "Folk", "Folk/Rock", "National Folk", "Swing",
     "Fast-Fusion", "Bebob", "Latin", "Revival", "Celtic", "Bluegrass",
     "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock",
     "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening",
     "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music",
     "Sonata", "Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire",
     "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad",
     "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock",
     "Drum Solo", "A Cappella", "Euro-House", "Dance Hall", "Goa",
     "Drum & Bass", "Club-House", "Hardcore", "Terror", "Indie", "BritPop",
     "Negerpunk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal",
     "Black Metal", "Crossover", "Contemporary Christian", "Christian Rock",
     "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop", "Synthpop"
};

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

static inline u8
FtoU8( mad_fixed_t sample )
{
     /* round */
     sample += (1 << (MAD_F_FRACBITS - 8));

     /* clip */
     if (sample >= MAD_F_ONE)
          sample = MAD_F_ONE - 1;
     else if (sample < -MAD_F_ONE)
          sample = -MAD_F_ONE;

     /* quantize */
     return (sample >> (MAD_F_FRACBITS - 7)) + 128;
}

static inline s16
FtoS16( mad_fixed_t sample )
{
     /* round */
     sample += (1 << (MAD_F_FRACBITS - 16));

     /* clip */
     if (sample >= MAD_F_ONE)
          sample = MAD_F_ONE - 1;
     else if (sample < -MAD_F_ONE)
          sample = -MAD_F_ONE;

     /* quantize */
     return sample >> (MAD_F_FRACBITS - 15);
}

static inline s24
FtoS24( mad_fixed_t sample )
{
     /* round */
     sample += (1 << (MAD_F_FRACBITS - 24));

     /* clip */
     if (sample >= MAD_F_ONE)
          sample = MAD_F_ONE - 1;
     else if (sample < -MAD_F_ONE)
          sample = -MAD_F_ONE;

     /* quantize */
     sample >>= (MAD_F_FRACBITS - 23);

     return (s24) { a:sample, b:sample >> 8, c:sample >> 16 };
}

static inline s32
FtoS32( mad_fixed_t sample )
{
     /* clip */
     if (sample >= MAD_F_ONE)
          sample = MAD_F_ONE - 1;
     else if (sample < -MAD_F_ONE)
          sample = -MAD_F_ONE;

     /* quantize */
     return sample << (31 - MAD_F_FRACBITS);
}

static inline float
FtoF32( mad_fixed_t sample )
{
     /* clip */
     if (sample >= MAD_F_ONE)
          sample = MAD_F_ONE - 1;
     else if (sample < -MAD_F_ONE)
          sample = -MAD_F_ONE;

     /* quantize */
     return (float) sample / MAD_F_ONE;
}

#define MAD_MIX_LOOP()                                                  \
do {                                                                    \
     TYPE *d = (TYPE*) dst;                                             \
     int   i;                                                           \
     if (channels == 2) {                                               \
          switch (mode) {                                               \
               case FSCM_MONO:                                          \
                    for (i = 0; i < frames; i++)                        \
                         d[i] = CONV( (left[i] + right[i]) >> 1 );      \
                    break;                                              \
               case FSCM_STEREO:                                        \
                    for (i = 0; i < frames; i++) {                      \
                         d[0] = CONV( left[i] );                        \
                         d[1] = CONV( right[i] );                       \
                         d += 2;                                        \
                    }                                                   \
                    break;                                              \
               default:                                                 \
                    for (i = 0; i < frames; i++) {                      \
                         *d++ = CONV( left[i] );                        \
                         if (FS_MODE_HAS_CENTER( mode ))                \
                              *d++ = CONV( (left[i] + right[i]) >> 1 ); \
                         *d++ = CONV( right[i] );                       \
                         switch (FS_MODE_NUM_REARS( mode )) {           \
                              case 2:                                   \
                                   *d++ = MUTE;                         \
                              case 1:                                   \
                                   *d++ = MUTE;                         \
                         }                                              \
                         if (FS_MODE_HAS_LFE( mode ))                   \
                              *d++ = MUTE;                              \
                    }                                                   \
                    break;                                              \
          }                                                             \
     }                                                                  \
     else {                                                             \
          switch (mode) {                                               \
               case FSCM_MONO:                                          \
                    for (i = 0; i < frames; i++)                        \
                         d[i] = CONV( left[i] );                        \
                    break;                                              \
               case FSCM_STEREO:                                        \
                    for (i = 0; i < frames; i++) {                      \
                         d[0] = d[1] = CONV( left[i] );                 \
                         d += 2;                                        \
                    }                                                   \
                    break;                                              \
               default:                                                 \
                    for (i = 0; i < frames; i++) {                      \
                         if (FS_MODE_HAS_CENTER( mode )) {              \
                              d[0] = d[1] = d[2] = CONV( left[i] );     \
                              d += 3;                                   \
                         } else {                                       \
                              d[0] = d[1] = CONV( left[i] );            \
                              d += 2;                                   \
                         }                                              \
                         switch (FS_MODE_NUM_REARS( mode )) {           \
                              case 2:                                   \
                                   *d++ = MUTE;                         \
                              case 1:                                   \
                                   *d++ = MUTE;                         \
                         }                                              \
                         if (FS_MODE_HAS_LFE( mode ))                   \
                              *d++ = MUTE;                              \
                    }                                                   \
                    break;                                              \
          }                                                             \
     }                                                                  \
} while (0)

static void
mad_mix_audio( mad_fixed_t    *left,
               mad_fixed_t    *right,
               char           *dst,
               int             frames,
               FSSampleFormat  f,
               int             channels,
               FSChannelMode   mode )
{
     switch (f) {
          case FSSF_U8:
               #define TYPE u8
               #define MUTE 128
               #define CONV FtoU8
               MAD_MIX_LOOP();
               #undef CONV
               #undef MUTE
               #undef TYPE
               break;

          case FSSF_S16:
               #define TYPE s16
               #define MUTE 0
               #define CONV FtoS16
               MAD_MIX_LOOP();
               #undef CONV
               #undef MUTE
               #undef TYPE
               break;

          case FSSF_S24:
               #define TYPE s24
               #define MUTE (s24) { 0, 0, 0 }
               #define CONV FtoS24
               MAD_MIX_LOOP();
               #undef CONV
               #undef MUTE
               #undef TYPE
               break;

          case FSSF_S32:
               #define TYPE s32
               #define MUTE 0
               #define CONV FtoS32
               MAD_MIX_LOOP();
               #undef CONV
               #undef MUTE
               #undef TYPE
               break;

          case FSSF_FLOAT:
               #define TYPE float
               #define MUTE 0
               #define CONV FtoF32
               MAD_MIX_LOOP();
               #undef CONV
               #undef MUTE
               #undef TYPE
               break;

          default:
               break;
     }
}

/**********************************************************************************************************************/

static void
MAD_Stop( IFusionSoundMusicProvider_MAD_data *data,
          bool                                now )
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

     if (data->buf) {
          D_FREE( data->buf );
          data->buf  = NULL;
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
MADStream( DirectThread *thread,
           void         *arg )
{
     IFusionSoundMusicProvider_MAD_data *data = arg;

     data->st.next_frame = NULL;

     direct_stream_wait( data->stream, data->len, NULL );

     while (data->status == FMSTATE_PLAY) {
          DirectResult   ret    = DR_OK;
          int            offset = 0;
          unsigned int   len    = data->len;
          struct timeval tv     = { 0, 500 };

          direct_mutex_lock( &data->lock );

          if (data->status != FMSTATE_PLAY) {
               direct_mutex_unlock( &data->lock );
               break;
          }

          if (data->seeked) {
               data->dest.stream->Flush( data->dest.stream );
               data->seeked = false;
          }

          if (data->st.next_frame) {
               offset = data->st.bufend - data->st.next_frame;
               direct_memmove( data->buf, data->st.next_frame, offset );
          }

          if (offset < data->len) {
               ret = direct_stream_wait( data->stream, data->len, &tv );
               if (ret != DR_TIMEOUT) {
                    ret = direct_stream_read( data->stream, data->len - offset, data->buf + offset, &len );
               }
          }

          if (ret) {
               if (ret == DR_EOF) {
                    if (data->flags & FMPLAY_LOOPING) {
                         direct_stream_seek( data->stream, 0 );
                    }
                    else {
                         data->finished = true;
                         data->status   = FMSTATE_FINISHED;
                         direct_waitqueue_broadcast( &data->cond );
                    }
               }
               direct_mutex_unlock( &data->lock );
               continue;
          }

          direct_mutex_unlock( &data->lock );

          mad_stream_buffer( &data->st, data->buf, len + offset );

          /* Converting to output format. */
          while (data->status == FMSTATE_PLAY && !data->seeked) {
               unsigned int pos = 0;

               if (mad_frame_decode( &data->frame, &data->st ) == -1) {
                    if (!MAD_RECOVERABLE(data->st.error))
                         break;
                    continue;
               }

               mad_synth_frame( &data->synth, &data->frame );

               while (pos < data->synth.pcm.length) {
                    void *dst;
                    int   frames;

                    if (data->dest.stream->Access( data->dest.stream, &dst, &frames ))
                         break;

                    if (frames > data->synth.pcm.length - pos)
                         frames = data->synth.pcm.length - pos;

                    mad_mix_audio( data->synth.pcm.samples[0] + pos, data->synth.pcm.samples[1] + pos, dst, frames,
                                   data->dest.sampleformat, data->synth.pcm.channels, data->dest.mode );

                    data->dest.stream->Commit( data->dest.stream, frames );

                    pos += frames;
               }
          }
     }

     return NULL;
}

static void *
MADBuffer( DirectThread *thread,
           void         *arg )
{
     IFusionSoundMusicProvider_MAD_data *data      = arg;
     int                                 written   = 0;
     int                                 blocksize = FS_CHANNELS_FOR_MODE( data->dest.mode ) *
                                                     FS_BYTES_PER_SAMPLE( data->dest.sampleformat );

     data->st.next_frame = NULL;

     direct_stream_wait( data->stream, data->len, NULL );

     while (data->status == FMSTATE_PLAY) {
          DirectResult   ret    = DR_OK;
          int            offset = 0;
          unsigned int   len    = data->len;
          struct timeval tv     = { 0, 500 };

          direct_mutex_lock( &data->lock );

          if (data->status != FMSTATE_PLAY) {
               direct_mutex_unlock( &data->lock );
               break;
          }

          data->seeked = false;

          if (data->st.next_frame) {
               offset = data->st.bufend - data->st.next_frame;
               direct_memmove( data->buf, data->st.next_frame, offset );
          }

          if (offset < data->len) {
               ret = direct_stream_wait( data->stream, data->len, &tv );
               if (ret != DR_TIMEOUT) {
                    ret = direct_stream_read( data->stream, data->len - offset, data->buf + offset, &len );
               }
          }

          if (ret) {
               if (ret == DR_EOF) {
                    if (data->flags & FMPLAY_LOOPING) {
                         direct_stream_seek( data->stream, 0 );
                    }
                    else {
                         data->finished = true;
                         data->status = FMSTATE_FINISHED;
                         if (data->buffer_callback && written) {
                              if (data->buffer_callback( written, data->buffer_callback_context ))
                                   data->status = FMSTATE_STOP;
                         }
                         direct_waitqueue_broadcast( &data->cond );
                    }
               }
               direct_mutex_unlock( &data->lock );
               continue;
          }

          direct_mutex_unlock( &data->lock );

          mad_stream_buffer( &data->st, data->buf, len + offset );

          /* Converting to output format. */
          while (data->status == FMSTATE_PLAY && !data->seeked) {
               mad_fixed_t *left, *right;
               int          length;
               int          frames;
               char        *dst;

               if (mad_frame_decode( &data->frame, &data->st ) == -1) {
                    if (!MAD_RECOVERABLE(data->st.error))
                         break;
                    continue;
               }

               mad_synth_frame( &data->synth, &data->frame );

               ret = data->dest.buffer->Lock( data->dest.buffer, (void*) &dst, &frames, NULL );
               if (ret) {
                    D_DERROR( ret, "MusicProvider/MAD: Could not lock buffer!\n" );
                    break;
               }

               left   = data->synth.pcm.samples[0];
               right  = data->synth.pcm.samples[1];
               length = data->synth.pcm.length;

               do {
                    len = MIN( frames - written, length );

                    mad_mix_audio( left, right, &dst[written*blocksize], len,
                                   data->dest.sampleformat, data->synth.pcm.channels, data->dest.mode );

                    left    += len;
                    right   += len;
                    length  -= len;
                    written += len;

                    if (written >= frames) {
                         if (data->buffer_callback) {
                              data->dest.buffer->Unlock( data->dest.buffer );
                              if (data->buffer_callback( written, data->buffer_callback_context )) {
                                   data->status = FMSTATE_STOP;
                                   direct_waitqueue_broadcast( &data->cond );
                                   break;
                              }
                              data->dest.buffer->Lock( data->dest.buffer, (void*) &dst, &frames, 0 );
                         }
                         written = 0;
                    }
               } while (length > 0);

               data->dest.buffer->Unlock( data->dest.buffer );
          }
     }

     return NULL;
}

/**********************************************************************************************************************/

static void
IFusionSoundMusicProvider_MAD_Destruct( IFusionSoundMusicProvider *thiz )
{
     IFusionSoundMusicProvider_MAD_data *data = thiz->priv;

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     MAD_Stop( data, true );

     direct_stream_destroy( data->stream );

     direct_waitqueue_deinit( &data->cond );
     direct_mutex_deinit( &data->lock );

     mad_synth_finish( &data->synth );
     mad_frame_finish( &data->frame );
     mad_stream_finish( &data->st );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IFusionSoundMusicProvider_MAD_AddRef( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_Release( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IFusionSoundMusicProvider_MAD_Destruct( thiz );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_GetCapabilities( IFusionSoundMusicProvider   *thiz,
                                               FSMusicProviderCapabilities *ret_caps )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_caps)
          return DR_INVARG;

     *ret_caps = FMCAPS_BASIC | FMCAPS_HALFRATE;
     if (direct_stream_seekable( data->stream ))
          *ret_caps |= FMCAPS_SEEK;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_GetTrackDescription( IFusionSoundMusicProvider *thiz,
                                                   FSTrackDescription        *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DR_INVARG;

     *ret_desc = data->desc;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_GetStreamDescription( IFusionSoundMusicProvider *thiz,
                                                    FSStreamDescription       *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DR_INVARG;

     ret_desc->flags        = FSSDF_BUFFERSIZE | FSSDF_CHANNELS | FSSDF_SAMPLEFORMAT | FSSDF_SAMPLERATE;
     ret_desc->buffersize   = data->samplerate / 8;
     ret_desc->channels     = data->channels;
     ret_desc->sampleformat = FSSF_S32;
     ret_desc->samplerate   = data->samplerate;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_GetBufferDescription( IFusionSoundMusicProvider *thiz,
                                                    FSBufferDescription       *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DR_INVARG;

     ret_desc->flags        = FSBDF_LENGTH | FSBDF_CHANNELS | FSBDF_SAMPLEFORMAT | FSBDF_SAMPLERATE;
     ret_desc->length       = MIN( data->frames, FS_MAX_FRAMES );
     ret_desc->channels     = data->channels;
     ret_desc->sampleformat = FSSF_S32;
     ret_desc->samplerate   = data->samplerate;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_PlayToStream( IFusionSoundMusicProvider *thiz,
                                            IFusionSoundStream        *destination )
{
     FSStreamDescription desc;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (!destination)
          return DR_INVARG;

     if (destination == data->dest.stream)
          return DR_OK;

     destination->GetDescription( destination, &desc );

     if (desc.samplerate != data->samplerate &&
         desc.samplerate != data->samplerate / 2)
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

     direct_mutex_lock( &data->lock );

     MAD_Stop( data, false );

     if (desc.samplerate == data->samplerate / 2)
          mad_stream_options( &data->st, MAD_OPTION_IGNORECRC | MAD_OPTION_HALFSAMPLERATE );
     else
          mad_stream_options( &data->st, MAD_OPTION_IGNORECRC );

     data->len = data->desc.bitrate * PREBUFFER_SIZE / 8;
     data->buf = D_MALLOC( data->len );
     if (!data->buf) {
          direct_mutex_unlock( &data->lock );
          return D_OOM();
     }

     /* Increase the sound stream reference counter. */
     destination->AddRef( destination );

     data->dest.stream       = destination;
     data->dest.sampleformat = desc.sampleformat;
     data->dest.mode         = desc.channelmode;
     data->dest.length       = desc.buffersize;

     if (data->finished) {
          direct_stream_seek( data->stream, 0 );
          data->finished = false;
     }

     data->status = FMSTATE_PLAY;

     direct_waitqueue_broadcast( &data->cond );

     data->thread = direct_thread_create( DTT_DEFAULT, MADStream, data, "MAD Stream" );

     direct_mutex_unlock( &data->lock );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_PlayToBuffer( IFusionSoundMusicProvider *thiz,
                                            IFusionSoundBuffer        *destination,
                                            FMBufferCallback           callback,
                                            void                      *ctx )
{
     FSBufferDescription desc;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (!destination)
          return DR_INVARG;

     if (destination == data->dest.buffer)
          return DR_OK;

     destination->GetDescription( destination, &desc );

     if (desc.samplerate != data->samplerate &&
         desc.samplerate != data->samplerate / 2)
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

     direct_mutex_lock( &data->lock );

     MAD_Stop( data, false );

     if (desc.samplerate == data->samplerate / 2)
          mad_stream_options( &data->st, MAD_OPTION_IGNORECRC | MAD_OPTION_HALFSAMPLERATE );
     else
          mad_stream_options( &data->st, MAD_OPTION_IGNORECRC );

     data->len = data->desc.bitrate * PREBUFFER_SIZE / 8;
     data->buf = D_MALLOC( data->len );
     if (!data->buf) {
          direct_mutex_unlock( &data->lock );
          return D_OOM();
     }

     /* Increase the sound buffer reference counter. */
     destination->AddRef( destination );

     data->dest.buffer             = destination;
     data->dest.sampleformat       = desc.sampleformat;
     data->dest.mode               = desc.channelmode;
     data->dest.length             = desc.length;
     data->buffer_callback         = callback;
     data->buffer_callback_context = ctx;

     if (data->finished) {
          direct_stream_seek( data->stream, 0 );
          data->finished = false;
     }

     data->status = FMSTATE_PLAY;

     direct_waitqueue_broadcast( &data->cond );

     data->thread = direct_thread_create( DTT_DEFAULT, MADBuffer, data, "MAD Buffer" );

     direct_mutex_unlock( &data->lock );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_Stop( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     direct_mutex_lock( &data->lock );

     MAD_Stop( data, false );

     direct_waitqueue_broadcast( &data->cond );

     direct_mutex_unlock( &data->lock );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_GetStatus( IFusionSoundMusicProvider *thiz,
                                         FSMusicProviderStatus     *ret_status )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_status)
          return DR_INVARG;

     *ret_status = data->status;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_SeekTo( IFusionSoundMusicProvider *thiz,
                                      double                     seconds )
{
     DirectResult ret  = DR_FAILURE;
     unsigned int offset;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (seconds < 0.0)
          return DR_INVARG;

     offset = seconds * (data->desc.bitrate >> 3);

     direct_mutex_lock( &data->lock );

     ret = direct_stream_seek( data->stream, offset );
     if (ret == DR_OK) {
          data->seeked   = true;
          data->finished = false;
     }

     direct_mutex_unlock( &data->lock );

     return ret;
}

static DirectResult
IFusionSoundMusicProvider_MAD_GetPos( IFusionSoundMusicProvider *thiz,
                                      double                    *ret_seconds )
{
     int offset;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DR_INVARG;

     offset = direct_stream_offset( data->stream );

     if (data->status == FMSTATE_PLAY && data->st.this_frame) {
          offset -= data->st.bufend - data->st.this_frame;
          offset  = (offset < 0) ? 0 : offset;
     }

     *ret_seconds = (double) offset / (data->desc.bitrate >> 3);

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_GetLength( IFusionSoundMusicProvider *thiz,
                                         double                    *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DR_INVARG;

     *ret_seconds = (double) data->frames / data->samplerate;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_SetPlaybackFlags( IFusionSoundMusicProvider    *thiz,
                                                FSMusicProviderPlaybackFlags  flags )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (flags & ~FMPLAY_LOOPING)
          return DR_UNSUPPORTED;

     if (flags & FMPLAY_LOOPING && !direct_stream_seekable( data->stream ))
          return DR_UNSUPPORTED;

     data->flags = flags;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_MAD_WaitStatus( IFusionSoundMusicProvider *thiz,
                                          FSMusicProviderStatus      mask,
                                          unsigned int               timeout )
{
     DirectResult ret;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     if (!mask || mask & ~FMSTATE_ALL)
          return DR_INVARG;

     if (timeout) {
          long long s;

          s = direct_clock_get_abs_micros() + timeout * 1000;

          while (direct_mutex_trylock( &data->lock )) {
               usleep( 1000 );
               if (direct_clock_get_abs_micros() >= s)
                    return DR_TIMEOUT;
          }

          while (!(data->status & mask)) {
               ret = direct_waitqueue_wait_timeout( &data->cond, &data->lock, timeout * 1000 );
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
     char *ext;

     if (ctx->mimetype && !strcmp( ctx->mimetype, "audio/mpeg" ))
          return DR_OK;

     ext = strrchr( ctx->filename, '.' );
     if (ext) {
          if (!strcasecmp( ext, ".mp1" ) ||
              !strcasecmp( ext, ".mp2" ) ||
              !strcasecmp( ext, ".mp3" ))
               return DR_OK;
     }

     return DR_UNSUPPORTED;
}

static DirectResult
Construct( IFusionSoundMusicProvider *thiz,
           const char                *filename,
           DirectStream              *stream )
{
     DirectResult    ret;
     u8              buf[16384];
     unsigned int    size;
     int             i;
     struct id3_tag  id3;
     const char     *version;
     unsigned int    pos   = 0;
     int             error = -1;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IFusionSoundMusicProvider_MAD )

     D_DEBUG_AT( MusicProvider_MAD, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->stream = direct_stream_dup( stream );

     mad_stream_init( &data->st );
     mad_frame_init( &data->frame );
     mad_synth_init( &data->synth );
     mad_stream_options( &data->st, MAD_OPTION_IGNORECRC );

     /* Find the first valid frame. */
     for (i = 0; i < 100; i++) {
          if (i == 0                             ||
              data->st.error == MAD_ERROR_BUFLEN ||
              data->st.error == MAD_ERROR_BUFPTR) {
               direct_stream_wait( data->stream, sizeof(buf), NULL );

               ret = direct_stream_peek( data->stream, sizeof(buf), pos, buf, &size );
               if (ret)
                    goto error;

               pos += size;

               mad_stream_buffer( &data->st, buf, size );
          }

          error = mad_frame_decode( &data->frame, &data->st );
          if (!error) {
               /* Get the number of frames from Xing header. */
               if (data->st.anc_bitlen >= 128 && mad_bit_read( &data->st.anc_ptr, 32 ) == XING_MAGIC) {
                    if (mad_bit_read( &data->st.anc_ptr, 32 ) & 1)
                         data->frames = mad_bit_read( &data->st.anc_ptr, 32 );
               }
               break;
          }
     }

     if (error) {
          D_ERROR( "MusicProvider/MAD: No valid frame found!\n" );
          ret = DR_FAILURE;
          goto error;
     }

     data->channels   = MAD_NCHANNELS( &data->frame.header );
     data->samplerate = data->frame.header.samplerate;

     size = direct_stream_length( data->stream );

     /* Get ID3 tag. */
     if (direct_stream_seekable( data->stream ) && !direct_stream_remote( data->stream )) {
          direct_stream_peek( data->stream, sizeof(id3), size - sizeof(id3), &id3, NULL );

          if (!strncmp( (char*) id3.tag, "TAG", 3 )) {
               size -= sizeof(id3);

               strncpy( data->desc.artist, (char*) id3.artist, sizeof(id3.artist) );
               strncpy( data->desc.title,  (char*) id3.title,  sizeof(id3.title) );
               strncpy( data->desc.album,  (char*) id3.album,  sizeof(id3.album) );
               data->desc.year = strtol( (char*) id3.year, NULL, 10 );
               if (id3.genre < D_ARRAY_SIZE(id3_genres))
                    strncpy( data->desc.genre, id3_genres[id3.genre], strlen( id3_genres[id3.genre] ) );
          }
     }

     switch (data->frame.header.flags & (MAD_FLAG_MPEG_2_5_EXT | MAD_FLAG_LSF_EXT)) {
          case (MAD_FLAG_MPEG_2_5_EXT | MAD_FLAG_LSF_EXT):
               version = "2.5";
               break;
          case MAD_FLAG_LSF_EXT:
               version = "2";
               break;
          default:
               version = "1";
               break;
     }

     if (data->frames) {
          snprintf( data->desc.encoding, FS_TRACK_DESC_ENCODING_LENGTH, "MPEG-%s Layer %d (VBR)", version,
                    data->frame.header.layer );

          switch (data->frame.header.layer) {
               case MAD_LAYER_I:
                    data->frames *= 384;
                    break;
               case MAD_LAYER_II:
                    data->frames *= 1152;
                    break;
               case MAD_LAYER_III:
               default:
                    if (data->frame.header.flags & (MAD_FLAG_LSF_EXT | MAD_FLAG_MPEG_2_5_EXT))
                         data->frames *= 576;
                    else
                         data->frames *= 1152;
                    break;
          }

          data->desc.bitrate = size * 8 / ((double) data->frames / data->samplerate);

     }
     else {
          snprintf( data->desc.encoding, FS_TRACK_DESC_ENCODING_LENGTH, "MPEG-%s Layer %d", version,
                    data->frame.header.layer );

          if (data->frame.header.bitrate < 8000)
               data->frame.header.bitrate = 8000;

          data->frames = D_ICEIL( (size * 8 / (double) data->frame.header.bitrate) * data->samplerate );

          data->desc.bitrate = data->frame.header.bitrate;
     }

     direct_recursive_mutex_init( &data->lock );
     direct_waitqueue_init( &data->cond );

     data->status = FMSTATE_STOP;

     thiz->AddRef               = IFusionSoundMusicProvider_MAD_AddRef;
     thiz->Release              = IFusionSoundMusicProvider_MAD_Release;
     thiz->GetCapabilities      = IFusionSoundMusicProvider_MAD_GetCapabilities;
     thiz->GetTrackDescription  = IFusionSoundMusicProvider_MAD_GetTrackDescription;
     thiz->GetStreamDescription = IFusionSoundMusicProvider_MAD_GetStreamDescription;
     thiz->GetBufferDescription = IFusionSoundMusicProvider_MAD_GetBufferDescription;
     thiz->PlayToStream         = IFusionSoundMusicProvider_MAD_PlayToStream;
     thiz->PlayToBuffer         = IFusionSoundMusicProvider_MAD_PlayToBuffer;
     thiz->Stop                 = IFusionSoundMusicProvider_MAD_Stop;
     thiz->GetStatus            = IFusionSoundMusicProvider_MAD_GetStatus;
     thiz->SeekTo               = IFusionSoundMusicProvider_MAD_SeekTo;
     thiz->GetPos               = IFusionSoundMusicProvider_MAD_GetPos;
     thiz->GetLength            = IFusionSoundMusicProvider_MAD_GetLength;
     thiz->SetPlaybackFlags     = IFusionSoundMusicProvider_MAD_SetPlaybackFlags;
     thiz->WaitStatus           = IFusionSoundMusicProvider_MAD_WaitStatus;

     return DR_OK;

error:
     mad_synth_finish( &data->synth );
     mad_frame_finish( &data->frame );
     mad_stream_finish( &data->st );

     direct_stream_destroy( stream );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
