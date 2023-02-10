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

#include <direct/list.h>
#include <direct/memcpy.h>
#include <direct/stream.h>
#include <direct/util.h>
#include <media/ifusionsoundmusicprovider.h>

D_DEBUG_DOMAIN( MusicProvider_Playlist, "MusicProvider/Playlist", "Playlist Music Provider" );

static DirectResult Probe    ( IFusionSoundMusicProvider_ProbeContext *ctx );

static DirectResult Construct( IFusionSoundMusicProvider              *thiz,
                               const char                             *filename,
                               DirectStream                           *stream );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IFusionSoundMusicProvider, Playlist )

/**********************************************************************************************************************/

typedef struct {
     DirectLink                 link;

     FSTrackID                  id;

     char                      *url;
     char                      *artist;
     char                      *title;
     char                      *album;

     IFusionSoundMusicProvider *provider;
} PlaylistEntry;

typedef struct {
     int                           ref;                     /* reference counter */

     DirectLink                   *playlist;
     PlaylistEntry                *selected;

     FSMusicProviderPlaybackFlags  flags;

     struct {
          IFusionSoundStream      *stream;
          IFusionSoundBuffer      *buffer;
     } dest;

     FMBufferCallback              buffer_callback;
     void                         *buffer_callback_context;
} IFusionSoundMusicProvider_Playlist_data;

/**********************************************************************************************************************/

#define space(c) ((c) == ' ' || (c) == '\t' || (c) == '\r' || (c) == '\n' || (c) == '"' || (c) == '\'')

static DirectResult
add_media( FSTrackID    id,
           const char  *url,
           const char  *artist,
           const char  *title,
           const char  *album,
           DirectLink **playlist )
{
     PlaylistEntry *entry;

     D_ASSERT( url != NULL );
     D_ASSERT( playlist != NULL );

     entry = D_CALLOC( 1, sizeof(PlaylistEntry) );
     if (!entry)
          return D_OOM();

     entry->id  = id;
     entry->url = D_STRDUP( url );

     if (artist && *artist)
          entry->artist = D_STRDUP( artist );
     if (title && *title)
          entry->title = D_STRDUP( title );
     if (album && *album)
          entry->album = D_STRDUP( album );

     direct_list_append( playlist, &entry->link );

     return DR_OK;
}

static void
remove_media( PlaylistEntry  *entry,
              DirectLink    **playlist )
{
     D_ASSERT( entry != NULL );
     D_ASSERT( playlist != NULL );

     direct_list_remove( playlist, &entry->link );

     if (entry->url)
          D_FREE( entry->url );

     if (entry->artist)
          D_FREE( entry->artist );
     if (entry->title)
          D_FREE( entry->title );
     if (entry->album)
          D_FREE( entry->album );

     if (entry->provider)
          entry->provider->Release( entry->provider );

     D_FREE( entry );
}

/**********************************************************************************************************************/

static char *
trim( char *s )
{
     char *e;

     while (*s && space( *s ))
          s++;

     e = s + strlen( s ) - 1;
     while (e > s && space( *e ))
          *e-- = '\0';

     return s;
}

static void
m3u_playlist_parse( DirectLink **playlist,
                    char        *src )
{
     char *end;
     char *title = NULL;
     int   id    = 0;

     while (src) {
          end = strchr( src, '\n' );
          if (end)
               *end = '\0';

          src = trim( src );
          if (*src == '#') {
               if (!strncmp( src + 1, "EXTINF:", 7 )) {
                    title = strchr( src + 8, ',' );
                    if (title)
                         title++;
               }
          }
          else if (*src) {
               add_media( id++, src, NULL, title, NULL, playlist );
               title = NULL;
          }

          src = end;
          if (src)
               src++;
     }
}

static void
pls_playlist_parse( DirectLink **playlist,
                    char        *src )
{
     char *end;
     int   id;

     while (src) {
          end = strchr( src, '\n' );
          if (end)
               *end = '\0';

          src = trim( src );
          if (!strncmp( src, "File", 4 )) {
               src += 4;
               id = atoi( src );
               src = strchr( src, '=' );
               if (id && src && *(src + 1))
                    add_media( id - 1, src + 1, NULL, NULL, NULL, playlist );
          }
          else if (!strncmp( src, "Title", 5 )) {
               src += 5;
               id = atoi( src );
               src = strchr( src, '=' );
               if (id && src && *(src + 1)) {
                    PlaylistEntry *entry;

                    direct_list_foreach (entry, *playlist) {
                         if (entry->id == id - 1) {
                              entry->title = D_STRDUP( src + 1 );
                              break;
                         }
                    }
               }
          }

          src = end;
          if (src)
               src++;
     }
}

static void
replace_xml_entities( char *s )
{
     char *d = s;

     while (*s) {
          if (*s == '&') {
               if (!strncmp( s + 1, "amp;", 4 )) {
                    *d++ = '&';
                    s += 5;
                    continue;
               }
               else if (!strncmp( s + 1, "apos;", 5 )) {
                    *d++ = '\'';
                    s += 6;
                    continue;
               }
               else if (!strncmp( s + 1, "gt;", 3 )) {
                    *d++ = '>';
                    s += 4;
                    continue;
               }
               else if (!strncmp( s + 1, "lt;", 3 )) {
                    *d++ = '<';
                    s += 4;
                    continue;
               }
               else if (!strncmp( s+1, "quot;", 5 )) {
                    *d++ = '"';
                    s += 6;
                    continue;
               }
          }
          *d++ = *s++;
     }

     *d = '\0';
}

static void
xspf_playlist_parse( DirectLink **playlist,
                     char        *src )
{
     char *end;
     char *url     = NULL;
     char *creator = NULL;
     char *title   = NULL;
     char *album   = NULL;
     int   id      = 0;

     while ((src = strchr( src, '<' ))) {
          if (!strncmp( src, "<!--", 4 )) {
               src += 4;
               end = strstr( src, "-->" );
               if (!end)
                    break;
               src = end + 3;
          }
          else if (!strncmp( src, "<track>", 7 )) {
               src += 7;
               url = creator = title = album = NULL;
          }
          else if (!strncmp( src, "<location>", 10 )) {
               src += 10;
               end = strstr( src, "</location>" );
               if (end > src) {
                    *end = '\0';
                    url = trim( src );
                    src = end + 11;
               }
          }
          else if (!strncmp( src, "<creator>", 9 )) {
               src += 9;
               end = strstr( src, "</creator>" );
               if (end > src) {
                    *end = '\0';
                    creator = trim( src );
                    src = end + 10;
               }
          }
          else if (!strncmp( src, "<title>", 7 )) {
               src += 7;
               end = strstr( src, "</title>" );
               if (end > src) {
                    *end = '\0';
                    title = trim( src );
                    src = end + 8;
               }
          }
          else if (!strncmp( src, "<album>", 7 )) {
               src += 7;
               end = strstr( src, "</album>" );
               if (end > src) {
                    *end = '\0';
                    album = trim( src );
                    src = end + 8;
               }
          }
          else if (!strncmp( src, "</track>", 8 )) {
               src += 8;
               if (url) {
                    if (creator)
                         replace_xml_entities( creator );
                    if (title)
                         replace_xml_entities( title );
                    if (album)
                         replace_xml_entities( album );

                    add_media( id++, url, creator, title, album, playlist );

                    url = creator = title = album = NULL;
               }
          }
          else {
               src++;
          }
     }
}

/**********************************************************************************************************************/

static void
Playlist_Stop( IFusionSoundMusicProvider_Playlist_data *data )
{
     if (data->dest.stream) {
          data->dest.stream->Release( data->dest.stream );
          data->dest.stream = NULL;
     }

     if (data->dest.buffer) {
          data->dest.buffer->Release( data->dest.buffer );
          data->dest.buffer = NULL;
     }
}

/**********************************************************************************************************************/

static void
IFusionSoundMusicProvider_Playlist_Destruct( IFusionSoundMusicProvider *thiz )
{
     IFusionSoundMusicProvider_Playlist_data *data = thiz->priv;
     PlaylistEntry                           *entry, *next;

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     Playlist_Stop( data );

     direct_list_foreach_safe (entry, next, data->playlist)
          remove_media( entry, &data->playlist );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IFusionSoundMusicProvider_Playlist_AddRef( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_Release( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IFusionSoundMusicProvider_Playlist_Destruct( thiz );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_GetCapabilities( IFusionSoundMusicProvider   *thiz,
                                                    FSMusicProviderCapabilities *ret_caps )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->selected->provider)
          return data->selected->provider->GetCapabilities( data->selected->provider, ret_caps );

     return DR_UNSUPPORTED;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_EnumTracks( IFusionSoundMusicProvider *thiz,
                                               FSTrackCallback            callback,
                                               void                      *ctx )
{
     PlaylistEntry *entry, *next;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (!callback)
          return DR_INVARG;

     direct_list_foreach_safe (entry, next, data->playlist) {
          FSTrackDescription desc;

          if (entry->provider) {
               entry->provider->GetTrackDescription( entry->provider, &desc );
          }
          else {
               if (IFusionSoundMusicProvider_Create( entry->url, &entry->provider))
                    continue;
               entry->provider->GetTrackDescription( entry->provider, &desc );
               entry->provider->Release( entry->provider );
               entry->provider = NULL;
          }

          if (entry->artist)
               direct_snputs( desc.artist, entry->artist, sizeof(desc.artist) );
          if (entry->title)
               direct_snputs( desc.title, entry->title, sizeof(desc.title) );
          if (entry->album)
               direct_snputs( desc.album, entry->album, sizeof(desc.album) );

          if (callback( entry->id, desc, ctx ))
               return DR_INTERRUPTED;
     }

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_GetTrackID( IFusionSoundMusicProvider *thiz,
                                               FSTrackID                 *ret_track_id )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_track_id)
          return DR_INVARG;

     *ret_track_id = data->selected->id;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_GetTrackDescription( IFusionSoundMusicProvider *thiz,
                                                        FSTrackDescription        *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DR_INVARG;

     memset( ret_desc, 0, sizeof(FSTrackDescription) );

     if (data->selected->provider)
          data->selected->provider->GetTrackDescription( data->selected->provider, ret_desc );

     if (data->selected->artist)
          direct_snputs( ret_desc->artist, data->selected->artist, sizeof(ret_desc->artist) );
     if (data->selected->title)
          direct_snputs( ret_desc->title, data->selected->title, sizeof(ret_desc->title) );
     if (data->selected->album)
          direct_snputs( ret_desc->album, data->selected->album, sizeof(ret_desc->album) );

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_GetStreamDescription( IFusionSoundMusicProvider *thiz,
                                                         FSStreamDescription       *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->selected->provider)
          return data->selected->provider->GetStreamDescription( data->selected->provider, ret_desc );

     return DR_UNSUPPORTED;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_GetBufferDescription( IFusionSoundMusicProvider *thiz,
                                                         FSBufferDescription       *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->selected->provider)
          return data->selected->provider->GetBufferDescription( data->selected->provider, ret_desc );

     return DR_UNSUPPORTED;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_SelectTrack( IFusionSoundMusicProvider *thiz,
                                                FSTrackID                  track_id )
{
     PlaylistEntry *entry;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     direct_list_foreach (entry, data->playlist) {
          DirectResult ret;

          if (entry->id != track_id)
               continue;

          if (data->selected) {
               if (data->selected->provider) {
                    data->selected->provider->Release( data->selected->provider );
                    data->selected->provider = NULL;
               }
          }

          data->selected = entry;

          ret = IFusionSoundMusicProvider_Create( entry->url, &entry->provider );
          if (ret)
               return ret;

          entry->provider->SetPlaybackFlags( entry->provider, data->flags );

          if (data->dest.stream)
               entry->provider->PlayToStream( entry->provider, data->dest.stream );

          if (data->dest.buffer)
               entry->provider->PlayToBuffer( entry->provider, data->dest.buffer,
                                              data->buffer_callback, data->buffer_callback_context );

          return DR_OK;
     }

     return DR_ITEMNOTFOUND;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_PlayToStream( IFusionSoundMusicProvider *thiz,
                                                 IFusionSoundStream        *destination )
{
     DirectResult ret = DR_UNSUPPORTED;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->dest.stream) {
          data->dest.stream->Release( data->dest.stream );
          data->dest.stream = NULL;
     }

     if (data->dest.buffer) {
          data->dest.buffer->Release( data->dest.buffer );
          data->dest.buffer = NULL;
     }

     if (data->selected->provider) {
          ret = data->selected->provider->PlayToStream( data->selected->provider, destination );
          if (ret)
               return ret;
     }
     else
          return DR_UNSUPPORTED;

     /* Increase the sound stream reference counter. */
     destination->AddRef( destination );

     data->dest.stream = destination;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_PlayToBuffer( IFusionSoundMusicProvider *thiz,
                                                 IFusionSoundBuffer        *destination,
                                                 FMBufferCallback           callback,
                                                 void                      *ctx )
{
     DirectResult ret = DR_UNSUPPORTED;

     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     Playlist_Stop( data );

     if (data->selected->provider) {
          ret = data->selected->provider->PlayToBuffer( data->selected->provider, destination, callback, ctx );
          if (ret)
               return ret;
     }
     else
          return DR_UNSUPPORTED;

     /* Increase the sound buffer reference counter. */
     destination->AddRef( destination );

     data->dest.buffer             = destination;
     data->buffer_callback         = callback;
     data->buffer_callback_context = ctx;

     return DR_OK;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_Stop( IFusionSoundMusicProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     Playlist_Stop( data );

     if (data->selected->provider)
          return data->selected->provider->Stop( data->selected->provider );

     return DR_UNSUPPORTED;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_GetStatus( IFusionSoundMusicProvider *thiz,
                                              FSMusicProviderStatus     *ret_status )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->selected->provider)
          return data->selected->provider->GetStatus( data->selected->provider, ret_status );

     return DR_UNSUPPORTED;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_SeekTo( IFusionSoundMusicProvider *thiz,
                                           double                     seconds )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->selected->provider)
          return data->selected->provider->SeekTo( data->selected->provider, seconds );

     return DR_UNSUPPORTED;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_GetPos( IFusionSoundMusicProvider *thiz,
                                           double                    *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->selected->provider)
          return data->selected->provider->GetPos( data->selected->provider, ret_seconds );

     return DR_UNSUPPORTED;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_GetLength( IFusionSoundMusicProvider *thiz,
                                              double                    *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->selected->provider)
          return data->selected->provider->GetLength( data->selected->provider, ret_seconds );

     return DR_UNSUPPORTED;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_SetPlaybackFlags( IFusionSoundMusicProvider    *thiz,
                                                     FSMusicProviderPlaybackFlags  flags )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     data->flags = flags;

     if (data->selected->provider)
          return data->selected->provider->SetPlaybackFlags( data->selected->provider, flags );

     return DR_UNSUPPORTED;
}

static DirectResult
IFusionSoundMusicProvider_Playlist_WaitStatus( IFusionSoundMusicProvider *thiz,
                                               FSMusicProviderStatus      mask,
                                               unsigned int               timeout )
{
     DIRECT_INTERFACE_GET_DATA( IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->selected->provider)
          return data->selected->provider->WaitStatus( data->selected->provider, mask, timeout );

     return DR_UNSUPPORTED;
}

/**********************************************************************************************************************/

typedef enum {
     PLT_NONE,
     PLT_M3U,
     PLT_PLS,
     PLT_XSPF
} PlaylistType;

static PlaylistType
get_playlist_type( const char *mimetype,
                   const char *filename,
                   char       *header,
                   int         header_size )
{
     char *end = header + header_size;

     if (mimetype) {
          if (!strcmp( mimetype, "audio/mpegurl" ) || !strcmp( mimetype, "audio/x-mpegurl" ))
               return PLT_M3U;
          if (!strcmp( mimetype, "audio/x-scpls" ))
               return PLT_PLS;
          if (!strcmp( mimetype, "application/xspf+xml" ))
               return PLT_XSPF;
     }

     if (filename) {
          char *ext = strrchr( filename, '.' );

          if (ext) {
               if (!strcasecmp( ext, ".m3u" ))
                    return PLT_M3U;
               if (!strcasecmp( ext, ".pls" ))
                    return PLT_PLS;
               if (!strcasecmp( ext, ".xspf" ))
                    return PLT_XSPF;
          }
     }

     while (header < end && *header && space( *header ))
          header++;

     if (!strncmp( header, "#EXTM3U", 7 ))
          return PLT_M3U;
     if (!strncmp( header, "[Playlist]", 10 ))
          return PLT_PLS;
     if (!strncmp( header, "<playlist", 9 ))
          return PLT_XSPF;
     if (!strncmp( header, "<?xml", 5 )) {
          header += 5;
          while (header < end && (header = strchr( header, '<' ))) {
               if (!strncmp( header, "<playlist", 9 ))
                    return PLT_XSPF;
               header++;
          }
     }

     return PLT_NONE;
}

static DirectResult
Probe( IFusionSoundMusicProvider_ProbeContext *ctx )
{
     if (get_playlist_type( ctx->mimetype, ctx->filename, (char*) ctx->header, sizeof(ctx->header) ))
          return DR_OK;

     return DR_UNSUPPORTED;
}

static DirectResult
Construct( IFusionSoundMusicProvider *thiz,
           const char                *filename,
           DirectStream              *stream )
{
     DirectResult  ret;
     unsigned int  size;
     char         *src = NULL;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IFusionSoundMusicProvider_Playlist )

     D_DEBUG_AT( MusicProvider_Playlist, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref = 1;

     size = direct_stream_length( stream );
     if (size) {
          int pos = 0;

          src = D_MALLOC( size + 1 );
          if (!src) {
               ret = D_OOM();
               goto error;
          }

          while (pos < size) {
               unsigned int len;

               direct_stream_wait( stream, size, NULL );

               ret = direct_stream_read( stream, size, src + pos, &len );
               if (ret)
                    goto error;

               pos += len;
          }
     }
     else {
          char buf[1024];

          while (1) {
               unsigned int len;

               direct_stream_wait( stream, sizeof(buf), NULL );

               ret = direct_stream_read( stream, sizeof(buf), buf, &len );
               if (ret)
                    break;

               src = D_REALLOC( src, size + len + 1 );
               if (src) {
                   direct_memcpy( src + size, buf, len );
                   size += len;
               }
          }

          if (!src)
               goto error;
     }
     src[size] = 0;

     switch (get_playlist_type( direct_stream_mime( stream ), filename, src, size )) {
          case PLT_M3U:
               m3u_playlist_parse( &data->playlist, src );
               break;
          case PLT_PLS:
               pls_playlist_parse( &data->playlist, src );
               break;
          case PLT_XSPF:
               xspf_playlist_parse( &data->playlist, src );
               break;
          default:
               D_ERROR( "MusicProvider/Playlist: Unknown playlist format!" );
               ret = DR_FAILURE;
               goto error;
     }

     if (!data->playlist) {
          ret = DR_FAILURE;
          goto error;
     }

     D_FREE( src );

     thiz->AddRef               = IFusionSoundMusicProvider_Playlist_AddRef;
     thiz->Release              = IFusionSoundMusicProvider_Playlist_Release;
     thiz->GetCapabilities      = IFusionSoundMusicProvider_Playlist_GetCapabilities;
     thiz->EnumTracks           = IFusionSoundMusicProvider_Playlist_EnumTracks;
     thiz->GetTrackID           = IFusionSoundMusicProvider_Playlist_GetTrackID;
     thiz->GetTrackDescription  = IFusionSoundMusicProvider_Playlist_GetTrackDescription;
     thiz->GetStreamDescription = IFusionSoundMusicProvider_Playlist_GetStreamDescription;
     thiz->GetBufferDescription = IFusionSoundMusicProvider_Playlist_GetBufferDescription;
     thiz->SelectTrack          = IFusionSoundMusicProvider_Playlist_SelectTrack;
     thiz->PlayToStream         = IFusionSoundMusicProvider_Playlist_PlayToStream;
     thiz->PlayToBuffer         = IFusionSoundMusicProvider_Playlist_PlayToBuffer;
     thiz->Stop                 = IFusionSoundMusicProvider_Playlist_Stop;
     thiz->GetStatus            = IFusionSoundMusicProvider_Playlist_GetStatus;
     thiz->SeekTo               = IFusionSoundMusicProvider_Playlist_SeekTo;
     thiz->GetPos               = IFusionSoundMusicProvider_Playlist_GetPos;
     thiz->GetLength            = IFusionSoundMusicProvider_Playlist_GetLength;
     thiz->SetPlaybackFlags     = IFusionSoundMusicProvider_Playlist_SetPlaybackFlags;
     thiz->WaitStatus           = IFusionSoundMusicProvider_Playlist_WaitStatus;

     /* Select the first track. */
     thiz->SelectTrack( thiz, 0 );

     return DR_OK;

error:
     if (src)
          D_FREE( src );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
