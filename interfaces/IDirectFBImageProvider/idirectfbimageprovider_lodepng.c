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

#include <direct/filesystem.h>
#include <display/idirectfbsurface.h>
#define  LODEPNG_NO_COMPILE_ENCODER
#include LODEPNG_C
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbimageprovider.h>

D_DEBUG_DOMAIN( ImageProvider_LodePNG, "ImageProvider/LodePNG", "LodePNG Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, LodePNG )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFB             *idirectfb;

     unsigned char         *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_LodePNG_data;

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_LodePNG_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_LodePNG_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_LodePNG, "%s( %p )\n", __FUNCTION__, thiz );

     free( data->image );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_LodePNG_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_LodePNG )

     D_DEBUG_AT( ImageProvider_LodePNG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_LodePNG_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_LodePNG )

     D_DEBUG_AT( ImageProvider_LodePNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_LodePNG_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_LodePNG_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                      DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_LodePNG )

     D_DEBUG_AT( ImageProvider_LodePNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_LodePNG_GetImageDescription( IDirectFBImageProvider *thiz,
                                                    DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_LodePNG )

     D_DEBUG_AT( ImageProvider_LodePNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_LodePNG_RenderTo( IDirectFBImageProvider *thiz,
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

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_LodePNG )

     D_DEBUG_AT( ImageProvider_LodePNG, "%s( %p )\n", __FUNCTION__, thiz );

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
IDirectFBImageProvider_LodePNG_SetRenderCallback( IDirectFBImageProvider *thiz,
                                                  DIRenderCallback        callback,
                                                  void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_LodePNG )

     D_DEBUG_AT( ImageProvider_LodePNG, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     unsigned int error;
     unsigned int width, height;
     LodePNGState state;

     /* Check for valid filename. */
     if (!ctx->filename)
          return DFB_UNSUPPORTED;

     lodepng_state_init( &state );
     state.decoder.ignore_crc = 1;

     /* Check the signature. */
     error = lodepng_inspect( &width, &height, &state, ctx->header, 33 );

     lodepng_state_cleanup( &state );

     return error ? DFB_UNSUPPORTED : DFB_OK;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     DirectFile                fd;
     DirectFileInfo            info;
     void                     *ptr;
     unsigned int              error;
     unsigned int              width, height;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_LodePNG )

     D_DEBUG_AT( ImageProvider_LodePNG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     /* Open the file. */
     ret = direct_file_open( &fd, buffer_data->filename, O_RDONLY, 0 );
     if (ret) {
          D_DERROR( ret, "ImageProvider/LodePNG: Failed to open file '%s'!\n", buffer_data->filename );
          DIRECT_DEALLOCATE_INTERFACE( thiz );
          return ret;
     }

     /* Query file size. */
     ret = direct_file_get_info( &fd, &info );
     if (ret) {
          D_DERROR( ret, "ImageProvider/LodePNG: Failed during get_info() of '%s'!\n", buffer_data->filename );
          goto error;
     }

     /* Memory-mapped file. */
     ret = direct_file_map( &fd, NULL, 0, info.size, DFP_READ, &ptr );
     if (ret) {
          D_DERROR( ret, "ImageProvider/LodePNG: Failed during mmap() of '%s'!\n", buffer_data->filename );
          goto error;
     }

     error = lodepng_decode32( &data->image, &width, &height, ptr, info.size );

     direct_file_unmap( ptr, info.size );
     direct_file_close( &fd );

     if (error) {
          D_ERROR( "ImageProvider/LodePNG: Error during decoding: %s!\n", lodepng_error_text( error ) );
          ret = DFB_FAILURE;
          goto error;
     }

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = width;
     data->desc.height      = height;
     data->desc.pixelformat = DSPF_ABGR;

     thiz->AddRef                = IDirectFBImageProvider_LodePNG_AddRef;
     thiz->Release               = IDirectFBImageProvider_LodePNG_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_LodePNG_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_LodePNG_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_LodePNG_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_LodePNG_SetRenderCallback;

     return DFB_OK;

error:
     direct_file_close( &fd );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
