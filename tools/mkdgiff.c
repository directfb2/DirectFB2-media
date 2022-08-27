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
#include <dgiff.h>
#include <directfb_strings.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define MAX_SIZE_COUNT  256
#define MAX_ROW_WIDTH  2047

static const DirectFBPixelFormatNames(format_names);

static const char            *filename      = NULL;
static bool                   debug         = false;
static DFBSurfacePixelFormat  format        = DSPF_A8;
static bool                   premultiplied = false;
static int                    size_count    = 0;
static int                    face_sizes[MAX_SIZE_COUNT];

#define DEBUG(...)                             \
     do {                                      \
          if (debug)                           \
               fprintf( stderr, __VA_ARGS__ ); \
     } while (0)

/**********************************************************************************************************************/

static void print_usage()
{
     int i = 0;

     fprintf( stderr, "DirectFB Glyph Image File Format Tool\n\n" );
     fprintf( stderr, "Usage: mkdgiff [options] font\n\n" );
     fprintf( stderr, "Options:\n\n" );
     fprintf( stderr, "  -d, --debug                 Output debug information.\n" );
     fprintf( stderr, "  -f, --format <pixelformat>  Choose the pixel format (default A8).\n" );
     fprintf( stderr, "  -p, --premultiplied         Use premultiplied alpha (default false, only for ARGB/ABGR).\n" );
     fprintf( stderr, "  -s, --sizes  <s1>[,s2...]   Set sizes to generate glyph images.\n" );
     fprintf( stderr, "  -h, --help                  Show this help message.\n\n" );
     fprintf( stderr, "Supported pixel formats:\n\n" );
     while (format_names[i].format != DSPF_UNKNOWN) {
          DFBSurfacePixelFormat format = format_names[i].format;
          if ( DFB_PIXELFORMAT_HAS_ALPHA ( format ) &&
              !DFB_PIXELFORMAT_IS_INDEXED( format ) &&
              !DFB_COLOR_IS_YUV          ( format )) {
               fprintf( stderr, "  %-10s %2d bits\n", format_names[i].name, DFB_BITS_PER_PIXEL( format ) );
          }
          ++i;
     }
     fprintf( stderr, "\n" );
}

static DFBBoolean parse_format( const char *arg )
{
     int i = 0;

     while (format_names[i].format != DSPF_UNKNOWN) {
          if (!strcasecmp( arg, format_names[i].name )              &&
               DFB_PIXELFORMAT_HAS_ALPHA ( format_names[i].format ) &&
              !DFB_PIXELFORMAT_IS_INDEXED( format_names[i].format ) &&
              !DFB_COLOR_IS_YUV          ( format_names[i].format )) {
               format = format_names[i].format;
               return DFB_TRUE;
          }

          ++i;
     }

     fprintf( stderr, "Invalid pixel format specified!\n" );

     return DFB_FALSE;
}

static DFBBoolean parse_sizes( const char *arg )
{
     int i    = 0;
     int size = 0;

     for (i = 0; arg[i]; i++) {
          switch (arg[i]) {
               case '0' ... '9':
                    if (size_count == MAX_SIZE_COUNT) {
                         fprintf( stderr, "Maximum number of sizes (%d) exceeded!\n", MAX_SIZE_COUNT );
                         return DFB_FALSE;
                    }
                    size = size * 10 + arg[i] - '0';
                    break;

               case ',':
                    if (size) {
                         face_sizes[size_count++] = size;
                         size = 0;
                    }
                    break;

               default:
                    fprintf( stderr, "Invalid character used in sizes argument!\n" );
                    return DFB_FALSE;
          }
     }

     if (size)
          face_sizes[size_count++] = size;

     return DFB_TRUE;
}

static DFBBoolean parse_command_line( int argc, char *argv[] )
{
     int n;

     for (n = 1; n < argc; n++) {
          const char *arg = argv[n];

          if (strcmp( arg, "-h" ) == 0 || strcmp( arg, "--help" ) == 0) {
               print_usage();
               return DFB_FALSE;
          }

          if (strcmp( arg, "-d" ) == 0 || strcmp( arg, "--debug" ) == 0) {
               debug = true;
               continue;
          }

          if (strcmp( arg, "-f" ) == 0 || strcmp( arg, "--format" ) == 0) {
               if (++n == argc) {
                    print_usage();
                    return DFB_FALSE;
               }

               if (!parse_format( argv[n] ))
                    return DFB_FALSE;

               continue;
          }

          if (strcmp( arg, "-p" ) == 0 || strcmp( arg, "--premultiplied" ) == 0) {
               premultiplied = true;
               continue;
          }

          if (strcmp( arg, "-s" ) == 0 || strcmp( arg, "--sizes" ) == 0) {
               if (++n == argc) {
                    print_usage();
                    return DFB_FALSE;
               }

               if (!parse_sizes( argv[n] ))
                    return DFB_FALSE;

               continue;
          }

          if (filename || access( arg, R_OK )) {
               print_usage();
               return DFB_FALSE;
          }

          filename = arg;
     }

     if (!filename) {
          print_usage();
          return DFB_FALSE;
     }

     return DFB_TRUE;
}

/**********************************************************************************************************************/

static FT_Error write_glyph( DGIFFGlyphInfo *glyph, FT_GlyphSlot slot, void *dst, int pitch )
{
     int  y;
     u8  *src = slot->bitmap.buffer;

     DEBUG( "  ->   %s( %p, %p, %p, %d ) <- width %d\n", __FUNCTION__, glyph, slot, dst, pitch, glyph->width );

     for (y = 0; y < glyph->height; y++) {
          int  i, j, n;
          u8  *dst8  = dst;
          u16 *dst16 = dst;
          u32 *dst32 = dst;

          switch (slot->bitmap.pixel_mode) {
               case ft_pixel_mode_grays:
                    switch (format) {
                         case DSPF_ABGR:
                         case DSPF_ARGB:
                              if (premultiplied) {
                                   for (i = 0; i < glyph->width; i++)
                                        dst32[i] = (src[i] << 24) | (src[i] << 16) | (src[i] <<  8) | src[i];
                              }
                              else {
                                   for (i = 0; i < glyph->width; i++)
                                        dst32[i] = (src[i] << 24) | 0xFFFFFF;
                              }
                              break;
                         case DSPF_AiRGB:
                              for (i = 0; i < glyph->width; i++)
                                   dst32[i] = ((src[i] ^ 0xFF) << 24) | 0xFFFFFF;
                              break;
                         case DSPF_ARGB8565:
                              for (i = 0, j = -1; i < glyph->width; ++i) {
                                   u32 d = (src[i] << 16) | 0xFFFF;
#ifdef WORDS_BIGENDIAN
                                   dst8[++j] = (d >> 16) & 0xFF;
                                   dst8[++j] = (d >>  8) & 0xFF;
                                   dst8[++j] =  d        & 0xFF;
#else
                                   dst8[++j] =  d        & 0xFF;
                                   dst8[++j] = (d >>  8) & 0xFF;
                                   dst8[++j] = (d >> 16) & 0xFF;
#endif
                              }
                              break;
                         case DSPF_ARGB1666:
                         case DSPF_ARGB6666:
                              for (i = 0, j = -1; i < glyph->width; ++i) {
                                   u32 d = (src[i] << 16) | 0x3FFFF;
#ifdef WORDS_BIGENDIAN
                                   dst8[++j] = (d >> 16) & 0xFF;
                                   dst8[++j] = (d >>  8) & 0xFF;
                                   dst8[++j] =  d        & 0xFF;
#else
                                   dst8[++j] =  d        & 0xFF;
                                   dst8[++j] = (d >>  8) & 0xFF;
                                   dst8[++j] = (d >> 16) & 0xFF;
#endif
                              }
                              break;
                         case DSPF_ARGB4444:
                              for (i = 0; i < glyph->width; i++)
                                   dst16[i] = (src[i] << 8) | 0xFFF;
                              break;
                         case DSPF_RGBA4444:
                              for (i = 0; i < glyph->width; i++)
                                   dst16[i] = 0xFFF0 | ((src[i] & 0xF0) >> 4);
                              break;
                         case DSPF_ARGB2554:
                              for (i = 0; i < glyph->width; i++)
                                   dst16[i] = (src[i] << 8) | 0x3FFF;
                              break;
                         case DSPF_ARGB1555:
                              for (i = 0; i < glyph->width; i++)
                                   dst16[i] = (src[i] << 8) | 0x7FFF;
                              break;
                         case DSPF_RGBA5551:
                              for (i = 0; i < glyph->width; i++)
                                   dst16[i] = 0xFFFE | ((src[i] & 0x80) >> 7);
                              break;
                         case DSPF_RGBAF88871:
                              for (i = 0; i < glyph->width; i++)
                                   dst32[i] = 0xFFFFFF00 | (src[i] & 0xFE);
                              break;
                         case DSPF_A8:
                              memcpy( dst, src, glyph->width );
                              break;
                         case DSPF_A4:
                              for (i = 0, j = 0; i < glyph->width; i += 2, j++)
                                   dst8[j] = (src[i] & 0xF0) | (src[i+1] >> 4);
                              break;
                         case DSPF_A1:
                              for (i = 0, j = 0; i < glyph->width; ++j) {
                                   u8 p = 0;
                                   for (n = 0; n < 8 && i < glyph->width; ++i, ++n)
                                        p |= (src[i] & 0x80) >> n;
                                   dst8[j] = p;
                              }
                              break;
                         case DSPF_A1_LSB:
                              for (i = 0, j = 0; i < glyph->width; ++j) {
                                   u8 p = 0;
                                   for (n = 0; n < 8 && i < glyph->width; ++i, ++n)
                                        p |= (src[i] & 0x80) >> (7 - n);
                                   dst8[j] = p;
                              }
                              break;
                         default:
                              fprintf( stderr, "Unsupported format for glyph rendering!\n" );
                              return FT_Err_Cannot_Render_Glyph;
                    }
                    break;

               case ft_pixel_mode_mono:
                    switch (format) {
                         case DSPF_ABGR:
                         case DSPF_ARGB:
                              if (premultiplied) {
                                   for (i = 0; i < glyph->width; i++)
                                        dst32[i] = (src[i>>3] & (1 << (7 - (i % 8)))) ? 0xFFFFFFFF : 0x00000000;
                              }
                              else {
                                   for (i = 0; i < glyph->width; i++)
                                        dst32[i] = ((src[i>>3] & (1 << (7 - (i % 8))) ? 0xFF : 0x00) << 24) | 0xFFFFFF;
                              }
                              break;
                         case DSPF_AiRGB:
                              for (i = 0; i < glyph->width; i++)
                                   dst32[i] = ((src[i>>3] & (1 << (7 - (i % 8))) ? 0x00 : 0xFF) << 24) | 0xFFFFFF;
                              break;
                         case DSPF_ARGB8565:
                              for (i = 0, j = -1; i < glyph->width; ++i) {
                                   u32 d = ((src[i>>3] & (1 << (7 - (i % 8))) ? 0xFF : 0x00) << 16) | 0xFFFF;
#ifdef WORDS_BIGENDIAN
                                   dst8[++j] = (d >> 16) & 0xFF;
                                   dst8[++j] = (d >>  8) & 0xFF;
                                   dst8[++j] =  d        & 0xFF;
#else
                                   dst8[++j] =  d        & 0xFF;
                                   dst8[++j] = (d >>  8) & 0xFF;
                                   dst8[++j] = (d >> 16) & 0xFF;
#endif
                              }
                              break;
                         case DSPF_ARGB1666:
                         case DSPF_ARGB6666:
                              for (i = 0, j = -1; i < glyph->width; ++i) {
                                   u32 d = ((src[i>>3] & (1 << (7 - (i % 8))) ? 0x3F : 0x00) << 18) | 0x3FFFF;
#ifdef WORDS_BIGENDIAN
                                   dst8[++j] = (d >> 16) & 0xFF;
                                   dst8[++j] = (d >>  8) & 0xFF;
                                   dst8[++j] =  d        & 0xFF;
#else
                                   dst8[++j] =  d        & 0xFF;
                                   dst8[++j] = (d >>  8) & 0xFF;
                                   dst8[++j] = (d >> 16) & 0xFF;
#endif
                              }
                              break;
                         case DSPF_ARGB4444:
                              for (i = 0; i < glyph->width; i++)
                                   dst16[i] = ((src[i>>3] & (1 << (7 - (i % 8))) ? 0xF : 0x0) << 12) | 0xFFF;
                              break;
                         case DSPF_RGBA4444:
                              for (i = 0; i < glyph->width; i++)
                                   dst16[i] = 0xFFF0 | (src[i>>3] & (1 << (7 - (i % 8))) ? 0xF : 0x0);
                              break;
                         case DSPF_ARGB2554:
                              for (i = 0; i < glyph->width; i++)
                                   dst16[i] = ((src[i>>3] & (1 << (7 - (i % 8))) ? 0x3 : 0x0) << 14) | 0x3FFF;
                              break;
                         case DSPF_ARGB1555:
                              for (i = 0; i < glyph->width; i++)
                                   dst16[i] = ((src[i>>3] & (1 << (7 - (i % 8))) ? 0x1 : 0x0) << 15) | 0x7FFF;
                              break;
                         case DSPF_RGBA5551:
                              for (i = 0; i < glyph->width; i++)
                                   dst16[i] = 0xFFFE | (src[i>>3] & (1 << (7 - (i % 8))) ? 0x1 : 0x0);
                              break;
                         case DSPF_RGBAF88871:
                              for (i = 0; i < glyph->width; i++)
                                   dst32[i] = 0xFFFFFF00 | (src[i>>3] & (1 << (7 - (i % 8))) ? 0xFE : 0x0);
                              break;
                         case DSPF_A8:
                              for (i = 0; i < glyph->width; i++)
                                   dst8[i] = (src[i>>3] & (1 << (7 - (i % 8)))) ? 0xFF : 0x00;
                              break;
                         case DSPF_A4:
                              for (i = 0, j = 0; i < glyph->width; i += 2, j++)
                                   dst8[j] = (src[i>>3]     & (1 << (7 -  (i      % 8))) ? 0xF0 : 0x00) |
                                             (src[(i+1)>>3] & (1 << (7 - ((i + 1) % 8))) ? 0x0F : 0x00);
                              break;
                         case DSPF_A1:
                              memcpy( dst, src, DFB_BYTES_PER_LINE(DSPF_A1, glyph->width) );
                              break;
                         case DSPF_A1_LSB:
                              for (i = 0, j = 0; i < glyph->width; ++j) {
                                   u8 p = 0;
                                   for (n = 0; n < 8 && i < glyph->width; ++i, ++n)
                                        p |= ((src[i] >> n) & 1) << (7 - n);
                                   dst8[j] = p;
                              }
                              break;
                         default:
                              fprintf( stderr, "Unsupported format for glyph rendering!\n" );
                              return FT_Err_Cannot_Render_Glyph;
                    }
                    break;

               default:
                    break;

          }

          src += slot->bitmap.pitch;
          dst += pitch;
     }

     return FT_Err_Ok;
}

static FT_Error do_face( FT_Face face, int size )
{
     FT_Error          ret;
     int               i;
     FT_ULong          code;
     FT_UInt           index;
     DGIFFFaceHeader   header;
     DGIFFGlyphInfo   *glyphs;
     DGIFFGlyphRow    *rows;
     void            **row_data;
     int               align        = DFB_PIXELFORMAT_ALIGNMENT( format );
     int               next_face    = sizeof(DGIFFFaceHeader);
     int               num_glyphs   = 0;
     int               num_rows     = 1;
     int               row_index    = 0;
     int               row_offset   = 0;
     int               total_height = 0;

     DEBUG( "%s( %p, %d ) <- %ld glyphs\n", __FUNCTION__, face, size, face->num_glyphs );

     /* Clear to not leak any data into file. */
     memset( &header, 0, sizeof(header) );

     /* Set the desired size. */
     ret = FT_Set_Char_Size( face, 0, size << 6, 0, 0 );
     if (ret) {
          fprintf( stderr, "Could not set pixel size to %d!\n", size );
          return ret;
     }

     glyphs   = calloc( face->num_glyphs, sizeof(DGIFFGlyphInfo) );
     rows     = calloc( face->num_glyphs, sizeof(DGIFFGlyphRow) );
     row_data = calloc( face->num_glyphs, sizeof(void*) );

     for (code = FT_Get_First_Char( face, &index ); index; code = FT_Get_Next_Char( face, code, &index )) {
          FT_GlyphSlot    slot;
          DGIFFGlyphInfo *glyph = &glyphs[num_glyphs];
          DGIFFGlyphRow  *row   = &rows[num_rows - 1];

          DEBUG( "  -> code %3lu - index %3u\n", code, index );

          if (num_glyphs == face->num_glyphs) {
               fprintf( stderr, "Actual number of characters is bigger than number of glyphs!\n" );
               goto out;
          }

          ret = FT_Load_Glyph( face, index, FT_LOAD_RENDER );
          if (ret) {
               fprintf( stderr, "Could not render glyph for character index %u!\n", index );
               goto out;
          }

          slot = face->glyph;

          glyph->unicode = code;
          glyph->width   = slot->bitmap.width;
          glyph->height  = slot->bitmap.rows;
          glyph->left    = slot->bitmap_left;
          glyph->top     = (face->size->metrics.ascender >> 6) - slot->bitmap_top;
          glyph->advance = slot->advance.x >> 6;

          num_glyphs++;

          if (row->width > 0 && row->width + glyph->width > MAX_ROW_WIDTH) {
               num_rows++;
               row++;
          }

          row->width += (glyph->width + align) & ~align;

          if (row->height < glyph->height)
               row->height = glyph->height;
     }

     for (i = 0; i < num_rows; i++) {
          DGIFFGlyphRow *row = &rows[i];

          DEBUG( "  ->   row %d, width %d, height %d\n", i, row->width, row->height );

          total_height += row->height;

          row->pitch = (DFB_BYTES_PER_LINE( format, row->width ) + 7) & ~7;

          row_data[i] = calloc( row->height, row->pitch );

          next_face += row->height * row->pitch;
     }

     DEBUG( "  -> %d glyphs, %d rows, total height %d\n", num_glyphs, num_rows, total_height );

     next_face += num_glyphs * sizeof(DGIFFGlyphInfo);
     next_face += num_rows * sizeof(DGIFFGlyphRow);

     for (i = 0; i < num_glyphs; i++) {
          DGIFFGlyphInfo *glyph = &glyphs[i];

          DEBUG( "  -> reloading character 0x%x (%d)\n", glyph->unicode, i );

          ret = FT_Load_Char( face, glyph->unicode, FT_LOAD_RENDER );
          if (ret) {
               fprintf( stderr, "Could not render glyph for unicode character 0x%x!\n", glyph->unicode );
               goto out;
          }

          if (row_offset > 0 && row_offset + glyph->width > MAX_ROW_WIDTH) {
               row_index++;
               row_offset = 0;
          }

          DEBUG( "  -> row offset %d\n", row_offset );

          ret = write_glyph( glyph, face->glyph,
                             row_data[row_index] + DFB_BYTES_PER_LINE( format, row_offset ), rows[row_index].pitch );
          if (ret) {
               fprintf( stderr, "Could not write glyph!\n" );
               goto out;
          }

          glyph->row    = row_index;
          glyph->offset = row_offset;

          row_offset += (glyph->width + align) & ~align;
     }

     D_ASSERT( row_index == num_rows - 1 );

     header.next_face   = next_face;
     header.size        = size;
     header.ascender    = face->size->metrics.ascender >> 6;
     header.descender   = face->size->metrics.descender >> 6;
     header.height      = header.ascender - header.descender + 1;
     header.max_advance = face->size->metrics.max_advance >> 6;
     header.pixelformat = format;
     header.num_glyphs  = num_glyphs;
     header.num_rows    = num_rows;

     DEBUG( "  -> ascender %d, descender %d\n", header.ascender, header.descender );
     DEBUG( "  -> height %d, max advance %d\n", header.height, header.max_advance );

     fwrite( &header, sizeof(header), 1, stdout );

     fwrite( glyphs, sizeof(*glyphs), num_glyphs, stdout );

     for (i = 0; i < num_rows; i++) {
          DGIFFGlyphRow *row = &rows[i];

          fwrite( row, sizeof(*row), 1, stdout );

          fwrite( row_data[i], row->pitch, row->height, stdout );
     }

out:
     for (i = 0; i < num_rows; i++) {
          if (row_data[i])
               free( row_data[i] );
     }

     free( row_data );
     free( rows );
     free( glyphs );

     return ret;
}

/**********************************************************************************************************************/

static DGIFFHeader header = {
     magic: { 'D', 'G', 'I', 'F', 'F'},
     major: 0,
     minor: 0,
     flags: 0x01,
};

int main( int argc, char *argv[] )
{
     FT_Error   ret;
     int        i;
     FT_Library library = NULL;
     FT_Face    face    = NULL;

     /* Parse the command line. */
     if (!parse_command_line( argc, argv ))
          return -1;

     if (premultiplied && (format != DSPF_ARGB || format != DSPF_ABGR)) {
          fprintf( stderr, "Premultiplied alpha only implemented for ARGB or ABGR!\n" );
          return -2;
     }

     if (!size_count) {
          DEBUG( "Using default font sizes 8, 10, 12, 16, 22, 32\n" );

          size_count = 6;

          face_sizes[0] =  8;
          face_sizes[1] = 10;
          face_sizes[2] = 12;
          face_sizes[3] = 16;
          face_sizes[4] = 22;
          face_sizes[5] = 32;
     }
     else {
          DEBUG( "Using font sizes" );
          for (i = 0; i < size_count - 1; i++)
               DEBUG( " %d,", face_sizes[i] );
          DEBUG( " %d\n", face_sizes[size_count-1] );
     }

     header.num_faces = size_count;

     ret = FT_Init_FreeType( &library );
     if (ret) {
          fprintf( stderr, "Initialization of the FreeType2 library failed!\n" );
          goto out;
     }

     ret = FT_New_Face( library, filename, 0, &face );
     if (ret) {
          if (ret == FT_Err_Unknown_File_Format)
               fprintf( stderr, "Unsupported font format!\n" );
          else
               fprintf( stderr, "Failed loading face!\n" );

          goto out;
     }

     ret = FT_Select_Charmap( face, ft_encoding_unicode );
     if (ret) {
          fprintf( stderr, "Couldn't select Unicode encoding, falling back to Latin1!\n" );

          ret = FT_Select_Charmap( face, ft_encoding_latin_1 );
          if (ret)
               fprintf( stderr, "Couldn't even select Latin1 encoding!\n" );
     }

     fwrite( &header, sizeof(header), 1, stdout );

     DEBUG( "Writing font\n" );
     for (i = 0; i < size_count; i++) {
          ret = do_face( face, face_sizes[i] );
          if (ret)
               goto out;
     }

out:
     if (face)
          FT_Done_Face( face );

     if (library)
          FT_Done_FreeType( library );

     return ret;
}
