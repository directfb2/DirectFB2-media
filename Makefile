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

include $(APPDIR)/Make.defs

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/graphics/DirectFB2/src

CFLAGS += -I.

ifeq ($(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_BMP),y)
CSRCS += interfaces/IDirectFBImageProvider/idirectfbimageprovider_bmp.c
endif

ifeq ($(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_GIF),y)
CSRCS += interfaces/IDirectFBImageProvider/idirectfbimageprovider_gif.c
endif

ifeq ($(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_LODEPNG),y)
CFLAGS += -I$(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_LODEPNG_DIR) -DLODEPNG_C=\"lodepng.cpp\"
CSRCS += interfaces/IDirectFBImageProvider/idirectfbimageprovider_lodepng.c
endif

ifeq ($(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_SCHRIFT),y)
CFLAGS += -I$(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_SCHRIFT_DIR)
CSRCS += $(shell echo $(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_SCHRIFT_DIR))/schrift.c
CSRCS += interfaces/IDirectFBFont/idirectfbfont_sft.c
endif

ifeq ($(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_SPNG),y)
CFLAGS += -I$(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_SPNG_DIR)/spng -DSPNG_USE_MINIZ
CFLAGS += -I$(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_MINIZ_DIR)
CSRCS += $(shell echo $(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_SPNG_DIR))/spng/spng.c
CSRCS += $(shell echo $(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_MINIZ_DIR))/miniz.c
CSRCS += $(shell echo $(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_MINIZ_DIR))/miniz_tinfl.c
CSRCS += interfaces/IDirectFBImageProvider/idirectfbimageprovider_spng.c
endif

ifeq ($(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_STB),y)
CFLAGS += -I$(CONFIG_GRAPHICS_DIRECTFB2_MEDIA_STB_DIR) -DSTB_IMAGE_H=\"stb_image.h\" -DSTB_TRUETYPE_H=\"stb_truetype.h\"
CSRCS += interfaces/IDirectFBFont/idirectfbfont_stb.c
CSRCS += interfaces/IDirectFBImageProvider/idirectfbimageprovider_stb.c
endif

config.h:
	$(Q) echo "#pragma once" > $@

context:: config.h

distclean::
	$(call DELFILE, config.h)

include $(APPDIR)/Application.mk
