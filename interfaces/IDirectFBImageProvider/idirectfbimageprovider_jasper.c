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
#include <direct/filesystem.h>
#include <display/idirectfbsurface.h>
#include <jasper/jasper.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbimageprovider.h>
#include <misc/gfx_util.h>

D_DEBUG_DOMAIN( ImageProvider_JasPer, "ImageProvider/JasPer", "JasPer Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, JasPer )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     jas_image_t           *jas_ptr;
     u32                   *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_JasPer_data;

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_JasPer_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_JasPer_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_JasPer, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     if (data->image)
          D_FREE( data->image );

     jas_image_destroy( data->jas_ptr );

     jas_cleanup();

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_JasPer_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JasPer )

     D_DEBUG_AT( ImageProvider_JasPer, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_JasPer_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JasPer )

     D_DEBUG_AT( ImageProvider_JasPer, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_JasPer_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JasPer_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                     DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JasPer )

     D_DEBUG_AT( ImageProvider_JasPer, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JasPer_GetImageDescription( IDirectFBImageProvider *thiz,
                                                   DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JasPer )

     D_DEBUG_AT( ImageProvider_JasPer, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JasPer_RenderTo( IDirectFBImageProvider *thiz,
                                        IDirectFBSurface       *destination,
                                        const DFBRectangle     *dest_rect )
{
     DFBResult               ret;
     IDirectFBSurface_data  *dst_data;
     DFBRectangle            rect;
     DFBRegion               clip;
     CoreSurfaceBufferLock   lock;
     DIRenderCallbackResult  cb_result = DIRCR_OK;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JasPer )

     D_DEBUG_AT( ImageProvider_JasPer, "%s( %p )\n", __FUNCTION__, thiz );

     if (!destination)
          return DFB_INVARG;

     dst_data = destination->priv;
     if (!dst_data)
          return DFB_DEAD;

     if (!dst_data->surface)
          return DFB_DESTROYED;

     if (dest_rect) {
          if (dest_rect->w < 1 || dest_rect->h < 1)
               return DFB_INVARG;

          rect = *dest_rect;
          rect.x += dst_data->area.wanted.x;
          rect.y += dst_data->area.wanted.y;
     }
     else
          rect = dst_data->area.wanted;

     dfb_region_from_rectangle( &clip, &dst_data->area.current );

     if (!dfb_rectangle_region_intersects( &rect, &clip ))
          return DFB_OK;

     ret = dfb_surface_lock_buffer( dst_data->surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
     if (ret)
          return ret;

     if (!data->image) {
          int  cmpts[3];
          int  tlx, tly;
          int  hs, vs;
          int  i, j;
          bool mono;
          bool direct = (rect.w == data->desc.width && rect.h == data->desc.height);

          if (jas_image_numcmpts( data->jas_ptr ) > 1) {
               cmpts[0] = jas_image_getcmptbytype( data->jas_ptr, JAS_IMAGE_CT_COLOR( JAS_CLRSPC_CHANIND_RGB_R ) );
               cmpts[1] = jas_image_getcmptbytype( data->jas_ptr, JAS_IMAGE_CT_COLOR( JAS_CLRSPC_CHANIND_RGB_G ) );
               cmpts[2] = jas_image_getcmptbytype( data->jas_ptr, JAS_IMAGE_CT_COLOR( JAS_CLRSPC_CHANIND_RGB_B ) );

               if (cmpts[0] < 0 || cmpts[1] < 0 || cmpts[2] < 0) {
                    dfb_surface_unlock_buffer( dst_data->surface, &lock );
                    return DFB_UNSUPPORTED;
               }

               mono = false;
          }
          else {
               cmpts[0] = cmpts[1] = cmpts[2] = 0;
               mono = true;
          }

          tlx = jas_image_cmpttlx( data->jas_ptr, 0 );
          tly = jas_image_cmpttly( data->jas_ptr, 0 );

          hs = jas_image_cmpthstep( data->jas_ptr, 0 );
          vs = jas_image_cmptvstep( data->jas_ptr, 0 );

          /* Allocate image data. */
          data->image = D_CALLOC( data->desc.height, data->desc.width * 4 );
          if (!data->image) {
               dfb_surface_unlock_buffer( dst_data->surface, &lock );
               return D_OOM();
          }

#define get_sample( n, x, y )                                           \
( {                                                                     \
     int s = jas_image_readcmptsample( data->jas_ptr, cmpts[n], x, y ); \
     s >>= jas_image_cmptprec( data->jas_ptr, cmpts[n] ) - 8;           \
     if (s > 255) s = 255;                                              \
     else if (s < 0) s = 0;                                             \
     s;                                                                 \
} )

          for (i = 0; i < data->desc.height; i++) {
               u32 *dst = data->image + i * data->desc.width;
               int  x, y;

               y = (i - tly) / vs;
               if (y >= 0 && y < data->desc.height) {
                    for (j = 0; j < data->desc.width; j++) {
                         x = (j - tlx) / hs;
                         if (x >= 0 && x < data->desc.width) {
                              unsigned int r, g, b;
                              if (mono) {
                                   r = g = b = get_sample( 0, x, y );
                              }
                              else {
                                   r = get_sample( 0, x, y );
                                   g = get_sample( 1, x, y );
                                   b = get_sample( 2, x, y );
                              }
                              *dst++ = 0xff000000 | (r << 16) | (g << 8) | b;
                         }
                         else {
                              *dst++ = 0;
                         }
                    }
               }
               else {
                    memset( dst, 0, data->desc.width * 4 );
               }

               if (direct) {
                    DFBRectangle r = { rect.x, rect.y + i, data->desc.width, 1 };

                    dfb_copy_buffer_32( data->image + i * data->desc.width,
                                        lock.addr, lock.pitch, &r, dst_data->surface, &clip );

                    if (data->render_callback) {
                         r = (DFBRectangle) { 0, i, data->desc.width, 1 };

                         cb_result = data->render_callback( &r, data->render_callback_context );
                    }
               }
          }

          if (!direct) {
               dfb_scale_linear_32( data->image, data->desc.width, data->desc.height,
                                    lock.addr, lock.pitch, &rect, dst_data->surface, &clip );

               if (data->render_callback) {
                    DFBRectangle r = { 0, 0, data->desc.width, data->desc.height };

                    cb_result = data->render_callback( &r, data->render_callback_context );
               }
          }

          if (cb_result != DIRCR_OK) {
               D_FREE( data->image );
               data->image = NULL;
               ret = DFB_INTERRUPTED;
          }
     }
     else {
          dfb_scale_linear_32( data->image, data->desc.width, data->desc.height,
                               lock.addr, lock.pitch, &rect, dst_data->surface, &clip );

          if (data->render_callback) {
               DFBRectangle r = { 0, 0, data->desc.width, data->desc.height };

               data->render_callback( &r, data->render_callback_context );
          }
     }

     dfb_surface_unlock_buffer( dst_data->surface, &lock );

     return ret;
}

static DFBResult
IDirectFBImageProvider_JasPer_SetRenderCallback( IDirectFBImageProvider *thiz,
                                                 DIRenderCallback        callback,
                                                 void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JasPer )

     D_DEBUG_AT( ImageProvider_JasPer, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     /* Check the JP2 or JPC signature. */
     if (!memcmp( ctx->header, "\x00\x00\x00\x0C\x6A\x50\x20\x20\x0D\x0A\x87\x0A", 12 ) ||
         !memcmp( ctx->header, "\xFF\x4F", 2 ))
          return DFB_OK;

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     jas_stream_t             *st;
     char                     *chunk       = NULL;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_JasPer )

     D_DEBUG_AT( ImageProvider_JasPer, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref = 1;

     jas_init();

     if (buffer_data->buffer) {
          st = jas_stream_memopen( buffer_data->buffer, buffer_data->length );
     }
     else if (buffer_data->filename) {
          st = jas_stream_fopen( buffer_data->filename, "rb" );
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

          st = jas_stream_memopen( chunk, size );
     }

     if (!st) {
          D_ERROR( "ImageProvider/JasPer: Failed to open stream!\n" );
          ret = DFB_UNSUPPORTED;
          goto error;
     }

     data->jas_ptr = jas_image_decode( st, -1, 0 );

     jas_stream_close( st );

     if (chunk)
          D_FREE( chunk );

     if (!data->jas_ptr) {
          ret = DFB_FAILURE;
          goto error;
     }

     switch (jas_image_numcmpts( data->jas_ptr )) {
          case 1:
          case 3:
               break;
          default:
               ret = DFB_UNSUPPORTED;
               goto error;
     }

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = jas_image_width( data->jas_ptr );
     data->desc.height      = jas_image_height( data->jas_ptr );
     data->desc.pixelformat = dfb_primary_layer_pixelformat();

     thiz->AddRef                = IDirectFBImageProvider_JasPer_AddRef;
     thiz->Release               = IDirectFBImageProvider_JasPer_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_JasPer_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_JasPer_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_JasPer_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_JasPer_SetRenderCallback;

     return DFB_OK;

error:
     if (data->jas_ptr)
          jas_image_destroy( data->jas_ptr );

     if (chunk)
          D_FREE( chunk );

     jas_cleanup();

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
