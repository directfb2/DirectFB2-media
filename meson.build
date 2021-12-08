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

project('DirectFB2-media', 'c')

directfb_dep = dependency('directfb-internal')

moduledir = directfb_dep.get_pkgconfig_variable('moduledir')

config_h = configuration_data()
config_h.set('WORDS_BIGENDIAN', host_machine.endian() == 'big', description: 'Byte ordering is bigendian.')
configure_file(configuration: config_h, output: 'config.h')
config_inc = include_directories('.')

if get_option('ft2')
  subdir('interfaces/IDirectFBFont')
endif

if get_option('png')
  subdir('interfaces/IDirectFBImageProvider')
endif
