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

if enable_bmp
  library('idirectfbimageprovider_bmp',
          'idirectfbimageprovider_bmp.c',
          dependencies: directfb_dep,
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBImageProvider'))
endif

if enable_ffmpeg
  library('idirectfbimageprovider_ffmpeg',
          'idirectfbimageprovider_ffmpeg.c',
          dependencies: [directfb_dep, ffmpeg_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBImageProvider'))
endif

if enable_jpeg
  library('idirectfbimageprovider_jpeg',
          'idirectfbimageprovider_jpeg.c',
          include_directories: config_inc,
          dependencies: [directfb_dep, jpeg_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBImageProvider'))
endif

if enable_lodepng != ''
  library('idirectfbimageprovider_lodepng',
          'idirectfbimageprovider_lodepng.c',
          c_args: '-DLODEPNG_C="lodepng.cpp"',
          dependencies: [directfb_dep, lodepng_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBImageProvider'))
endif

if enable_png
  library('idirectfbimageprovider_png',
          'idirectfbimageprovider_png.c',
          include_directories: config_inc,
          dependencies: [directfb_dep, png_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBImageProvider'))
endif

if enable_stb_image != ''
  library('idirectfbimageprovider_stb',
          'idirectfbimageprovider_stb.c',
          c_args: '-DSTB_IMAGE_H="@0@"'.format(enable_stb_image),
          dependencies: directfb_dep,
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBImageProvider'))
endif

if enable_tiff
  library('idirectfbimageprovider_tiff',
          'idirectfbimageprovider_tiff.c',
          dependencies: [directfb_dep, tiff_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBImageProvider'))
endif

if enable_webp
  library('idirectfbimageprovider_webp',
          'idirectfbimageprovider_webp.c',
          dependencies: [directfb_dep, webp_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBImageProvider'))
endif

if enable_yuv
  library('idirectfbimageprovider_yuv',
          'idirectfbimageprovider_yuv.c',
          dependencies: directfb_dep,
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBImageProvider'))
endif
