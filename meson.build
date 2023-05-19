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

project('DirectFB2-media', 'c',
        version: '2.0.0',
        meson_version: '>= 0.57')

directfb_dep = dependency('directfb-internal')

moduledir = directfb_dep.get_variable(pkgconfig: 'moduledir')

config_h = configuration_data()

config_h.set('WORDS_BIGENDIAN', host_machine.endian() == 'big', description: 'Byte ordering is bigendian.')

enable_fusionsound = get_option('fusionsound')

fusionsound_dep = []
if enable_fusionsound
  fusionsound_dep = dependency('fusionsound-internal', required: false)
  if fusionsound_dep.found()
    config_h.set('HAVE_FUSIONSOUND', 1, description: 'Define to 1 if you are compiling FusionSound support.')
  else
    enable_fusionsound = false
  endif
endif

configure_file(configuration: config_h, output: 'config.h')

config_inc = include_directories('.')

fs = import('fs')

enable_bmp          = get_option('bmp')
enable_ffmpeg       = get_option('ffmpeg')
enable_ft2          = get_option('ft2')
enable_gif          = get_option('gif')
enable_gstreamer    = get_option('gstreamer')
enable_imlib2       = get_option('imlib2')
enable_jasper       = get_option('jasper')
enable_jpeg         = get_option('jpeg')
enable_lodepng      = get_option('lodepng')
enable_mad          = get_option('mad')
enable_mng          = get_option('mng')
enable_openexr      = get_option('openexr')
enable_playlist     = get_option('playlist')
enable_png          = get_option('png')
enable_sft          = get_option('sft')
enable_stb_image    = get_option('stb_image')
enable_stb_truetype = get_option('stb_truetype')
enable_svg          = get_option('svg')
enable_swfdec       = get_option('swfdec')
enable_tiff         = get_option('tiff')
enable_tremor       = get_option('tremor')
enable_v4l          = get_option('v4l')
enable_vorbis       = get_option('vorbis')
enable_webp         = get_option('webp')
enable_yuv          = get_option('yuv')

if enable_ffmpeg
  ffmpeg_dep = [dependency('libavcodec',  required: false),
                dependency('libavformat', required: false),
                dependency('libavutil',   required: false),
                dependency('libswscale',  required: false)]

  if enable_fusionsound
    ffmpeg_dep += dependency('libswresample', required: false)
  endif

  foreach dep : ffmpeg_dep
    if not dep.found()
      warning('FFmpeg image provider, music provider and video provider will not be built.')
      enable_ffmpeg = false
      break
    endif
  endforeach
endif

if enable_ft2
  ft2_dep = [dependency('freetype2', required: false),
             meson.get_compiler('c').find_library('m', required: false)]

  foreach dep : ft2_dep
    if not dep.found()
      warning('FreeType2 font provider will not be built.')
      enable_ft2 = false
      break
    endif
  endforeach
endif

if enable_gstreamer
  gstreamer_dep = dependency('gstreamer-app-1.0', required: false)

  if not gstreamer_dep.found()
    warning('GStreamer video provider will not be built.')
    enable_gstreamer = false
  endif
endif

if enable_imlib2
  imlib2_dep = dependency('imlib2', required: false)

  if not imlib2_dep.found()
    warning('Imlib2 image provider will not be built.')
    enable_imlib2 = false
  endif
endif

if enable_jasper
  jasper_dep = dependency('jasper', required: false)

  if not jasper_dep.found()
    warning('JasPer image provider will not be built.')
    enable_jasper = false
  endif
endif

if enable_jpeg
  jpeg_dep = dependency('libjpeg', required: false)

  if not jpeg_dep.found()
    warning('JPEG image provider will not be built.')
    enable_jpeg = false
  endif
endif

if enable_lodepng != 'false'
  if not fs.is_dir(enable_lodepng)
    warning('LodePNG image provider will not be built.')
    enable_lodepng = 'false'
  else
    lodepng_dep = declare_dependency(include_directories: include_directories(enable_lodepng))
  endif
endif

if enable_mad and enable_fusionsound
  mad_dep = dependency('mad', required: false)

  if not mad_dep.found()
    warning('MAD music provider will not be built.')
    enable_mad = false
  endif
endif

if enable_mng
  mng_dep = dependency('libmng', required: false)

  if not mng_dep.found()
    warning('MNG video provider will not be built.')
    enable_mng = false
  endif
endif

if enable_openexr
  openexr_dep = dependency('OpenEXR', required: false)

  if not openexr_dep.found()
    warning('OpenEXR image provider will not be built.')
    enable_openexr = false
  endif
endif

if enable_png
  png_dep = dependency('libpng', required: false)

  if not png_dep.found()
    warning('PNG image provider will not be built.')
    enable_png = false
  endif
endif

if enable_sft
  sft_dep = dependency('libschrift', required: false)

  if not sft_dep.found()
    warning('Schrift font provider will not be built.')
    enable_sft = false
  endif
endif

if enable_stb_image != 'false'
  if not fs.is_file(enable_stb_image)
    warning('STB image provider will not be built.')
    enable_stb_image = 'false'
  endif
endif

if enable_stb_truetype != 'false'
  stb_truetype_dep = meson.get_compiler('c').find_library('m', required: false)

  if not fs.is_file(enable_stb_truetype) or not stb_truetype_dep.found()
    warning('STB font provider will not be built.')
    enable_stb_truetype = 'false'
  endif
endif

if enable_svg
  svg_dep = dependency('libsvg-cairo', required: false)

  if not svg_dep.found()
    warning('SVG image provider will not be built.')
    enable_svg = false
  endif
endif

if enable_swfdec
  swfdec_dep = dependency('swfdec-0.5', required: false)

  if not swfdec_dep.found()
    warning('Swfdec video provider will not be built.')
    enable_swfdec = false
  endif
endif

if enable_tiff
  tiff_dep = dependency('libtiff-4', required: false)

  if not tiff_dep.found()
    warning('TIFF image provider will not be built.')
    enable_tiff = false
  endif
endif

if enable_tremor and enable_fusionsound
  tremor_dep = dependency('vorbisidec', required: false)

  if not tremor_dep.found()
    warning('Tremor music provider will not be built.')
    enable_tremor = false
  endif
endif

if enable_vorbis and enable_fusionsound
  vorbis_dep = [dependency('vorbisfile', required: false),
                meson.get_compiler('c').find_library('m', required: false)]

  foreach dep : vorbis_dep
    if not dep.found()
      warning('Vorbis music provider will not be built.')
      enable_vorbis = false
      break
    endif
  endforeach
endif

if enable_webp
  webp_dep = dependency('libwebp', required: false)

  if not webp_dep.found()
    warning('WebP image provider will not be built.')
    enable_webp = false
  endif
endif

pkgconfig = import('pkgconfig')

subdir('interfaces/IDirectFBFont')
subdir('interfaces/IDirectFBImageProvider')
subdir('interfaces/IDirectFBVideoProvider')
if enable_fusionsound
  subdir('interfaces/IFusionSoundMusicProvider')
endif

if get_option('default_library') != 'shared'
  meson.add_install_script('dfb-update-pkgconfig', skip_if_destdir: true)
  if enable_fusionsound
    meson.add_install_script('fs-update-pkgconfig', skip_if_destdir: true)
  endif
endif

summary = [
  '',
  'Building Font Modules:',
  '  FreeType2   @0@'.format(enable_ft2),
  '  Schrift     @0@'.format(enable_sft),
  '  STB         @0@'.format(enable_stb_truetype),
  '',
  'Building Image Provider Modules:',
  '  BMP         @0@'.format(enable_bmp),
  '  FFmpeg      @0@'.format(enable_ffmpeg),
  '  GIF         @0@'.format(enable_gif),
  '  Imlib2      @0@'.format(enable_imlib2),
  '  JasPer      @0@'.format(enable_jasper),
  '  JPEG        @0@'.format(enable_jpeg),
  '  LodePNG     @0@'.format(enable_lodepng),
  '  OpenEXR     @0@'.format(enable_openexr),
  '  PNG         @0@'.format(enable_png),
  '  STB         @0@'.format(enable_stb_image),
  '  SVG         @0@'.format(enable_svg),
  '  TIFF        @0@'.format(enable_tiff),
  '  WebP        @0@'.format(enable_webp),
  '  YUV         @0@'.format(enable_yuv),
  '',
  'Building Video Provider Modules:',
  '  FFmpeg      @0@'.format(enable_ffmpeg),
  '  GStreamer   @0@'.format(enable_gstreamer),
  '  MNG         @0@'.format(enable_mng),
  '  Swfdec      @0@'.format(enable_swfdec),
  '  Video4Linux @0@'.format(enable_v4l),
  ''
]

if enable_fusionsound
summary += [
  'Building Music Provider Modules:',
  '  FFmpeg      @0@'.format(enable_ffmpeg),
  '  MAD         @0@'.format(enable_mad),
  '  Playlist    @0@'.format(enable_playlist),
  '  Tremor      @0@'.format(enable_tremor),
  '  Vorbis      @0@'.format(enable_vorbis),
  ''
]
endif

message('\n  '.join(summary))
