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
#include <schrift.h>

D_DEBUG_DOMAIN( Font_Schrift, "Font/Schrift", "Schrift Font Provider" );

static DFBResult Probe    ( IDirectFBFont_ProbeContext *ctx );

static DFBResult Construct( IDirectFBFont              *thiz,
                            CoreDFB                    *core,
                            IDirectFBFont_ProbeContext *ctx,
                            DFBFontDescription         *desc );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBFont, Schrift )

/**********************************************************************************************************************/

static DFBResult
sftUTF8GetCharacterIndex( CoreFont     *thiz,
                          unsigned int  character,
                          unsigned int *ret_index )
{
     SFT *sft = thiz->impl_data;

     D_MAGIC_ASSERT( thiz, CoreFont );
     D_ASSERT( ret_index != NULL );

     sft_lookup( sft, character, (SFT_Glyph*) ret_index );

     return DFB_OK;
}

static DFBResult
sftUTF8DecodeText( CoreFont     *thiz,
                   const void   *text,
                   int           length,
                   unsigned int *ret_indices,
                   int          *ret_num )
{
     SFT      *sft   = thiz->impl_data;
     const u8 *bytes = text;
     int       pos   = 0;
     int       num   = 0;

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

          sft_lookup( sft, c, (SFT_Glyph*) &ret_indices[num++] );
     }

     *ret_num = num;

     return DFB_OK;
}

static const CoreFontEncodingFuncs sftUTF8Funcs = {
     .GetCharacterIndex = sftUTF8GetCharacterIndex,
     .DecodeText        = sftUTF8DecodeText,
};

/**********************************************************************************************************************/

static DFBResult
get_glyph_info( CoreFont      *thiz,
                unsigned int   index,
                CoreGlyphData *info )
{
     SFT_GMetrics  metrics;
     SFT          *sft = thiz->impl_data;

     sft_gmetrics( sft, index, &metrics );

     info->xadvance = metrics.advanceWidth * 256;
     info->width    = metrics.minWidth;
     info->height   = metrics.minHeight;

     return DFB_OK;
}

static DFBResult
render_glyph( CoreFont      *thiz,
              unsigned int   index,
              CoreGlyphData *info )
{
     DFBResult              ret;
     SFT_Image              image;
     SFT_GMetrics           metrics;
     int                    y;
     u8                    *src;
     SFT                   *sft     = thiz->impl_data;
     CoreSurface           *surface = info->surface;
     CoreSurfaceBufferLock  lock;

     image.pixels = D_CALLOC( info->height, info->width );
     image.width  = info->width;
     image.height = info->height;

     sft_render( sft, index, image );

     ret = dfb_surface_lock_buffer( surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
     if (ret) {
          D_DERROR( ret, "Font/Schrift: Unable to lock surface!\n" );
          return ret;
     }

     if (info->width + info->start > surface->config.size.w)
          info->width = surface->config.size.w - info->start;

     if (info->height > surface->config.size.h)
          info->height = surface->config.size.h;

     sft_gmetrics( sft, index, &metrics );

     info->left = metrics.leftSideBearing - thiz->ascender * thiz->up_unit_x;
     info->top  = metrics.yOffset         - thiz->ascender * thiz->up_unit_y;

     src = image.pixels;

     lock.addr += DFB_BYTES_PER_LINE( surface->config.format, info->start );

     for (y = 0; y < info->height; y++) {
          direct_memcpy( lock.addr, src, info->width );

          src += info->width;
          lock.addr += lock.pitch;
     }

     dfb_surface_unlock_buffer( surface, &lock );

     D_FREE( image.pixels );

     return DFB_OK;
}

/**********************************************************************************************************************/

static void
IDirectFBFont_Schrift_Destruct( IDirectFBFont *thiz )
{
     SFT *sft = ((IDirectFBFont_data*) thiz->priv)->font->impl_data;

     D_DEBUG_AT( Font_Schrift, "%s( %p )\n", __FUNCTION__, thiz );

     sft_freefont( sft->font );

     D_FREE( sft );

     IDirectFBFont_Destruct( thiz );
}

static DirectResult
IDirectFBFont_Schrift_Release( IDirectFBFont *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBFont )

     D_DEBUG_AT( Font_Schrift, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBFont_Schrift_Destruct( thiz );

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBFont_ProbeContext *ctx )
{
     SFT_Font *font;

     /* Check for valid filename. */
     if (!ctx->filename)
          return DFB_UNSUPPORTED;

     font = sft_loadfile( ctx->filename );
     if (!font)
          return DFB_UNSUPPORTED;

     sft_freefont( font );

     return DFB_OK;
}

static DFBResult
Construct( IDirectFBFont              *thiz,
           CoreDFB                    *core,
           IDirectFBFont_ProbeContext *ctx,
           DFBFontDescription         *desc )
{
     DFBResult     ret;
     SFT_LMetrics  metrics;
     SFT          *sft  = NULL;
     CoreFont     *font = NULL;

     D_DEBUG_AT( Font_Schrift, "%s( %p )\n", __FUNCTION__, thiz );

     /* Check for valid description. */
     if (!(desc->flags & DFDESC_HEIGHT))
          return DFB_INVARG;

     if (desc->flags & DFDESC_ROTATION)
          return DFB_UNSUPPORTED;

     D_DEBUG_AT( Font_Schrift, "  -> font at pixel height %d\n", desc->height );

     /* Open the file. */
     sft = D_CALLOC( 1, sizeof(SFT) );
     if (!sft) {
          ret = D_OOM();
          goto error;
     }

     sft->font = sft_loadfile( ctx->filename );
     if (!sft->font) {
          D_ERROR( "Font/Schrift: Failed to load font file '%s'!\n", ctx->filename );
          ret = DFB_FAILURE;
          goto error;
     }

     sft->xScale = desc->height;
     sft->yScale = desc->height;
     sft->flags  = SFT_DOWNWARD_Y;

     /* Create the font object. */
     ret = dfb_font_create( core, desc, &font );
     if (ret)
          goto error;

     /* Fill font information. */
     sft_lmetrics( sft, &metrics );

     font->ascender  = (metrics.ascender  - (int) metrics.ascender  > 0) ? metrics.ascender  + 1 : metrics.ascender;
     font->descender = (metrics.descender - (int) metrics.descender < 0) ? metrics.descender - 1 : metrics.ascender;
     font->height    = font->ascender - font->descender + 1;
     font->up_unit_x =  0.0;
     font->up_unit_y = -1.0;
     font->flags     = CFF_SUBPIXEL_ADVANCE;

     CORE_FONT_DEBUG_AT( Font_Schrift, font );

     font->GetGlyphData = get_glyph_info;
     font->RenderGlyph  = render_glyph;

     font->impl_data = sft;

     ret = dfb_font_register_encoding( font, "UTF8", &sftUTF8Funcs, DTEID_UTF8 );
     if (ret)
          goto error;

     IDirectFBFont_Construct( thiz, font );

     thiz->Release = IDirectFBFont_Schrift_Release;

     return DFB_OK;

error:
     if (font)
          dfb_font_destroy( font );

     if (sft)
          D_FREE( sft );

     return ret;
}
