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
#include <core/layers.h>
#include <direct/memcpy.h>
#include <display/idirectfbsurface.h>
#include <gfx/clip.h>
#include <media/idirectfbimageprovider.h>
#include <misc/gfx_util.h>
#include <png.h>

D_DEBUG_DOMAIN( ImageProvider_PNG, "ImageProvider/PNG", "PNG Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, PNG )

/**********************************************************************************************************************/

enum {
     STAGE_ABORT = -2,
     STAGE_ERROR = -1,
     STAGE_START =  0,
     STAGE_INFO,
     STAGE_IMAGE,
     STAGE_END
};

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFBDataBuffer   *buffer;

     int                    stage;
     png_structp            png_ptr;
     png_infop              info_ptr;
     int                    bpp;
     int                    color_type;
     u32                    color_key;
     bool                   color_keyed;
     int                    pitch;
     u32                    palette[256];
     DFBColor               colors[256];
     void                  *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_PNG_data;

/* Called at the start of the progressive load, once we have image info. */
static void png_info_callback( png_structp png_read_ptr, png_infop png_info_ptr );

/* Called for each row, note that we get duplicate row numbers for interlaced image. */
static void png_row_callback ( png_structp png_read_ptr, png_bytep new_row, png_uint_32 row_num, int pass_num );

/* Called after reading the entire image. */
static void png_end_callback ( png_structp png_read_ptr, png_infop png_info_ptr );

/* Pipe data into libpng until stage is different from the one specified. */
static DFBResult push_data_until_stage( IDirectFBImageProvider_PNG_data *data, int stage, int buffer_size );

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_PNG_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_PNG_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_PNG, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     if (data->image)
          D_FREE( data->image );

     /* Destroy the PNG read handle. */
     png_destroy_read_struct( &data->png_ptr, &data->info_ptr, NULL );

     /* Decrease the data buffer reference counter. */
     if (data->buffer)
          data->buffer->Release( data->buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_PNG_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_PNG )

     D_DEBUG_AT( ImageProvider_PNG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_PNG_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_PNG )

     D_DEBUG_AT( ImageProvider_PNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_PNG_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_PNG_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                  DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_PNG )

     D_DEBUG_AT( ImageProvider_PNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_PNG_GetImageDescription( IDirectFBImageProvider *thiz,
                                                DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_PNG )

     D_DEBUG_AT( ImageProvider_PNG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     if (data->color_type & PNG_COLOR_MASK_ALPHA)
          ret_desc->caps |= DICAPS_ALPHACHANNEL;

     if (data->color_keyed) {
          ret_desc->caps       |= DICAPS_COLORKEY;
          ret_desc->colorkey_r  = (data->color_key & 0xff0000) >> 16;
          ret_desc->colorkey_g  = (data->color_key & 0x00ff00) >>  8;
          ret_desc->colorkey_b  =  data->color_key & 0x0000ff;
     }

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_PNG_RenderTo( IDirectFBImageProvider *thiz,
                                     IDirectFBSurface       *destination,
                                     const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRectangle           clipped;
     DFBRegion              clip;
     CoreSurfaceBufferLock  lock;
     int                    bit_depth;
     int                    x, y;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_PNG )

     D_DEBUG_AT( ImageProvider_PNG, "%s( %p )\n", __FUNCTION__, thiz );

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

     /* setjmp() must be called in every function that calls a PNG-reading libpng function. */
     if (setjmp( png_jmpbuf( data->png_ptr ) )) {
          D_ERROR( "ImageProvider/PNG: Error during decoding!\n" );

          if (data->stage < STAGE_IMAGE)
               return DFB_FAILURE;

          /* Set error stage. */
          data->stage = STAGE_ERROR;
     }

     /* Read until image is completely decoded. */
     if (data->stage != STAGE_ERROR) {
          ret = push_data_until_stage( data, STAGE_END, 16384 );
          if (ret)
               return ret;
     }

     clipped = rect;

     dfb_region_from_rectangle( &clip, &dst_data->area.current );

     D_DEBUG_AT( ImageProvider_PNG, "  -> clip    "DFB_RECT_FORMAT"\n", DFB_RECTANGLE_VALS_FROM_REGION( &clip ) );

     if (!dfb_rectangle_intersect_by_region( &clipped, &clip ))
          return DFB_INVAREA;

     D_DEBUG_AT( ImageProvider_PNG, "  -> clipped "DFB_RECT_FORMAT"\n", DFB_RECTANGLE_VALS( &clipped ) );

     bit_depth = png_get_bit_depth( data->png_ptr, data->info_ptr );

     ret = dfb_surface_lock_buffer( dst_data->surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
     if (ret)
          return ret;

     switch (data->color_type) {
          case PNG_COLOR_TYPE_PALETTE:
               if (dst_data->surface->config.format == DSPF_LUT8 && bit_depth == 8) {
                    dfb_clip_rectangle( &clip, &rect );
                    if (rect.x == 0                                &&
                        rect.y == 0                                &&
                        rect.w == dst_data->surface->config.size.w &&
                        rect.h == dst_data->surface->config.size.h &&
                        rect.w == data->desc.width                 &&
                        rect.h == data->desc.height) {
                         for (y = 0; y < data->desc.height; y++)
                              direct_memcpy( lock.addr + lock.pitch * y, data->image + data->pitch * y,
                                             data->desc.width );

                         break;
                    }
               }
               /* fall through */

          case PNG_COLOR_TYPE_GRAY: {
               if (data->bpp == 16) {
                    dfb_scale_linear_32( data->image, data->desc.width, data->desc.height,
                                         lock.addr, lock.pitch, &rect, dst_data->surface, &clip );
                    break;
               }

               void *image_argb = D_MALLOC( data->desc.width * data->desc.height * 4 );
               if (!image_argb) {
                    dfb_surface_unlock_buffer( dst_data->surface, &lock );
                    return D_OOM();
               }

               if (data->color_type == PNG_COLOR_TYPE_GRAY) {
                    int num = 1 << bit_depth;

                    for (x = 0; x < num; x++) {
                         int value = x * 255 / (num - 1);

                         data->palette[x] = 0xff000000 | (value << 16) | (value << 8) | value;
                    }
               }

               switch (bit_depth) {
                    case 8:
                         for (y = 0; y < data->desc.height; y++) {
                              u8  *S = data->image + data->pitch * y;
                              u32 *D = image_argb + data->desc.width * y * 4;

                              for (x = 0; x < data->desc.width; x++)
                                   D[x] = data->palette[S[x]];
                         }
                         break;

                    case 4:
                         for (y = 0; y < data->desc.height; y++) {
                              u8  *S = data->image + data->pitch * y;
                              u32 *D = image_argb  + data->desc.width * y * 4;

                              for (x = 0; x < data->desc.width; x++) {
                                   if (x & 1)
                                        D[x] = data->palette[S[x>>1]&0xf];
                                   else
                                        D[x] = data->palette[S[x>>1]>>4];
                              }
                         }
                         break;

                    case 2:
                         for (y = 0; y < data->desc.height; y++) {
                              int  n = 6;
                              u8  *S = data->image + data->pitch * y;
                              u32 *D = image_argb  + data->desc.width * y * 4;

                              for (x = 0; x < data->desc.width; x++) {
                                   D[x] = data->palette[(S[x>>2]>>n)&3];
                                   n = n ? n - 2 : 6;
                              }
                         }
                         break;

                    case 1:
                         for (y = 0; y < data->desc.height; y++) {
                              int  n = 7;
                              u8  *S = data->image + data->pitch * y;
                              u32 *D = image_argb  + data->desc.width * y * 4;

                              for (x = 0; x < data->desc.width; x++) {
                                   D[x] = data->palette[(S[x>>3]>>n)&1];
                                   n = n ? n - 1 : 7;
                              }
                         }
                         break;

                    default:
                         D_ERROR( "ImageProvider/PNG: Unsupported indexed bit depth %d!\n", bit_depth );
               }

               dfb_scale_linear_32( image_argb, data->desc.width, data->desc.height,
                                    lock.addr, lock.pitch, &rect, dst_data->surface, &clip );

               D_FREE( image_argb );
               break;
          }

          default:
               dfb_scale_linear_32( data->image, data->desc.width, data->desc.height,
                                    lock.addr, lock.pitch, &rect, dst_data->surface, &clip );
               break;
     }

     dfb_surface_unlock_buffer( dst_data->surface, &lock );

     if (data->stage != STAGE_END)
          ret = DFB_INCOMPLETE;

     return ret;
}

static DFBResult
IDirectFBImageProvider_PNG_SetRenderCallback( IDirectFBImageProvider *thiz,
                                              DIRenderCallback        callback,
                                              void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_PNG )

     D_DEBUG_AT( ImageProvider_PNG, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     /* Check PNG signature. */
     if (!png_sig_cmp( ctx->header, 0, 8 ))
          return DFB_OK;

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult ret = DFB_FAILURE;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_PNG )

     D_DEBUG_AT( ImageProvider_PNG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->buffer = buffer;

     /* Increase the data buffer reference counter. */
     buffer->AddRef( buffer );

     /* Create the PNG read handle. */
     data->png_ptr = png_create_read_struct( PNG_LIBPNG_VER_STRING, NULL, NULL, NULL );
     if (!data->png_ptr)
          goto error;

     /* setjmp() must be called in every function that calls a PNG-reading libpng function. */
     if (setjmp( png_jmpbuf( data->png_ptr ) )) {
          D_ERROR( "ImageProvider/PNG: Error reading header!\n" );
          goto error;
     }

     /* Create the PNG info handle. */
     data->info_ptr = png_create_info_struct( data->png_ptr );
     if (!data->info_ptr)
          goto error;

     /* Setup progressive image loading. */
     png_set_progressive_read_fn( data->png_ptr, data, png_info_callback, png_row_callback, png_end_callback );

     /* Read until info callback is called. */
     ret = push_data_until_stage( data, STAGE_INFO, 64 );
     if (ret)
          goto error;

     thiz->AddRef                = IDirectFBImageProvider_PNG_AddRef;
     thiz->Release               = IDirectFBImageProvider_PNG_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_PNG_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_PNG_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_PNG_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_PNG_SetRenderCallback;

     return DFB_OK;

error:
     if (data->image)
          D_FREE( data->image );

     if (data->png_ptr)
          png_destroy_read_struct( &data->png_ptr, data->info_ptr ? &data->info_ptr : NULL, NULL );

     buffer->Release( buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}

/**********************************************************************************************************************/

static int SortColors( const void *a, const void *b )
{
     return *((const u8*) a) - *((const u8*) b);
}

/*
 * Look for a color that is not in the colormap and ideally not even close to the colors used in the colormap.
 */
static u32
FindColorKey( int  n_colors,
              u8  *cmap )
{
     u32 color = 0xff000000;
     u8  csort[n_colors];
     int i, j, index, d;

     if (n_colors < 1)
          return color;

     for (i = 0; i < 3; i++) {
          direct_memcpy( csort, cmap + n_colors * i, n_colors );
          qsort( csort, n_colors, 1, SortColors );

          for (j = 1, index = 0, d = 0; j < n_colors; j++) {
               if (csort[j] - csort[j-1] > d) {
                    d = csort[j] - csort[j-1];
                    index = j;
               }
          }

          if (csort[0] > d) {
               d = csort[0];
               index = n_colors;
          }

          if (0xff - csort[n_colors-1] > d) {
               index = n_colors + 1;
          }

          if (index < n_colors)
               csort[0] = csort[index] - d / 2;
          else if (index == n_colors)
               csort[0] = 0;
          else
               csort[0] = 0xff;

          color |= csort[0] << (8 * (2 - i));
     }

     return color;
}

static void
png_info_callback( png_structp png_read_ptr,
                   png_infop   png_info_ptr )
{
     int                              i;
     IDirectFBImageProvider_PNG_data *data           = png_get_progressive_ptr( png_read_ptr );
     DFBSurfacePixelFormat            primary_format = dfb_primary_layer_pixelformat();

     u32 bpp1[2]  = { 0, 0xff };
     u32 bpp2[4]  = { 0, 0x55, 0xaa, 0xff };
     u32 bpp4[16] = { 0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };

     /* Check error stage. */
     if (data->stage < 0)
          return;

     /* Set info stage. */
     data->stage = STAGE_INFO;

     data->desc.flags = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;

     png_get_IHDR( data->png_ptr, data->info_ptr, (png_uint_32*) &data->desc.width, (png_uint_32*) &data->desc.height,
                   &data->bpp, &data->color_type, NULL, NULL, NULL );

     if (data->color_type & PNG_COLOR_MASK_ALPHA)
          data->desc.pixelformat = DFB_PIXELFORMAT_HAS_ALPHA( primary_format ) ? primary_format : DSPF_ARGB;
     else
          data->desc.pixelformat = primary_format;

     if (png_get_valid( data->png_ptr, data->info_ptr, PNG_INFO_tRNS )) {
          data->color_keyed = true;

          if (data->color_type == PNG_COLOR_TYPE_PALETTE) {
               /* Color key based on palette. */
               png_colorp    palette;
               png_bytep     trans_alpha;
               png_color_16p trans_color;
               int           num_palette = 0, num_colors = 0, num_trans = 0;
               u8            cmap[3][256];

               if (png_get_PLTE( data->png_ptr, data->info_ptr, &palette, &num_palette )) {
                    if (png_get_tRNS( data->png_ptr, data->info_ptr, &trans_alpha, &num_trans, &trans_color )) {
                         num_colors = MIN( 256, num_palette );

                         for (i = 0; i < num_colors; i++) {
                              cmap[0][i] = palette[i].red;
                              cmap[1][i] = palette[i].green;
                              cmap[2][i] = palette[i].blue;
                         }

                         data->color_key = FindColorKey( num_colors, &cmap[0][0] );

                         for (i = 0; i < num_trans; i++) {
                              if (!trans_alpha[i]) {
                                   palette[i].red   = (data->color_key & 0xff0000) >> 16;
                                   palette[i].green = (data->color_key & 0x00ff00) >>  8;
                                   palette[i].blue  =  data->color_key & 0x0000ff;
                              }
                         }
                    }
               }
          }
          else if (data->color_type == PNG_COLOR_TYPE_GRAY) {
               /* Color key based on trans gray value. */
               png_bytep     trans_alpha;
               png_color_16p trans_color;
               int           num_trans = 0;

               if (png_get_tRNS( data->png_ptr, data->info_ptr, &trans_alpha, &num_trans, &trans_color )) {
                    switch (data->bpp) {
                         case 1:
                              data->color_key = (bpp1[trans_color[0].gray] << 16) |
                                                (bpp1[trans_color[0].gray] <<  8) |
                                                 bpp1[trans_color[0].gray];
                              break;
                         case 2:
                              data->color_key = (bpp2[trans_color[0].gray] << 16) |
                                                (bpp2[trans_color[0].gray] <<  8) |
                                                 bpp2[trans_color[0].gray];
                              break;
                         case 4:
                              data->color_key = (bpp4[trans_color[0].gray] << 16) |
                                                (bpp4[trans_color[0].gray] <<  8) |
                                                 bpp4[trans_color[0].gray];
                              break;
                         case 8:
                              data->color_key = ((trans_color[0].gray & 0x00ff) << 16) |
                                                ((trans_color[0].gray & 0x00ff) <<  8) |
                                                 (trans_color[0].gray & 0x00ff);
                              break;
                         case 16:
                         default:
                              data->color_key = ((trans_color[0].gray & 0xff00) << 8) |
                                                 (trans_color[0].gray & 0xff00)       |
                                                ((trans_color[0].gray & 0xff00) >> 8);
                              break;
                    }
               }
          }
          else {
               /* Color key based on trans rgb value. */
               png_bytep     trans_alpha;
               png_color_16p trans_color;
               int           num_trans = 0;

               if (png_get_tRNS( data->png_ptr, data->info_ptr, &trans_alpha, &num_trans, &trans_color )) {
                    switch (data->bpp) {
                         case 1:
                              data->color_key = (((bpp1[trans_color[0].red])   << 16) |
                                                 ((bpp1[trans_color[0].green]) <<  8) |
                                                 ((bpp1[trans_color[0].blue])));
                              break;
                         case 2:
                              data->color_key = (((bpp2[trans_color[0].red])   << 16) |
                                                 ((bpp2[trans_color[0].green]) << 8)  |
                                                 ((bpp2[trans_color[0].blue])));
                              break;
                         case 4:
                              data->color_key = (((bpp4[trans_color[0].red])   << 16) |
                                                 ((bpp4[trans_color[0].green]) <<  8) |
                                                 ((bpp4[trans_color[0].blue])));
                              break;
                         case 8:
                              data->color_key = (((trans_color[0].red   & 0x00ff) << 16) |
                                                 ((trans_color[0].green & 0x00ff) <<  8) |
                                                 ((trans_color[0].blue  & 0x00ff)));
                              break;
                         case 16:
                         default:
                              data->color_key = (((trans_color[0].red   & 0xff00) << 8) |
                                                  (trans_color[0].green & 0xff00)       |
                                                 ((trans_color[0].blue  & 0xff00) >> 8));
                              break;
                    }
               }
          }
     }

     switch (data->color_type) {
          case PNG_COLOR_TYPE_PALETTE: {
               data->pitch = (data->desc.width + 7) & ~7;

               png_colorp    palette;
               png_bytep     trans_alpha;
               png_color_16p trans_color;
               int           num_palette = 0, num_colors = 0, num_trans = 0;

               png_get_PLTE( data->png_ptr, data->info_ptr, &palette, &num_palette );

               png_get_tRNS( data->png_ptr, data->info_ptr, &trans_alpha, &num_trans, &trans_color );

               num_colors = MIN( 256, num_palette );

               for (i = 0; i < num_colors; i++) {
                    data->colors[i].a = (i < num_trans) ? trans_alpha[i] : 0xff;
                    data->colors[i].r = palette[i].red;
                    data->colors[i].g = palette[i].green;
                    data->colors[i].b = palette[i].blue;

                    data->palette[i] = (data->colors[i].a << 24) |
                                       (data->colors[i].r << 16) |
                                       (data->colors[i].g <<  8) |
                                        data->colors[i].b;
               }

               data->desc.flags           |= DSDESC_PALETTE;
               data->desc.palette.entries  = data->colors;
               data->desc.palette.size     = 256;
               break;
          }

          case PNG_COLOR_TYPE_GRAY:
               if (data->bpp < 16) {
                    data->pitch = data->desc.width;
                    break;
               }
               /* fall through */

          case PNG_COLOR_TYPE_GRAY_ALPHA:
               png_set_gray_to_rgb( data->png_ptr );
               /* fall through */

          default:
               data->pitch = data->desc.width * 4;

               if (!data->color_keyed)
                    png_set_strip_16( data->png_ptr );

#ifdef WORDS_BIGENDIAN
               if (!(data->color_type & PNG_COLOR_MASK_ALPHA))
                    png_set_filler( data->png_ptr, 0xff, PNG_FILLER_BEFORE );

               png_set_swap_alpha( data->png_ptr );
#else
               if (!(data->color_type & PNG_COLOR_MASK_ALPHA))
                    png_set_filler( data->png_ptr, 0xff, PNG_FILLER_AFTER );

               png_set_bgr( data->png_ptr );
#endif
               break;
     }

     png_set_interlace_handling( data->png_ptr );

     png_read_update_info( data->png_ptr, data->info_ptr );
}

static void
png_row_callback( png_structp png_read_ptr,
                  png_bytep   new_row,
                  png_uint_32 row_num,
                  int         pass_num )
{
     IDirectFBImageProvider_PNG_data *data = png_get_progressive_ptr( png_read_ptr );

     /* Check error stage. */
     if (data->stage < 0)
          return;

     /* Set image decoding stage. */
     data->stage = STAGE_IMAGE;

     /* Check image data pointer. */
     if (!data->image) {
          /* Allocate image data. */
          data->image = D_CALLOC( data->desc.height, data->pitch );
          if (!data->image) {
               D_OOM();

               /* Set error stage. */
               data->stage = STAGE_ERROR;

               return;
          }
     }

     /* Write to image data. */
     if (data->bpp == 16 && data->color_keyed) {
          u8 *dst = data->image + row_num * data->pitch;
          u8 *src = new_row;

          if (src) {
               int src_advance          = 8;
               int src16_advance        = 4;
               int dst32_advance        = 1;
               int src16_initial_offset = 0;
               int dst32_initial_offset = 0;

               /* Even lines 0, 2, 4 ... */
               if (!(row_num % 2)) {
                    switch (pass_num) {
                         case 1:
                              src_advance          = 64;
                              src16_advance        = 32;
                              dst32_advance        = 8;
                              src16_initial_offset = 16;
                              dst32_initial_offset = 4;
                              break;
                         case 3:
                              src_advance          = 32;
                              src16_advance        = 16;
                              dst32_advance        = 4;
                              src16_initial_offset = 8;
                              dst32_initial_offset = 2;
                              break;
                         case 5:
                              src_advance          = 16;
                              src16_advance        = 8;
                              dst32_advance        = 2;
                              src16_initial_offset = 4;
                              dst32_initial_offset = 1;
                              break;
                         default:
                              break;
                    }
               }

               png_bytep     trans;
               png_color_16p trans_color;
               int           num_trans = 0;

               png_get_tRNS( data->png_ptr, data->info_ptr, &trans, &num_trans, &trans_color );

               u16 *src16 = (u16*) src + src16_initial_offset;
               u32 *dst32 = (u32*) dst + dst32_initial_offset;

               int remaining = data->desc.width - dst32_initial_offset;

               while (remaining > 0) {
                    int keyed = 0;

#ifdef WORDS_BIGENDIAN
                    u16 comp_r = src16[1];
                    u16 comp_g = src16[2];
                    u16 comp_b = src16[3];
                    u32 pixel32 = src[1] << 24 | src[3] << 16 | src[5] << 8 | src[7];
#else
                    u16 comp_r = src16[2];
                    u16 comp_g = src16[1];
                    u16 comp_b = src16[0];
                    u32 pixel32 = src[6] << 24 | src[4] << 16 | src[2] << 8 | src[0];
#endif
                    /* The pixel matches the color key in a resolution of 16 bits per channel. */
                    if ((comp_r == trans_color[0].gray && data->color_type == PNG_COLOR_TYPE_GRAY) ||
                        (comp_g == trans_color[0].green &&
                         comp_b == trans_color[0].blue  &&
                         comp_r == trans_color[0].red)) {
                         keyed = 1;
                    }

                    /* The pixel is not keyed but the color key matches in the reduced color space: LSB is toggled. */
                    if (!keyed && pixel32 == (0xff000000 | data->color_key)) {
                         D_ONCE( "ImageProvider/PNG: Adjusting pixel data to protect it from being keyed!\n" );
                         pixel32 ^= 0x00000001;
                    }

                    *dst32 = pixel32;

                    src   += src_advance;
                    src16 += src16_advance;
                    dst32 += dst32_advance;

                    remaining -= dst32_advance;
               }
          }
     }
     else
         png_progressive_combine_row( data->png_ptr, data->image + row_num * data->pitch, new_row );

     if (data->render_callback) {
          DIRenderCallbackResult cb_result;
          DFBRectangle           r = { 0, row_num, data->desc.width, 1 };

          cb_result = data->render_callback( &r, data->render_callback_context );
          if (cb_result != DIRCR_OK) {
               /* Set abort stage. */
               data->stage = STAGE_ABORT;
          }
     }
}

static void
png_end_callback( png_structp png_read_ptr,
                  png_infop   png_info_ptr )
{
     IDirectFBImageProvider_PNG_data *data = png_get_progressive_ptr( png_read_ptr );

     /* Check error stage. */
     if (data->stage < 0)
          return;

     /* Set end stage. */
     data->stage = STAGE_END;
}

static DFBResult
push_data_until_stage( IDirectFBImageProvider_PNG_data *data,
                       int                              stage,
                       int                              buffer_size )
{
     DFBResult            ret;
     IDirectFBDataBuffer *buffer = data->buffer;

     while (data->stage < stage) {
          unsigned int  len;
          unsigned char buf[buffer_size];

          /* Check error stage. */
          if (data->stage < 0)
               return DFB_FAILURE;

          while (buffer->HasData( buffer ) == DFB_OK) {
               D_DEBUG_AT( ImageProvider_PNG, "Retrieving data (up to %d bytes)...\n", buffer_size );

               ret = buffer->GetData( buffer, buffer_size, buf, &len );
               if (ret)
                    return ret;

               D_DEBUG_AT( ImageProvider_PNG, "  -> got %u bytes\n", len );

               png_process_data( data->png_ptr, data->info_ptr, buf, len );

               D_DEBUG_AT( ImageProvider_PNG, "  -> %u bytes processed\n", len );

               if (data->stage < 0 || data->stage >= stage) {
                    switch (data->stage) {
                         case STAGE_ABORT: return DFB_INTERRUPTED;
                         case STAGE_ERROR: return DFB_FAILURE;
                         default:          return DFB_OK;
                    }
               }
          }

          D_DEBUG_AT( ImageProvider_PNG, "Waiting for data...\n" );

          if (buffer->WaitForData( buffer, 1 ) == DFB_EOF)
               return DFB_FAILURE;
     }

     return DFB_OK;
}
