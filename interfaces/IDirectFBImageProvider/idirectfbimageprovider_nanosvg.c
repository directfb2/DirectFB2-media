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

#include <direct/memcpy.h>
#include <display/idirectfbsurface.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbimageprovider.h>
#define  NANOSVG_IMPLEMENTATION
#define  NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>

D_DEBUG_DOMAIN( ImageProvider_NanoSVG, "ImageProvider/NanoSVG", "NanoSVG Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, NanoSVG )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFB             *idirectfb;

     unsigned char         *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_NanoSVG_data;

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_NanoSVG_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_NanoSVG_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_NanoSVG, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     D_FREE( data->image );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_NanoSVG_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_NanoSVG )

     D_DEBUG_AT( ImageProvider_NanoSVG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_NanoSVG_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_NanoSVG )

     D_DEBUG_AT( ImageProvider_NanoSVG, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_NanoSVG_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_NanoSVG_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                      DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_NanoSVG )

     D_DEBUG_AT( ImageProvider_NanoSVG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_NanoSVG_GetImageDescription( IDirectFBImageProvider *thiz,
                                                    DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_NanoSVG )

     D_DEBUG_AT( ImageProvider_NanoSVG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_NanoSVG_RenderTo( IDirectFBImageProvider *thiz,
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

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_NanoSVG )

     D_DEBUG_AT( ImageProvider_NanoSVG, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBImageProvider_NanoSVG_SetRenderCallback( IDirectFBImageProvider *thiz,
                                                  DIRenderCallback        callback,
                                                  void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_NanoSVG )

     D_DEBUG_AT( ImageProvider_NanoSVG, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     int i;

     /* Look for the magic. */
     for (i = 0; i < sizeof(ctx->header) - 5; i++) {
          if (!memcmp( &ctx->header[i], "<?xml", 5 ))
               return DFB_OK;
     }

     /* Else look for the file extension. */
     if (ctx->filename && strrchr( ctx->filename, '.' )) {
          if (!strcasecmp( strrchr( ctx->filename, '.' ), ".svg" ))
               return DFB_OK;
     }

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     int                       width, height;
     void                     *chunk       = NULL;
     NSVGimage                *im          = NULL;
     NSVGrasterizer           *rast        = NULL;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_NanoSVG )

     D_DEBUG_AT( ImageProvider_NanoSVG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     if (buffer_data->buffer) {
          chunk = D_MALLOC( buffer_data->length );
          direct_memcpy( chunk, buffer_data->buffer, buffer_data->length );
          im = nsvgParse( chunk, "px", 96 );
     }
     else if (buffer_data->filename) {
          im = nsvgParseFromFile( buffer_data->filename, "px", 96 );
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

          im = nsvgParse( chunk, "px", 96 );
     }

     if (!im) {
          D_ERROR( "ImageProvider/NanoSVG: Failed to parse SVG!\n" );
          ret = DFB_UNSUPPORTED;
          goto error;
     }

     if (chunk)
          D_FREE( chunk );

     width  = im->width;
     height = im->height;

     /* Allocate image data. */
     data->image = D_CALLOC( height, width * 4 );
     if (!data->image) {
          ret = D_OOM();
          goto error;
     }

     rast = nsvgCreateRasterizer();
     if (!rast) {
          D_ERROR( "ImageProvider/NanoSVG: Failed to create rasterizer!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     nsvgRasterize( rast, im, 0, 0, 1, data->image, width, height, width * 4 );

     nsvgDeleteRasterizer( rast );

     nsvgDelete( im );

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = width;
     data->desc.height      = height;
     data->desc.pixelformat = DSPF_ABGR;

     thiz->AddRef                = IDirectFBImageProvider_NanoSVG_AddRef;
     thiz->Release               = IDirectFBImageProvider_NanoSVG_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_NanoSVG_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_NanoSVG_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_NanoSVG_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_NanoSVG_SetRenderCallback;

     return DFB_OK;

error:
     if (data->image)
          D_FREE( data->image );

     if (im)
          nsvgDelete( im );

     if (chunk)
          D_FREE( chunk );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
