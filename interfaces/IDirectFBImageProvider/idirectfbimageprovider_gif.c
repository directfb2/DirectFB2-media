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
#include <direct/memcpy.h>
#include <display/idirectfbsurface.h>
#include <media/idirectfbimageprovider.h>
#include <misc/gfx_util.h>

D_DEBUG_DOMAIN( ImageProvider_GIF, "ImageProvider/GIF", "GIF Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, GIF )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     u32                    color_key;
     bool                   color_keyed;
     u32                   *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_GIF_data;

/* Alloc GIF image. */
static u32 *gif_image_alloc ( IDirectFBDataBuffer *buffer,
                              int                 *ret_width,
                              int                 *ret_height,
                              u32                 *ret_color_key,
                              int                 *ret_transparent,
                              u8                   cmap[3][256] );

/* Decode GIF image. */
static void gif_image_decode( IDirectFBDataBuffer *buffer,
                              u32                 *image,
                              int                  width,
                              int                  height,
                              u32                  color_key,
                              int                  transparent,
                              u8                   cmap[3][256] );

/* Free GIF image. */
static void gif_image_free  ( u32 *image );

/**********************************************************************************************************************/

static DFBResult
FetchData( IDirectFBDataBuffer *buffer,
            void               *buf,
            unsigned int        len )
{
     DFBResult ret;

     ret = buffer->WaitForData( buffer, len );
     if (ret == DFB_OK)
          ret = buffer->GetData( buffer, len, buf, NULL );

     if (ret)
          return ret;

     return DFB_OK;
}

static int GetDataBlock( IDirectFBDataBuffer *buffer,
                         u8                  *buf )
{
     DFBResult     ret;
     unsigned char count;

     ret = FetchData( buffer, &count, 1 );
     if (ret) {
          D_ERROR( "ImageProvider/GIF: Failed to read Data Block Size!\n" );
          return -1;
     }

     if (count) {
          ret = FetchData( buffer, buf, count );
          if (ret) {
               D_ERROR( "ImageProvider/GIF: Failed to read Data Block Values!\n" );
               return -1;
          }
     }

     return count;
}

/**********************************************************************************************************************/

#define MAX_LZW_BITS 12

typedef struct {
    int  min_code_size, code_size;
    u8   buf[257];
    int  curbit, lastbit, lastbyte;
    int  clear_code, first_code, old_code;
    int  max_code, max_code_size;
    int  table[2][1<<MAX_LZW_BITS];
    int  stack[2*(1<<MAX_LZW_BITS)];
    int *sp;
} LZWContext;

static int
GetCode( IDirectFBDataBuffer *buffer,
         LZWContext          *ctx )
{
     int i, j;
     int code = 0;

     if (ctx->curbit + ctx->code_size >= ctx->lastbit) {
          int count;

          ctx->buf[0] = ctx->buf[ctx->lastbyte-2];
          ctx->buf[1] = ctx->buf[ctx->lastbyte-1];

          count = GetDataBlock( buffer, &ctx->buf[2] );
          if (count < 0)
               return -1;

          ctx->curbit   = ctx->curbit - ctx->lastbit + 16;
          ctx->lastbit  = 8 * (2 + count);
          ctx->lastbyte = 2 + count;
     }

     for (i = ctx->curbit, j = 0; j < ctx->code_size; ++i, ++j)
          code |= ((ctx->buf[i / 8] & (1 << (i % 8))) != 0) << j;

     ctx->curbit += ctx->code_size;

     return code;
}

static int
LZWDecode( IDirectFBDataBuffer *buffer,
           LZWContext          *ctx )
{
     int c, code;

     if (ctx->sp > ctx->stack) {
          code = *--ctx->sp;
          return code;
     }

     while ((code = GetCode( buffer, ctx )) >= 0) {
          if (code == ctx->clear_code) {
               ctx->code_size = ctx->min_code_size + 1;
               ctx->max_code = ctx->clear_code + 2;
               ctx->max_code_size = 2 * ctx->clear_code;
               for (c = 0; c < ctx->clear_code; ++c) {
                    ctx->table[0][c] = 0;
                    ctx->table[1][c] = c;
               }
               for (; c < 1 << MAX_LZW_BITS; ++c) {
                    ctx->table[0][c] = ctx->table[1][c] = 0;
               }
               ctx->sp = ctx->stack;

               code = ctx->first_code = ctx->old_code = GetCode( buffer, ctx );
               break;
          }
          else
               c = code;

          if (code >= ctx->max_code) {
               *ctx->sp++ = ctx->first_code;
               code       = ctx->old_code;
          }

          while (code >= ctx->clear_code) {
               *ctx->sp++ = ctx->table[1][code];
               code       = ctx->table[0][code];
          }

          *ctx->sp++ = ctx->first_code = ctx->table[1][code];
          code       = ctx->max_code;

          if (code < 1 << MAX_LZW_BITS) {
               ctx->table[0][code] = ctx->old_code;
               ctx->table[1][code] = ctx->first_code;

               ++ctx->max_code;

               if ((ctx->max_code >= ctx->max_code_size) && (ctx->max_code_size < 1 << MAX_LZW_BITS)) {
                    ctx->max_code_size *= 2;
                    ++ctx->code_size;
               }
          }

          ctx->old_code = c;

          if (ctx->sp > ctx->stack) {
               code = *--ctx->sp;
               break;
          }
     }

     return code;
}

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_GIF_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_GIF_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_GIF, "%s( %p )\n", __FUNCTION__, thiz );

     gif_image_free( data->image );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_GIF_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GIF )

     D_DEBUG_AT( ImageProvider_GIF, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_GIF_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GIF )

     D_DEBUG_AT( ImageProvider_GIF, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_GIF_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_GIF_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                  DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GIF )

     D_DEBUG_AT( ImageProvider_GIF, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_GIF_GetImageDescription( IDirectFBImageProvider *thiz,
                                                DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GIF )

     D_DEBUG_AT( ImageProvider_GIF, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     if (data->color_keyed) {
          ret_desc->caps       |= DICAPS_COLORKEY;
          ret_desc->colorkey_r  = (data->color_key & 0xff0000) >> 16;
          ret_desc->colorkey_g  = (data->color_key & 0x00ff00) >>  8;
          ret_desc->colorkey_b  =  data->color_key & 0x0000ff;
     }

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_GIF_RenderTo( IDirectFBImageProvider *thiz,
                                     IDirectFBSurface       *destination,
                                     const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRegion              clip;
     CoreSurfaceBufferLock  lock;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GIF )

     D_DEBUG_AT( ImageProvider_GIF, "%s( %p )\n", __FUNCTION__, thiz );

     dst_data = destination->priv;
     if (!dst_data)
          return DFB_DEAD;

     if (!dst_data->surface)
          return DFB_DESTROYED;

     dfb_region_from_rectangle( &clip, &dst_data->area.current );

     if (dest_rect) {
          if (dest_rect->w < 1 || dest_rect->h < 1)
               return DFB_INVARG;

          rect = *dest_rect;
          rect.x += dst_data->area.wanted.x;
          rect.y += dst_data->area.wanted.y;
     }
     else
          rect = dst_data->area.wanted;

     if (!dfb_rectangle_region_intersects( &rect, &clip ))
          return DFB_OK;

     ret = dfb_surface_lock_buffer( dst_data->surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
     if (ret)
          return ret;

     dfb_scale_linear_32( data->image, data->desc.width, data->desc.height,
                          lock.addr, lock.pitch, &rect, dst_data->surface, &clip );

     dfb_surface_unlock_buffer( dst_data->surface, &lock );

     if (data->render_callback) {
          DFBRectangle r = { 0, 0, data->desc.width, data->desc.height };

          data->render_callback( &r, data->render_callback_context );
     }

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_GIF_SetRenderCallback( IDirectFBImageProvider *thiz,
                                              DIRenderCallback        callback,
                                              void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GIF )

     D_DEBUG_AT( ImageProvider_GIF, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     /* Check the magic. */
     if (!strncmp( (const char*) ctx->header, "GIF87a", 6 ) ||
         !strncmp( (const char*) ctx->header, "GIF89a", 6 ))
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
     int       width, height;
     u32       color_key;
     int       transparent;
     u8        cmap[3][256];

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_GIF )

     D_DEBUG_AT( ImageProvider_GIF, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref = 1;

     data->image = gif_image_alloc( buffer, &width, &height, &color_key, &transparent, cmap );
     if (!data->image) {
          ret = DFB_FAILURE;
          goto error;
     }

     gif_image_decode( buffer, data->image, width, height, color_key, transparent, cmap );

     data->color_key   = color_key;
     data->color_keyed = (transparent != -1);

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = width;
     data->desc.height      = height;
     data->desc.pixelformat = dfb_primary_layer_pixelformat();

     thiz->AddRef                = IDirectFBImageProvider_GIF_AddRef;
     thiz->Release               = IDirectFBImageProvider_GIF_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_GIF_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_GIF_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_GIF_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_GIF_SetRenderCallback;

     return DFB_OK;

error:
     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}

/**********************************************************************************************************************/

static int SortColors( const void *a, const void *b )
{
     return *((const u8 *) a) - *((const u8 *) b);
}

/*
 * Look for a color that is not in the colormap and ideally not even close to the colors used in the colormap.
 */
static u32
FindColorKey( int num_colors,
              u8  cmap[3][256] )
{
     int i, j, index, d;
     u8  csort[num_colors];
     u32 color = 0xff000000;

     if (num_colors < 1)
          return color;

     for (i = 0; i < 3; i++) {
          direct_memcpy( csort, cmap[i], num_colors );
          qsort( csort, num_colors, 1, SortColors );

          for (j = 1, index = 0, d = 0; j < num_colors; j++) {
               if (csort[j] - csort[j-1] > d) {
                    d = csort[j] - csort[j-1];
                    index = j;
               }
          }

          if (csort[0] > d) {
               d = csort[0];
               index = num_colors;
          }

          if (0xff - csort[num_colors-1] > d) {
               index = num_colors + 1;
          }

          if (index < num_colors)
               csort[0] = csort[index] - d / 2;
          else if (index == num_colors)
               csort[0] = 0;
          else
               csort[0] = 0xff;

          color |= csort[0] << (8 * (2 - i));
     }

     return color;
}

static u32 *
gif_image_alloc( IDirectFBDataBuffer *buffer,
                 int                 *ret_width,
                 int                 *ret_height,
                 u32                 *ret_color_key,
                 int                 *ret_transparent,
                 u8                   cmap[3][256] )
{
     DFBResult  ret;
     u8         buf[255];
     int        i, num_colors;
     u16        width, height;
     u32        color_key;
     int        transparent = -1;
     u32       *image       = NULL;

     /* Header */
     ret = FetchData( buffer, buf, 6 );
     if (ret) {
          D_ERROR( "ImageProvider/GIF: Failed to read Signature and Version fields!\n" );
          goto out;
     }

     /* Logical Screen Descriptor */
     ret = FetchData( buffer, buf, 7 );
     if (ret) {
          D_ERROR( "ImageProvider/GIF: Failed to read Logical Screen Descriptor!\n" );
          goto out;
     }

     /* Global Color Table Flag */
     if (buf[4] && 0x80) {
          num_colors = 2 << (buf[4] & 0x07);

          for (i = 0; i < num_colors; ++i) {
               ret = FetchData( buffer, buf, 3 );
               if (ret) {
                    D_ERROR( "ImageProvider/GIF: Failed to read Global Color Table!\n" );
                    goto out;
               }

               cmap[0][i] = buf[0];
               cmap[1][i] = buf[1];
               cmap[2][i] = buf[2];
          }
     }

     /* Loop through segments. */
     while (!image) {
          /* Segment ID */
          ret = FetchData( buffer, buf, 1 );
          if (ret) {
               D_ERROR( "ImageProvider/GIF: Failed to read Segment ID!\n" );
               goto out;
          }

          /* Check for Extension Block Segment */
          if (buf[0] == '!') { /* Extension Introducer: 0x21 */
               /* Label */
               ret = FetchData( buffer, buf, 1 );
               if (ret) {
                    D_ERROR( "ImageProvider/GIF: Failed to read Label!\n" );
                    goto out;
               }

               switch (buf[0]) {
                    case 0xF9: /* Graphic Control Label */
                         if (GetDataBlock( buffer, buf ) < 0)
                              goto out;
                         if ((buf[0] & 0x1) != 0)
                              transparent = buf[3];
                         break;
                    default:
                         break;
               }

               while ((i = GetDataBlock( buffer, buf )) != 0) {
                    if (i < 0)
                         goto out;
               }

               continue;
          }

          /* Check for Image Segment */
          if (buf[0] != ',') { /* Image Separator: 0x2C */
               D_ERROR( "ImageProvider/GIF: Invalid Image Separator %c!\n", buf[0] );
               goto out;
          }

          /* Image Descriptor */
          ret = FetchData( buffer, buf, 9 );
          if (ret) {
               D_ERROR( "ImageProvider/GIF: Failed to read Image Descriptor!\n" );
               goto out;
          }

          width  = (buf[5] << 8) | buf[4];
          height = (buf[7] << 8) | buf[6];

          /* Local Color Table Flag */
          if (!(buf[8] && 0x80)) {
               /* Global Color Table */
               color_key = (transparent != -1) ? FindColorKey( num_colors, cmap ) : 0;
          }
          else {
               /* Local Color Table */
               num_colors = 2 << (buf[8] & 0x07);

               for (i = 0; i < num_colors; ++i) {
                    ret = FetchData( buffer, buf, 3 );
                    if (ret) {
                         D_ERROR( "ImageProvider/GIF: Failed to read Local Color Table!\n" );
                         goto out;
                    }

                    cmap[0][i] = buf[0];
                    cmap[1][i] = buf[1];
                    cmap[2][i] = buf[2];
               }

               color_key = (transparent != -1) ? FindColorKey( num_colors, cmap ) : 0;
          }

          /* Interlace Flag */
          if (buf[8] && 0x40) {
               D_ERROR( "ImageProvider/GIF: Unsupported Interlace Flag!\n" );
               goto out;
          }

          image = D_MALLOC( width * height * 4 );
          if (!image) {
               D_OOM();
               goto out;
          }

          *ret_width       = width;
          *ret_height      = height;
          *ret_color_key   = color_key;
          *ret_transparent = transparent;
     }

out:
     return image;
}

void
gif_image_decode( IDirectFBDataBuffer *buffer,
                  u32                 *image,
                  int                  width,
                  int                  height,
                  u32                  color_key,
                  int                  transparent,
                  u8                   cmap[3][256] )
{
     DFBResult  ret;
     int        code;
     LZWContext ctx;
     int        xpos = 0;
     int        ypos = 0;

     memset( &ctx, 0, sizeof(LZWContext) );

     ret = FetchData( buffer, &ctx.min_code_size, 1 );
     if (ret) {
          D_ERROR( "ImageProvider/GIF: Failed to read LZW minimum code size!\n" );
          return;
     }
     else {
          ctx.code_size  = ctx.min_code_size + 1;
          ctx.clear_code = 1 << ctx.min_code_size;
          ctx.lastbyte   = 2;
     }

     while ((code = LZWDecode( buffer, &ctx )) >= 0 ) {
          u32 *dst = image + ypos * width + xpos;

          if (code == transparent)
               *dst++ = color_key;
          else
               *dst++ = 0xff000000 | cmap[0][code] << 16 | cmap[1][code] << 8 | cmap[2][code];

          ++xpos;

          if (xpos == width) {
               xpos = 0;
               ++ypos;
          }

          if (ypos >= height)
               break;
     }
}

void
gif_image_free( u32 *image )
{
     D_FREE( image );
}
