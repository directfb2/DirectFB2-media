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
#include <dfiff.h>
#include <directfb_strings.h>
#include <gfx/convert.h>
#include <png.h>

static const DirectFBPixelFormatNames(format_names);

static const char            *filename      = NULL;
static bool                   debug         = false;
static DFBSurfacePixelFormat  format        = DSPF_UNKNOWN;
static bool                   premultiplied = false;
static int                    width         = 0;
static int                    height        = 0;

#define DEBUG(...)                             \
     do {                                      \
          if (debug)                           \
               fprintf( stderr, __VA_ARGS__ ); \
     } while (0)

/**********************************************************************************************************************/

static void print_usage()
{
     int i = 0;

     fprintf( stderr, "DirectFB Fast Image File Format Tool\n\n" );
     fprintf( stderr, "Usage: mkdfiff [options] image\n\n" );
     fprintf( stderr, "Options:\n\n" );
     fprintf( stderr, "  -d, --debug                         Output debug information.\n" );
     fprintf( stderr, "  -f, --format      <pixelformat>     Choose the pixel format (default ARGB or RGB32).\n" );
     fprintf( stderr, "  -s, --size        <width>x<height>  Set image size (for raw input image).\n" );
     fprintf( stderr, "  -p, --premultiply                   Generate premultiplied pixels (default false).\n" );
     fprintf( stderr, "  -h, --help                          Show this help message.\n\n" );
     fprintf( stderr, "Supported pixel formats:\n\n" );
     while (format_names[i].format != DSPF_UNKNOWN) {
          if ( DFB_BYTES_PER_PIXEL       ( format_names[i].format ) >= 1 &&
              !DFB_PIXELFORMAT_IS_INDEXED( format_names[i].format )      &&
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
          if (!strcasecmp( arg, format_names[i].name )                   &&
               DFB_BYTES_PER_PIXEL       ( format_names[i].format ) >= 1 &&
              !DFB_PIXELFORMAT_IS_INDEXED( format_names[i].format )      &&
              !DFB_COLOR_IS_YUV          ( format_names[i].format )) {
               format = format_names[i].format;
               return DFB_TRUE;
          }
          ++i;
     }

     fprintf( stderr, "Invalid pixel format specified!\n" );

     return DFB_FALSE;
}

static DFBBoolean parse_size( const char *arg )
{
     if (sscanf( arg, "%dx%d", &width, &height ) == 2)
         return DFB_TRUE;

     fprintf( stderr, "Invalid size specified!\n" );

     return DFB_FALSE;
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

          if (strcmp( arg, "-s" ) == 0 || strcmp( arg, "--size" ) == 0) {
               if (++n == argc) {
                    print_usage();
                    return DFB_FALSE;
               }

               if (!parse_size( argv[n] ))
                    return DFB_FALSE;

               continue;
          }

          if (strcmp( arg, "-p" ) == 0 || strcmp( arg, "--premultiply" ) == 0) {
               premultiplied = true;
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

static DFBResult load_image( DFBSurfaceDescription *desc )
{
     DFBSurfacePixelFormat  dest_format;
     DFBSurfacePixelFormat  src_format;
     char                   signature[8];
     int                    bytes, type;
     int                    pitch, x, y;
     FILE                  *fp;
     png_structp            png_ptr  = NULL;
     png_infop              info_ptr = NULL;
     unsigned char         *data     = NULL;

     dest_format = format;

     desc->flags                = DSDESC_NONE;
     desc->preallocated[0].data = NULL;

     fp = fopen( filename, "rb" );
     if (!fp) {
          fprintf( stderr, "Failed to open '%s'!\n", filename );
          goto out;
     }

     if (width && height) {
          if (!dest_format) {
               fprintf( stderr, "No format specified!\n" );
               goto out;
          }

          if (premultiplied) {
               fprintf( stderr, "Generate premultiplied pixels is not supported for raw input image!\n" );
               goto out;
          }

          pitch = (DFB_BYTES_PER_LINE( dest_format, width ) + 7) & ~7;

          data = malloc( height * pitch );
          if (!data) {
               fprintf( stderr, "Failed to allocate %d bytes!\n", height * pitch );
               goto out;
          }
          else {
               fread( data, 1, height * pitch, fp );
          }
     }
     else {
          bytes = fread( signature, 1, sizeof(signature), fp );

          if (png_sig_cmp( (unsigned char*) signature, 0, bytes )) {
               fprintf( stderr, "File '%s' doesn't seem to be a PNG image!\n", filename );
               goto out;
          }

          png_ptr = png_create_read_struct( PNG_LIBPNG_VER_STRING, NULL, NULL, NULL );
          if (!png_ptr)
               goto out;

          if (setjmp( png_jmpbuf( png_ptr ) )) {
               if (desc->preallocated[0].data) {
                    free( desc->preallocated[0].data );
                    desc->preallocated[0].data = NULL;
               }

               /* data might have been clobbered */
               data = NULL;

               goto out;
          }

          info_ptr = png_create_info_struct( png_ptr );
          if (!info_ptr)
               goto out;

          png_init_io( png_ptr, fp );

          png_set_sig_bytes( png_ptr, bytes );

          png_read_info( png_ptr, info_ptr );

          png_get_IHDR( png_ptr, info_ptr, (png_uint_32*) &width, (png_uint_32*) &height, &bytes,
                        &type, NULL, NULL, NULL );

          if (bytes == 16)
               png_set_strip_16( png_ptr );

     #ifdef WORDS_BIGENDIAN
          png_set_swap_alpha( png_ptr );
     #else
          png_set_bgr( png_ptr );
     #endif

          src_format = (type & PNG_COLOR_MASK_ALPHA) ? DSPF_ARGB : DSPF_RGB32;

          switch (type) {
               case PNG_COLOR_TYPE_GRAY:
                    if (dest_format == DSPF_A8) {
                         src_format = DSPF_A8;
                         break;
                    }
                    /* fall through */

               case PNG_COLOR_TYPE_GRAY_ALPHA:
                    png_set_gray_to_rgb( png_ptr );
                    break;

               case PNG_COLOR_TYPE_PALETTE:
                    png_set_palette_to_rgb( png_ptr );
                    /* fall through */

               case PNG_COLOR_TYPE_RGB:
                    /* fall through */

               case PNG_COLOR_TYPE_RGB_ALPHA:
                    if (dest_format == DSPF_RGB24) {
                         png_set_strip_alpha( png_ptr );
                         src_format = DSPF_RGB24;
                    }
                    break;
          }

          switch (src_format) {
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

          pitch = (DFB_BYTES_PER_LINE( src_format, width ) + 7) & ~7;

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

          if (!dest_format)
               dest_format = src_format;

          if (premultiplied) {
               for (y = 0; y < height; y++) {
                    u32 *p = (u32*) (data + y * pitch);

                    for (x = 0; x < width; x++) {
                         u32 s = p[x];
                         u32 a = (s >> 24) + 1;

                         p[x] = ((((s & 0x00FF00FF) * a) >> 8) & 0x00FF00FF) |
                                ((((s & 0x0000FF00) * a) >> 8) & 0x0000FF00) |
                                (   s & 0xFF000000                         );
                    }
               }
          }

          if (DFB_BYTES_PER_PIXEL( src_format ) != DFB_BYTES_PER_PIXEL( dest_format )) {
               unsigned char *s, *d, *dest;
               int            d_pitch;
               int            h = height;

               d_pitch = (DFB_BYTES_PER_LINE( dest_format, width ) + 7) & ~7;

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
                         goto out;
               }

               free( data );
               data = dest;
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

     desc->flags                 = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT | DSDESC_PREALLOCATED;
     desc->width                 = width;
     desc->height                = height;
     desc->pixelformat           = dest_format;
     desc->preallocated[0].data  = data;
     desc->preallocated[0].pitch = pitch;

     data = NULL;

 out:
     if (data)
          free( data );

     if (png_ptr)
          png_destroy_read_struct( &png_ptr, &info_ptr, NULL );

     if (fp)
          fclose( fp );

     return desc->flags ? DFB_OK : DFB_FAILURE;
}

/**********************************************************************************************************************/

static DFIFFHeader header = {
     magic: { 'D', 'F', 'I', 'F', 'F' },
     major: 0,
     minor: 0,
     flags: 0x01
};

#define DFIFF_FLAG_PREMULTIPLIED 0x02

int main( int argc, char *argv[] )
{
     int                   i;
     DFBSurfaceDescription desc;

     /* Parse the command line. */
     if (!parse_command_line( argc, argv ))
          return -1;

     if (load_image( &desc ))
          return -2;

     for (i = 0; i < D_ARRAY_SIZE(format_names); i++) {
          if (format_names[i].format == desc.pixelformat) {
               DEBUG( "Writing image: %dx%d, %s\n", desc.width, desc.height, format_names[i].name );
               break;
          }
     }

     header.width  = desc.width;
     header.height = desc.height;
     header.format = desc.pixelformat;
     header.pitch  = desc.preallocated[0].pitch;

     if (premultiplied)
          header.flags |= DFIFF_FLAG_PREMULTIPLIED;

     fwrite( &header, sizeof(header), 1, stdout );

     fwrite( desc.preallocated[0].data, header.pitch, header.height, stdout );

     free( desc.preallocated[0].data );

     return 0;
}
