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
#include <media/idirectfbimageprovider.h>
#define  STB_IMAGE_IMPLEMENTATION
#define  STBI_NO_HDR
#define  STBI_NO_LINEAR
#include STB_IMAGE_H

D_DEBUG_DOMAIN( ImageProvider_STB, "ImageProvider/STB", "STB Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, STB )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFBDataBuffer   *buffer;
     IDirectFB             *idirectfb;

     void                  *ptr;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_STB_data;

/**********************************************************************************************************************/

static int
readSTB( void *user,
         char *buf,
         int   size )
{
     unsigned int                     len;
     IDirectFBImageProvider_STB_data *data = user;

     data->buffer->GetData( data->buffer, size, buf, &len );

     return len;
}

static void
skipSTB( void *user,
         int   n )
{
     unsigned int                     offset;
     IDirectFBImageProvider_STB_data *data = user;

     data->buffer->GetPosition( data->buffer, &offset );
     data->buffer->SeekTo( data->buffer, offset + n );
}

static int
eofSTB( void *user )
{
     IDirectFBImageProvider_STB_data *data = user;

     return data->buffer->HasData( data->buffer ) ? 1 : 0;
}

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_STB_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_STB_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_STB, "%s( %p )\n", __FUNCTION__, thiz );

     stbi_image_free( data->ptr );

     /* Decrease the data buffer reference counter. */
     if (data->buffer)
          data->buffer->Release( data->buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_STB_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_STB )

     D_DEBUG_AT( ImageProvider_STB, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_STB_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_STB )

     D_DEBUG_AT( ImageProvider_STB, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_STB_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_STB_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                  DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_STB )

     D_DEBUG_AT( ImageProvider_STB, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_STB_GetImageDescription( IDirectFBImageProvider *thiz,
                                                DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_STB )

     D_DEBUG_AT( ImageProvider_STB, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_STB_RenderTo( IDirectFBImageProvider *thiz,
                                     IDirectFBSurface       *destination,
                                     const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRegion              clip;
     DFBRegion              old_clip;
     int                    pitch;
     void                  *ptr;
     IDirectFBSurface      *source;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_STB )

     D_DEBUG_AT( ImageProvider_STB, "%s( %p )\n", __FUNCTION__, thiz );

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

     direct_memcpy( ptr, data->ptr, data->desc.width * data->desc.height * 4 );

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
IDirectFBImageProvider_STB_SetRenderCallback( IDirectFBImageProvider *thiz,
                                              DIRenderCallback        callback,
                                              void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_STB )

     D_DEBUG_AT( ImageProvider_STB, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     stbi__context s;

     stbi__start_mem( &s, ctx->header, D_ARRAY_SIZE(ctx->header) );

     if (stbi__bmp_test( &s ))
          return DFB_OK;

     if (stbi__jpeg_test( &s ))
          return DFB_OK;

     if (stbi__gif_test( &s ))
          return DFB_OK;

     if (stbi__png_test( &s ))
          return DFB_OK;

     if (stbi__tga_test( &s ))
          return DFB_OK;

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult         ret = DFB_FAILURE;
     stbi_io_callbacks callbacks;
     int               width, height;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_STB )

     D_DEBUG_AT( ImageProvider_STB, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->buffer = buffer;

     /* Increase the data buffer reference counter. */
     buffer->AddRef( buffer );

     data->idirectfb = idirectfb;

     callbacks.read = readSTB;
     callbacks.skip = skipSTB;
     callbacks.eof  = eofSTB;

     data->ptr = stbi_load_from_callbacks( &callbacks, data, &width, &height, NULL, 4 );
     if (!data->ptr)
          goto error;

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = width;
     data->desc.height      = height;
     data->desc.pixelformat = DSPF_ABGR;

     thiz->AddRef                = IDirectFBImageProvider_STB_AddRef;
     thiz->Release               = IDirectFBImageProvider_STB_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_STB_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_STB_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_STB_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_STB_SetRenderCallback;

     return DFB_OK;

error:
     buffer->Release( buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
