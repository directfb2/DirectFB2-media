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

#include <config.h>
#include <display/idirectfbsurface.h>
#include <media/idirectfbimageprovider.h>
#include <misc/gfx_util.h>

D_DEBUG_DOMAIN( ImageProvider_BMP, "ImageProvider/BMP", "BMP Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, BMP )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFBDataBuffer   *buffer;

     int                    depth;
     unsigned int           img_offset;
     unsigned int           num_colors;
     DFBColor               colors[256];
     u32                   *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_BMP_data;

/**********************************************************************************************************************/

static DFBResult
fetch_data( IDirectFBDataBuffer *buffer,
            void                *buf,
            int                  size )
{
     DFBResult ret;

     while (size > 0) {
          unsigned int len;

          ret = buffer->WaitForData( buffer, size );
          if (ret == DFB_OK)
               ret = buffer->GetData( buffer, size, buf, &len );

          if (ret)
               return ret;

          buf += len;
          size -= len;
     }

     return DFB_OK;
}

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_BMP_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_BMP_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_BMP, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     if (data->image)
          D_FREE( data->image );

     /* Decrease the data buffer reference counter. */
     if (data->buffer)
          data->buffer->Release( data->buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_BMP_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_BMP )

     D_DEBUG_AT( ImageProvider_BMP, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_BMP_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_BMP )

     D_DEBUG_AT( ImageProvider_BMP, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_BMP_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_BMP_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                  DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_BMP )

     D_DEBUG_AT( ImageProvider_BMP, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_BMP_GetImageDescription( IDirectFBImageProvider *thiz,
                                                DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_BMP )

     D_DEBUG_AT( ImageProvider_BMP, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
bmp_decode_row( IDirectFBImageProvider_BMP_data *data,
                int                              row_num )
{
     DFBResult  ret;
     int        x;
     u32       *dst;
     int        pitch = (((data->desc.width*data->depth + 7) >> 3) + 3) & ~3;
     u8         buf[pitch];

     ret = fetch_data( data->buffer, buf, pitch );
     if (ret)
          return ret;

     dst = data->image + row_num * data->desc.width;

     switch (data->depth) {
          case 1:
               for (x = 0; x < data->desc.width; x++) {
                    int      i = buf[x>>3] & (0x80 >> (x & 7));
                    DFBColor c = data->colors[i];

                    dst[x] = c.b | (c.g << 8) | (c.r << 16) | (c.a << 24);
               }
               break;

          case 4:
               for (x = 0; x < data->desc.width; x++) {
                    int      i = buf[x>>1] & (0xf0 >> ((x&1) << 2));
                    DFBColor c = data->colors[i];

                    dst[x] = c.b | (c.g << 8) | (c.r << 16) | (c.a << 24);
               }
               break;

          case 8:
               for (x = 0; x < data->desc.width; x++) {
                    DFBColor c = data->colors[buf[x]];

                    dst[x] = c.b | (c.g << 8) | (c.r << 16) | (c.a << 24);
               }
               break;

          case 16:
               for (x = 0; x < data->desc.width; x++) {
                    u32 r, g, b;
                    u16 c;

                    c = buf[x*2+0] | (buf[x*2+1] << 8);
                    r = (c >> 10) & 0x1f;
                    g = (c >>  5) & 0x1f;
                    b =  c        & 0x1f;
                    r = (r << 3) | (r >> 2);
                    g = (g << 3) | (g >> 2);
                    b = (b << 3) | (b >> 2);

                    dst[x] = b | (g << 8) | (r << 16) | 0xff000000;
               }
               break;

          case 24:
               for (x = 0; x < data->desc.width; x++) {
#ifdef WORDS_BIGENDIAN
                    dst[x] = buf[x*3+2] | (buf[x*3+1] << 8) | (buf[x*3+0] << 16) | 0xff000000;
#else
                    dst[x] = buf[x*3+0] | (buf[x*3+1] << 8) | (buf[x*3+2] << 16) | 0xff000000;
#endif
               }
               break;

          case 32:
               for (x = 0; x < data->desc.width; x++) {
#ifdef WORDS_BIGENDIAN
                    dst[x] = buf[x*4+2] | (buf[x*4+1] << 8) | (buf[x*4+0] << 16) | (buf[x*4+3] << 24);
#else
                    dst[x] = buf[x*4+1] | (buf[x*4+2] << 8) | (buf[x*4+3] << 16) | (buf[x*4+0] << 24);
#endif
               }
               break;

          default:
               break;
     }

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_BMP_RenderTo( IDirectFBImageProvider *thiz,
                                     IDirectFBSurface       *destination,
                                     const DFBRectangle     *dest_rect )
{
     DFBResult               ret;
     IDirectFBSurface_data  *dst_data;
     DFBRectangle            rect;
     DFBRegion               clip;
     CoreSurfaceBufferLock   lock;
     DIRenderCallbackResult  cb_result = DIRCR_OK;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_BMP )

     D_DEBUG_AT( ImageProvider_BMP, "%s( %p )\n", __FUNCTION__, thiz );

     if (!destination)
          return DFB_INVARG;

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

     ret = dfb_surface_lock_buffer( dst_data->surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
     if (ret)
          return ret;

     if (!data->image) {
          int  y;
          bool direct = (rect.w == data->desc.width && rect.h == data->desc.height && data->render_callback);

          if (data->desc.pixelformat == DSPF_LUT8 && dst_data->surface->config.format == DSPF_LUT8) {
               IDirectFBPalette *palette;

               ret = destination->GetPalette( destination, &palette );
               if (ret) {
                    dfb_surface_unlock_buffer( dst_data->surface, &lock );
                    return ret;
               }

               palette->SetEntries( palette, data->colors, data->num_colors, 0 );
               palette->Release( palette );
          }

          /* Allocate image data. */
          data->image = D_CALLOC( data->desc.height, data->desc.width * 4 );
          if (!data->image) {
               dfb_surface_unlock_buffer( dst_data->surface, &lock );
               return D_OOM();
          }

          data->buffer->SeekTo( data->buffer, data->img_offset );

          for (y = data->desc.height - 1; y >= 0 && cb_result == DIRCR_OK; y--) {
               ret = bmp_decode_row( data, y );
               if (ret)
                    break;

               if (direct) {
                    DFBRectangle r = { rect.x, rect.y + y, data->desc.width, 1 };

                    dfb_copy_buffer_32( data->image + y * data->desc.width,
                                        lock.addr, lock.pitch, &r, dst_data->surface, &clip );

                    if (data->render_callback) {
                         r = (DFBRectangle) { 0, y, data->desc.width, 1 };

                         cb_result = data->render_callback( &r, data->render_callback_context );
                    }
               }
          }

          if (!direct) {
               dfb_scale_linear_32( data->image, data->desc.width, data->desc.height,
                                    lock.addr, lock.pitch, &rect, dst_data->surface, &clip );

               if (data->render_callback) {
                    DFBRectangle r = { 0, 0, data->desc.width, data->desc.height };

                    cb_result = data->render_callback( &r, data->render_callback_context );
               }
          }

          if (cb_result != DIRCR_OK) {
               D_FREE( data->image );
               data->image = NULL;
               ret = DFB_INTERRUPTED;
          }
     }
     else {
          dfb_scale_linear_32( data->image, data->desc.width, data->desc.height,
                               lock.addr, lock.pitch, &rect, dst_data->surface, &clip );

          if (data->render_callback) {
               DFBRectangle r = { 0, 0, data->desc.width, data->desc.height };

               data->render_callback( &r, data->render_callback_context );
          }
     }

     dfb_surface_unlock_buffer( dst_data->surface, &lock );

     return ret;
}

static DFBResult
IDirectFBImageProvider_BMP_SetRenderCallback( IDirectFBImageProvider *thiz,
                                              DIRenderCallback        callback,
                                              void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_BMP )

     D_DEBUG_AT( ImageProvider_BMP, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     /* Check the magic. */
     if (ctx->header[0] == 'B' && ctx->header[1] == 'M')
          return DFB_OK;

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult ret;
     u32       bihsize;
     u32       tmp;
     u8        buf[54];

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_BMP )

     D_DEBUG_AT( ImageProvider_BMP, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->buffer = buffer;

     /* Increase the data buffer reference counter. */
     buffer->AddRef( buffer );

     ret = fetch_data( data->buffer, buf, sizeof(buf) );
     if (ret)
          goto error;

     data->desc.flags = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;

     /* 2 bytes: Magic */

     /* 4 bytes: FileSize */

     /* 4 bytes: Reserved */

     /* 4 bytes: DataOffset */
     data->img_offset = buf[10] | (buf[11] << 8) | (buf[12] << 16) | (buf[13] << 24);
     if (data->img_offset < 54) {
          D_ERROR( "ImageProvider/BMP: Invalid data offset %08x!\n", data->img_offset );
          ret = DFB_FAILURE;
          goto error;
     }

     /* 4 bytes: HeaderSize */
     bihsize = buf[14] | (buf[15] << 8) | (buf[16] << 16) | (buf[17] << 24);
     if (bihsize < 40) {
          D_ERROR( "ImageProvider/BMP: Invalid header size %u!\n", bihsize );
          ret = DFB_FAILURE;
          goto error;
     }

     /* 4 bytes: Width */
     data->desc.width = buf[18] | (buf[19] << 8) | (buf[20] << 16) | (buf[21] << 24);
     if (data->desc.width < 1 || data->desc.width > 0xffff) {
          D_ERROR( "ImageProvider/BMP: Invalid width %d!\n", data->desc.width );
          ret = DFB_FAILURE;
          goto error;
     }

     /* 4 bytes: Height */
     data->desc.height = buf[22] | (buf[23] << 8) | (buf[24] << 16) | (buf[25] << 24);
     if (data->desc.height < 1 || data->desc.height > 0xffff) {
          D_ERROR( "ImageProvider/BMP: Invalid height %d!\n", data->desc.height );
          ret = DFB_FAILURE;
          goto error;
     }

     /* 2 bytes: Planes */
     tmp = buf[26] | (buf[27] << 8);
     if (tmp != 1) {
          D_ERROR( "ImageProvider/BMP: Unsupported number of planes %u!\n", tmp );
          ret = DFB_UNSUPPORTED;
          goto error;
     }

     /* 2 bytes: Depth */
     data->depth = buf[28] | (buf[29] << 8);
     switch (data->depth) {
          case 1:
          case 4:
          case 8:
               data->desc.pixelformat = DSPF_LUT8;
          case 16:
          case 24:
          case 32:
               data->desc.pixelformat = DSPF_RGB32;
               break;
          default:
               D_ERROR( "ImageProvider/BMP: Unsupported depth %d!\n", data->depth );
               ret = DFB_UNSUPPORTED;
               goto error;
     }

     /* 4 bytes: Compression */
     tmp = buf[30] | (buf[31] << 8) | (buf[32] << 16) | (buf[33] << 24);
     switch (tmp) {
          case 0:
               break;
          default:
               D_ERROR( "ImageProvider/BMP: Unsupported compression %u!\n", tmp );
               ret = DFB_UNSUPPORTED;
               goto error;
     }

     /* 4 bytes: CompressedSize */

     /* 4 bytes: HorizontalResolution */

     /* 4 bytes: VerticalResolution */

     /* 4 bytes: UsedColors */
     data->num_colors = buf[46] | (buf[47] << 8) | (buf[48] << 16) | (buf[49] << 24);
     if (!data->num_colors || data->num_colors > 256)
          data->num_colors = 1 << data->depth;

     /* 4 bytes: ImportantColors */

     /* Skip remaining bytes. */
     if (bihsize > 40) {
          bihsize -= 40;
          while (bihsize--) {
               u8 b;
               ret = fetch_data( data->buffer, &b, 1 );
               if (ret)
                    goto error;
          }
     }

     /* Palette */
     if (data->desc.pixelformat == DSPF_LUT8) {
          ret = fetch_data( data->buffer, data->colors, data->num_colors * 4 );
          if (ret)
               goto error;
     }

     thiz->AddRef                = IDirectFBImageProvider_BMP_AddRef;
     thiz->Release               = IDirectFBImageProvider_BMP_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_BMP_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_BMP_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_BMP_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_BMP_SetRenderCallback;

     return DFB_OK;

error:
     buffer->Release( buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
