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

if enable_ffmpeg
  library('ifusionsoundmusicprovider_ffmpeg',
          'ifusionsoundmusicprovider_ffmpeg.c',
          dependencies: [fusionsound_dep, ffmpeg_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IFusionSoundMusicProvider'))

  pkgconfig.generate(filebase: 'fusionsound-interface-musicprovider_ffmpeg',
                     variables: 'moduledir=' + moduledir,
                     name: 'FusionSound-interface-musicprovider_ffmpeg',
                     description: 'FFmpeg music provider',
                     requires_private: 'libavformat',
                     libraries_private: ['-L${moduledir}/interfaces/IFusionSoundMusicProvider',
                                         '-Wl,--whole-archive -lifusionsoundmusicprovider_ffmpeg -Wl,--no-whole-archive'])
endif

if enable_mad
  library('ifusionsoundmusicprovider_mad',
          'ifusionsoundmusicprovider_mad.c',
          include_directories: config_inc,
          dependencies: [fusionsound_dep, mad_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IFusionSoundMusicProvider'))

  pkgconfig.generate(filebase: 'fusionsound-interface-musicprovider_mad',
                     variables: 'moduledir=' + moduledir,
                     name: 'FusionSound-interface-musicprovider_mad',
                     description: 'MAD music provider',
                     requires_private: 'libmad',
                     libraries_private: ['-L${moduledir}/interfaces/IFusionSoundMusicProvider',
                                         '-Wl,--whole-archive -lifusionsoundmusicprovider_mad -Wl,--no-whole-archive'])
endif

if enable_playlist
  library('ifusionsoundmusicprovider_playlist',
          'ifusionsoundmusicprovider_playlist.c',
          dependencies: fusionsound_dep,
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IFusionSoundMusicProvider'))

  pkgconfig.generate(filebase: 'fusionsound-interface-musicprovider_playlist',
                     variables: 'moduledir=' + moduledir,
                     name: 'FusionSound-interface-musicprovider_playlist',
                     description: 'Playlist music provider',
                     libraries_private: ['-L${moduledir}/interfaces/IFusionSoundMusicProvider',
                                         '-Wl,--whole-archive -lifusionsoundmusicprovider_playlist -Wl,--no-whole-archive'])
endif

if enable_vorbis
  library('ifusionsoundmusicprovider_vorbis',
          'ifusionsoundmusicprovider_vorbis.c',
          include_directories: config_inc,
          dependencies: [fusionsound_dep, vorbis_dep],
          install: true,
          install_dir: join_paths(moduledir, 'interfaces/IFusionSoundMusicProvider'))

  pkgconfig.generate(filebase: 'fusionsound-interface-musicprovider_vorbis',
                     variables: 'moduledir=' + moduledir,
                     name: 'FusionSound-interface-musicprovider_vorbis',
                     description: 'Vorbis music provider',
                     requires_private: 'vorbisfile',
                     libraries_private: ['-L${moduledir}/interfaces/IFusionSoundMusicProvider',
                                         '-Wl,--whole-archive -lifusionsoundmusicprovider_vorbis -Wl,--no-whole-archive'])
endif
