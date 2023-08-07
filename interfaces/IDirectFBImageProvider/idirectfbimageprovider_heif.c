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

#include <display/idirectfbsurface.h>
#include <libheif/heif.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbimageprovider.h>

D_DEBUG_DOMAIN( ImageProvider_HEIF, "ImageProvider/HEIF", "HEIF Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, HEIF )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFB             *idirectfb;

     struct heif_context   *context;
     struct heif_image     *image;
     const uint8_t         *rgba;
     int                    stride;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_HEIF_data;

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_HEIF_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_HEIF_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_HEIF, "%s( %p )\n", __FUNCTION__, thiz );

     heif_image_release( data->image );

     heif_context_free( data->context );

     heif_deinit();

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_HEIF_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_HEIF )

     D_DEBUG_AT( ImageProvider_HEIF, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_HEIF_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_HEIF )

     D_DEBUG_AT( ImageProvider_HEIF, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_HEIF_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_HEIF_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                   DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_HEIF )

     D_DEBUG_AT( ImageProvider_HEIF, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_HEIF_GetImageDescription( IDirectFBImageProvider *thiz,
                                                 DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_HEIF )

     D_DEBUG_AT( ImageProvider_HEIF, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_HEIF_RenderTo( IDirectFBImageProvider *thiz,
                                      IDirectFBSurface       *destination,
                                      const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRegion              clip;
     DFBRegion              old_clip;
     DFBSurfaceDescription  desc;
     IDirectFBSurface      *source;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_HEIF )

     D_DEBUG_AT( ImageProvider_HEIF, "%s( %p )\n", __FUNCTION__, thiz );

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

     dfb_region_from_rectangle( &clip, &dst_data->area.current );

     if (!dfb_rectangle_region_intersects( &rect, &clip ))
          return DFB_OK;
     else
          clip = DFB_REGION_INIT_FROM_RECTANGLE( &rect );

     desc = data->desc;

     desc.flags                 |= DSDESC_PREALLOCATED;
     desc.preallocated[0].data   = (void*) data->rgba;
     desc.preallocated[0].pitch  = data->stride;

     ret = data->idirectfb->CreateSurface( data->idirectfb, &desc, &source );
     if (ret)
          return ret;

     destination->GetClip( destination, &old_clip );

     destination->SetClip( destination, &clip );

     destination->StretchBlit( destination, source, NULL, &rect );

     destination->SetClip( destination, &old_clip );

     destination->ReleaseSource( destination );

     source->Release( source );

     if (data->render_callback) {
          DFBRectangle r = { 0, 0, data->desc.width, data->desc.height };

          data->render_callback( &r, data->render_callback_context );
     }

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_HEIF_SetRenderCallback( IDirectFBImageProvider *thiz,
                                               DIRenderCallback        callback,
                                               void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_HEIF )

     D_DEBUG_AT( ImageProvider_HEIF, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     /* Check the magic. */
     if (!strncmp( (const char*) ctx->header + 4, "ftyp", 4 ))
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
     struct heif_error         err;
     struct heif_image_handle *image_handle;
     void                     *chunk       = NULL;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_HEIF )

     D_DEBUG_AT( ImageProvider_HEIF, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     err = heif_init( NULL );
     if (err.code != heif_error_Ok) {
          D_ERROR( "ImageProvider/HEIF: Initialization of the HEIF library failed: %s!\n", err.message );
          DIRECT_DEALLOCATE_INTERFACE( thiz );
          return DFB_FAILURE;
     }

     data->context = heif_context_alloc();
     if (!data->context) {
          D_ERROR( "ImageProvider/HEIF: Failed to create HEIF context!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     if (buffer_data->buffer) {
          heif_context_read_from_memory_without_copy( data->context, buffer_data->buffer, buffer_data->length, NULL );
     }
     else if (buffer_data->filename) {
          heif_context_read_from_file( data->context, buffer_data->filename, NULL );
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

          heif_context_read_from_memory_without_copy( data->context, chunk, size, NULL );
     }

     err = heif_context_get_primary_image_handle( data->context, &image_handle );
     if (err.code != heif_error_Ok) {
          D_ERROR( "ImageProvider/HEIF: Failed to get handle to the primary image: %s!\n", err.message );
          ret = DFB_FAILURE;
          goto error;
     }

     err = heif_decode_image( image_handle, &data->image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, NULL );
     if (err.code != heif_error_Ok) {
          D_ERROR( "ImageProvider/HEIF: Error during decoding: %s!\n", err.message );
          ret = DFB_FAILURE;
          goto error;
     }

     heif_image_handle_release( image_handle );

     if (chunk)
          D_FREE( chunk );

     data->rgba = heif_image_get_plane_readonly( data->image, heif_channel_interleaved, &data->stride );

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = heif_image_get_width( data->image, heif_channel_interleaved );
     data->desc.height      = heif_image_get_height( data->image, heif_channel_interleaved );
     data->desc.pixelformat = DSPF_ABGR;

     thiz->AddRef                = IDirectFBImageProvider_HEIF_AddRef;
     thiz->Release               = IDirectFBImageProvider_HEIF_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_HEIF_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_HEIF_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_HEIF_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_HEIF_SetRenderCallback;

     return DFB_OK;

error:
     if (chunk)
          D_FREE( chunk );

     if (data->context)
          heif_context_free( data->context );

     heif_deinit();

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
