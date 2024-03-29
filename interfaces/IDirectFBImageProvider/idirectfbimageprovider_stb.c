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
#define  STB_IMAGE_IMPLEMENTATION
#define  STBI_NO_HDR
#define  STBI_NO_LINEAR
#include <stb_image.h>

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

     IDirectFB             *idirectfb;

     stbi_uc               *image;

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
     unsigned int         len    = 0;
     IDirectFBDataBuffer *buffer = user;

     buffer->GetData( buffer, size, buf, &len );

     return len;
}

static void
skipSTB( void *user,
         int   n )
{
     unsigned int         offset;
     IDirectFBDataBuffer *buffer = user;

     buffer->GetPosition( buffer, &offset );
     buffer->SeekTo( buffer, offset + n );
}

static int
eofSTB( void *user )
{
     IDirectFBDataBuffer *buffer = user;

     return buffer->HasData( buffer ) ? 1 : 0;
}

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_STB_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_STB_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_STB, "%s( %p )\n", __FUNCTION__, thiz );

     stbi_image_free( data->image );

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
     DFBSurfaceDescription  desc;
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

     if (direct_getenv( "D_STREAM_BYPASS" ) && ctx->filename) {
          if (strrchr( ctx->filename, '.' ) &&
              (strcasecmp( strrchr( ctx->filename, '.' ), ".bmp"  ) == 0 ||
               strcasecmp( strrchr( ctx->filename, '.' ), ".gif"  ) == 0 ||
               strcasecmp( strrchr( ctx->filename, '.' ), ".jpg"  ) == 0 ||
               strcasecmp( strrchr( ctx->filename, '.' ), ".jpeg" ) == 0 ||
               strcasecmp( strrchr( ctx->filename, '.' ), ".png"  ) == 0 ||
               strcasecmp( strrchr( ctx->filename, '.' ), ".tga"  ) == 0))
               return DFB_OK;
          else
               return DFB_UNSUPPORTED;
     }

     stbi__start_mem( &s, ctx->header, sizeof(ctx->header) );

     if (stbi__bmp_test( &s ))
          return DFB_OK;

     if (stbi__gif_test( &s ))
          return DFB_OK;

     if (stbi__jpeg_test( &s ))
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
     DFBResult                 ret;
     stbi_io_callbacks         callbacks;
     int                       width, height;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_STB )

     D_DEBUG_AT( ImageProvider_STB, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     callbacks.read = readSTB;
     callbacks.skip = skipSTB;
     callbacks.eof  = eofSTB;

     if (!(direct_getenv( "D_STREAM_BYPASS" ) && buffer_data->filename))
          data->image = stbi_load_from_callbacks( &callbacks, buffer, &width, &height, NULL, 4 );
     else
          data->image = stbi_load( buffer_data->filename, &width, &height, NULL, 4 );

     if (!data->image) {
          ret = DFB_FAILURE;
          goto error;
     }

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
     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
