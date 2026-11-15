#!/bin/bash
# The Makefile: the user's CPPFLAGS reach every compile line, generated protocol files
# follow their XML. Dry runs of make, no compositor needed.
cd "$(dirname "$0")/.." || exit 1
LOG=build/build.log
exec 3>&1 >"$LOG" 2>&1
bad() { echo "FAIL: $1 (see $LOG)" >&3; exit 1; }

for target in screenShot build/clipget build/pngcmp; do
	make -n -B CPPFLAGS=-DSHOT_BUILD_TEST "$target" | grep -v wayland-scanner |
		grep -q -- -DSHOT_BUILD_TEST || bad "CPPFLAGS don't reach $target"
done
echo "ok: CPPFLAGS reach every compile line" >&3

WP=$(pkg-config --variable=pkgdatadir wayland-protocols)
while read -r target xml; do
	make "$target" || bad "can't make $target"
	make -n -W "$xml" "$target" | grep -q wayland-scanner ||
		bad "$target isn't made again after $xml changes"
done <<LIST
build/ext-data-control-v1-client-protocol.h $WP/staging/ext-data-control/ext-data-control-v1.xml
build/ext-data-control-v1-protocol.c $WP/staging/ext-data-control/ext-data-control-v1.xml
build/viewporter-client-protocol.h $WP/stable/viewporter/viewporter.xml
build/viewporter-protocol.c $WP/stable/viewporter/viewporter.xml
build/xdg-shell-protocol.c $WP/stable/xdg-shell/xdg-shell.xml
build/xdg-output-unstable-v1-client-protocol.h $WP/unstable/xdg-output/xdg-output-unstable-v1.xml
build/xdg-output-unstable-v1-protocol.c $WP/unstable/xdg-output/xdg-output-unstable-v1.xml
build/wlr-screencopy-unstable-v1-protocol.c protocols/wlr-screencopy-unstable-v1.xml
build/wlr-layer-shell-unstable-v1-client-protocol.h protocols/wlr-layer-shell-unstable-v1.xml
LIST
echo "ok: generated protocol files are made again when their XML changes" >&3
echo "PASS: build" >&3
