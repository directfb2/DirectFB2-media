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
#include <media/idirectfbimageprovider.h>
#include <webp/decode.h>

D_DEBUG_DOMAIN( ImageProvider_WebP, "ImageProvider/WebP", "WebP Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, WebP )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFBDataBuffer   *buffer;
     IDirectFB             *idirectfb;

     size_t                 image_size;
     uint8_t               *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_WebP_data;

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_WebP_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_WebP_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_WebP, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     D_FREE( data->image );

     /* Decrease the data buffer reference counter. */
     if (data->buffer)
          data->buffer->Release( data->buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_WebP_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_WebP )

     D_DEBUG_AT( ImageProvider_WebP, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_WebP_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_WebP )

     D_DEBUG_AT( ImageProvider_WebP, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_WebP_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_WebP_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                   DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_WebP )

     D_DEBUG_AT( ImageProvider_WebP, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_WebP_GetImageDescription( IDirectFBImageProvider *thiz,
                                                 DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_WebP )

     D_DEBUG_AT( ImageProvider_WebP, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DFB_PIXELFORMAT_HAS_ALPHA( data->desc.pixelformat ) ? DICAPS_ALPHACHANNEL : DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_WebP_RenderTo( IDirectFBImageProvider *thiz,
                                      IDirectFBSurface       *destination,
                                      const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRegion              clip;
     DFBRegion              old_clip;
     WebPDecoderConfig      config;
     unsigned int           len;
     VP8StatusCode          status;
     int                    pitch;
     void                  *ptr;
     IDirectFBSurface      *source;
     WebPIDecoder          *idec;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_WebP )

     D_DEBUG_AT( ImageProvider_WebP, "%s( %p )\n", __FUNCTION__, thiz );

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

     ret = data->idirectfb->CreateSurface( data->idirectfb, &data->desc, &source );
     if (ret)
          return ret;

     source->Lock( source, DSLF_WRITE, &ptr, &pitch );

     idec = WebPINewDecoder( &config.output );

     config.output.colorspace         = (data->desc.pixelformat == DSPF_ARGB) ? MODE_bgrA : MODE_BGR;
     config.output.u.RGBA.rgba        = ptr;
     config.output.u.RGBA.stride      = pitch;
     config.output.u.RGBA.size        = pitch * data->desc.height;
     config.output.is_external_memory = 1;

     ret = data->buffer->SeekTo( data->buffer, 0 );
     if (ret)
          return ret;

     status = VP8_STATUS_NOT_ENOUGH_DATA;

     while (data->buffer->HasData( data->buffer ) == DFB_OK) {
          ret = data->buffer->GetData( data->buffer, data->image_size, data->image, &len );
          if (ret)
               break;

          status = WebPIAppend( idec, data->image, len );
          if (!(status == VP8_STATUS_OK || status == VP8_STATUS_SUSPENDED))
               break;
     }

     WebPIDelete( idec );

     WebPFreeDecBuffer( &config.output );

     if (ret || status != VP8_STATUS_OK)
          return ret ?: DFB_FAILURE;

     source->Unlock( source );

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
IDirectFBImageProvider_WebP_SetRenderCallback( IDirectFBImageProvider *thiz,
                                               DIRenderCallback        callback,
                                               void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_WebP )

     D_DEBUG_AT( ImageProvider_WebP, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     if (WebPGetInfo( ctx->header, D_ARRAY_SIZE( ctx->header ), NULL, NULL ))
          return DFB_OK;

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult             ret;
     unsigned int          read;
     uint8_t               buf[32];
     WebPBitstreamFeatures features;

     DIRECT_ALLOCATE_INTERFACE_DATA(thiz, IDirectFBImageProvider_WebP)

     D_DEBUG_AT( ImageProvider_WebP, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->buffer = buffer;

     /* Increase the data buffer reference counter. */
     buffer->AddRef( buffer );

     data->idirectfb = idirectfb;

     ret = data->buffer->WaitForData( data->buffer, sizeof(buf) );
     if (ret == DFB_OK)
          ret = data->buffer->PeekData( data->buffer, sizeof(buf), 0, buf, &read );

     if (ret)
          goto error;

     if (WebPGetFeatures( buf, sizeof(buf), &features ) != VP8_STATUS_OK) {
          ret = DFB_FAILURE;
          goto error;
     }

     ret = data->buffer->PeekData( data->buffer, 4, 4, &data->image_size, &read );
     if (ret)
          goto error;

     data->image_size += 8;

     /* Allocate image data. */
     data->image = D_MALLOC( data->image_size );
     if (!data->image) {
          ret = D_OOM();
          goto error;
     }

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT | DSDESC_CAPS;
     data->desc.width       = features.width;
     data->desc.height      = features.height;
     data->desc.pixelformat = features.has_alpha ? DSPF_ARGB : DSPF_RGB24;
     data->desc.caps        = DFB_PIXELFORMAT_HAS_ALPHA( data->desc.pixelformat ) ? DSCAPS_PREMULTIPLIED : DSCAPS_NONE;

     thiz->AddRef                = IDirectFBImageProvider_WebP_AddRef;
     thiz->Release               = IDirectFBImageProvider_WebP_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_WebP_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_WebP_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_WebP_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_WebP_SetRenderCallback;

     return DFB_OK;

error:
     buffer->Release( buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
