# screenShot - a screenshot of the screen or a rectangle, into a file or the clipboard

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = wayland-client wayland-cursor cairo

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
XDG_OUTPUT = $(WAYLAND_PROTOCOLS)/unstable/xdg-output/xdg-output-unstable-v1.xml
DATA_CONTROL = $(WAYLAND_PROTOCOLS)/staging/ext-data-control/ext-data-control-v1.xml
VIEWPORTER = $(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml
XDG_SHELL = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
SCREENCOPY = protocols/wlr-screencopy-unstable-v1.xml
LAYER_SHELL = protocols/wlr-layer-shell-unstable-v1.xml

PROTO_H = build/ext-data-control-v1-client-protocol.h \
	build/viewporter-client-protocol.h \
	build/wlr-layer-shell-unstable-v1-client-protocol.h \
	build/wlr-screencopy-unstable-v1-client-protocol.h \
	build/xdg-output-unstable-v1-client-protocol.h
# The layer shell refers to xdg_popup, so the code of xdg-shell has to be there.
PROTO_C = build/ext-data-control-v1-protocol.c build/viewporter-protocol.c \
	build/wlr-layer-shell-unstable-v1-protocol.c \
	build/wlr-screencopy-unstable-v1-protocol.c build/xdg-output-unstable-v1-protocol.c \
	build/xdg-shell-protocol.c

# CFLAGS and LDLIBS stay the user's, also on the command line: `make CFLAGS=-O0`
# must not lose the include paths.
CFLAGS ?= -O2 -g
SHOT_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
SHOT_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

all: screenShot

screenShot: screenShot.c version.h $(PROTO_H) $(PROTO_C)
	$(CC) $(SHOT_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ screenShot.c $(PROTO_C) $(SHOT_LIBS) $(LDLIBS)

build:
	mkdir -p build

build/ext-data-control-v1-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(DATA_CONTROL) $@
build/ext-data-control-v1-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(DATA_CONTROL) $@
build/wlr-screencopy-unstable-v1-client-protocol.h: $(SCREENCOPY) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/wlr-screencopy-unstable-v1-protocol.c: $(SCREENCOPY) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/viewporter-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(VIEWPORTER) $@
build/viewporter-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(VIEWPORTER) $@
build/wlr-layer-shell-unstable-v1-client-protocol.h: $(LAYER_SHELL) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/wlr-layer-shell-unstable-v1-protocol.c: $(LAYER_SHELL) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/xdg-shell-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL) $@
build/xdg-output-unstable-v1-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(XDG_OUTPUT) $@
build/xdg-output-unstable-v1-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(XDG_OUTPUT) $@

# Pastes from the clipboard and compares pictures, for the tests.
build/clipget: tests/clipget.c build/ext-data-control-v1-client-protocol.h \
		build/ext-data-control-v1-protocol.c
	$(CC) $(SHOT_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/clipget.c \
		build/ext-data-control-v1-protocol.c $(SHOT_LIBS) $(LDLIBS)
build/pngcmp: tests/pngcmp.c | build
	$(CC) $(SHOT_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/pngcmp.c $(SHOT_LIBS) $(LDLIBS)

install: screenShot
	install -Dm755 screenShot $(DESTDIR)$(PREFIX)/bin/screenShot

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/screenShot

# Needs a built mwc next to this directory, or in MWC_DIR, plus grim.
test: screenShot build/clipget build/pngcmp
	tests/test.sh

clean:
	rm -rf screenShot build

.PHONY: all install uninstall test clean
