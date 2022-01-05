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

enable_ffmpeg = get_option('ffmpeg')

if enable_ffmpeg
  ffmpeg_dep = [dependency('libavcodec',  required: false),
                dependency('libavformat', required: false),
                dependency('libavutil',   required: false),
                dependency('libswscale',  required: false)]

  foreach dep : ffmpeg_dep
    if not dep.found()
      warning('FFmpeg video provider will not be built.')
      enable_ffmpeg = false
    endif
  endforeach

  if enable_ffmpeg
    library('idirectfbvideoprovider_ffmpeg',
            'idirectfbvideoprovider_ffmpeg.c',
            include_directories: config_inc,
            dependencies: [directfb_dep, ffmpeg_dep, fusionsound_dep],
            install: true,
            install_dir: join_paths(moduledir, 'interfaces/IDirectFBVideoProvider'))
  endif
endif
