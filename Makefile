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

# CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, also on the command line:
# `make CFLAGS=-O0` must not lose the include paths.
CFLAGS ?= -O2 -g
SHOT_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
SHOT_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

all: screenShot

screenShot: screenShot.c version.h build/gitversion.h $(PROTO_H) $(PROTO_C)
	$(CC) $(SHOT_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ screenShot.c \
		$(PROTO_C) $(SHOT_LIBS) $(LDLIBS)

build:
	mkdir -p build

# The commit a build is from, after the version: +DATE.TIME.HASH, its commit time in
# UTC, and .dirty for changes not committed. Rewritten only when it changes, so
# screenShot is built again only then; empty without git.
build/gitversion.h: FORCE | build
	@v=$$(TZ=UTC0 git log -1 --format=%cd.%h --date=format-local:%Y%m%d.%H%M 2>/dev/null); \
	if [ -n "$$v" ] && ! git diff --quiet HEAD 2>/dev/null; then v=$$v.dirty; fi; \
	line="#define SCREENSHOT_GIT \"$${v:++$$v}\""; \
	[ "$$(cat $@ 2>/dev/null)" = "$$line" ] || echo "$$line" >$@

FORCE:

build/ext-data-control-v1-client-protocol.h: $(DATA_CONTROL) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/ext-data-control-v1-protocol.c: $(DATA_CONTROL) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/wlr-screencopy-unstable-v1-client-protocol.h: $(SCREENCOPY) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/wlr-screencopy-unstable-v1-protocol.c: $(SCREENCOPY) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/viewporter-client-protocol.h: $(VIEWPORTER) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/viewporter-protocol.c: $(VIEWPORTER) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/wlr-layer-shell-unstable-v1-client-protocol.h: $(LAYER_SHELL) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/wlr-layer-shell-unstable-v1-protocol.c: $(LAYER_SHELL) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/xdg-shell-protocol.c: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/xdg-output-unstable-v1-client-protocol.h: $(XDG_OUTPUT) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/xdg-output-unstable-v1-protocol.c: $(XDG_OUTPUT) | build
	$(WAYLAND_SCANNER) private-code $< $@

# Pastes from the clipboard and compares pictures, for the tests.
build/clipget: tests/clipget.c build/ext-data-control-v1-client-protocol.h \
		build/ext-data-control-v1-protocol.c
	$(CC) $(SHOT_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/clipget.c \
		build/ext-data-control-v1-protocol.c $(SHOT_LIBS) $(LDLIBS)
build/pngcmp: tests/pngcmp.c | build
	$(CC) $(SHOT_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/pngcmp.c \
		$(SHOT_LIBS) $(LDLIBS)
# A filesystem without hard links, and someone else's files, through LD_PRELOAD.
build/nolink.so: tests/nolink.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ tests/nolink.c
build/otheruid.so: tests/otheruid.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ tests/otheruid.c
# A name swapped between looking and opening, as in a shared folder.
build/swapopen.so: tests/swapopen.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ tests/swapopen.c -ldl

install: screenShot
	install -Dm755 screenShot $(DESTDIR)$(PREFIX)/bin/screenShot

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/screenShot

# The Makefile itself, without a compositor.
build-test: | build
	tests/build.sh

# Needs a built mwc next to this directory, or in MWC_DIR, plus grim.
test: build-test screenShot build/clipget build/pngcmp build/nolink.so build/otheruid.so \
		build/swapopen.so
	tests/test.sh

# C lines at most 100 columns, tabs as 8, as in mwc.
LINT_MAX = 100
lint:
	@awk -v max=$(LINT_MAX) '{ \
		n = 0; \
		for (i = 1; i <= length($$0); i++) \
			n = substr($$0, i, 1) == "\t" ? n + 8 - n % 8 : n + 1; \
		if (n > max) { print FILENAME ":" FNR ": " n " columns"; bad = 1 } \
	} END { exit bad }' screenShot.c tests/*.c

clean:
	rm -rf screenShot build

.PHONY: all install uninstall build-test test lint clean FORCE
