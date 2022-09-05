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
#include <core/fonts.h>
#include <core/surface_buffer.h>
#include <direct/memcpy.h>
#include <direct/utf8.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <media/idirectfbfont.h>
#include <misc/conf.h>

D_DEBUG_DOMAIN( Font_FT2, "Font/FT2", "FreeType2 Font Provider" );

static DFBResult Probe    ( IDirectFBFont_ProbeContext *ctx );

static DFBResult Construct( IDirectFBFont              *thiz,
                            CoreDFB                    *core,
                            IDirectFBFont_ProbeContext *ctx,
                            DFBFontDescription         *desc );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBFont, FT2 )

/**********************************************************************************************************************/

static FT_Library  library           = NULL;
static int         library_ref_count = 0;
static DirectMutex library_mutex     = DIRECT_MUTEX_INITIALIZER();

typedef struct {
     FT_Face      face;
     int          disable_charmap;
     int          fixed_advance;
     bool         fixed_clip;
     unsigned int indices[256];
     int          outline_radius;
     int          outline_opacity;
     float        up_unit_x;
     float        up_unit_y;
} FT2ImplData;

#define KERNING_CACHE_MIN    0
#define KERNING_CACHE_MAX  127
#define KERNING_CACHE_SIZE (KERNING_CACHE_MAX - KERNING_CACHE_MIN + 1)

typedef struct {
     bool initialised;
     char x;
     char y;
} KerningCacheEntry;

typedef struct {
     FT2ImplData       base;
     KerningCacheEntry kerning[KERNING_CACHE_SIZE][KERNING_CACHE_SIZE];
} FT2ImplKerningData;

/**********************************************************************************************************************/

#define CHAR_INDEX(c) (((c) < 256) ? data->indices[c] : FT_Get_Char_Index( data->face, c ))

static DFBResult
ft2UTF8GetCharacterIndex( CoreFont     *thiz,
                          unsigned int  character,
                          unsigned int *ret_index )
{
     FT2ImplData *data = thiz->impl_data;

     D_MAGIC_ASSERT( thiz, CoreFont );
     D_ASSERT( ret_index != NULL );

     if (data->disable_charmap)
          *ret_index = character;
     else {
          direct_mutex_lock( &library_mutex );

          *ret_index = CHAR_INDEX( character );

          direct_mutex_unlock( &library_mutex );
     }

     return DFB_OK;
}

static DFBResult
ft2UTF8DecodeText( CoreFont     *thiz,
                   const void   *text,
                   int           length,
                   unsigned int *ret_indices,
                   int          *ret_num )
{
     FT2ImplData *data  = thiz->impl_data;
     const u8    *bytes = text;
     int          pos   = 0;
     int          num   = 0;

     D_MAGIC_ASSERT( thiz, CoreFont );
     D_ASSERT( text != NULL );
     D_ASSERT( length >= 0 );
     D_ASSERT( ret_indices != NULL );
     D_ASSERT( ret_num != NULL );

     direct_mutex_lock( &library_mutex );

     while (pos < length) {
          unsigned int c;

          if (bytes[pos] < 128) {
               c = bytes[pos++];
          }
          else {
               c = DIRECT_UTF8_GET_CHAR( &bytes[pos] );
               pos += DIRECT_UTF8_SKIP(bytes[pos]);
          }

          if (data->disable_charmap)
               ret_indices[num++] = c;
          else
               ret_indices[num++] = CHAR_INDEX( c );
     }

     direct_mutex_unlock( &library_mutex );

     *ret_num = num;

     return DFB_OK;
}

static const CoreFontEncodingFuncs ft2UTF8Funcs = {
     .GetCharacterIndex = ft2UTF8GetCharacterIndex,
     .DecodeText        = ft2UTF8DecodeText,
};

/**********************************************************************************************************************/

static DFBResult
ft2Latin1GetCharacterIndex( CoreFont     *thiz,
                            unsigned int  character,
                            unsigned int *ret_index )
{
     FT2ImplData *data = thiz->impl_data;

     D_MAGIC_ASSERT( thiz, CoreFont );
     D_ASSERT( ret_index != NULL );

     if (data->disable_charmap)
          *ret_index = character;
     else
          *ret_index = data->indices[character];

     return DFB_OK;
}

static DFBResult
ft2Latin1DecodeText( CoreFont     *thiz,
                     const void   *text,
                     int           length,
                     unsigned int *ret_indices,
                     int          *ret_num )
{
     FT2ImplData *data  = thiz->impl_data;
     const u8    *bytes = text;
     int          i;

     D_MAGIC_ASSERT( thiz, CoreFont );
     D_ASSERT( text != NULL );
     D_ASSERT( length >= 0 );
     D_ASSERT( ret_indices != NULL );
     D_ASSERT( ret_num != NULL );

     if (data->disable_charmap) {
          for (i = 0; i < length; i++)
               ret_indices[i] = bytes[i];
     }
     else {
          for (i = 0; i < length; i++)
               ret_indices[i] = data->indices[bytes[i]];
     }

     *ret_num = length;

     return DFB_OK;
}

static const CoreFontEncodingFuncs ft2Latin1Funcs = {
     .GetCharacterIndex = ft2Latin1GetCharacterIndex,
     .DecodeText        = ft2Latin1DecodeText,
};

/**********************************************************************************************************************/

static DFBResult
get_glyph_info( CoreFont      *thiz,
                unsigned int   index,
                CoreGlyphData *info )
{
     FT_Error     err;
     FT_Face      face;
     FT_Int       load_flags;
     FT2ImplData *data = thiz->impl_data;

     direct_mutex_lock( &library_mutex );

     face = data->face;

     load_flags = (long) face->generic.data;

     if ((err = FT_Load_Glyph( face, index, load_flags ))) {
          D_DEBUG_AT( Font_FT2, "Could not load glyph for character index #%u!\n", index );
          direct_mutex_unlock( &library_mutex );
          return DFB_FAILURE;
     }

     if (face->glyph->format != ft_glyph_format_bitmap) {
          load_flags &= FT_LOAD_TARGET_MONO;

          err = FT_Render_Glyph( face->glyph, load_flags ? ft_render_mode_mono : ft_render_mode_normal );
          if (err) {
               D_ERROR( "Font/FT2: Could not render glyph for character index #%u!\n", index );
               direct_mutex_unlock( &library_mutex );
               return DFB_FAILURE;
          }
     }

     direct_mutex_unlock( &library_mutex );

     info->width  = face->glyph->bitmap.width;
     info->height = face->glyph->bitmap.rows;

     if (data->fixed_advance) {
          info->xadvance = -data->fixed_advance * thiz->up_unit_y;
          info->yadvance =  data->fixed_advance * thiz->up_unit_x;
     }
     else {
          info->xadvance =  face->glyph->advance.x << 2;
          info->yadvance = -face->glyph->advance.y << 2;
     }

     if (data->fixed_clip && info->width > data->fixed_advance)
          info->width = data->fixed_advance;

     if (info->layer == 1 && info->width > 0 && info->height > 0) {
          info->width  += data->outline_radius;
          info->height += data->outline_radius;
     }

     return DFB_OK;
}

static DFBResult
render_glyph( CoreFont      *thiz,
              unsigned int   index,
              CoreGlyphData *info )
{
     DFBResult              ret;
     FT_Error               err;
     FT_Face                face;
     FT_Int                 load_flags;
     int                    y;
     u8                    *src;
     FT2ImplData           *data    = thiz->impl_data;
     CoreSurface           *surface = info->surface;
     CoreSurfaceBufferLock  lock;

     direct_mutex_lock( &library_mutex );

     face = data->face;

     load_flags = (long) face->generic.data;
     load_flags |= FT_LOAD_RENDER;

     if ((err = FT_Load_Glyph( face, index, load_flags ))) {
          D_DEBUG_AT( Font_FT2, "Could not load glyph for character index #%u!\n", index );
          direct_mutex_unlock( &library_mutex );
          return DFB_FAILURE;
     }

     direct_mutex_unlock( &library_mutex );

     ret = dfb_surface_lock_buffer( surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
     if (ret) {
          D_DERROR( ret, "Font/FT2: Unable to lock surface!\n" );
          return ret;
     }

     if (info->width + info->start > surface->config.size.w)
          info->width = surface->config.size.w - info->start;

     if (info->height > surface->config.size.h)
          info->height = surface->config.size.h;

     /* bitmap_left and bitmap_top are relative to the glyph's origin on the baseline.
        info->left  and info->top  are relative to the top left of the character cell. */
     info->left =  face->glyph->bitmap_left - thiz->ascender * thiz->up_unit_x;
     info->top  = -face->glyph->bitmap_top  - thiz->ascender * thiz->up_unit_y;

     if (info->layer == 1 && info->width > 0 && info->height > 0) {
          int   xoffset, yoffset;
          void *blurred = NULL;
          int   radius  = data->outline_radius;

          switch (face->glyph->bitmap.pixel_mode) {
               case ft_pixel_mode_grays:
                    blurred = D_CALLOC( 1, (info->width + radius) * (info->height + radius) );
                    if (blurred) {
                         for (yoffset = 0; yoffset < radius; yoffset++) {
                              for (xoffset = 0; xoffset < radius; xoffset++) {
                                   src = face->glyph->bitmap.buffer;

                                   for (y = 0; y < info->height; y++) {
                                        int  i;
                                        u8  *dst8 = blurred + xoffset + (y + yoffset) * (info->width + radius);

                                        for (i = 0; i < info->width; i++) {
                                             int val = dst8[i] + src[i] / radius;

                                             dst8[i] = (val < 255) ? val : 255;
                                        }

                                        src += face->glyph->bitmap.pitch;
                                   }
                              }
                         }
                    }
                    else
                         D_OOM();
                    break;

               case ft_pixel_mode_mono:
                    D_UNIMPLEMENTED();
                    break;

               default:
                    break;
          }

          info->width  += radius;
          info->height += radius;
          info->left   -= (radius - 1) / 2;
          info->top    -= (radius - 1) / 2;

          if (blurred) {
               src = blurred;
               lock.addr += DFB_BYTES_PER_LINE( surface->config.format, info->start );

               for (y = 0; y < info->height; y++) {
                    int  i;
                    u8  *dst8  = lock.addr;
                    u32 *dst32 = lock.addr;

                    switch (face->glyph->bitmap.pixel_mode) {
                         case ft_pixel_mode_grays:
                              switch (surface->config.format) {
                                   case DSPF_ARGB:
                                   case DSPF_ABGR:
                                        if (thiz->surface_caps & DSCAPS_PREMULTIPLIED) {
                                             for (i = 0; i < info->width; i++)
                                                  dst32[i] = ((data->outline_opacity + 1) * src[i] / 256) * 0x01010101;
                                        }
                                        else {
                                             for (i = 0; i < info->width; i++)
                                                  dst32[i] = (((data->outline_opacity + 1) * src[i] / 256) << 24) |
                                                             0xFFFFFF;
                                        }
                                        break;
                                   case DSPF_A8:
                                        for (i = 0; i < info->width; i++)
                                             dst8[i] = (data->outline_opacity + 1) * src[i] / 256;
                                        break;
                                   default:
                                        D_UNIMPLEMENTED();
                                        break;
                              }
                              break;

                         case ft_pixel_mode_mono:
                              D_UNIMPLEMENTED();
                              break;

                         default:
                              break;

                    }

                    src += info->width;
                    lock.addr += lock.pitch;
               }

               D_FREE( blurred );
          }
     }
     else {
          if (data->fixed_clip) {
               while (info->left + info->width > data->fixed_advance)
                    info->left--;

               if (info->left < 0)
                    info->left = 0;

               if (info->width > data->fixed_advance)
                    info->width = data->fixed_advance;
          }

          src = face->glyph->bitmap.buffer;

          lock.addr += DFB_BYTES_PER_LINE( surface->config.format, info->start );

          for (y = 0; y < info->height; y++) {
               int  i, j, n;
               u8  *dst8  = lock.addr;
               u16 *dst16 = lock.addr;
               u32 *dst32 = lock.addr;

               switch (face->glyph->bitmap.pixel_mode) {
                    case ft_pixel_mode_grays:
                         switch (surface->config.format) {
                              case DSPF_ARGB:
                              case DSPF_ABGR:
                                   if (thiz->surface_caps & DSCAPS_PREMULTIPLIED) {
                                        for (i = 0; i < info->width; i++)
                                             dst32[i] = src[i] * 0x01010101;
                                   }
                                   else
                                        for (i = 0; i < info->width; i++)
                                             dst32[i] = (src[i] << 24) | 0xFFFFFF;
                                   break;
                              case DSPF_AiRGB:
                                   for (i = 0; i < info->width; i++)
                                        dst32[i] = ((src[i] ^ 0xFF) << 24) | 0xFFFFFF;
                                   break;
                              case DSPF_ARGB8565:
                                   for (i = 0, j = -1; i < info->width; ++i) {
                                       u32 d;

                                       if (thiz->surface_caps & DSCAPS_PREMULTIPLIED) {
                                            d = src[i] * 0x01010101;
                                            d = ((d & 0xFF000000) >> 8) |
                                                ((d & 0x00F80000) >> 8) |
                                                ((d & 0x0000FC00) >> 5) |
                                                ((d & 0x000000F8) >> 3);
                                       }
                                       else
                                            d = (src[i] << 16) | 0xFFFF;
#ifdef WORDS_BIGENDIAN
                                        dst8[++j] = (d >> 16) & 0xFF;
                                        dst8[++j] = (d >>  8) & 0xFF;
                                        dst8[++j] = (d >>  0) & 0xFF;
#else
                                        dst8[++j] = (d >>  0) & 0xFF;
                                        dst8[++j] = (d >>  8) & 0xFF;
                                        dst8[++j] = (d >> 16) & 0xFF;
#endif
                                   }
                                   break;
                              case DSPF_ARGB4444:
                              case DSPF_RGBA4444:
                                   if (thiz->surface_caps & DSCAPS_PREMULTIPLIED) {
                                        for (i = 0; i < info->width; i++)
                                             dst16[i] = (src[i] >> 4) * 0x1111;
                                   }
                                   else {
                                        if (surface->config.format == DSPF_ARGB4444) {
                                             for (i = 0; i < info->width; i++)
                                                  dst16[i] = (src[i] << 8) | 0x0FFF;
                                        }
                                        else {
                                             for (i = 0; i < info->width; i++)
                                                  dst16[i] = (src[i] >> 4) | 0xFFF0;
                                        }
                                   }
                                   break;
                              case DSPF_ARGB2554:
                                   for (i = 0; i < info->width; i++)
                                        dst16[i] = (src[i] << 8) | 0x3FFF;
                                   break;
                              case DSPF_ARGB1555:
                                   if (thiz->surface_caps & DSCAPS_PREMULTIPLIED) {
                                        for (i = 0; i < info->width; i++) {
                                             unsigned short x = src[i] >> 3;
                                             dst16[i] = ((src[i] & 0x80) << 8) | (x << 10) | (x << 5) | x;
                                        }
                                   }
                                   else {
                                        for (i = 0; i < info->width; i++)
                                             dst16[i] = (src[i] << 8) | 0x7FFF;
                                   }
                                   break;
                              case DSPF_RGBA5551:
                                   if (thiz->surface_caps & DSCAPS_PREMULTIPLIED) {
                                        for (i = 0; i < info->width; i++) {
                                             unsigned short x = src[i] >> 3;
                                             dst16[i] = (x << 11) | (x << 6) | (x << 1) | (src[i] >> 7);
                                        }
                                   }
                                   else {
                                        for (i = 0; i < info->width; i++)
                                             dst16[i] = 0xFFFE | (src[i] >> 7);
                                   }
                                   break;
                              case DSPF_A8:
                                   direct_memcpy( lock.addr, src, info->width );
                                   break;
                              case DSPF_A4:
                                   for (i = 0, j = 0; i < info->width; i += 2, j++)
                                        dst8[j] = (src[i] & 0xF0) | (src[i+1] >> 4);
                                   break;
                              case DSPF_A1:
                                   for (i = 0, j = 0; i < info->width; ++j) {
                                        u8 p = 0;

                                        for (n = 0; n < 8 && i < info->width; ++i, ++n)
                                             p |= (src[i] & 0x80) >> n;

                                        dst8[j] = p;
                                   }
                                   break;
                              case DSPF_A1_LSB:
                                   for (i = 0, j = 0; i < info->width; ++j) {
                                        u8 p = 0;

                                        for (n = 0; n < 8 && i < info->width; ++i, ++n)
                                             p |= (src[i] & 0x80) >> (7-n);

                                        dst8[j] = p;
                                   }
                                   break;
                              case DSPF_LUT2:
                                   for (i = 0, j = 0; i < info->width; ++j) {
                                        u8 p = 0;

                                        for (n = 0; n < 8 && i < info->width; ++i, n += 2)
                                             p |= (src[i] & 0xC0) >> n;

                                        dst8[j] = p;
                                   }
                                   break;
                              default:
                                   D_UNIMPLEMENTED();
                                   break;
                         }
                         break;

                    case ft_pixel_mode_mono:
                         switch (surface->config.format) {
                              case DSPF_ARGB:
                              case DSPF_ABGR:
                                   if (thiz->surface_caps & DSCAPS_PREMULTIPLIED) {
                                        for (i = 0; i < info->width; i++)
                                             dst32[i] = (src[i>>3] & (1 << (7 - (i % 8)))) ? 0xFFFFFFFF : 0x00000000;
                                   }
                                   else {
                                        for (i = 0; i < info->width; i++)
                                             dst32[i] = (((src[i>>3] & (1 << (7 - (i % 8)))) ? 0xFF : 0x00) << 24) |
                                                        0xFFFFFF;
                                   }
                                   break;
                              case DSPF_AiRGB:
                                   if (thiz->surface_caps & DSCAPS_PREMULTIPLIED) {
                                        for (i = 0; i < info->width; i++)
                                             dst32[i] = (src[i>>3] & (1 << (7 - (i % 8)))) ? 0x00FFFFFF : 0xFF000000;
                                   }
                                   else {
                                        for (i = 0; i < info->width; i++)
                                             dst32[i] = (((src[i>>3] & (1 << (7 - (i % 8)))) ? 0x00 : 0xFF) << 24) |
                                                        0xFFFFFF;
                                   }
                                   break;
                              case DSPF_ARGB8565:
                                   for (i = 0, j = -1; i < info->width; ++i) {
                                        u32 d;

                                        if (thiz->surface_caps & DSCAPS_PREMULTIPLIED) {
                                             d = (src[i>>3] & (1 << (7 - (i % 8)))) ? 0xFFFFFF : 0x000000;
                                        }
                                        else
                                             d = (((src[i>>3] & (1 << (7 - (i % 8)))) ? 0xFF : 0x00) << 16) | 0xFFFF;
#ifdef WORDS_BIGENDIAN
                                        dst8[++j] = (d >> 16) & 0xFF;
                                        dst8[++j] = (d >>  8) & 0xFF;
                                        dst8[++j] = (d >>  0) & 0xFF;
#else
                                        dst8[++j] = (d >>  0) & 0xFF;
                                        dst8[++j] = (d >>  8) & 0xFF;
                                        dst8[++j] = (d >> 16) & 0xFF;
#endif
                                   }
                                   break;
                              case DSPF_ARGB4444:
                                   for (i = 0; i < info->width; i++)
                                        dst16[i] = (((src[i>>3] & (1 << (7 - (i % 8)))) ? 0xF : 0x0) << 12) | 0xFFF;
                                   break;
                              case DSPF_RGBA4444:
                                   for (i = 0; i < info->width; i++)
                                        dst16[i] = ((src[i>>3] & (1 << (7 - (i % 8)))) ? 0xF : 0x0) | 0xFFF0;
                                   break;
                              case DSPF_ARGB2554:
                                   for (i = 0; i < info->width; i++)
                                        dst16[i] = (((src[i>>3] & (1 << (7 - (i % 8)))) ? 0x3 : 0x0) << 14) | 0x3FFF;
                                   break;
                              case DSPF_ARGB1555:
                                   for (i = 0; i < info->width; i++)
                                        dst16[i] = (((src[i>>3] & (1 << (7 - (i % 8)))) ? 0x1 : 0x0) << 15) | 0x7FFF;
                                   break;
                              case DSPF_RGBA5551:
                                   for (i = 0; i < info->width; i++)
                                        dst16[i] = ((src[i>>3] & (1 << (7 - (i % 8)))) ? 0x1 : 0x0) | 0xFFFE;
                                   break;
                              case DSPF_A8:
                                   for (i = 0; i < info->width; i++)
                                        dst8[i] = (src[i>>3] & (1 << (7 - (i % 8)))) ? 0xFF : 0x00;
                                   break;
                              case DSPF_A4:
                                   for (i = 0, j = 0; i < info->width; i += 2, j++)
                                        dst8[j] = ((src[i>>3] & (1 << (7 - (i % 8)))) ? 0xF0 : 0x00) |
                                                  ((src[(i+1)>>3] & (1 << (7 - ((i + 1) % 8)))) ? 0x0F : 0x00);
                                   break;
                              case DSPF_A1:
                                   direct_memcpy( lock.addr, src, DFB_BYTES_PER_LINE( DSPF_A1, info->width ) );
                                   break;
                              case DSPF_A1_LSB:
                                   for (i = 0, j = 0; i < info->width; ++j) {
                                        u8 p = 0;

                                        for (n = 0; n < 8 && i < info->width; ++i, ++n)
                                             p |= (((src[i] >> n) & 1) << (7 - n));

                                        dst8[j] = p;
                                   }
                                   break;
                              default:
                                   D_UNIMPLEMENTED();
                                   break;
                         }
                         break;

                    default:
                         break;

               }

               src += face->glyph->bitmap.pitch;
               lock.addr += lock.pitch;
          }
     }

     dfb_surface_unlock_buffer( surface, &lock );

     return DFB_OK;
}

static DFBResult
get_kerning( CoreFont     *thiz,
             unsigned int  prev,
             unsigned int  current,
             int          *kern_x,
             int          *kern_y )
{
     FT_Vector           vector;
     KerningCacheEntry  *cache;
     FT2ImplKerningData *data = thiz->impl_data;

     D_ASSUME( kern_x != NULL || kern_y != NULL );

     /* Use cached values if characters are in the cachable range and the cache entry is already filled. */
     if (prev    >= KERNING_CACHE_MIN && prev    <= KERNING_CACHE_MAX &&
         current >= KERNING_CACHE_MIN && current <= KERNING_CACHE_MAX) {
          cache = &data->kerning[prev-KERNING_CACHE_MIN][current-KERNING_CACHE_MIN];

          if (!cache->initialised && FT_HAS_KERNING( data->base.face )) {
               direct_mutex_lock( &library_mutex );

               /* Lookup kerning values for the character pair. */
               FT_Get_Kerning( data->base.face, prev, current, ft_kerning_default, &vector );

               direct_mutex_unlock( &library_mutex );

               /* Fill cache. */
               cache->x           = (int) (-vector.x * data->base.up_unit_x + vector.y * data->base.up_unit_x) >> 6;
               cache->y           = (int) ( vector.y * data->base.up_unit_y + vector.x * data->base.up_unit_x) >> 6;
               cache->initialised = true;
          }

          if (kern_x)
               *kern_x = cache->x;

          if (kern_y)
               *kern_y = cache->y;

          return DFB_OK;
     }

     direct_mutex_lock( &library_mutex );

     /* Lookup kerning values for the character pair. */
     FT_Get_Kerning( data->base.face, prev, current, ft_kerning_default, &vector );

     direct_mutex_unlock( &library_mutex );

     /* Convert to integer. */
     if (kern_x)
          *kern_x = (int) (-vector.x * thiz->up_unit_y + vector.y * thiz->up_unit_x) >> 6;

     if (kern_y)
          *kern_y = (int) ( vector.y * thiz->up_unit_y + vector.x * thiz->up_unit_x) >> 6;

     return DFB_OK;
}

static DFBResult
init_freetype()
{
     FT_Error err;

     direct_mutex_lock( &library_mutex );

     if (!library) {
          D_DEBUG_AT( Font_FT2, "Initializing the FreeType2 library\n" );

          err = FT_Init_FreeType( &library );
          if (err) {
               library = NULL;
               direct_mutex_unlock( &library_mutex );
               return DFB_FAILURE;
          }
     }

     library_ref_count++;

     direct_mutex_unlock( &library_mutex );

     return DFB_OK;
}

static void
release_freetype()
{
     direct_mutex_lock( &library_mutex );

     if (library && --library_ref_count == 0) {
          D_DEBUG_AT( Font_FT2, "Releasing the FreeType2 library\n" );

          FT_Done_FreeType( library );
          library = NULL;
     }

     direct_mutex_unlock( &library_mutex );
}

/**********************************************************************************************************************/

static void
IDirectFBFont_FT2_Destruct( IDirectFBFont *thiz )
{
     FT2ImplData *data = ((IDirectFBFont_data *) thiz->priv)->font->impl_data;

     D_DEBUG_AT( Font_FT2, "%s( %p )\n", __FUNCTION__, thiz );

     direct_mutex_lock( &library_mutex );

     FT_Done_Face( data->face );

     direct_mutex_unlock( &library_mutex );

     D_FREE( data );

     IDirectFBFont_Destruct( thiz );

     release_freetype();
}

static DirectResult
IDirectFBFont_FT2_Release( IDirectFBFont *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBFont )

     D_DEBUG_AT( Font_FT2, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBFont_FT2_Destruct( thiz );

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBFont_ProbeContext *ctx )
{
     DFBResult ret;
     FT_Error  err;
     FT_Face   face;

     if (!ctx->content)
          return DFB_UNSUPPORTED;

     /* Initialize the FreeType library object. */
     ret = init_freetype();
     if (ret) {
          D_DERROR( ret, "Font/FT2: Initialization of the FreeType2 library failed!\n" );
          return ret;
     }

     /* Open the font loaded into memory. */
     direct_mutex_lock( &library_mutex );

     err = FT_New_Memory_Face( library, ctx->content, ctx->content_size, 0, &face );
     if (!err)
          FT_Done_Face( face );

     direct_mutex_unlock( &library_mutex );

     release_freetype();

     return err ? DFB_UNSUPPORTED : DFB_OK;
}

static DFBResult
Construct( IDirectFBFont              *thiz,
           CoreDFB                    *core,
           IDirectFBFont_ProbeContext *ctx,
           DFBFontDescription         *desc )
{
     DFBResult    ret;
     int          i;
     FT_Error     err;
     FT_Face      face       = NULL;
     FT_Int       load_flags = FT_LOAD_DEFAULT;
     FT_ULong     mask       = 0;
     float        sin_rot    = 0.0;
     float        cos_rot    = 1.0;
     int          fw         = 0;
     int          fh         = 0;
     CoreFont    *font       = NULL;
     FT2ImplData *data       = NULL;

     D_DEBUG_AT( Font_FT2, "%s( %p )\n", __FUNCTION__, thiz );

     /* Check for valid description. */
     if (!(desc->flags & (DFDESC_HEIGHT | DFDESC_WIDTH | DFDESC_FRACT_HEIGHT | DFDESC_FRACT_WIDTH)))
          return DFB_INVARG;

     D_DEBUG_AT( Font_FT2, "  -> file '%s' (index %u) at pixel size %dx%d and rotation %d\n", ctx->filename,
                 desc->flags & DFDESC_INDEX        ? desc->index        : 0,
                 desc->flags & DFDESC_FRACT_WIDTH  ? desc->fract_width  :
                 desc->flags & DFDESC_WIDTH        ? desc->width        : 0,
                 desc->flags & DFDESC_FRACT_HEIGHT ? desc->fract_height :
                 desc->flags & DFDESC_HEIGHT       ? desc->height       : 0,
                 desc->flags & DFDESC_ROTATION     ? desc->rotation     : 0 );

     /* Initialize the FreeType library object. */
     ret = init_freetype();
     if (ret) {
          D_DERROR( ret, "Font/FT2: Initialization of the FreeType2 library failed!\n" );
          return ret;
     }

     /* Open the font loaded into memory. */
     direct_mutex_lock( &library_mutex );

     err = FT_New_Memory_Face( library, ctx->content, ctx->content_size, (desc->flags & DFDESC_INDEX) ? desc->index : 0,
                               &face );

     direct_mutex_unlock( &library_mutex );

     if (err) {
          switch (err) {
               case FT_Err_Unknown_File_Format:
                    D_ERROR( "Font/FT2: Unsupported font format in file '%s'!\n", ctx->filename );
                    break;
               default:
                    D_ERROR( "Font/FT2: Failed loading face %u from font file '%s'!\n",
                              (desc->flags & DFDESC_INDEX) ? desc->index : 0, ctx->filename );
                    break;
          }
          ret = DFB_FAILURE;
          goto error;
     }

     if ((desc->flags & DFDESC_ROTATION) && desc->rotation) {
          if (!FT_IS_SCALABLE( face )) {
               D_ERROR( "Font/FT2: Face %u from font file '%s' is not scalable so cannot be rotated!\n",
                         (desc->flags & DFDESC_INDEX) ? desc->index : 0, ctx->filename );
               ret = DFB_UNSUPPORTED;
               goto error;
          }

          float rot_radians = 2.0 * M_PI * desc->rotation / (1 << 24);

          sin_rot = sin( rot_radians );
          cos_rot = cos( rot_radians );

          int sin_rot_fx = sin_rot * 65536;
          int cos_rot_fx = cos_rot * 65536;

          FT_Matrix matrix;

          matrix.xx =  cos_rot_fx;
          matrix.xy = -sin_rot_fx;
          matrix.yx =  sin_rot_fx;
          matrix.yy =  cos_rot_fx;

          direct_mutex_lock( &library_mutex );

          FT_Set_Transform( face, &matrix, NULL );

          direct_mutex_unlock( &library_mutex );
     }

     if (desc->flags & DFDESC_ATTRIBUTES) {
          if (desc->attributes & DFFA_NOHINTING)
               load_flags |= FT_LOAD_NO_HINTING;
          if (desc->attributes & DFFA_NOBITMAP)
               load_flags |= FT_LOAD_NO_BITMAP;
          if (desc->attributes & DFFA_AUTOHINTING)
               load_flags |= FT_LOAD_FORCE_AUTOHINT;
          if (desc->attributes & DFFA_SOFTHINTING)
               load_flags |= FT_LOAD_TARGET_LIGHT;
          if (desc->attributes & DFFA_VERTICAL_LAYOUT)
               load_flags |= FT_LOAD_VERTICAL_LAYOUT;
     }

     if (dfb_config->font_format == DSPF_A1       ||
         dfb_config->font_format == DSPF_A1_LSB   ||
         dfb_config->font_format == DSPF_ARGB1555 ||
         dfb_config->font_format == DSPF_RGBA5551 ||
         ((desc->flags & DFDESC_ATTRIBUTES) && (desc->attributes & DFFA_MONOCHROME)))
          load_flags |= FT_LOAD_TARGET_MONO;

     if (!((desc->flags & DFDESC_ATTRIBUTES) && (desc->attributes & DFFA_NOCHARMAP))) {
          direct_mutex_lock( &library_mutex );

          err = FT_Select_Charmap( face, ft_encoding_unicode );

          direct_mutex_unlock( &library_mutex );

          if (err) {
               D_DEBUG_AT( Font_FT2, "  -> couldn't select Unicode encoding, falling back to Latin1\n" );

               direct_mutex_lock( &library_mutex );

               err = FT_Select_Charmap( face, ft_encoding_latin_1 );

               direct_mutex_unlock( &library_mutex );
          }

          if (err) {
               D_DEBUG_AT( Font_FT2, "  -> couldn't select Unicode/Latin1 encoding, trying Symbol\n" );

               direct_mutex_lock( &library_mutex );

               err = FT_Select_Charmap( face, ft_encoding_symbol );

               direct_mutex_unlock( &library_mutex );

               if (!err) {
                    mask = 0xF000;
               }
               else {
                    D_ERROR( "Font/FT2: Could not select charmap!\n" );
                    ret = DFB_FAILURE;
                    goto error;
               }
          }
     }

     if (desc->flags & DFDESC_FRACT_HEIGHT)
          fh = desc->fract_height;
     else if (desc->flags & DFDESC_HEIGHT)
          fh = desc->height << 6;

     if (desc->flags & DFDESC_FRACT_WIDTH)
          fw = desc->fract_width;
     else if (desc->flags & DFDESC_WIDTH)
          fw = desc->width << 6;

     direct_mutex_lock( &library_mutex );

     err = FT_Set_Char_Size( face, fw, fh, 0, 0 );

     direct_mutex_unlock( &library_mutex );

     if (err) {
          D_ERROR( "Font/FT2: Could not set pixel size to %dx%d!\n",
                   desc->flags & DFDESC_FRACT_WIDTH  ? desc->fract_width  :
                   desc->flags & DFDESC_WIDTH        ? desc->width        : 0,
                   desc->flags & DFDESC_FRACT_HEIGHT ? desc->fract_height :
                   desc->flags & DFDESC_HEIGHT       ? desc->height       : 0 );
          ret = DFB_FAILURE;
          goto error;
     }

     face->generic.data      = (void*)(long) load_flags;
     face->generic.finalizer = NULL;

     /* Create the font object. */
     ret = dfb_font_create( core, desc, ctx->filename, &font );
     if (ret)
          goto error;

     /* Fill font information. */
     font->attributes = (desc->flags & DFDESC_ATTRIBUTES) ? desc->attributes : DFFA_NONE;

     D_ASSERT( font->pixel_format == DSPF_ARGB     ||
               font->pixel_format == DSPF_ABGR     ||
               font->pixel_format == DSPF_AiRGB    ||
               font->pixel_format == DSPF_ARGB8565 ||
               font->pixel_format == DSPF_ARGB4444 ||
               font->pixel_format == DSPF_RGBA4444 ||
               font->pixel_format == DSPF_ARGB2554 ||
               font->pixel_format == DSPF_ARGB1555 ||
               font->pixel_format == DSPF_RGBA5551 ||
               font->pixel_format == DSPF_A8       ||
               font->pixel_format == DSPF_A4       ||
               font->pixel_format == DSPF_A1       ||
               font->pixel_format == DSPF_A1_LSB   ||
               font->pixel_format == DSPF_LUT2 );

     font->ascender   = face->size->metrics.ascender >> 6;
     font->descender  = face->size->metrics.descender >> 6;
     font->height     = font->ascender - font->descender + 1;
     font->maxadvance = face->size->metrics.max_advance >> 6;
     font->up_unit_x  = -sin_rot;
     font->up_unit_y  = -cos_rot;
     font->flags      = CFF_SUBPIXEL_ADVANCE;

     CORE_FONT_DEBUG_AT( Font_FT2, font );

     D_DEBUG_AT( Font_FT2, "  -> maxadvance = %d, up unit: %5.2f,%5.2f\n",
                 font->maxadvance, font->up_unit_x, font->up_unit_y );

     font->GetGlyphData = get_glyph_info;
     font->RenderGlyph  = render_glyph;

     /* Allocate implementation data. */
     if (FT_HAS_KERNING( face ) && !(font->attributes & DFFA_NOKERNING)) {
          font->GetKerning = get_kerning;
          data = D_CALLOC( 1, sizeof(FT2ImplKerningData) );
     }
     else
          data = D_CALLOC( 1, sizeof(FT2ImplData) );

     if (!data) {
          ret = D_OOM();
          goto error;
     }

     data->face            = face;
     data->disable_charmap = font->attributes & DFFA_NOCHARMAP;

     if (desc->flags & DFDESC_FIXEDADVANCE) {
          data->fixed_advance = desc->fixed_advance;
          font->maxadvance    = desc->fixed_advance;

          if (font->attributes & DFFA_FIXEDCLIP)
               data->fixed_clip = true;
     }

     for (i = 0; i < 256; i++)
          data->indices[i] = FT_Get_Char_Index( face, i | mask );

     if (font->attributes & DFFA_OUTLINED) {
          if (desc->flags & DFDESC_OUTLINE_WIDTH)
               data->outline_radius = 1 + (desc->outline_width >> 16) * 2;
          else
               data->outline_radius = 3;

          if (desc->flags & DFDESC_OUTLINE_OPACITY)
               data->outline_opacity = desc->outline_opacity;
          else
               data->outline_opacity = 0xFF;
     }

     data->up_unit_x = font->up_unit_x;
     data->up_unit_y = font->up_unit_y;

     font->impl_data = data;

     ret = dfb_font_register_encoding( font, "UTF8",   &ft2UTF8Funcs,   DTEID_UTF8 );
     if (ret)
          goto error;

     ret = dfb_font_register_encoding( font, "Latin1", &ft2Latin1Funcs, DTEID_OTHER );
     if (ret)
          goto error;

     IDirectFBFont_Construct( thiz, font );

     thiz->Release = IDirectFBFont_FT2_Release;

     return DFB_OK;

error:
     if (font) {
          if (data)
               D_FREE( data );

          dfb_font_destroy( font );
     }

     if (face) {
          direct_mutex_lock( &library_mutex );
          FT_Done_Face( face );
          direct_mutex_unlock( &library_mutex );
     }

     release_freetype();

     return ret;
}
