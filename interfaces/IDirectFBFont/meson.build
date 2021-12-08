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

ft2_dep = [dependency('freetype2', required: false), meson.get_compiler('c').find_library('m')]

foreach dep : ft2_dep
  if not dep.found()
    warning('FreeType2 font provider will not be built.')
    subdir_done()
  endif
endforeach

library('idirectfbfont_ft2',
        'idirectfbfont_ft2.c',
        include_directories: config_inc,
        dependencies: [directfb_dep, ft2_dep],
        install: true,
        install_dir: join_paths(moduledir, 'interfaces/IDirectFBFont'))
