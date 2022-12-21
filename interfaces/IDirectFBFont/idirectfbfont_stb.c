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

#include <core/fonts.h>
#include <core/surface_buffer.h>
#include <direct/memcpy.h>
#include <direct/utf8.h>
#include <media/idirectfbfont.h>
#define  STB_TRUETYPE_IMPLEMENTATION
#include STB_TRUETYPE_H

D_DEBUG_DOMAIN( Font_STB, "Font/STB", "STB Font Provider" );

static DFBResult Probe    ( IDirectFBFont_ProbeContext *ctx );

static DFBResult Construct( IDirectFBFont              *thiz,
                            CoreDFB                    *core,
                            IDirectFBFont_ProbeContext *ctx,
                            DFBFontDescription         *desc );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBFont, STB )

/**********************************************************************************************************************/

static DFBResult
stbUTF8GetCharacterIndex( CoreFont     *thiz,
                          unsigned int  character,
                          unsigned int *ret_index )
{
     stbtt_fontinfo *fontinfo = thiz->impl_data;

     D_MAGIC_ASSERT( thiz, CoreFont );
     D_ASSERT( ret_index != NULL );

     *ret_index = stbtt_FindGlyphIndex( fontinfo, character );

     return DFB_OK;
}

static DFBResult
stbUTF8DecodeText( CoreFont     *thiz,
                   const void   *text,
                   int           length,
                   unsigned int *ret_indices,
                   int          *ret_num )
{
     stbtt_fontinfo *fontinfo = thiz->impl_data;
     const u8       *bytes    = text;
     int             pos      = 0;
     int             num      = 0;

     D_MAGIC_ASSERT( thiz, CoreFont );
     D_ASSERT( text != NULL );
     D_ASSERT( length >= 0 );
     D_ASSERT( ret_indices != NULL );
     D_ASSERT( ret_num != NULL );

     while (pos < length) {
          unsigned int c;

          if (bytes[pos] < 128) {
               c = bytes[pos++];
          }
          else {
               c = DIRECT_UTF8_GET_CHAR( &bytes[pos] );
               pos += DIRECT_UTF8_SKIP(bytes[pos]);
          }

          ret_indices[num++] = stbtt_FindGlyphIndex( fontinfo, c );
     }

     *ret_num = num;

     return DFB_OK;
}

static const CoreFontEncodingFuncs stbUTF8Funcs = {
     .GetCharacterIndex = stbUTF8GetCharacterIndex,
     .DecodeText        = stbUTF8DecodeText,
};

/**********************************************************************************************************************/

static DFBResult
get_glyph_info( CoreFont      *thiz,
                unsigned int   index,
                CoreGlyphData *info )
{
     int             advanceWidth;
     float           scale;
     int             x0, y0, x1, y1;
     stbtt_fontinfo *fontinfo = thiz->impl_data;

     scale = stbtt_ScaleForPixelHeight( fontinfo, thiz->description.height );

     stbtt_GetGlyphHMetrics( fontinfo, index, &advanceWidth, NULL );
     stbtt_GetGlyphBitmapBox( fontinfo, index, scale, scale, &x0, &y0, &x1, &y1 );

     info->xadvance = advanceWidth * 256 * stbtt_ScaleForMappingEmToPixels( fontinfo, thiz->description.height );
     info->width    = x1 - x0;
     info->height   = y1 - y0;

     return DFB_OK;
}

static DFBResult
render_glyph( CoreFont      *thiz,
              unsigned int   index,
              CoreGlyphData *info )
{
     DFBResult              ret;
     unsigned char         *bitmap;
     float                  scale;
     int                    x0, y0;
     int                    y;
     u8                    *src;
     stbtt_fontinfo        *fontinfo = thiz->impl_data;
     CoreSurface           *surface  = info->surface;
     CoreSurfaceBufferLock  lock;

     bitmap = D_CALLOC( info->height, info->width );

     scale = stbtt_ScaleForPixelHeight( fontinfo, thiz->description.height );

     stbtt_MakeGlyphBitmap( fontinfo, bitmap, info->width, info->height, info->width, scale, scale, index );

     ret = dfb_surface_lock_buffer( surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
     if (ret) {
          D_DERROR( ret, "Font/STB: Unable to lock surface!\n" );
          return ret;
     }

     if (info->width + info->start > surface->config.size.w)
          info->width = surface->config.size.w - info->start;

     if (info->height > surface->config.size.h)
          info->height = surface->config.size.h;

     stbtt_GetGlyphBitmapBox( fontinfo, index, scale, scale, &x0, &y0, NULL, NULL );

     info->left = x0 - thiz->ascender * thiz->up_unit_x;
     info->top  = y0 - thiz->ascender * thiz->up_unit_y - 1;

     src = bitmap;

     lock.addr += DFB_BYTES_PER_LINE( surface->config.format, info->start );

     for (y = 0; y < info->height; y++) {
          direct_memcpy( lock.addr, src, info->width );

          src += info->width;
          lock.addr += lock.pitch;
     }

     dfb_surface_unlock_buffer( surface, &lock );

     D_FREE( bitmap );

     return DFB_OK;
}

/**********************************************************************************************************************/

static void
IDirectFBFont_STB_Destruct( IDirectFBFont *thiz )
{
     stbtt_fontinfo *fontinfo = ((IDirectFBFont_data*) thiz->priv)->font->impl_data;

     D_DEBUG_AT( Font_STB, "%s( %p )\n", __FUNCTION__, thiz );

     D_FREE( fontinfo );

     IDirectFBFont_Destruct( thiz );
}

static DirectResult
IDirectFBFont_STB_Release( IDirectFBFont *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBFont )

     D_DEBUG_AT( Font_STB, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBFont_STB_Destruct( thiz );

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBFont_ProbeContext *ctx )
{
     int            err;
     stbtt_fontinfo fontinfo;

     if (!ctx->content)
          return DFB_UNSUPPORTED;

     err = stbtt_InitFont( &fontinfo, ctx->content, 0 );

     return err ? DFB_OK : DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBFont              *thiz,
           CoreDFB                    *core,
           IDirectFBFont_ProbeContext *ctx,
           DFBFontDescription         *desc )
{
     DFBResult       ret;
     int             err;
     int             ascent, descent;
     stbtt_fontinfo *fontinfo = NULL;
     CoreFont       *font     = NULL;

     D_DEBUG_AT( Font_STB, "%s( %p )\n", __FUNCTION__, thiz );

     /* Check for valid description. */
     if (!(desc->flags & DFDESC_HEIGHT))
          return DFB_INVARG;

     if (desc->flags & DFDESC_ROTATION)
          return DFB_UNSUPPORTED;

     D_DEBUG_AT( Font_STB, "  -> file '%s' at pixel height %d\n", ctx->filename, desc->height );

     /* Open the font loaded into memory. */
     fontinfo = D_CALLOC( 1, sizeof(stbtt_fontinfo) );
     if (!fontinfo) {
          ret = D_OOM();
          goto error;
     }

     err = stbtt_InitFont( fontinfo, ctx->content, 0 );
     if (!err) {
          D_ERROR( "Font/STB: Failed to load font file '%s'!\n", ctx->filename );
          ret = DFB_FAILURE;
          goto error;
     }

     /* Create the font object. */
     ret = dfb_font_create( core, desc, ctx->filename, &font );
     if (ret)
          goto error;

     /* Fill font information. */
     stbtt_GetFontVMetrics( fontinfo, &ascent, &descent, NULL );

     font->ascender  = ceil(  ascent  * stbtt_ScaleForMappingEmToPixels( fontinfo, desc->height ) );
     font->descender = floor( descent * stbtt_ScaleForMappingEmToPixels( fontinfo, desc->height ) );
     font->height    = font->ascender - font->descender + 1;
     font->up_unit_x =  0.0;
     font->up_unit_y = -1.0;
     font->flags     = CFF_SUBPIXEL_ADVANCE;

     CORE_FONT_DEBUG_AT( Font_STB, font );

     font->GetGlyphData = get_glyph_info;
     font->RenderGlyph  = render_glyph;

     font->impl_data = fontinfo;

     ret = dfb_font_register_encoding( font, "UTF8", &stbUTF8Funcs, DTEID_UTF8 );
     if (ret)
          goto error;

     IDirectFBFont_Construct( thiz, font );

     thiz->Release = IDirectFBFont_STB_Release;

     return DFB_OK;

error:
     if (font)
          dfb_font_destroy( font );

     if (fontinfo)
          D_FREE( fontinfo );

     return ret;
}
