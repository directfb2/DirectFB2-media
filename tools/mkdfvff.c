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

#include <dfvff.h>
#include <direct/util.h>
#include <directfb_strings.h>

static const DirectFBPixelFormatNames(format_names);
static const DirectFBColorSpaceNames(colorspace_names);

static const char            *filename   = NULL;
static bool                   debug      = false;
static DFBSurfacePixelFormat  format     = DSPF_YUV444P;
static DFBSurfaceColorSpace   colorspace = DSCS_BT601;
static unsigned int           fps_num    = 24;
static unsigned int           fps_den    = 1;
static int                    width      = 0;
static int                    height     = 0;
static unsigned long          nframes    = 0;

#define DEBUG(...)                             \
     do {                                      \
          if (debug)                           \
               fprintf( stderr, __VA_ARGS__ ); \
     } while (0)

/**********************************************************************************************************************/

static void print_usage()
{
     int i = 0;

     fprintf( stderr, "DirectFB Fast Video File Format Tool\n\n" );
     fprintf( stderr, "Usage: mkdfvff [options] video\n\n" );
     fprintf( stderr, "Options:\n\n" );
     fprintf( stderr, "  -d, --debug                           Output debug information.\n" );
     fprintf( stderr, "  -f, --format     <pixelformat>        Choose the pixel format (default YUV444P).\n" );
     fprintf( stderr, "  -c, --colorspace <colorspace>         Choose the color space (default BT601).\n" );
     fprintf( stderr, "  -r, --rate       <fps_num>/<fps_den>  Choose the frame rate (default 24).\n" );
     fprintf( stderr, "  -s, --size       <width>x<height>     Set video frame size (for raw input video).\n" );
     fprintf( stderr, "  -n, --nframes    <nframes>            Set the number of video frames to output.\n" );
     fprintf( stderr, "  -h, --help                            Show this help message.\n\n" );
     fprintf( stderr, "Supported pixel formats:\n\n" );
     while (format_names[i].format != DSPF_UNKNOWN) {
          if (DFB_BYTES_PER_PIXEL( format_names[i].format ) < 3 &&
              DFB_COLOR_IS_YUV   ( format_names[i].format )) {
               fprintf( stderr, "  %-10s %2d byte(s)",
                        format_names[i].name, DFB_BYTES_PER_PIXEL( format_names[i].format ) );
               if (DFB_PLANAR_PIXELFORMAT( format_names[i].format )) {
                    int planes = DFB_PLANE_MULTIPLY( format_names[i].format, 10 );
                    fprintf( stderr, " (x %d.%d)", planes / 10, planes % 10 );
               }
               fprintf( stderr, "\n" );
          }
          ++i;
     }
     fprintf( stderr, "\n" );
     i = 0;
     fprintf( stderr, "Supported color spaces:\n\n" );
     while (colorspace_names[i].colorspace != DSCS_UNKNOWN) {
          DFBSurfaceColorSpace colorspace = colorspace_names[i].colorspace;
          if (colorspace != DSCS_RGB) {
               fprintf( stderr, "  %s\n", colorspace_names[i].name );
          }
          ++i;
     }
     fprintf( stderr, "\n" );
}

static DFBBoolean parse_format( const char *arg )
{
     int i = 0;

     while (format_names[i].format != DSPF_UNKNOWN) {
          if (!strcasecmp( arg, format_names[i].name ) &&
              DFB_BYTES_PER_PIXEL( format_names[i].format ) < 3 &&
              DFB_COLOR_IS_YUV   ( format_names[i].format )) {
               format = format_names[i].format;
               return DFB_TRUE;
          }

          ++i;
     }

     fprintf( stderr, "Invalid pixel format specified!\n" );

     return DFB_FALSE;
}

static DFBBoolean parse_colorspace( const char *arg )
{
     int i = 0;

     while (colorspace_names[i].colorspace != DSCS_UNKNOWN) {
          if (!strcasecmp( arg, colorspace_names[i].name ) &&
              colorspace_names[i].colorspace != DSCS_RGB) {
               colorspace = colorspace_names[i].colorspace;
               return DFB_TRUE;
          }

          ++i;
     }

     fprintf( stderr, "Invalid color space specified!\n" );

     return DFB_FALSE;
}

static DFBBoolean parse_rate( const char *arg )
{
     if (sscanf( arg, "%u/%u", &fps_num, &fps_den ) == 2)
         return DFB_TRUE;

     fprintf( stderr, "Invalid frame rate specified!\n" );

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

          if (strcmp( arg, "-c" ) == 0 || strcmp( arg, "--colorspace" ) == 0) {
               if (++n == argc) {
                    print_usage();
                    return DFB_FALSE;
               }

               if (!parse_colorspace( argv[n] ))
                    return DFB_FALSE;

               continue;
          }

          if (strcmp( arg, "-r" ) == 0 || strcmp( arg, "--rate" ) == 0) {
               if (++n == argc) {
                    print_usage();
                    return DFB_FALSE;
               }

               if (!parse_rate( argv[n] ))
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

          if (strcmp( arg, "-n" ) == 0 || strcmp( arg, "--nframes" ) == 0) {
               if (++n == argc) {
                    print_usage();
                    return DFB_FALSE;
               }

               nframes = strtoul( argv[n], NULL, 10 );

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

static long load_video( DFBSurfaceDescription *desc )
{
     int            frame_size;
     struct stat    st;
     FILE          *fp;
     unsigned char *data = NULL;

     desc->flags = DSDESC_NONE;

     frame_size = DFB_BYTES_PER_LINE( format, width ) * DFB_PLANE_MULTIPLY( format, height );

     fp = fopen( filename, "rb" );
     if (!fp) {
          fprintf( stderr, "Failed to open '%s'!\n", filename );
          goto out;
     }

     if (!width || !height) {
          fprintf( stderr, "No size specified!\n" );
          goto out;
     }

     if (!nframes) {
          stat( filename, &st );

          nframes = st.st_size / frame_size;
     }

     data = malloc( frame_size );
     if (!data) {
          fprintf( stderr, "Failed to allocate %d bytes!\n", frame_size );
          goto out;
     }

     desc->flags                 = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT | DSDESC_PREALLOCATED |
                                   DSDESC_COLORSPACE;
     desc->width                 = width;
     desc->height                = height;
     desc->pixelformat           = format;
     desc->preallocated[0].pitch = DFB_BYTES_PER_LINE( format, width );
     desc->preallocated[0].data  = data;
     desc->colorspace            = colorspace;

 out:
     if (fp)
          fclose( fp );

     return desc->flags ? DFB_OK : DFB_FAILURE;
}

static void write_frames( DFBSurfaceDescription *desc )
{
     int            frame_size;
     unsigned long  frame;
     FILE          *fp = NULL;

     frame_size = DFB_BYTES_PER_LINE( format, width ) * DFB_PLANE_MULTIPLY( format, height );

     fp = fopen( filename, "rb" );

     for (frame = 0; frame < nframes; frame++) {
          fread( desc->preallocated[0].data, 1, frame_size, fp );

          fwrite( desc->preallocated[0].data, 1, frame_size, stdout );
     }

     fclose( fp );
}

/**********************************************************************************************************************/

static DFVFFHeader header = {
     magic: { 'D', 'F', 'V', 'F', 'F' },
     major: 0,
     minor: 0,
     flags: 0x01
};

int main( int argc, char *argv[] )
{
     int                   i, j;
     DFBSurfaceDescription desc;

     /* Parse the command line. */
     if (!parse_command_line( argc, argv ))
          return -1;

     if (load_video( &desc ))
          return -2;

     for (i = 0; i < D_ARRAY_SIZE(format_names); i++) {
          if (format_names[i].format == desc.pixelformat) {
               for (j = 0; j < D_ARRAY_SIZE(colorspace_names); j++) {
                    if (colorspace_names[j].colorspace == desc.colorspace) {
                         DEBUG( "Writing video (%lu frames): %dx%d, %s(%s), %u/%u fps\n", nframes, desc.width,
                                 desc.height, format_names[i].name, colorspace_names[j].name, fps_num, fps_den );
                         break;
                    }
               }
          }
     }

     header.width      = desc.width;
     header.height     = desc.height;
     header.format     = desc.pixelformat;
     header.colorspace = desc.colorspace;

     header.framerate_num = fps_num;
     header.framerate_den = fps_den;

     fwrite( &header, sizeof(header), 1, stdout );

     write_frames( &desc );

     free( desc.preallocated[0].data );

     return 0;
}
