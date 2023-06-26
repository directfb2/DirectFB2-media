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

#include <avif/avif.h>
#include <display/idirectfbsurface.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbimageprovider.h>

D_DEBUG_DOMAIN( ImageProvider_AVIF, "ImageProvider/AVIF", "AVIF Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, AVIF )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFB             *idirectfb;

     avifDecoder           *dec;
     avifRGBImage           rgb;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_AVIF_data;

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_AVIF_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_AVIF_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_AVIF, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     avifRGBImageFreePixels( &data->rgb );

     avifDecoderDestroy( data->dec );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_AVIF_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_AVIF )

     D_DEBUG_AT( ImageProvider_AVIF, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_AVIF_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_AVIF )

     D_DEBUG_AT( ImageProvider_AVIF, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_AVIF_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_AVIF_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                   DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_AVIF )

     D_DEBUG_AT( ImageProvider_AVIF, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_AVIF_GetImageDescription( IDirectFBImageProvider *thiz,
                                                 DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_AVIF )

     D_DEBUG_AT( ImageProvider_AVIF, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_AVIF_RenderTo( IDirectFBImageProvider *thiz,
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

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_AVIF )

     D_DEBUG_AT( ImageProvider_AVIF, "%s( %p )\n", __FUNCTION__, thiz );

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
     desc.preallocated[0].data   = data->rgb.pixels;
     desc.preallocated[0].pitch  = data->rgb.rowBytes;

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
IDirectFBImageProvider_AVIF_SetRenderCallback( IDirectFBImageProvider *thiz,
                                               DIRenderCallback        callback,
                                               void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_AVIF )

     D_DEBUG_AT( ImageProvider_AVIF, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     /* Check the magic. */
     if (!strncmp( (const char*) ctx->header + 4, "ftypavif", 8 ))
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
     avifResult                result;
     char                     *chunk       = NULL;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_AVIF )

     D_DEBUG_AT( ImageProvider_AVIF, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     data->dec = avifDecoderCreate();
     if (!data->dec) {
          D_ERROR( "ImageProvider/AVIF: Failed to create AVIF decoder!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     if (buffer_data->buffer)
          avifDecoderSetIOMemory( data->dec, buffer_data->buffer, buffer_data->length );
     else if (buffer_data->filename)
          avifDecoderSetIOFile( data->dec, buffer_data->filename );
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

          avifDecoderSetIOMemory( data->dec, (const uint8_t*) chunk, size );
     }

     result = avifDecoderParse( data->dec );
     if (result != AVIF_RESULT_OK) {
          D_ERROR( "ImageProvider/AVIF: Failed to parse image: %s!\n", avifResultToString( result ) );
          ret = DFB_FAILURE;
          goto error;
     }

     result = avifDecoderNextImage( data->dec );
     if (result != AVIF_RESULT_OK) {
          D_ERROR( "ImageProvider/AVIF: Error during decoding: %s!\n", avifResultToString( result ) );
          ret = DFB_FAILURE;
          goto error;
     }

     if (chunk)
          D_FREE( chunk );

     avifRGBImageSetDefaults( &data->rgb, data->dec->image );

     /* Allocate image data. */
     avifRGBImageAllocatePixels( &data->rgb );

     /* Conversion from YUV. */
     avifImageYUVToRGB( data->dec->image, &data->rgb );

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = data->dec->image->width;
     data->desc.height      = data->dec->image->height;
     data->desc.pixelformat = DSPF_ABGR;

     thiz->AddRef                = IDirectFBImageProvider_AVIF_AddRef;
     thiz->Release               = IDirectFBImageProvider_AVIF_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_AVIF_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_AVIF_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_AVIF_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_AVIF_SetRenderCallback;

     return DFB_OK;

error:
     if (chunk)
          D_FREE( chunk );

     if (data->dec)
          avifDecoderDestroy( data->dec );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
