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

fusionsound = []
if enable_fusionsound
  fusionsound = 'fusionsound'
endif

if enable_ffmpeg
  library('idirectfbvideoprovider_ffmpeg',
          'idirectfbvideoprovider_ffmpeg.c',
          include_directories: config_inc,
          dependencies: [directfb_dep, ffmpeg_dep, fusionsound_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBVideoProvider'))

  pkgconfig.generate(filebase: 'directfb-interface-videoprovider_ffmpeg',
                     variables: 'moduledir=' + moduledir,
                     name: 'DirectFB-interface-videoprovider_ffmpeg',
                     description: 'FFmpeg video provider',
                     requires_private: ['libavformat', 'libswscale', fusionsound],
                     libraries_private: ['-L${moduledir}/interfaces/IDirectFBVideoProvider',
                                         '-Wl,--whole-archive -lidirectfbvideoprovider_ffmpeg -Wl,--no-whole-archive'])
endif

if enable_gstreamer
  library('idirectfbvideoprovider_gstreamer',
          'idirectfbvideoprovider_gstreamer.c',
          include_directories: config_inc,
          dependencies: [directfb_dep, gstreamer_dep, fusionsound_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBVideoProvider'))

  pkgconfig.generate(filebase: 'directfb-interface-videoprovider_gstreamer',
                     variables: 'moduledir=' + moduledir,
                     name: 'DirectFB-interface-videoprovider_gstreamer',
                     description: 'GStreamer video provider',
                     requires_private: ['gstreamer-app-1.0', fusionsound],
                     libraries_private: ['-L${moduledir}/interfaces/IDirectFBVideoProvider',
                                         '-Wl,--whole-archive -lidirectfbvideoprovider_gstreamer -Wl,--no-whole-archive'])
endif

if enable_libmpeg3
  library('idirectfbvideoprovider_libmpeg3',
          'idirectfbvideoprovider_libmpeg3.c',
          include_directories: config_inc,
          dependencies: [directfb_dep, libmpeg3_dep, fusionsound_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBVideoProvider'))

  pkgconfig.generate(filebase: 'directfb-interface-videoprovider_libmpeg3',
                     variables: 'moduledir=' + moduledir,
                     name: 'DirectFB-interface-videoprovider_libmpeg3',
                     description: 'Libmpeg3 video provider',
                     requires_private: 'libmpeg3',
                     libraries_private: ['-L${moduledir}/interfaces/IDirectFBVideoProvider',
                                         '-Wl,--whole-archive -lidirectfbvideoprovider_libmpeg3 -Wl,--no-whole-archive'])
endif

if enable_mng
  library('idirectfbvideoprovider_mng',
          'idirectfbvideoprovider_mng.c',
          dependencies: [directfb_dep, mng_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBVideoProvider'))

  pkgconfig.generate(filebase: 'directfb-interface-videoprovider_mng',
                     variables: 'moduledir=' + moduledir,
                     name: 'DirectFB-interface-videoprovider_mng',
                     description: 'MNG video provider',
                     requires_private: 'libmng',
                     libraries_private: ['-L${moduledir}/interfaces/IDirectFBVideoProvider',
                                         '-Wl,--whole-archive -lidirectfbvideoprovider_mng -Wl,--no-whole-archive'])
endif

if enable_plm != 'false'
  library('idirectfbvideoprovider_plm',
          'idirectfbvideoprovider_plm.c',
          include_directories: config_inc,
          dependencies: [directfb_dep, plm_dep, fusionsound_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBVideoProvider'))

  pkgconfig.generate(filebase: 'directfb-interface-videoprovider_plm',
                     variables: 'moduledir=' + moduledir,
                     name: 'DirectFB-interface-videoprovider_plm',
                     description: 'PL_MPEG video provider',
                     libraries_private: ['-L${moduledir}/interfaces/IDirectFBVideoProvider',
                                         '-Wl,--whole-archive -lidirectfbvideoprovider_plm -Wl,--no-whole-archive'])
endif

if enable_swfdec
  library('idirectfbvideoprovider_swfdec',
          'idirectfbvideoprovider_swfdec.c',
          include_directories: config_inc,
          dependencies: [directfb_dep, swfdec_dep, fusionsound_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBVideoProvider'))

  pkgconfig.generate(filebase: 'directfb-interface-videoprovider_swfdec',
                     variables: 'moduledir=' + moduledir,
                     name: 'DirectFB-interface-videoprovider_swfdec',
                     description: 'Swfdec video provider',
                     requires_private: ['swfdec-0.5', fusionsound],
                     libraries_private: ['-L${moduledir}/interfaces/IDirectFBVideoProvider',
                                         '-Wl,--whole-archive -lidirectfbvideoprovider_swfdec -Wl,--no-whole-archive'])
endif

if enable_v4l
  library('idirectfbvideoprovider_v4l',
          'idirectfbvideoprovider_v4l.c',
          dependencies: directfb_dep,
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IDirectFBVideoProvider'))

  pkgconfig.generate(filebase: 'directfb-interface-videoprovider_v4l',
                     variables: 'moduledir=' + moduledir,
                     name: 'DirectFB-interface-videoprovider_v4l',
                     description: 'Video4Linux video provider',
                     libraries_private: ['-L${moduledir}/interfaces/IDirectFBVideoProvider',
                                         '-Wl,--whole-archive -lidirectfbvideoprovider_v4l -Wl,--no-whole-archive'])
endif
