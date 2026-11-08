#!/bin/bash
# Black-box test of msc under a headless mwc. Every picture is checked against grim's of
# the same screen, pixel for pixel. It has its own Wayland socket and runtime directory.
#
# Needs a built mwc in MWC_DIR (by default ../mwc), grim, and from here build/clipget and
# build/pngcmp. Output goes to build/test.log.
MSC_DIR=$(cd "$(dirname "$0")/.." && pwd)
MSC=$MSC_DIR/msc
CLIPGET=$MSC_DIR/build/clipget
PNGCMP=$MSC_DIR/build/pngcmp
LOG=$MSC_DIR/build/test.log
MWC_DIR=${MWC_DIR:-$MSC_DIR/../mwc}
for tool in "$MSC" "$CLIPGET" "$PNGCMP"; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
cd "$MWC_DIR" || exit 1
. tests/lib.sh
require_tools grim

export HOME=$SMOKE_DIR/home
mkdir -p "$HOME" "$XDG_CONFIG_HOME/mwc"
WALL=$SMOKE_DIR/wall.png
"$PNGCMP" gen "$WALL" 1280 720 || fail "cannot make the wallpaper"
export MWC_WALLPAPER=$WALL
MSC_ERR=$SMOKE_DIR/msc.err

check() { # <description> <shell condition>
	eval "$2" || fail "$1"
	echo "ok: $1"
}

run_msc() { # [args...]: with WAYLAND_DISPLAY, output in $OUT, status in $STATUS
	OUT=$(WAYLAND_DISPLAY=wayland-0 "$MSC" "$@" 2>"$MSC_ERR")
	STATUS=$?
	cat "$MSC_ERR" >>"$LOG"
}

# same_as_grim <description> <file> [grim args...]: grim sees the same screen.
same_as_grim() {
	local what=$1 file=$2 diff
	shift 2
	WAYLAND_DISPLAY=wayland-0 grim "$@" "$SMOKE_DIR/grim.png" 2>>"$LOG" ||
		fail "$what: grim failed"
	diff=$("$PNGCMP" same "$SMOKE_DIR/grim.png" "$file") || fail "$what: $diff"
	echo "ok: $what"
}

# The wallpaper is up once the middle of the screen is no longer the plain colour.
begin() {
	start_mwc 'exec sleep 300'
	local i
	for ((i = 0; i < 50; i++)); do
		WAYLAND_DISPLAY=wayland-0 "$MSC" -f "$SMOKE_DIR/up.png" >/dev/null 2>&1 &&
			WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/up2.png" 2>/dev/null &&
			"$PNGCMP" same "$SMOKE_DIR/up.png" "$SMOKE_DIR/up2.png" >/dev/null &&
			"$PNGCMP" same "$SMOKE_DIR/up.png" "$WALL" >/dev/null && break
		sleep 0.1
	done
}

# ---- the command line -----------------------------------------------------------------

check "-v prints the version" "[ \"\$(\"$MSC\" -v)\" = \"msc \$(sed -n 's/.*MSC_VERSION \"\\(.*\\)\"/\\1/p' \"$MSC_DIR/version.h\")\" ]"
"$MSC" -x >/dev/null 2>&1
check "an unknown option: status 2" "[ $? = 2 ]"
"$MSC" -c -f x.png >/dev/null 2>&1
check "-c and -f together: status 2" "[ $? = 2 ]"
"$MSC" -d soon >/dev/null 2>&1
check "-d wants a number" "[ $? = 2 ]"

# ---- one monitor, a file ---------------------------------------------------------------

begin
run_msc -f "$SMOKE_DIR/a.png"
check "-f: status 0" "[ $STATUS = 0 ]"
check "-f prints the path" "[ \"\$OUT\" = \"$SMOKE_DIR/a.png\" ]"
check "-f: what is on the screen, the wallpaper" "\"$PNGCMP\" same \"$SMOKE_DIR/a.png\" \"$WALL\""
same_as_grim "-f: the same as grim" "$SMOKE_DIR/a.png"
check "-f: no temporary file is left" "[ -z \"\$(ls -A \"$SMOKE_DIR\" | grep '^\\.msc-')\" ]"
(cd "$SMOKE_DIR" && WAYLAND_DISPLAY=wayland-0 "$MSC" -f rel.png >"$SMOKE_DIR/rel.out" 2>>"$LOG")
check "-f with a relative path prints it absolute" "[ \"\$(cat \"$SMOKE_DIR/rel.out\")\" = \"$SMOKE_DIR/rel.png\" ]"
run_msc -f "$SMOKE_DIR/a.png"
check "-f overwrites a file it is told to" "[ $STATUS = 0 ]"
WAYLAND_DISPLAY=wayland-0 "$MSC" -f - >"$SMOKE_DIR/stdout.png" 2>>"$LOG"
check "-f - writes the PNG to stdout" "\"$PNGCMP\" same \"$SMOKE_DIR/stdout.png\" \"$WALL\""
run_msc -f "$SMOKE_DIR/no/such/dir/a.png"
check "-f into a folder that is not there: status 1" "[ $STATUS = 1 ]"
check "... and a message" "grep -q 'cannot write' \"$MSC_ERR\""

run_msc -l
check "-l lists the monitor" "[ \"\$OUT\" = 'HEADLESS-1 1280x720+0+0 scale 1' ]"
run_msc -o HEADLESS-1 -f "$SMOKE_DIR/o.png"
check "-o HEADLESS-1" "[ $STATUS = 0 ] && \"$PNGCMP\" same \"$SMOKE_DIR/o.png\" \"$WALL\""
run_msc -o DP-9 -f "$SMOKE_DIR/o.png"
check "-o with a name that is not there: status 1" "[ $STATUS = 1 ]"
check "... which says how to find the names" "grep -q 'msc -l' \"$MSC_ERR\""
run_msc -p -f "$SMOKE_DIR/p.png"
check "-p: status 0" "[ $STATUS = 0 ]"
run_msc -d 0.3 -f "$SMOKE_DIR/d.png"
check "-d 0.3: status 0" "[ $STATUS = 0 ]"

# ---- the pictures folder ----------------------------------------------------------------

run_msc
check "no options: status 0" "[ $STATUS = 0 ]"
check "no user-dirs.dirs: into ~/Pictures/Screenshots" \
	"[[ \"\$OUT\" == \"$HOME/Pictures/Screenshots/Screenshot_\"*.png ]] && [ -f \"\$OUT\" ]"
check "named by the date and time" \
	"[[ \"\$(basename \"\$OUT\")\" =~ ^Screenshot_[0-9]{4}-[0-9]{2}-[0-9]{2}_[0-9]{2}-[0-9]{2}-[0-9]{2}\\.png$ ]]"
same_as_grim "the picture is the screen" "$OUT"

cat >"$XDG_CONFIG_HOME/user-dirs.dirs" <<'DIRS'
# written by xdg-user-dirs-update
XDG_DESKTOP_DIR="$HOME/Schreibtisch"
XDG_PICTURES_DIR="$HOME/Bilder"
DIRS
first=
for i in 1 2 3; do
	run_msc
	[ "$STATUS" = 0 ] || fail "into XDG_PICTURES_DIR: status $STATUS"
	first=${first:-$OUT}
done
check "XDG_PICTURES_DIR from user-dirs.dirs: ~/Bilder/Screenshots" \
	"[[ \"\$first\" == \"$HOME/Bilder/Screenshots/Screenshot_\"*.png ]]"
check "three in a row are three files, none overwritten" \
	"[ \"\$(ls \"$HOME/Bilder/Screenshots\" | grep -c '^Screenshot_.*\\.png$')\" = 3 ]"
echo 'XDG_PICTURES_DIR="/abs/elsewhere/../x"' >"$XDG_CONFIG_HOME/user-dirs.dirs"
echo "XDG_PICTURES_DIR=\"$SMOKE_DIR/abs pics\"" >>"$XDG_CONFIG_HOME/user-dirs.dirs"
run_msc
check "an absolute XDG_PICTURES_DIR, the last line wins, spaces are fine" \
	"[[ \"\$OUT\" == \"$SMOKE_DIR/abs pics/Screenshots/\"*.png ]]"
rm "$XDG_CONFIG_HOME/user-dirs.dirs"

# ---- without WAYLAND_DISPLAY -------------------------------------------------------------

# As from an ssh login or a Claude session started outside the desktop.
OUT=$(env -u WAYLAND_DISPLAY "$MSC" -f "$SMOKE_DIR/nodisp.png" 2>>"$LOG")
check "without WAYLAND_DISPLAY it finds the compositor in XDG_RUNTIME_DIR" \
	"[ \"\$OUT\" = \"$SMOKE_DIR/nodisp.png\" ]"
same_as_grim "... and takes its screen" "$SMOKE_DIR/nodisp.png"
# Something else listening first: a dead socket, a lock file.
: >"$SMOKE_DIR/wayland-.lock"
OUT=$(env -u WAYLAND_DISPLAY "$MSC" -l 2>>"$LOG")
check "... past a lock file" "[ \"\$OUT\" = 'HEADLESS-1 1280x720+0+0 scale 1' ]"
rm -f "$SMOKE_DIR/wayland-.lock"
WAYLAND_DISPLAY=wayland-9 "$MSC" -l >/dev/null 2>"$MSC_ERR"
check "a WAYLAND_DISPLAY that is not there: status 1" "[ $? = 1 ]"
check "... and it says which" "grep -q 'wayland-9' \"$MSC_ERR\""
env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$SMOKE_DIR/home" "$MSC" -l >/dev/null 2>"$MSC_ERR"
check "no compositor in the runtime directory: status 1" "[ $? = 1 ]"

# ---- the clipboard -------------------------------------------------------------------------

msc_count() { pgrep -x msc | wc -l; }
run_msc -c
check "-c: status 0, at once" "[ $STATUS = 0 ]"
check "-c: nothing on stdout" "[ -z \"\$OUT\" ]"
check "-c offers image/png" "[ \"\$(WAYLAND_DISPLAY=wayland-0 \"$CLIPGET\" types)\" = image/png ]"
WAYLAND_DISPLAY=wayland-0 "$CLIPGET" image/png >"$SMOKE_DIR/clip.png" 2>>"$LOG"
same_as_grim "-c: what comes out of the clipboard is the screen" "$SMOKE_DIR/clip.png"
WAYLAND_DISPLAY=wayland-0 "$CLIPGET" image/png >"$SMOKE_DIR/clip2.png" 2>>"$LOG"
same_as_grim "it can be pasted again" "$SMOKE_DIR/clip2.png"
check "one msc stays behind for the clipboard" "[ \"\$(msc_count)\" = 1 ]"
run_msc -c
wait_for "a new copy ends the one before" test "$(msc_count)" = 1
WAYLAND_DISPLAY=wayland-0 "$CLIPGET" image/png >"$SMOKE_DIR/clip3.png" 2>>"$LOG"
same_as_grim "and the new one serves" "$SMOKE_DIR/clip3.png"
stop_mwc
for ((i = 0; i < 25; i++)); do
	[ "$(msc_count)" = 0 ] && break
	sleep 0.2
done
check "the clipboard msc ends with the compositor" "[ \"\$(msc_count)\" = 0 ]"

# ---- turned, scaled and several monitors -------------------------------------------------------

for t in 90 180 270 flipped flipped-90 flipped-180 flipped-270; do
	echo "Mwm*HEADLESS-1*outputTransform: $t" >"$XDG_CONFIG_HOME/mwc/mwmrc"
	begin
	run_msc -f "$SMOKE_DIR/t.png"
	[ "$STATUS" = 0 ] || fail "transform $t: status $STATUS"
	same_as_grim "transform $t: the same as grim" "$SMOKE_DIR/t.png"
	stop_mwc
done

printf 'Mwm*HEADLESS-1*outputScale: 2\n' >"$XDG_CONFIG_HOME/mwc/mwmrc"
begin
run_msc -f "$SMOKE_DIR/s.png"
check "scale 2: every pixel of the monitor" "[ \"\$(\"$PNGCMP\" size \"$SMOKE_DIR/s.png\")\" = 1280x720 ]"
same_as_grim "scale 2: the same as grim" "$SMOKE_DIR/s.png"
run_msc -l
check "-l at scale 2: logical size" "[ \"\$OUT\" = 'HEADLESS-1 640x360+0+0 scale 2' ]"
stop_mwc
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

export WLR_HEADLESS_OUTPUTS=2
begin
run_msc -l
check "-l with two monitors" "[ \"\$(echo \"\$OUT\" | wc -l)\" = 2 ]"
run_msc -f "$SMOKE_DIR/two.png"
check "two monitors side by side: one picture" "[ \"\$(\"$PNGCMP\" size \"$SMOKE_DIR/two.png\")\" = 2560x720 ]"
same_as_grim "two monitors: the same as grim" "$SMOKE_DIR/two.png"
run_msc -l
second=$(echo "$OUT" | sed -n 2p | cut -d' ' -f1)
run_msc -o "$second" -f "$SMOKE_DIR/second.png"
same_as_grim "-o $second: only that one" "$SMOKE_DIR/second.png" -o "$second"
stop_mwc
unset WLR_HEADLESS_OUTPUTS

echo "PASS: msc"
