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

if enable_ft2
  library('idirectfbfont_ft2',
          'idirectfbfont_ft2.c',
          include_directories: config_inc,
          dependencies: [directfb_dep, ft2_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBFont'))

  pkgconfig.generate(filebase: 'directfb-interface-font_ft2',
                     variables: 'moduledir=' + moduledir,
                     name: 'DirectFB-interface-font_ft2',
                     description: 'FreeType2 font provider',
                     requires_private: 'freetype2',
                     libraries_private: ['-L${moduledir}/interfaces/IDirectFBFont',
                                         '-Wl,--whole-archive -lidirectfbfont_ft2 -Wl,--no-whole-archive', '-lm'])
endif

if enable_sft
  library('idirectfbfont_sft',
          'idirectfbfont_sft.c',
          dependencies: [directfb_dep, sft_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBFont'))

  pkgconfig.generate(filebase: 'directfb-interface-font_sft',
                     variables: 'moduledir=' + moduledir,
                     name: 'DirectFB-interface-font_sft',
                     description: 'Schrift font provider',
                     requires_private: 'libschrift',
                     libraries_private: ['-L${moduledir}/interfaces/IDirectFBFont',
                                         '-Wl,--whole-archive -lidirectfbfont_sft -Wl,--no-whole-archive'])
endif

if enable_stb_truetype != 'false'
  library('idirectfbfont_stb',
          'idirectfbfont_stb.c',
          c_args: '-DSTB_TRUETYPE_H="@0@"'.format(enable_stb_truetype),
          dependencies: [directfb_dep, stb_truetype_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBFont'))

  pkgconfig.generate(filebase: 'directfb-interface-font_stb',
                     variables: 'moduledir=' + moduledir,
                     name: 'DirectFB-interface-font_stb',
                     description: 'STB font provider',
                     libraries_private: ['-L${moduledir}/interfaces/IDirectFBFont',
                                         '-Wl,--whole-archive -lidirectfbfont_stb -Wl,--no-whole-archive', '-lm'])
endif
