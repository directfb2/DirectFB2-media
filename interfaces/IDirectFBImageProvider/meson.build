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

enable_jpeg = get_option('jpeg')

if enable_jpeg
  jpeg_dep = dependency('libjpeg', required: false)

  if not jpeg_dep.found()
    if meson.get_compiler('c').has_header('jpeglib.h')
      jpeg_dep = meson.get_compiler('c').find_library('jpeg', required: false)
      if not jpeg_dep.found()
        warning('JPEG image provider will not be built.')
        enable_jpeg = false
      endif
    endif
  endif

  if enable_jpeg
    library('idirectfbimageprovider_jpeg',
            'idirectfbimageprovider_jpeg.c',
            include_directories: config_inc,
            dependencies: [directfb_dep, jpeg_dep],
            install: true,
            install_dir: join_paths(moduledir, 'interfaces/IDirectFBImageProvider'))
  endif
endif

enable_png = get_option('png')

if enable_png
  png_dep = dependency('libpng', required: false)

  if not png_dep.found()
    warning('PNG image provider will not be built.')
    enable_png = false
  endif

  if enable_png
    library('idirectfbimageprovider_png',
            'idirectfbimageprovider_png.c',
            include_directories: config_inc,
            dependencies: [directfb_dep, png_dep],
            install: true,
            install_dir: join_paths(moduledir, 'interfaces/IDirectFBImageProvider'))
  endif
endif
