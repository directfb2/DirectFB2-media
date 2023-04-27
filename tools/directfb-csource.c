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
#include <directfb_strings.h>
#include <gfx/convert.h>
#include <png.h>

static const DirectFBPixelFormatNames(format_names);

static const char            *filename    = NULL;
static DFBSurfacePixelFormat  format      = DSPF_UNKNOWN;
static bool                   rawdata     = false;
static DFBColor               transparent = { .a = 0 };
static const char            *name        = NULL;

/**********************************************************************************************************************/

static void print_usage()
{
     int i = 0;

     fprintf( stderr, "Data Header File Generation Utility for DirectFB Code\n\n" );
     fprintf( stderr, "Usage: directfb-csource [options] <filename>\n\n" );
     fprintf( stderr, "  --format=<pixelformat>    Choose the pixel format.\n" );
     fprintf( stderr, "  --raw                     Dump file directly to header\n" );
     fprintf( stderr, "  --transparent=<AARRGGBB>  Set completely transparent pixels to this color value.\n" );
     fprintf( stderr, "  --name=<identifer>        Specifies the identifier name for the generated variables.\n" );
     fprintf( stderr, "  --help                    Show this help message.\n\n");
     fprintf( stderr, "Supported pixel formats:\n\n" );
     while (format_names[i].format != DSPF_UNKNOWN) {
          if (  DFB_BYTES_PER_PIXEL       ( format_names[i].format ) >= 1                                    &&
              (!DFB_PIXELFORMAT_IS_INDEXED( format_names[i].format ) || format_names[i].format == DSPF_LUT8) &&
               !DFB_COLOR_IS_YUV          ( format_names[i].format )) {
               fprintf( stderr, "  %-10s %2d bits\n",
                        format_names[i].name, DFB_BITS_PER_PIXEL( format_names[i].format ) );
          }
          ++i;
     }
     fprintf( stderr, "\n" );
}

static DFBBoolean parse_format( const char *arg )
{
     int i = 0;

     while (format_names[i].format != DSPF_UNKNOWN) {
          if (!strcasecmp( arg, format_names[i].name )                                                       &&
                DFB_BYTES_PER_PIXEL       ( format_names[i].format ) >= 1                                     &&
              (!DFB_PIXELFORMAT_IS_INDEXED( format_names[i].format ) || format_names[i].format == DSPF_LUT8) &&
               !DFB_COLOR_IS_YUV          ( format_names[i].format )) {
               format = format_names[i].format;
               return DFB_TRUE;
          }
          ++i;
     }

     fprintf( stderr, "Invalid pixel format specified!\n" );

     return DFB_FALSE;
}

static DFBBoolean parse_transparent( const char *arg )
{
     char *error;
     u32   argb;

     argb = strtoul( arg, &error, 16 );

     if (*error) {
          fprintf( stderr, "Invalid transparent color specified!\n" );
          return DFB_FALSE;
     }

     transparent.b = argb & 0xFF;
     argb >>= 8;
     transparent.g = argb & 0xFF;
     argb >>= 8;
     transparent.r = argb & 0xFF;
     argb >>= 8;
     transparent.a = argb & 0xFF;

     return DFB_TRUE;
}

static DFBBoolean parse_command_line( int argc, char *argv[] )
{
     int n;

     for (n = 1; n < argc; n++) {
          if (strncmp (argv[n], "--", 2) == 0) {
               const char *arg = argv[n] + 2;

               if (strcmp( arg, "help" ) == 0) {
                    print_usage();
                    return DFB_FALSE;
               }

               if (strncmp( arg, "format=", 7 ) == 0) {
                    if (!parse_format( arg + 7 ))
                         return DFB_FALSE;

                    continue;
               }

               if (strcmp( arg, "raw" ) == 0) {
                    rawdata = true;
                    continue;
               }

               if (strncmp( arg, "transparent=", 12 ) == 0) {
                    if (!parse_transparent( arg + 12 ))
                         return DFB_FALSE;

                    continue;
               }

               if (strncmp( arg, "name=", 5 ) == 0 && !name) {
                    name = arg + 5;
                    if (*name)
                         continue;
               }
          }

          if (filename || access( argv[n], R_OK )) {
               print_usage();
               return DFB_FALSE;
          }

          filename = argv[n];
     }

     if (!filename) {
          print_usage();
          return DFB_FALSE;
     }

     return DFB_TRUE;
}

/**********************************************************************************************************************/

static DFBResult load_file( unsigned char **ret_data, unsigned int *ret_len )
{
     struct         stat st;
     FILE          *fp;
     unsigned int   len  = 0;
     unsigned char *data = NULL;

     fp = fopen( filename, "rb" );
     if (!fp) {
          fprintf( stderr, "Failed to open '%s'!\n", filename );
          goto out;
     }

     if (stat( filename, &st ) < 0) {
          fprintf( stderr, "Failed to get file status!\n" );
          goto out;
     }
     else
         len = st.st_size;

     data = malloc( len );
     if (!data) {
          fprintf( stderr, "Failed to allocate %d bytes!\n", (int) st.st_size );
          goto out;
     }
     else
         fread( data, 1, len, fp );

     *ret_data = data;
     *ret_len  = len;

     data = NULL;

out:
     if (data)
          free( data );

     if (fp)
          fclose( fp );

     return len ? DFB_OK : DFB_FAILURE;
}

static DFBResult load_image( DFBSurfaceDescription *desc )
{
     DFBSurfacePixelFormat  dest_format;
     int                    width, height;
     int                    pitch;
     char                   signature[8];
     FILE                  *fp;
     png_structp            png_ptr         = NULL;
     png_infop              info_ptr        = NULL;
     unsigned char         *data            = NULL;
     DFBColor              *palette_entries = NULL;
     int                    palette_size    = 0;

     desc->flags = DSDESC_NONE;

     fp = fopen( filename, "rb" );
     if (!fp) {
          fprintf( stderr, "Failed to open '%s'!\n", filename );
          goto out;
     }

     fread( signature, 1, sizeof(signature), fp );

     if (!strncmp( signature, "DFIFF", 5 )) {
          if (format) {
               fprintf( stderr, "Choose the pixel format is not supported for DFIFF input image!\n" );
               goto out;
          }

          fread( &width,       1, sizeof(u32), fp );
          fread( &height,      1, sizeof(u32), fp );
          fread( &dest_format, 1, sizeof(u32), fp );
          fread( &pitch,       1, sizeof(u32), fp );

          data = malloc( height * pitch );
          if (!data) {
               fprintf( stderr, "Failed to allocate %d bytes!\n", height * pitch );
               goto out;
          }
          else
               fread( data, 1, height * pitch, fp );
     }
     else if (!png_sig_cmp( (unsigned char*) signature, 0, 8 )) {
          DFBSurfacePixelFormat src_format;
          int                   bpp, type, y;

          png_ptr = png_create_read_struct( PNG_LIBPNG_VER_STRING, NULL, NULL, NULL );
          if (!png_ptr) {
               fprintf( stderr, "Failed to create PNG read handle!\n" );
               goto out;
          }

          if (setjmp( png_jmpbuf( png_ptr ) )) {
               fprintf( stderr, "Failed to read PNG file!\n" );
               goto out;
          }

          info_ptr = png_create_info_struct( png_ptr );
          if (!info_ptr) {
               fprintf( stderr, "Failed to create PNG info handle!\n" );
               goto out;
          }

          png_init_io( png_ptr, fp );

          png_set_sig_bytes( png_ptr, 8 );

          png_read_info( png_ptr, info_ptr );

          png_get_IHDR( png_ptr, info_ptr, (png_uint_32*) &width, (png_uint_32*) &height,
                        &bpp, &type, NULL, NULL, NULL );

          if (bpp == 16)
               png_set_strip_16( png_ptr );

#ifdef WORDS_BIGENDIAN
          png_set_swap_alpha( png_ptr );
#else
          png_set_bgr( png_ptr );
#endif

          src_format = (type & PNG_COLOR_MASK_ALPHA) ? DSPF_ARGB : DSPF_RGB32;

          switch (type) {
               case PNG_COLOR_TYPE_GRAY:
                    if (format == DSPF_A8) {
                         src_format = DSPF_A8;
                         break;
                    }
                    /* fall through */

               case PNG_COLOR_TYPE_GRAY_ALPHA:
                    png_set_gray_to_rgb( png_ptr );
                    break;

               case PNG_COLOR_TYPE_PALETTE:
                    if (format == DSPF_LUT8) {
                         src_format = DSPF_LUT8;
                         break;
                    }
                    png_set_palette_to_rgb( png_ptr );
                    /* fall through */

               case PNG_COLOR_TYPE_RGB:
                    /* fall through */

               case PNG_COLOR_TYPE_RGB_ALPHA:
                    if (format == DSPF_RGB24) {
                         png_set_strip_alpha( png_ptr );
                         src_format = DSPF_RGB24;
                    }
                    break;
          }

          switch (src_format) {
               case DSPF_LUT8: {
                    png_colorp palette;
                    int        num;

                    png_get_PLTE( png_ptr, info_ptr, &palette, &num );

                    if (num) {
                         int i;

                         palette_size = MIN( num, 256 );

                         num = palette_size * sizeof(DFBColor);

                         palette_entries = malloc( num );
                         if (!palette_entries) {
                              fprintf( stderr, "Failed to allocate %d bytes!\n", num );
                              goto out;
                         }

                         for (i = 0; i < palette_size; i++) {
                              palette_entries[i].a = 0xFF;
                              palette_entries[i].r = palette[i].red;
                              palette_entries[i].g = palette[i].green;
                              palette_entries[i].b = palette[i].blue;
                         }

                         if (png_get_valid( png_ptr, info_ptr, PNG_INFO_tRNS )) {
                              png_byte *alpha;

                              png_get_tRNS( png_ptr, info_ptr, &alpha, &num, NULL );

                              for (i = 0; i < MIN( num, palette_size ); i++)
                                   palette_entries[i].a = alpha[i];
                         }
                    }
                    break;
               }
               case DSPF_RGB32:
                     png_set_filler( png_ptr, 0xFF,
#ifdef WORDS_BIGENDIAN
                                     PNG_FILLER_BEFORE
#else
                                     PNG_FILLER_AFTER
#endif
                                   );
                     break;
               case DSPF_ARGB:
               case DSPF_A8:
                    if (png_get_valid( png_ptr, info_ptr, PNG_INFO_tRNS ))
                         png_set_tRNS_to_alpha( png_ptr );
                    break;
               default:
                    break;
          }

          pitch = width * DFB_BYTES_PER_PIXEL( src_format );
          if (pitch & 3)
               pitch += 4 - (pitch & 3);

          data = malloc( height * pitch );
          if (!data) {
               fprintf( stderr, "Failed to allocate %d bytes!\n", height * pitch );
               goto out;
          }
          else {
               png_bytep row_ptrs[height];

               for (y = 0; y < height; y++)
                    row_ptrs[y] = data + y * pitch;

               png_read_image( png_ptr, row_ptrs );
          }

          /* replace color in completely transparent pixels */
          if (transparent.a && DFB_PIXELFORMAT_HAS_ALPHA( src_format )) {
               unsigned char *row;
               int            h;

               for (row = data, h = height; h; h--, row += pitch) {
                    u32 *pixel;
                    int  w;

                    for (pixel = (u32*) row, w = width; w; w--, pixel++) {
                         if ((*pixel & 0xFF000000) == 0)
                              *pixel = dfb_color_to_argb( &transparent );
                    }
               }
          }

          dest_format = format ?: src_format;

          if (DFB_BYTES_PER_PIXEL( src_format ) != DFB_BYTES_PER_PIXEL( dest_format )) {
               int            d_pitch;
               unsigned char *dest, *s, *d;
               int            h = height;

               d_pitch = width * DFB_BYTES_PER_PIXEL( dest_format );
               if (d_pitch & 3)
                    d_pitch += 4 - (d_pitch & 3);

               dest = malloc( height * d_pitch );
               if (!dest) {
                    fprintf( stderr, "Failed to allocate %d bytes!\n", height * d_pitch );
                    goto out;
               }

               switch (dest_format) {
                    case DSPF_RGB444:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
                              dfb_argb_to_rgb444( (u32*) s, (u16*) d, width );
                         break;
                    case DSPF_RGB555:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
                              dfb_argb_to_rgb555( (u32*) s, (u16*) d, width );
                         break;
                    case DSPF_BGR555:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
                              dfb_argb_to_bgr555( (u32*) s, (u16*) d, width );
                         break;
                    case DSPF_RGB16:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
                              dfb_argb_to_rgb16( (u32*) s, (u16*) d, width );
                         break;
                    case DSPF_RGB18:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
#ifdef WORDS_BIGENDIAN
                              dfb_argb_to_rgb18be( (u32*) s, (u8*) d, width );
#else
                              dfb_argb_to_rgb18le( (u32*) s, (u8*) d, width );
#endif
                         break;
                    case DSPF_ARGB1666:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
#ifdef WORDS_BIGENDIAN
                              dfb_argb_to_argb1666be( (u32*) s, (u8*) d, width );
#else
                              dfb_argb_to_argb1666le( (u32*) s, (u8*) d, width );
#endif
                         break;
                    case DSPF_ARGB6666:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
#ifdef WORDS_BIGENDIAN
                              dfb_argb_to_argb6666be( (u32*) s, (u8*) d, width );
#else
                              dfb_argb_to_argb6666le( (u32*) s, (u8*) d, width );
#endif
                         break;
                    case DSPF_ARGB8565:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
#ifdef WORDS_BIGENDIAN
                              dfb_argb_to_argb8565be( (u32*) s, (u8*) d, width );
#else
                              dfb_argb_to_argb8565le( (u32*) s, (u8*) d, width );
#endif
                         break;
                    case DSPF_ARGB1555:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
                              dfb_argb_to_argb1555( (u32*) s, (u16*) d, width );
                         break;
                    case DSPF_RGBA5551:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
                              dfb_argb_to_rgba5551( (u32*) s, (u16*) d, width );
                         break;
                    case DSPF_ARGB2554:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
                              dfb_argb_to_argb2554( (u32*) s, (u16*) d, width );
                         break;
                    case DSPF_ARGB4444:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
                              dfb_argb_to_argb4444( (u32*) s, (u16*) d, width );
                         break;
                    case DSPF_RGBA4444:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
                              dfb_argb_to_rgba4444( (u32*) s, (u16*) d, width );
                         break;
                    case DSPF_RGB332:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
                              dfb_argb_to_rgb332( (u32*) s, (u8*) d, width );
                         break;
                    case DSPF_A8:
                         for (s = data, d = dest; h; h--, s += pitch, d += d_pitch)
                              dfb_argb_to_a8( (u32*) s, (u8*) d, width );
                         break;
                    default:
                         fprintf( stderr, "Unsupported format conversion!\n" );
                         free( dest );
                         goto out;
               }

               free( data );
               data  = dest;
               pitch = d_pitch;
          }
          else if (dest_format == DSPF_ABGR) {
               unsigned char *s;
               int            h = height;

               for (s = data; h; h--, s += pitch)
                    dfb_argb_to_abgr( (u32*) s, (u32*) s, width );
          }
          else if (dest_format == DSPF_RGBAF88871) {
               unsigned char *s;
               int            h = height;

               for (s = data; h; h--, s += pitch)
                    dfb_argb_to_rgbaf88871( (u32*) s, (u32*) s, width );
          }
     }
     else {
          fprintf( stderr, "Invalid signature in file '%s'!\n", filename );
          goto out;
     }

     desc->flags                 = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT | DSDESC_PREALLOCATED;
     desc->width                 = width;
     desc->height                = height;
     desc->pixelformat           = dest_format;
     desc->preallocated[0].data  = data;
     desc->preallocated[0].pitch = pitch;
     desc->palette.entries       = palette_entries;
     desc->palette.size          = palette_size;

     data            = NULL;
     palette_entries = NULL;

out:
     if (palette_entries)
          free( palette_entries );

     if (data)
          free( data );

     if (png_ptr)
          png_destroy_read_struct( &png_ptr, &info_ptr, NULL );

     if (fp)
          fclose( fp );

     return desc->flags ? DFB_OK : DFB_FAILURE;
}

/**********************************************************************************************************************/

static char *variable_name( const char *s )
{
     char *vname = strdup( s );
     char *v     = vname;

     while (DFB_TRUE) {
          switch (*v) {
               case 0:
                    return vname;
               case 'a'...'z':
               case 'A'...'Z':
               case '0'...'9':
               case '_':
                    break;
               default:
                    *v = '_';
          }

          v++;
     }
}

typedef struct {
     int  pos;
     bool pad;
} CSourceData;

static __inline__ void save_uchar( CSourceData *csource, unsigned char d )
{
     if (csource->pos > 70) {
          fprintf( stdout, "\"\n  \"" );

          csource->pos = 3;
          csource->pad = false;
     }

     if (d < 33 || d > 126) {
          fprintf( stdout, "\\%o", d );
          csource->pos += 1 + 1 + (d > 7) + (d > 63);
          csource->pad = d < 64;
          return;
     }

     if (d == '\\') {
          fprintf( stdout, "\\\\" );
          csource->pos += 2;
     }
     else if (d == '"') {
          fprintf( stdout, "\\\"" );
          csource->pos += 2;
     }
     else if (csource->pad && d >= '0' && d <= '9') {
          fprintf( stdout, "\"\"%c", d );
          csource->pos += 3;
     }
     else {
          fputc( d, stdout );
          csource->pos += 1;
     }

     csource->pad = false;
}

static void dump_data( const char *vname, const unsigned char *data, unsigned int len )
{
     CSourceData csource;

     fprintf( stdout, "static const unsigned char %s_data[] =\n", vname );
     fprintf( stdout, "  \"" );

     csource.pos = 3;
     csource.pad = false;

     do
          save_uchar( &csource, *data++ );
     while (--len);

     fprintf( stdout, "\";\n\n" );
}

static void dump_rawdata( unsigned char *data, unsigned int len )
{
     char *vname = variable_name( name ?: strrchr( filename, '/' ) ?: filename );

     /* dump comment */
     fprintf( stdout, "/* DirectFB raw data dump created by directfb-csource */\n\n" );

     /* dump data */
     dump_data( vname, data, len );

     free( vname );
}

static void dump_dsdesc( DFBSurfaceDescription *desc )
{
     int   i;
     char *vname = variable_name( name ?: strrchr( filename, '/' ) ?: filename );

     /* dump comment */
     fprintf( stdout, "/* DirectFB surface dump created by directfb-csource */\n\n" );

     /* dump data */
     dump_data( vname, desc->preallocated[0].data, desc->height * desc->preallocated[0].pitch );

     /* dump palette */
     if (desc->palette.size > 0) {
          fprintf( stdout, "static const DFBColor %s_palette[%d] = {\n", vname, desc->palette.size );
          for (i = 0; i < desc->palette.size; i++)
               fprintf( stdout, "  { 0x%02x, 0x%02x, 0x%02x, 0x%02x }%c\n", desc->palette.entries[i].a,
                        desc->palette.entries[i].r, desc->palette.entries[i].g, desc->palette.entries[i].b,
                        i + 1 < desc->palette.size ? ',' : ' ' );
          fprintf( stdout, "};\n\n" );
     }

     /* dump description */
     fprintf( stdout, "static const DFBSurfaceDescription %s_desc = {\n", vname );
     fprintf( stdout, "  flags                   : DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT |\n"
                      "                            DSDESC_PREALLOCATED" );
     if (desc->palette.size > 0)
          fprintf( stdout, " | DSDESC_PALETTE" );
     fprintf( stdout, ",\n" );
     fprintf( stdout, "  width                   : %d,\n", desc->width );
     fprintf( stdout, "  height                  : %d,\n", desc->height );
     for (i = 0; i < D_ARRAY_SIZE(format_names); i++) {
          if (format_names[i].format == desc->pixelformat) {
               break;
          }
     }
     fprintf( stdout, "  pixelformat             : DSPF_%s,\n", format_names[i].name );
     fprintf( stdout, "  preallocated : {{  data : (void*) %s_data,\n", vname );
     fprintf( stdout, "                    pitch : %d }}", desc->preallocated[0].pitch );
     if (desc->palette.size > 0) {
          fprintf( stdout, ",\n");
          fprintf( stdout, "  palette :    {  entries : %s_palette,\n", vname );
          fprintf( stdout, "                     size : %d  }", desc->palette.size );
     }
     fprintf( stdout, "\n};\n" );

     free( vname );
}

/**********************************************************************************************************************/

int main( int argc, char *argv[] )
{
     /* Parse the command line. */
     if (!parse_command_line( argc, argv ))
          return -1;

     if (rawdata) {
          unsigned char *data;
          unsigned int   len;

          if (load_file( &data, &len ))
               return -2;

          dump_rawdata( data, len );

          free( data );
     }
     else {
          DFBSurfaceDescription desc;

          if (load_image( &desc ))
               return -2;

          dump_dsdesc( &desc );

          if (desc.palette.size)
               free( (DFBColor*) desc.palette.entries );
          free( desc.preallocated[0].data );
     }

     return 0;
}
