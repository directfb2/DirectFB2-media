#  This file is part of DirectFB.
#
#  This library is free software; you can redistribute it and/or
#  modify it under the terms of the GNU Lesser General Public
#  License as published by the Free Software Foundation; either
#  version 2.1 of the License, or (at your option) any later version.
#
#  This library is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#  Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public
#  License along with this library; if not, write to the Free Software
#  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA

if enable_avif
  library('idirectfbimageprovider_avif',
          'idirectfbimageprovider_avif.c',
          dependencies: [avif_dep, directfb_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_avif',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_avif',
                       description: 'AVIF image provider',
                       requires_private: 'libavif',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_avif -Wl,--no-whole-archive'])
  endif
endif

if enable_bmp
  library('idirectfbimageprovider_bmp',
          'idirectfbimageprovider_bmp.c',
          include_directories: config_inc,
          dependencies: directfb_dep,
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_bmp',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_bmp',
                       description: 'BMP image provider',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_bmp -Wl,--no-whole-archive'])
  endif
endif

if enable_ffmpeg
  library('idirectfbimageprovider_ffmpeg',
          'idirectfbimageprovider_ffmpeg.c',
          dependencies: [directfb_dep, ffmpeg_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_ffmpeg',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_ffmpeg',
                       description: 'FFmpeg image provider',
                       requires_private: ['libavformat', 'libswscale'],
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_ffmpeg -Wl,--no-whole-archive'])
  endif
endif

if enable_gdkpixbuf
  library('idirectfbimageprovider_gdkpixbuf',
          'idirectfbimageprovider_gdkpixbuf.c',
          dependencies: [directfb_dep, gdkpixbuf_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_gdkpixbuf',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_gdkpixbuf',
                       description: 'GdkPixbuf image provider',
                       requires_private: 'gdk-pixbuf-2.0',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_gdkpixbuf -Wl,--no-whole-archive'])
  endif
endif

if enable_gif
  library('idirectfbimageprovider_gif',
          'idirectfbimageprovider_gif.c',
          dependencies: directfb_dep,
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_gif',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_gif',
                       description: 'GIF image provider',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_gif -Wl,--no-whole-archive'])
  endif
endif

if enable_heif
  library('idirectfbimageprovider_heif',
          'idirectfbimageprovider_heif.c',
          dependencies: [directfb_dep, heif_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_heif',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_heif',
                       description: 'HEIF image provider',
                       requires_private: 'libheif',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_heif -Wl,--no-whole-archive'])
  endif
endif

if enable_imlib2
  library('idirectfbimageprovider_imlib2',
          'idirectfbimageprovider_imlib2.c',
          dependencies: [directfb_dep, imlib2_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_imlib2',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_imlib2',
                       description: 'Imlib2 image provider',
                       requires_private: 'imlib2',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_imlib2 -Wl,--no-whole-archive'])
  endif
endif

if enable_jasper
  library('idirectfbimageprovider_jasper',
          'idirectfbimageprovider_jasper.c',
          dependencies: [directfb_dep, jasper_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_jasper',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_jasper',
                       description: 'JasPer image provider',
                       requires_private: 'jasper',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_jasper -Wl,--no-whole-archive'])
  endif
endif

if enable_jpeg
  library('idirectfbimageprovider_jpeg',
          'idirectfbimageprovider_jpeg.c',
          include_directories: config_inc,
          dependencies: [directfb_dep, jpeg_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_jpeg',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_jpeg',
                       description: 'JPEG image provider',
                       requires_private: 'libjpeg',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_jpeg -Wl,--no-whole-archive'])
  endif
endif

if enable_jxl
  library('idirectfbimageprovider_jxl',
          'idirectfbimageprovider_jxl.c',
          dependencies: [directfb_dep, jxl_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_jxl',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_jxl',
                       description: 'JPEG XL image provider',
                       requires_private: 'libjxl',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_jxl -Wl,--no-whole-archive'])
  endif
endif

if enable_lodepng != 'false'
  library('idirectfbimageprovider_lodepng',
          'idirectfbimageprovider_lodepng.c',
          c_args: '-DLODEPNG_SRC="lodepng.cpp"',
          dependencies: [directfb_dep, lodepng_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_lodepng',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_lodepng',
                       description: 'LodePNG image provider',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_lodepng -Wl,--no-whole-archive'])
  endif
endif

if enable_nanosvg != 'false'
  library('idirectfbimageprovider_nanosvg',
          'idirectfbimageprovider_nanosvg.c',
          dependencies: [directfb_dep, nanosvg_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_nanosvg',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_nanosvg',
                       description: 'NanoSVG image provider',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_nanosvg -Wl,--no-whole-archive'])
  endif
endif

if enable_openexr
  library('idirectfbimageprovider_openexr',
          'idirectfbimageprovider_openexr.c',
          dependencies: [directfb_dep, openexr_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_openexr',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_openexr',
                       description: 'OpenEXR image provider',
                       requires_private: 'OpenEXR',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_openexr -Wl,--no-whole-archive'])
  endif
endif

if enable_png
  library('idirectfbimageprovider_png',
          'idirectfbimageprovider_png.c',
          include_directories: config_inc,
          dependencies: [directfb_dep, png_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_png',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_png',
                       description: 'PNG image provider',
                       requires_private: 'libpng',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_png -Wl,--no-whole-archive'])
  endif
endif

if enable_spng
  library('idirectfbimageprovider_spng',
          'idirectfbimageprovider_spng.c',
          dependencies: [directfb_dep, spng_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_spng',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_spng',
                       description: 'Simple PNG image provider',
                       requires_private: 'spng',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_spng -Wl,--no-whole-archive'])
  endif
endif

if enable_stb_image != 'false'
  library('idirectfbimageprovider_stb',
          'idirectfbimageprovider_stb.c',
          dependencies: [directfb_dep, stb_image_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_stb',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_stb',
                       description: 'STB image provider',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_stb -Wl,--no-whole-archive'])
  endif
endif

if enable_svg
  library('idirectfbimageprovider_svg',
          'idirectfbimageprovider_svg.c',
          dependencies: [directfb_dep, svg_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_svg',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_svg',
                       description: 'SVG image provider',
                       requires_private: 'libsvg-cairo',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_svg -Wl,--no-whole-archive'])
  endif
endif

if enable_tiff
  library('idirectfbimageprovider_tiff',
          'idirectfbimageprovider_tiff.c',
          dependencies: [directfb_dep, tiff_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_tiff',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_tiff',
                       description: 'TIFF image provider',
                       requires_private: 'libtiff-4',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_tiff -Wl,--no-whole-archive'])
  endif
endif

if enable_webp
  library('idirectfbimageprovider_webp',
          'idirectfbimageprovider_webp.c',
          dependencies: [directfb_dep, webp_dep],
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_webp',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_webp',
                       description: 'WebP image provider',
                       requires_private: 'libwebp',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_webp -Wl,--no-whole-archive'])
  endif
endif

if enable_yuv
  library('idirectfbimageprovider_yuv',
          'idirectfbimageprovider_yuv.c',
          dependencies: directfb_dep,
          install: true,
          install_dir: moduledir / 'interfaces/IDirectFBImageProvider')

  if get_option('default_library') == 'static'
    pkgconfig.generate(filebase: 'directfb-interface-imageprovider_yuv',
                       variables: 'moduledir=' + moduledir,
                       name: 'DirectFB-interface-imageprovider_yuv',
                       description: 'YUV image provider',
                       libraries_private: ['-L${moduledir}/interfaces/IDirectFBImageProvider',
                                           '-Wl,--whole-archive -lidirectfbimageprovider_yuv -Wl,--no-whole-archive'])
  endif
endif
