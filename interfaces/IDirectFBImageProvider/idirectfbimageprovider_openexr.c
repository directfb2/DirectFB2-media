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

#include <direct/system.h>
#include <display/idirectfbsurface.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbimageprovider.h>
#include <ImfCRgbaFile.h>

D_DEBUG_DOMAIN( ImageProvider_OpenEXR, "ImageProvider/OpenEXR", "OpenEXR Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, OpenEXR )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFB             *idirectfb;

     ImfRgba               *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_OpenEXR_data;

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_OpenEXR_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_OpenEXR_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_OpenEXR, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     D_FREE( data->image );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_OpenEXR_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_OpenEXR )

     D_DEBUG_AT( ImageProvider_OpenEXR, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_OpenEXR_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_OpenEXR )

     D_DEBUG_AT( ImageProvider_OpenEXR, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_OpenEXR_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_OpenEXR_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                      DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_OpenEXR )

     D_DEBUG_AT( ImageProvider_OpenEXR, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_OpenEXR_GetImageDescription( IDirectFBImageProvider *thiz,
                                                    DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_OpenEXR )

     D_DEBUG_AT( ImageProvider_OpenEXR, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_OpenEXR_RenderTo( IDirectFBImageProvider *thiz,
                                         IDirectFBSurface       *destination,
                                         const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRegion              clip;
     DFBRegion              old_clip;
     int                    i, j;
     int                    pitch;
     void                  *ptr;
     IDirectFBSurface      *source;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_OpenEXR )

     D_DEBUG_AT( ImageProvider_OpenEXR, "%s( %p )\n", __FUNCTION__, thiz );

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

     for (i = 0; i < data->desc.height; i++) {
          for (j = 0; j < data->desc.width; j++) {
               *(u8*) ptr++ = CLAMP( 255 * ImfHalfToFloat(data->image[i*data->desc.width+j].r), 0, 255 );
               *(u8*) ptr++ = CLAMP( 255 * ImfHalfToFloat(data->image[i*data->desc.width+j].g), 0, 255 );
               *(u8*) ptr++ = CLAMP( 255 * ImfHalfToFloat(data->image[i*data->desc.width+j].b), 0, 255 );
               *(u8*) ptr++ = CLAMP( 255 * ImfHalfToFloat(data->image[i*data->desc.width+j].a), 0, 255 );
          }
     }

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
IDirectFBImageProvider_OpenEXR_SetRenderCallback( IDirectFBImageProvider *thiz,
                                                  DIRenderCallback        callback,
                                                  void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_OpenEXR )

     D_DEBUG_AT( ImageProvider_OpenEXR, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     /* Check for valid filename. */
     if (!ctx->filename)
          return DFB_UNSUPPORTED;

     if (direct_getenv( "D_STREAM_BYPASS" )) {
          if (strrchr( ctx->filename, '.' ) &&
              strcasecmp( strrchr( ctx->filename, '.' ), ".exr" ) == 0)
               return DFB_OK;
          else
               return DFB_UNSUPPORTED;
     }

     /* Check the magic. */
     if (ctx->header[0] == 0x76 && ctx->header[1] == 0x2f && ctx->header[2] == 0x31 && ctx->header[3] == 0x01)
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
     int                       width, height;
     int                       xmin, ymin, xmax, ymax;
     const ImfHeader          *header;
     ImfInputFile             *file = NULL;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_OpenEXR )

     D_DEBUG_AT( ImageProvider_OpenEXR, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     file = ImfOpenInputFile( buffer_data->filename );
     if (!file) {
          DIRECT_DEALLOCATE_INTERFACE( thiz );
          return DFB_UNSUPPORTED;
     }

     header = ImfInputHeader( file );

     ImfHeaderDataWindow( header, &xmin, &ymin, &xmax, &ymax );

     width  = xmax - xmin + 1;
     height = ymax - ymin + 1;

     /* Allocate image data. */
     data->image = D_CALLOC( width * height, sizeof(ImfRgba) );
     if (!data->image) {
          ret = D_OOM();
          goto error;
     }

     ImfInputSetFrameBuffer( file, data->image, 1, width );
     ImfInputReadPixels( file, 0, height - 1 );

     ImfCloseInputFile( file );

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = width;
     data->desc.height      = height;
     data->desc.pixelformat = DSPF_ABGR;

     thiz->AddRef                = IDirectFBImageProvider_OpenEXR_AddRef;
     thiz->Release               = IDirectFBImageProvider_OpenEXR_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_OpenEXR_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_OpenEXR_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_OpenEXR_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_OpenEXR_SetRenderCallback;

     return DFB_OK;

error:
     ImfCloseInputFile( file );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
