# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2026 gavin_darkglider
# Copyright (C) 2026 theofficialgman
# Copyright (C) 2026 Anthropic (Claude AI assistant contributions)
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.

# Makefile for the flip4_present prototype (VK_LAYER_TEGRA_dc_present)
#
# Builds an implicit Vulkan layer that fixes Tegra L4T r32.x's broken X11
# WSI presentation by presenting directly through the display controller's
# TEGRA_DC_EXT_FLIP4 ioctl instead of Vulkan WSI. See README.md.
#
# Variables (override on the make command line or in your build recipe):
#
#   CROSS_COMPILE     toolchain prefix, e.g. aarch64-linux-gnu-
#   SYSROOT           target sysroot for headers and libraries
#   PREFIX            install prefix (default /usr)
#   LAYERLIBDIR       where the shared object goes; defaults to a
#                     multiarch path under $(PREFIX)/lib but Lakka and
#                     other distros that don't use multiarch should
#                     override this to plain $(PREFIX)/lib
#   LAYERJSONDIR      where the implicit-layer JSON goes; default is
#                     /usr/share/vulkan/implicit_layer.d
#   DESTDIR           staging root for installation
#
# The library_path in the JSON is intentionally a bare filename, not an
# absolute or "./"-relative path, so the Vulkan loader searches the
# standard library dirs and the same JSON works regardless of where
# LAYERLIBDIR puts the .so -- see README.md, "Update 2026-08-19 part 2",
# for why a "./"-prefixed path (only correct for ad-hoc VK_LAYER_PATH
# testing straight from this directory) would break a real install.

CROSS_COMPILE ?=
SYSROOT       ?=
PREFIX        ?= /usr

# Default to Ubuntu/Debian multiarch. Lakka's package recipe overrides
# this to $(PREFIX)/lib.
ARCH          ?= aarch64-linux-gnu
LAYERLIBDIR   ?= $(PREFIX)/lib/$(ARCH)
LAYERJSONDIR  ?= $(PREFIX)/share/vulkan/implicit_layer.d

# CC selection:
# - If CC was set on the command line or in the environment, use that (this is
#   how Lakka and other cross-build recipes inject their toolchain).
# - Otherwise synthesize from CROSS_COMPILE (which is empty by default, giving
#   plain "gcc" — fine for native builds).
# - "make"'s built-in default of CC=cc counts as "default" origin, which is why
#   we test against that rather than using a plain ?= assignment.
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
INSTALL ?= install

ifneq ($(SYSROOT),)
SYSROOT_FLAGS := --sysroot=$(SYSROOT)
else
SYSROOT_FLAGS :=
endif

TARGET   := libVkLayer_tegra_dc_present.so
SRC      := flip_layer.c
JSON     := VkLayer_tegra_dc_present.json
MAPFILE  := flip_layer.map

CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS  += $(SYSROOT_FLAGS)
# -I. so flip_layer.c's own quoted local includes (tegra_dc_ext.h,
# gob_swizzle_spv.h, uapi/...) resolve regardless of how/where make is
# invoked from.
CFLAGS  += -I.

# Always required for a shared library, regardless of what the environment
# provides in CFLAGS:
# - -fPIC: position-independent code (mandatory for .so)
# - -fvisibility=hidden: hide internal symbols. The version script then
#   only exports vkNegotiateLoaderLayerInterfaceVersion. Importantly, we
#   do NOT export vkGetInstanceProcAddr or vkGetDeviceProcAddr — exporting
#   them causes the Vulkan loader's ICD-load path to dlsym our symbols and
#   recursively re-enter our layer while the loader-side mutex is held,
#   deadlocking vkCreateInstance. The loader gets those entrypoints from
#   the negotiate-supplied function pointers instead.
LAYER_CFLAGS := -fPIC -fvisibility=hidden

# LDFLAGS handling:
# - The environment / build recipe may set LDFLAGS to inject sysroot, search
#   paths, hardening flags, etc. We want to combine with those, NOT replace.
# - But -shared and --version-script are NON-OPTIONAL for our build (without
#   -shared the linker builds an executable, looks for _start/main, and fails).
#   So they go into a separate LAYER_LDFLAGS that's always added.
LAYER_LDFLAGS := -shared -Wl,--no-undefined -Wl,--version-script=$(MAPFILE) $(SYSROOT_FLAGS)

# LDLIBS: we deliberately do NOT link libGL, libGLX, or libX11 at build
# time. See the big comment near the top of flip_layer.c for the
# loader-deadlock rationale; the short version is that if libGL is in our
# DT_NEEDED, the Vulkan loader's dlopen() of our .so triggers libGL's
# constructor which can call back into the loader during a critical
# section, causing recursive-mutex deadlock during vkCreateInstance.
# Instead we dlopen() the GL/X11/XRandR libraries at CreateSwapchain time
# and resolve every function via dlsym.
# -ldl gives us dlopen/dlsym.
LDLIBS  := -lpthread -ldl

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC) $(MAPFILE)
	$(CC) $(LAYER_CFLAGS) $(CFLAGS) $(LAYER_LDFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

install: all
	$(INSTALL) -d $(DESTDIR)$(LAYERLIBDIR)
	$(INSTALL) -m 0755 $(TARGET) $(DESTDIR)$(LAYERLIBDIR)/$(TARGET)
	$(INSTALL) -d $(DESTDIR)$(LAYERJSONDIR)
	$(INSTALL) -m 0644 $(JSON) $(DESTDIR)$(LAYERJSONDIR)/$(JSON)

uninstall:
	rm -f $(DESTDIR)$(LAYERLIBDIR)/$(TARGET)
	rm -f $(DESTDIR)$(LAYERJSONDIR)/$(JSON)

clean:
	rm -f $(TARGET)
