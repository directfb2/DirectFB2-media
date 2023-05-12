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
#include <display/idirectfbsurface.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbimageprovider.h>
#include <misc/gfx_util.h>
#define X_DISPLAY_MISSING
#include <Imlib2.h>

D_DEBUG_DOMAIN( ImageProvider_Imlib2, "ImageProvider/Imlib2", "Imlib2 Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, Imlib2 )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     Imlib_Image            image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_Imlib2_data;

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_Imlib2_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_Imlib2_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_Imlib2, "%s( %p )\n", __FUNCTION__, thiz );

     imlib_context_set_image( data->image );

     imlib_free_image_and_decache();

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_Imlib2_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_Imlib2 )

     D_DEBUG_AT( ImageProvider_Imlib2, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_Imlib2_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_Imlib2 )

     D_DEBUG_AT( ImageProvider_Imlib2, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_Imlib2_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_Imlib2_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                     DFBSurfaceDescription  *ret_desc)
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_Imlib2 )

     D_DEBUG_AT( ImageProvider_Imlib2, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_Imlib2_GetImageDescription( IDirectFBImageProvider *thiz,
                                                   DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_Imlib2 )

     D_DEBUG_AT( ImageProvider_Imlib2, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     imlib_context_set_image( data->image );

     ret_desc->caps = imlib_image_has_alpha() ? DICAPS_ALPHACHANNEL : DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_Imlib2_RenderTo( IDirectFBImageProvider *thiz,
                                        IDirectFBSurface       *destination,
                                        const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRegion              clip;
     CoreSurfaceBufferLock  lock;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_Imlib2 )

     D_DEBUG_AT( ImageProvider_Imlib2, "%s( %p )\n", __FUNCTION__, thiz );

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

     imlib_context_set_image( data->image );

     ret = dfb_surface_lock_buffer( dst_data->surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
     if (ret)
          return ret;

     dfb_scale_linear_32( imlib_image_get_data_for_reading_only(), data->desc.width, data->desc.height,
                          lock.addr, lock.pitch, &rect, dst_data->surface, &clip );

     dfb_surface_unlock_buffer( dst_data->surface, &lock );

     if (data->render_callback) {
          DFBRectangle r = { 0, 0, data->desc.width, data->desc.height };

          data->render_callback( &r, data->render_callback_context );
     }

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_Imlib2_SetRenderCallback( IDirectFBImageProvider *thiz,
                                                 DIRenderCallback        callback,
                                                 void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_Imlib2 )

     D_DEBUG_AT( ImageProvider_Imlib2, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static int
progress( Imlib_Image image,
          char        percent,
          int         update_x,
          int         update_y,
          int         update_w,
          int         update_h )
{
     return 1;
}

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     Imlib_Image image;

     /* Check for valid filename. */
     if (!ctx->filename)
          return DFB_UNSUPPORTED;

     imlib_context_set_progress_function( progress );

     image = imlib_load_image( ctx->filename );
     if (image)
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
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_Imlib2 )

     D_DEBUG_AT( ImageProvider_Imlib2, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref = 1;

     /* The image is already loaded and is in cache. */
     data->image = imlib_load_image( buffer_data->filename );
     if (!data->image) {
          ret = DFB_FAILURE;
          goto error;
     }

     imlib_context_set_image( data->image );

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = imlib_image_get_width();
     data->desc.height      = imlib_image_get_height();
     data->desc.pixelformat = imlib_image_has_alpha() ? DSPF_ARGB : dfb_primary_layer_pixelformat();

     thiz->AddRef                = IDirectFBImageProvider_Imlib2_AddRef;
     thiz->Release               = IDirectFBImageProvider_Imlib2_Release;
     thiz->SetRenderCallback     = IDirectFBImageProvider_Imlib2_SetRenderCallback;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_Imlib2_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_Imlib2_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_Imlib2_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_Imlib2_SetRenderCallback;

     return DFB_OK;

error:
     imlib_free_image_and_decache();

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
