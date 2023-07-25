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

#include <spng.h>
#include <display/idirectfbsurface.h>
#include <media/idirectfbimageprovider.h>

D_DEBUG_DOMAIN( ImageProvider_SPNG, "ImageProvider/SPNG", "SPNG Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, SPNG )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFB             *idirectfb;

     void                  *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_SPNG_data;

/**********************************************************************************************************************/

static int
read_fn( spng_ctx *spng,
         void     *user,
         void     *buf,
         size_t    size )
{
     DFBResult            ret;
     unsigned int         len;
     IDirectFBDataBuffer *buffer = user;

      ret = buffer->WaitForData( buffer, size );
      if (ret == DFB_OK)
           ret = buffer->GetData( buffer, size, buf, &len );

      if (ret)
           return ret != DFB_EOF ? SPNG_IO_ERROR : SPNG_IO_EOF;

     return SPNG_OK;
}

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_SPNG_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_SPNG_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_SPNG, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     D_FREE( data->image );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_SPNG_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SPNG )

     D_DEBUG_AT( ImageProvider_SPNG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_SPNG_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SPNG )

     D_DEBUG_AT( ImageProvider_SPNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_SPNG_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_SPNG_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                   DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SPNG )

     D_DEBUG_AT( ImageProvider_SPNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_SPNG_GetImageDescription( IDirectFBImageProvider *thiz,
                                                 DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SPNG )

     D_DEBUG_AT( ImageProvider_SPNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_SPNG_RenderTo( IDirectFBImageProvider *thiz,
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

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SPNG )

     D_DEBUG_AT( ImageProvider_SPNG, "%s( %p )\n", __FUNCTION__, thiz );

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
     desc.preallocated[0].data   = data->image;
     desc.preallocated[0].pitch  = data->desc.width * 4;

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
IDirectFBImageProvider_SPNG_SetRenderCallback( IDirectFBImageProvider *thiz,
                                               DIRenderCallback        callback,
                                               void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SPNG )

     D_DEBUG_AT( ImageProvider_SPNG, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     int               result;
     struct spng_ihdr  ihdr;
     spng_ctx         *spng = NULL;

     spng = spng_ctx_new( 0 );
     if (!spng)
          return DFB_UNSUPPORTED;

     spng_set_png_buffer( spng, ctx->header, sizeof(ctx->header) );

     result = spng_get_ihdr( spng, &ihdr );

     spng_ctx_free( spng );

     return result ? DFB_UNSUPPORTED : DFB_OK;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult         ret;
     int               result;
     struct spng_ihdr  ihdr;
     size_t            size;
     spng_ctx         *spng = NULL;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_SPNG )

     D_DEBUG_AT( ImageProvider_SPNG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     spng = spng_ctx_new( 0 );
     if (!spng) {
          D_ERROR( "ImageProvider/SPNG: Failed to create SPNG context!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     spng_set_png_stream( spng, read_fn, buffer );

     result = spng_get_ihdr( spng, &ihdr );
     if (result) {
          D_ERROR( "ImageProvider/SPNG: Failed to read PNG header!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     result = spng_decoded_image_size( spng, SPNG_FMT_RGBA8, &size );
     if (result) {
          D_ERROR( "ImageProvider/SPNG: Failed to get image output buffer size!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     /* Allocate image data. */
     data->image = D_MALLOC( size );
     if (!data->image) {
          ret = D_OOM();
          goto error;
     }

     result = spng_decode_image( spng, data->image, size, SPNG_FMT_RGBA8, 0 );
     if (result) {
          D_ERROR( "ImageProvider/SPNG: Error during decoding!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     spng_ctx_free( spng );

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = ihdr.width;
     data->desc.height      = ihdr.height;
     data->desc.pixelformat = DSPF_ABGR;

     thiz->AddRef                = IDirectFBImageProvider_SPNG_AddRef;
     thiz->Release               = IDirectFBImageProvider_SPNG_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_SPNG_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_SPNG_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_SPNG_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_SPNG_SetRenderCallback;

     return DFB_OK;

error:
     if (data->image)
          D_FREE( data->image );

     if (spng)
          spng_ctx_free( spng );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
