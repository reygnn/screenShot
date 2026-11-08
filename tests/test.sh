#!/bin/bash
# Black-box test of screenShot under a headless mwc. Every picture is checked against grim's of
# the same screen, pixel for pixel. It has its own Wayland socket and runtime directory.
#
# Needs a built mwc in MWC_DIR (by default ../mwc), grim, and from here build/clipget and
# build/pngcmp. Output goes to build/test.log.
SHOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
SHOT=$SHOT_DIR/screenShot
CLIPGET=$SHOT_DIR/build/clipget
PNGCMP=$SHOT_DIR/build/pngcmp
LOG=$SHOT_DIR/build/test.log
MWC_DIR=${MWC_DIR:-$SHOT_DIR/../mwc}
for tool in "$SHOT" "$CLIPGET" "$PNGCMP"; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
cd "$MWC_DIR" || exit 1
. tests/lib.sh
require_tools grim
for tool in build/vinput build/pngtool; do
	[ -x "$tool" ] || { echo "FAIL: $MWC_DIR/$tool not built (make $tool)"; exit 1; }
done
# For the simulated mouse and keyboard.
export MWC_TEST_STATE=$SMOKE_DIR/state

export HOME=$SMOKE_DIR/home
mkdir -p "$HOME" "$XDG_CONFIG_HOME/mwc"
WALL=$SMOKE_DIR/wall.png
"$PNGCMP" gen "$WALL" 1280 720 || fail "cannot make the wallpaper"
export MWC_WALLPAPER=$WALL
SHOT_ERR=$SMOKE_DIR/screenShot.err

check() { # <description> <shell condition>
	eval "$2" || fail "$1"
	echo "ok: $1"
}

run_shot() { # [args...]: with WAYLAND_DISPLAY, output in $OUT, status in $STATUS
	OUT=$(WAYLAND_DISPLAY=wayland-0 "$SHOT" "$@" 2>"$SHOT_ERR")
	STATUS=$?
	cat "$SHOT_ERR" >>"$LOG"
}

# same_as_grim <description> <file> [grim args...]: grim sees the same screen.
same_as_grim() {
	local what=$1 file=$2 diff
	shift 2
	WAYLAND_DISPLAY=wayland-0 grim "$@" "$SMOKE_DIR/grim.png" 2>>"$LOG" ||
		fail "$what: grim failed"
	if ! diff=$("$PNGCMP" same "$SMOKE_DIR/grim.png" "$file"); then
		cp "$SMOKE_DIR/grim.png" "$SHOT_DIR/build/fail-grim.png"
		cp "$file" "$SHOT_DIR/build/fail-screenShot.png"
		fail "$what: $diff (pictures in build/fail-*.png)"
	fi
	echo "ok: $what"
}

# The wallpaper is up once the middle of the screen is no longer the plain colour.
begin() {
	start_mwc 'exec sleep 300'
	local i
	for ((i = 0; i < 50; i++)); do
		WAYLAND_DISPLAY=wayland-0 "$SHOT" -f "$SMOKE_DIR/up.png" >/dev/null 2>&1 &&
			WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/up2.png" 2>/dev/null &&
			"$PNGCMP" same "$SMOKE_DIR/up.png" "$SMOKE_DIR/up2.png" >/dev/null &&
			"$PNGCMP" same "$SMOKE_DIR/up.png" "$WALL" >/dev/null && break
		sleep 0.1
	done
}

# ---- the command line -----------------------------------------------------------------

check "-v prints the version" "[ \"\$(\"$SHOT\" -v)\" = \"screenShot \$(sed -n 's/.*SCREENSHOT_VERSION \"\\(.*\\)\"/\\1/p' \"$SHOT_DIR/version.h\")\" ]"
"$SHOT" -x >/dev/null 2>&1
check "an unknown option: status 2" "[ $? = 2 ]"
"$SHOT" -c -f x.png >/dev/null 2>&1
check "-c and -f together: status 2" "[ $? = 2 ]"
"$SHOT" -d soon >/dev/null 2>&1
check "-d wants a number" "[ $? = 2 ]"

# ---- one monitor, a file ---------------------------------------------------------------

begin
run_shot -f "$SMOKE_DIR/a.png"
check "-f: status 0" "[ $STATUS = 0 ]"
check "-f prints the path" "[ \"\$OUT\" = \"$SMOKE_DIR/a.png\" ]"
check "-f: what is on the screen, the wallpaper" "\"$PNGCMP\" same \"$SMOKE_DIR/a.png\" \"$WALL\""
same_as_grim "-f: the same as grim" "$SMOKE_DIR/a.png"
check "-f: no temporary file is left" "[ -z \"\$(ls -A \"$SMOKE_DIR\" | grep '^\\.screenShot-')\" ]"
(cd "$SMOKE_DIR" && WAYLAND_DISPLAY=wayland-0 "$SHOT" -f rel.png >"$SMOKE_DIR/rel.out" 2>>"$LOG")
check "-f with a relative path prints it absolute" "[ \"\$(cat \"$SMOKE_DIR/rel.out\")\" = \"$SMOKE_DIR/rel.png\" ]"
run_shot -f "$SMOKE_DIR/a.png"
check "-f overwrites a file it is told to" "[ $STATUS = 0 ]"
WAYLAND_DISPLAY=wayland-0 "$SHOT" -f - >"$SMOKE_DIR/stdout.png" 2>>"$LOG"
check "-f - writes the PNG to stdout" "\"$PNGCMP\" same \"$SMOKE_DIR/stdout.png\" \"$WALL\""
run_shot -f "$SMOKE_DIR/no/such/dir/a.png"
check "-f into a folder that is not there: status 1" "[ $STATUS = 1 ]"
check "... and a message" "grep -q 'cannot write' \"$SHOT_ERR\""

run_shot -l
check "-l lists the monitor" "[ \"\$OUT\" = 'HEADLESS-1 1280x720+0+0 scale 1' ]"
run_shot -o HEADLESS-1 -f "$SMOKE_DIR/o.png"
check "-o HEADLESS-1" "[ $STATUS = 0 ] && \"$PNGCMP\" same \"$SMOKE_DIR/o.png\" \"$WALL\""
run_shot -o DP-9 -f "$SMOKE_DIR/o.png"
check "-o with a name that is not there: status 1" "[ $STATUS = 1 ]"
check "... which says how to find the names" "grep -q 'screenShot -l' \"$SHOT_ERR\""
run_shot -p -f "$SMOKE_DIR/p.png"
check "-p: status 0" "[ $STATUS = 0 ]"
run_shot -d 0.3 -f "$SMOKE_DIR/d.png"
check "-d 0.3: status 0" "[ $STATUS = 0 ]"

# ---- the pictures folder ----------------------------------------------------------------

run_shot
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
	run_shot
	[ "$STATUS" = 0 ] || fail "into XDG_PICTURES_DIR: status $STATUS"
	first=${first:-$OUT}
done
check "XDG_PICTURES_DIR from user-dirs.dirs: ~/Bilder/Screenshots" \
	"[[ \"\$first\" == \"$HOME/Bilder/Screenshots/Screenshot_\"*.png ]]"
check "three in a row are three files, none overwritten" \
	"[ \"\$(ls \"$HOME/Bilder/Screenshots\" | grep -c '^Screenshot_.*\\.png$')\" = 3 ]"
echo 'XDG_PICTURES_DIR="/abs/elsewhere/../x"' >"$XDG_CONFIG_HOME/user-dirs.dirs"
echo "XDG_PICTURES_DIR=\"$SMOKE_DIR/abs pics\"" >>"$XDG_CONFIG_HOME/user-dirs.dirs"
run_shot
check "an absolute XDG_PICTURES_DIR, the last line wins, spaces are fine" \
	"[[ \"\$OUT\" == \"$SMOKE_DIR/abs pics/Screenshots/\"*.png ]]"
rm "$XDG_CONFIG_HOME/user-dirs.dirs"

# ---- without WAYLAND_DISPLAY -------------------------------------------------------------

# As from an ssh login or a Claude session started outside the desktop.
OUT=$(env -u WAYLAND_DISPLAY "$SHOT" -f "$SMOKE_DIR/nodisp.png" 2>>"$LOG")
check "without WAYLAND_DISPLAY it finds the compositor in XDG_RUNTIME_DIR" \
	"[ \"\$OUT\" = \"$SMOKE_DIR/nodisp.png\" ]"
same_as_grim "... and takes its screen" "$SMOKE_DIR/nodisp.png"
# Something else listening first: a dead socket, a lock file.
: >"$SMOKE_DIR/wayland-.lock"
OUT=$(env -u WAYLAND_DISPLAY "$SHOT" -l 2>>"$LOG")
check "... past a lock file" "[ \"\$OUT\" = 'HEADLESS-1 1280x720+0+0 scale 1' ]"
rm -f "$SMOKE_DIR/wayland-.lock"
WAYLAND_DISPLAY=wayland-9 "$SHOT" -l >/dev/null 2>"$SHOT_ERR"
check "a WAYLAND_DISPLAY that is not there: status 1" "[ $? = 1 ]"
check "... and it says which" "grep -q 'wayland-9' \"$SHOT_ERR\""
env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$SMOKE_DIR/home" "$SHOT" -l >/dev/null 2>"$SHOT_ERR"
check "no compositor in the runtime directory: status 1" "[ $? = 1 ]"

# ---- the clipboard -------------------------------------------------------------------------

shot_count() { pgrep -x screenShot | wc -l; }
run_shot -c
check "-c: status 0, at once" "[ $STATUS = 0 ]"
check "-c: nothing on stdout" "[ -z \"\$OUT\" ]"
check "-c offers image/png" "[ \"\$(WAYLAND_DISPLAY=wayland-0 \"$CLIPGET\" types)\" = image/png ]"
WAYLAND_DISPLAY=wayland-0 "$CLIPGET" image/png >"$SMOKE_DIR/clip.png" 2>>"$LOG"
same_as_grim "-c: what comes out of the clipboard is the screen" "$SMOKE_DIR/clip.png"
WAYLAND_DISPLAY=wayland-0 "$CLIPGET" image/png >"$SMOKE_DIR/clip2.png" 2>>"$LOG"
same_as_grim "it can be pasted again" "$SMOKE_DIR/clip2.png"
check "one screenShot stays behind for the clipboard" "[ \"\$(shot_count)\" = 1 ]"
run_shot -c
wait_for "a new copy ends the one before" test "$(shot_count)" = 1
WAYLAND_DISPLAY=wayland-0 "$CLIPGET" image/png >"$SMOKE_DIR/clip3.png" 2>>"$LOG"
same_as_grim "and the new one serves" "$SMOKE_DIR/clip3.png"
stop_mwc
for ((i = 0; i < 25; i++)); do
	[ "$(shot_count)" = 0 ] && break
	sleep 0.2
done
check "the clipboard screenShot ends with the compositor" "[ \"\$(shot_count)\" = 0 ]"

# ---- -r: a rectangle ---------------------------------------------------------------------

# region_start [args...]: screenShot -r in the background, until its still is up. The red
# quarter at 600,50 is dimmed then.
region_start() {
	WAYLAND_DISPLAY=wayland-0 "$SHOT" -r "$@" >"$SMOKE_DIR/r.out" 2>"$SHOT_ERR" &
	RPID=$!
	local i got
	for ((i = 0; i < 50; i++)); do
		if WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/r-shot.png" 2>/dev/null; then
			got=$(build/pngtool pixel "$SMOKE_DIR/r-shot.png" "${DIM_X:-600}" "${DIM_Y:-50}")
			[[ $got == 7f0000 || $got == 800000 ]] && { echo "ok: -r: the still is up, dimmed"; return 0; }
		fi
		sleep 0.1
	done
	fail "-r: no dimmed still (pixel is ${got:-unreadable})"
}

# region_end: waits for screenShot -r to end, its status in $STATUS, its stdout in $OUT.
region_end() {
	local i
	for ((i = 0; i < 50; i++)); do
		kill -0 "$RPID" 2>/dev/null || break
		sleep 0.1
	done
	kill -0 "$RPID" 2>/dev/null && { kill "$RPID"; fail "-r did not end"; }
	wait "$RPID"
	STATUS=$?
	OUT=$(cat "$SMOKE_DIR/r.out")
	cat "$SHOT_ERR" >>"$LOG"
}

pixel_now() { # <x> <y>
	WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/now.png" 2>/dev/null &&
		build/pngtool pixel "$SMOKE_DIR/now.png" "$1" "$2"
}

begin
start_vinput
region_start -f "$SMOKE_DIR/r.png"
do_input move 100 100
do_input down left
do_input move 400 300
for ((i = 0; i < 50; i++)); do
	[ "$(pixel_now 200 150)" = ff0000 ] && break
	sleep 0.1
done
check "-r: within the rectangle the still is as it is" "[ \"\$(pixel_now 200 150)\" = ff0000 ]"
check "-r: outside it stays dimmed" "[[ \"\$(pixel_now 600 50)\" == 7f0000 || \"\$(pixel_now 600 50)\" == 800000 ]]"
do_input up left
region_end
check "-r: status 0 after the drag" "[ $STATUS = 0 ]"
check "-r: prints the path" "[ \"\$OUT\" = \"$SMOKE_DIR/r.png\" ]"
pixel_is "-r: the still is gone afterwards" 600 50 ff0000
same_as_grim "-r: the rectangle, the same as grim's" "$SMOKE_DIR/r.png" -g "100,100 300x200"

region_start -f "$SMOKE_DIR/r2.png"
do_input move 400 300
do_input down left
do_input move 100 100
do_input up left
region_end
same_as_grim "-r: dragged the other way, the same rectangle" "$SMOKE_DIR/r2.png" -g "100,100 300x200"

region_start -f "$SMOKE_DIR/r3.png"
do_input move 700 500
do_input click left
region_end
check "-r: a click without a drag: status 0" "[ $STATUS = 0 ]"
same_as_grim "-r: a click takes the whole monitor" "$SMOKE_DIR/r3.png"

region_start -f "$SMOKE_DIR/esc.png"
do_input move 100 100
do_input down left
do_input move 300 300
do_input key Escape
region_end
do_input up left
check "-r: Esc gives up, status 1" "[ $STATUS = 1 ]"
check "... and says so" "grep -q 'given up' \"$SHOT_ERR\""
check "... and writes nothing" "[ ! -e \"$SMOKE_DIR/esc.png\" ]"
pixel_is "-r: the still is gone after Esc" 600 50 ff0000

region_start -f "$SMOKE_DIR/right.png"
do_input move 100 100
do_input click right
region_end
check "-r: the right button gives up, status 1" "[ $STATUS = 1 ] && [ ! -e \"$SMOKE_DIR/right.png\" ]"

region_start -c
do_input move 50 60
do_input down left
do_input move 250 160
do_input up left
region_end
check "-r -c: status 0" "[ $STATUS = 0 ]"
WAYLAND_DISPLAY=wayland-0 "$CLIPGET" image/png >"$SMOKE_DIR/rclip.png" 2>>"$LOG"
same_as_grim "-r -c: the rectangle in the clipboard" "$SMOKE_DIR/rclip.png" -g "50,60 200x100"
stop_vinput
stop_mwc

# ---- turned, scaled and several monitors -------------------------------------------------------

for t in 90 180 270 flipped flipped-90 flipped-180 flipped-270; do
	echo "Mwm*HEADLESS-1*outputTransform: $t" >"$XDG_CONFIG_HOME/mwc/mwmrc"
	begin
	run_shot -f "$SMOKE_DIR/t.png"
	[ "$STATUS" = 0 ] || fail "transform $t: status $STATUS"
	same_as_grim "transform $t: the same as grim" "$SMOKE_DIR/t.png"
	stop_mwc
done

printf 'Mwm*HEADLESS-1*outputScale: 2\n' >"$XDG_CONFIG_HOME/mwc/mwmrc"
begin
run_shot -f "$SMOKE_DIR/s.png"
check "scale 2: every pixel of the monitor" "[ \"\$(\"$PNGCMP\" size \"$SMOKE_DIR/s.png\")\" = 1280x720 ]"
same_as_grim "scale 2: the same as grim" "$SMOKE_DIR/s.png"
run_shot -l
check "-l at scale 2: logical size" "[ \"\$OUT\" = 'HEADLESS-1 640x360+0+0 scale 2' ]"
start_vinput 640 360
DIM_X=600 DIM_Y=50 region_start -f "$SMOKE_DIR/rs.png"
do_input move 100 100
do_input down left
do_input move 300 200
do_input up left
region_end
check "-r at scale 2: every pixel of the rectangle" "[ \"\$(\"$PNGCMP\" size \"$SMOKE_DIR/rs.png\")\" = 400x200 ]"
# The pointer is at 100.5 logical, on pixel 201: screenShot cuts there, finer than grim -g.
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/full2.png" 2>>"$LOG"
"$PNGCMP" crop "$SMOKE_DIR/full2.png" 201 201 400 200 "$SMOKE_DIR/want2.png"
diff=$("$PNGCMP" same "$SMOKE_DIR/want2.png" "$SMOKE_DIR/rs.png") ||
	fail "-r at scale 2: on the pixels the pointer was on: $diff"
echo "ok: -r at scale 2: on the pixels the pointer was on, not rounded to the logical ones"
stop_vinput
stop_mwc
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

export WLR_HEADLESS_OUTPUTS=2
begin
run_shot -l
check "-l with two monitors" "[ \"\$(echo \"\$OUT\" | wc -l)\" = 2 ]"
run_shot -f "$SMOKE_DIR/two.png"
check "two monitors side by side: one picture" "[ \"\$(\"$PNGCMP\" size \"$SMOKE_DIR/two.png\")\" = 2560x720 ]"
same_as_grim "two monitors: the same as grim" "$SMOKE_DIR/two.png"
run_shot -l
second=$(echo "$OUT" | sed -n 2p | cut -d' ' -f1)
run_shot -o "$second" -f "$SMOKE_DIR/second.png"
same_as_grim "-o $second: only that one" "$SMOKE_DIR/second.png" -o "$second"
start_vinput 2560 720
region_start -f "$SMOKE_DIR/rtwo.png"
do_input move 1200 100
do_input down left
do_input move 1400 300
do_input up left
region_end
check "-r across two monitors: status 0" "[ $STATUS = 0 ]"
same_as_grim "-r across two monitors: the same as grim's" "$SMOKE_DIR/rtwo.png" -g "1200,100 200x200"
stop_vinput
stop_mwc
unset WLR_HEADLESS_OUTPUTS

echo "PASS: screenShot"
