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

#include <core/layers.h>
#include <direct/thread.h>
#include <display/idirectfbsurface.h>
#include <libmng.h>
#include <media/idirectfbvideoprovider.h>
#include <misc/gfx_util.h>

D_DEBUG_DOMAIN( VideoProvider_MNG, "VideoProvider/MNG", "MNG Video Provider" );

static DFBResult Probe    ( IDirectFBVideoProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBVideoProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBVideoProvider, MNG )

/**********************************************************************************************************************/

typedef struct {
     int                            ref;                    /* reference counter */

     IDirectFBDataBuffer           *buffer;

     DFBBoolean                     seekable;

     mng_handle                     handle;
     mng_uint32                     delay;
     u32                           *image;

     DFBSurfaceDescription          desc;

     DFBVideoProviderStatus         status;
     DFBVideoProviderPlaybackFlags  flags;

     DirectThread                  *thread;
     DirectMutex                    lock;

     IDirectFBSurface              *dest;
     DFBRectangle                   rect;

     DVFrameCallback                frame_callback;
     void                          *frame_callback_context;
} IDirectFBVideoProvider_MNG_data;

/**********************************************************************************************************************/

static mng_ptr
memalloc( mng_size_t size )
{
     return D_CALLOC( 1, size );
}

static void
memfree( mng_ptr    ptr,
         mng_size_t size )
{
     D_FREE( ptr );
}

static mng_bool
openstream( mng_handle handle )
{
     return MNG_TRUE;
}

static mng_bool
closestream( mng_handle handle )
{
     return MNG_TRUE;
}

static mng_bool
readdata( mng_handle  handle,
          mng_ptr     buf,
          mng_uint32  len,
          mng_uint32 *read )
{
     DFBResult                        ret;
     IDirectFBVideoProvider_MNG_data *data = mng_get_userdata( handle );

     if (data->buffer->HasData( data->buffer ) == DFB_OK) {
         ret = data->buffer->GetData( data->buffer, len, buf, read );
         if (ret)
             return MNG_FALSE;
     }
     else
         return MNG_FALSE;

     return MNG_TRUE;
}

static mng_bool
processheader( mng_handle handle,
               mng_uint32 width,
               mng_uint32 height )
{
     IDirectFBVideoProvider_MNG_data *data = mng_get_userdata( handle );

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = width;
     data->desc.height      = height;
     data->desc.pixelformat = dfb_primary_layer_pixelformat();

     data->image = D_MALLOC( width * height * 4 );

     mng_set_canvasstyle( handle, MNG_CANVAS_BGRA8 );

     return MNG_TRUE;
}

static mng_ptr
getcanvasline( mng_handle handle,
               mng_uint32 linenr )
{
     IDirectFBVideoProvider_MNG_data *data = mng_get_userdata( handle );

     return data->image + data->desc.width * linenr;
}

static mng_bool
refresh( mng_handle handle,
         mng_uint32 x,
         mng_uint32 y,
         mng_uint32 width,
         mng_uint32 height )
{
     DFBResult                        ret;
     IDirectFBSurface_data           *dst_data;
     DFBRegion                        clip;
     CoreSurfaceBufferLock            lock;
     IDirectFBVideoProvider_MNG_data *data = mng_get_userdata( handle );

     dst_data = data->dest->priv;
     if (!dst_data || !dst_data->surface)
          return MNG_FALSE;

     dfb_region_from_rectangle( &clip, &dst_data->area.current );

     if (!dfb_rectangle_region_intersects( &data->rect, &clip ))
          return MNG_TRUE;

     ret = dfb_surface_lock_buffer( dst_data->surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
     if (ret)
          return MNG_FALSE;

     dfb_scale_linear_32( data->image, data->desc.width, data->desc.height,
                          lock.addr, lock.pitch, &data->rect, dst_data->surface, &clip );

     dfb_surface_unlock_buffer( dst_data->surface, &lock );

     if (data->frame_callback)
          data->frame_callback( data->frame_callback_context );

     return MNG_TRUE;
}

static mng_uint32
gettickcount( mng_handle handle )
{
     return direct_clock_get_millis();
}

static mng_bool
settimer( mng_handle handle,
          mng_uint32 msecs )
{
     IDirectFBVideoProvider_MNG_data *data = mng_get_userdata( handle );

     data->delay = msecs;

     return MNG_TRUE;
}

static void *
MNGVideo( DirectThread *thread,
          void         *arg )
{
     mng_retcode                      retcode;
     IDirectFBVideoProvider_MNG_data *data = arg;

     direct_mutex_lock( &data->lock );

     retcode = mng_display( data->handle );

     direct_mutex_unlock( &data->lock );

     while (data->status != DVSTATE_STOP) {
          direct_mutex_lock( &data->lock );

          if ((data->flags & DVPLAY_LOOPING) && retcode == MNG_NOERROR) {
               mng_display_reset( data->handle );

               retcode = mng_display( data->handle );
          }

          if (data->delay) {
               usleep( data->delay * 1000 );

               if (data->status == DVSTATE_STOP) {
                    direct_mutex_unlock( &data->lock );
                    break;
               }

               retcode = mng_display_resume( data->handle );
          }

          direct_mutex_unlock( &data->lock );
     }

     return NULL;
}

/**********************************************************************************************************************/

static void
IDirectFBVideoProvider_MNG_Destruct( IDirectFBVideoProvider *thiz )
{
     IDirectFBVideoProvider_MNG_data *data = thiz->priv;

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     thiz->Stop( thiz );

     if (data->image)
          D_FREE( data->image );

     direct_mutex_deinit( &data->lock );

     mng_cleanup( &data->handle );

     /* Decrease the data buffer reference counter. */
     if (data->buffer)
          data->buffer->Release( data->buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBVideoProvider_MNG_AddRef( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBVideoProvider_MNG_Release( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBVideoProvider_MNG_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_MNG_GetCapabilities( IDirectFBVideoProvider       *thiz,
                                            DFBVideoProviderCapabilities *ret_caps )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_caps)
          return DFB_INVARG;

     *ret_caps = DVCAPS_BASIC | DVCAPS_SCALE;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_MNG_GetSurfaceDescription( IDirectFBVideoProvider *thiz,
                                                  DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_MNG_GetStreamDescription( IDirectFBVideoProvider *thiz,
                                                 DFBStreamDescription   *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     memset( ret_desc, 0, sizeof(DFBStreamDescription) );

     ret_desc->caps = DVSCAPS_VIDEO;

     snprintf( ret_desc->video.encoding, DFB_STREAM_DESC_ENCODING_LENGTH, "mng" );

     ret_desc->video.aspect  = (double) data->desc.width / data->desc.height;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_MNG_PlayTo( IDirectFBVideoProvider *thiz,
                                   IDirectFBSurface       *destination,
                                   const DFBRectangle     *dest_rect,
                                   DVFrameCallback         callback,
                                   void                   *ctx )
{
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

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

     data->dest                   = destination;
     data->rect                   = rect;
     data->frame_callback         = callback;
     data->frame_callback_context = ctx;

     data->status = DVSTATE_PLAY;

     data->thread = direct_thread_create( DTT_DEFAULT, MNGVideo, data, "MNG Video" );

     direct_mutex_unlock( &data->lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_MNG_Stop( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->status == DVSTATE_STOP)
          return DFB_OK;

     data->status = DVSTATE_STOP;

     if (data->thread) {
          direct_thread_join( data->thread );
          direct_thread_destroy( data->thread );
          data->thread = NULL;
     }

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_MNG_GetStatus( IDirectFBVideoProvider *thiz,
                                      DFBVideoProviderStatus *ret_status )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_status)
          return DFB_INVARG;

     *ret_status = data->status;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_MNG_GetPos( IDirectFBVideoProvider *thiz,
                                   double                 *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DFB_INVARG;

     *ret_seconds = (double) mng_get_runtime( data->handle ) / 1000;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_MNG_GetLength( IDirectFBVideoProvider *thiz,
                                      double                 *ret_seconds )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_seconds)
          return DFB_INVARG;

     *ret_seconds = (double) mng_get_totalplaytime( data->handle ) / 1000;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_MNG_SetPlaybackFlags( IDirectFBVideoProvider        *thiz,
                                             DFBVideoProviderPlaybackFlags  flags )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (flags & ~DVPLAY_LOOPING)
          return DFB_UNSUPPORTED;

     if (flags & DVPLAY_LOOPING && !data->seekable)
          return DFB_UNSUPPORTED;

     data->flags = flags;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_MNG_SetDestination( IDirectFBVideoProvider *thiz,
                                           IDirectFBSurface       *destination,
                                           const DFBRectangle     *dest_rect )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p, %4d,%4d-%4dx%4d )\n", __FUNCTION__,
                 thiz, dest_rect->x, dest_rect->y, dest_rect->w, dest_rect->h );

     if (!dest_rect)
          return DFB_INVARG;

     if (dest_rect->w < 1 || dest_rect->h < 1)
          return DFB_INVARG;

     data->rect = *dest_rect;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBVideoProvider_ProbeContext *ctx )
{
     /* Check the signature. */
     if (ctx->header[0] == 0x8a && ctx->header[1] == 0x4d && ctx->header[2] == 0x4e && ctx->header[3] == 0x47 &&
         ctx->header[4] == 0x0d && ctx->header[5] == 0x0a && ctx->header[6] == 0x1a && ctx->header[7] == 0x0a)
          return DFB_OK;

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBVideoProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBVideoProvider_MNG )

     D_DEBUG_AT( VideoProvider_MNG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->buffer = buffer;

     /* Increase the data buffer reference counter. */
     buffer->AddRef( buffer );

     data->seekable = (buffer->SeekTo( buffer, 0 ) == DFB_OK);

     data->handle = mng_initialize( data, memalloc, memfree, NULL );
     if (!data->handle)
          goto error;

     mng_setcb_openstream   ( data->handle, openstream );
     mng_setcb_closestream  ( data->handle, closestream );
     mng_setcb_readdata     ( data->handle, readdata );
     mng_setcb_processheader( data->handle, processheader );
     mng_setcb_getcanvasline( data->handle, getcanvasline );
     mng_setcb_refresh      ( data->handle, refresh );
     mng_setcb_gettickcount ( data->handle, gettickcount );
     mng_setcb_settimer     ( data->handle, settimer );

     mng_read( data->handle );

     data->status = DVSTATE_STOP;

     direct_mutex_init( &data->lock );

     thiz->AddRef                = IDirectFBVideoProvider_MNG_AddRef;
     thiz->Release               = IDirectFBVideoProvider_MNG_Release;
     thiz->GetCapabilities       = IDirectFBVideoProvider_MNG_GetCapabilities;
     thiz->GetSurfaceDescription = IDirectFBVideoProvider_MNG_GetSurfaceDescription;
     thiz->GetStreamDescription  = IDirectFBVideoProvider_MNG_GetStreamDescription;
     thiz->PlayTo                = IDirectFBVideoProvider_MNG_PlayTo;
     thiz->Stop                  = IDirectFBVideoProvider_MNG_Stop;
     thiz->GetStatus             = IDirectFBVideoProvider_MNG_GetStatus;
     thiz->GetPos                = IDirectFBVideoProvider_MNG_GetPos;
     thiz->GetLength             = IDirectFBVideoProvider_MNG_GetLength;
     thiz->SetPlaybackFlags      = IDirectFBVideoProvider_MNG_SetPlaybackFlags;
     thiz->SetDestination        = IDirectFBVideoProvider_MNG_SetDestination;

     return DFB_OK;

error:
     buffer->Release( buffer );

     if (data->image)
          D_FREE( data->image );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return DFB_FAILURE;
}
