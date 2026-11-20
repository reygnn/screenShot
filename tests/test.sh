#!/bin/bash
# Black-box test of screenShot under a headless mwc. Every picture is checked against grim's of
# the same screen, pixel for pixel, or against the frame itself where grim is wrong. It has
# its own Wayland socket and runtime directory.
#
# Needs a built mwc in MWC_DIR (by default ../mwc) with build/vinput, build/pngtool and
# build/capturec, grim, and from here build/clipget, build/pngcmp, build/nolink.so and
# build/otheruid.so. Output goes to build/test.log.
SHOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
SHOT=$SHOT_DIR/screenShot
CLIPGET=$SHOT_DIR/build/clipget
PNGCMP=$SHOT_DIR/build/pngcmp
NOLINK=$SHOT_DIR/build/nolink.so
OTHERUID=$SHOT_DIR/build/otheruid.so
LOG=$SHOT_DIR/build/test.log
MWC_DIR=${MWC_DIR:-$SHOT_DIR/../mwc}
for tool in "$SHOT" "$CLIPGET" "$PNGCMP" "$NOLINK" "$OTHERUID"; do
	[ -f "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
cd "$MWC_DIR" || exit 1
. tests/lib.sh
# Nothing here may reach the running session, also before the first mwc: a regression
# in the option checks would otherwise copy into its clipboard or freeze its screen.
export XDG_RUNTIME_DIR=$SMOKE_DIR
unset WAYLAND_DISPLAY
require_tools grim
for tool in build/vinput build/pngtool build/capturec; do
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

ms() { echo $(($(date +%s%N) / 1000000)); }

expect() { # <description> <shell condition>: once, no state refresh, also without mwc
	eval "$2" || fail "$1"
	echo "ok: $1"
}

run_shot() { # [args...]: with WAYLAND_DISPLAY, output in $OUT, status in $STATUS
	OUT=$(WAYLAND_DISPLAY=wayland-0 "$SHOT" -q "$@" 2>"$SHOT_ERR")
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

pixel_now() { # <x> <y>
	WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/now.png" 2>/dev/null &&
		build/pngtool pixel "$SMOKE_DIR/now.png" "$1" "$2"
}

# The wallpaper is up once the corner is no longer mwc's root colour. Its quarters
# are pure colours, whichever way the monitor is turned.
wallpaper_up() {
	local got
	got=$(pixel_now 1 1) && [ -n "$got" ] && [ "$got" != 5b7792 ]
}

begin() {
	start_mwc 'exec sleep 300'
	wait_for "the wallpaper is up" wallpaper_up
}

# clip [-s] <type>|types: pastes, but fails instead of hanging the test.
clip() { WAYLAND_DISPLAY=wayland-0 timeout "$TIMEOUT" "$CLIPGET" "$@"; }

# poll <description> <command...>: like wait_for, for when mwc may be gone.
poll() {
	local what=$1 i
	shift
	for ((i = 0; i < TIMEOUT * 5; i++)); do
		"$@" && { echo "ok: $what"; return 0; }
		sleep 0.2
	done
	fail "timed out waiting for $what"
}

# ---- the command line -----------------------------------------------------------------

# The version from version.h, then the commit the build is from where git knows it.
version=$(sed -n 's/^#define SCREENSHOT_VERSION "\([^"]*\)".*/\1/p' "$SHOT_DIR/version.h")
[ -n "$version" ] || fail "no SCREENSHOT_VERSION in version.h"
fmt=--date=format-local:%Y%m%d.%H%M
if commit=$(TZ=UTC0 git -C "$SHOT_DIR" log -1 --format=%cd.%h $fmt 2>/dev/null); then
	git -C "$SHOT_DIR" diff --quiet HEAD || commit=$commit.dirty
	version=$version+$commit
fi
got=$("$SHOT" -v)
expect "-v prints screenShot $version" "[ \"\$got\" = \"screenShot $version\" ]"
"$SHOT" -x >/dev/null 2>&1
expect "an unknown option: status 2" "[ $? = 2 ]"
"$SHOT" -c -f x.png >/dev/null 2>&1
expect "-c and -f together: status 2" "[ $? = 2 ]"
"$SHOT" -d soon >/dev/null 2>&1
expect "-d wants a number" "[ $? = 2 ]"
for d in "" nan 0x10 inf -1; do
	"$SHOT" -d "$d" -l >/dev/null 2>&1
	expect "-d '$d': status 2" "[ $? = 2 ]"
done
# -l and -v stand alone, whatever the order: nothing given with them is ignored. -q
# holds for them anyway.
for args in "-l -c" "-l -f x.png" "-l -o X" "-l -r" "-l -p" "-l -d 1" "-l -v" \
		"-v -l" "-v -c" "-v -o X" "-v x" "-l x" "-h x" "-h -c" "-h -v" "-l -h"; do
	"$SHOT" $args >/dev/null 2>&1
	expect "$args: status 2" "[ $? = 2 ]"
done
"$SHOT" -h | grep -q 'Esc or another button gives up'
expect "-h: any button but the left one gives -r up" "[ $? = 0 ]"

# ---- one monitor, a file ---------------------------------------------------------------

begin
run_shot -f "$SMOKE_DIR/a.png"
expect "-f: status 0" "[ $STATUS = 0 ]"
expect "-f prints the path" "[ \"\$OUT\" = \"$SMOKE_DIR/a.png\" ]"
expect "-f: what is on the screen, the wallpaper" "\"$PNGCMP\" same \"$SMOKE_DIR/a.png\" \"$WALL\""
same_as_grim "-f: the same as grim" "$SMOKE_DIR/a.png"
expect "-f: no temporary file is left" "[ -z \"\$(ls -A \"$SMOKE_DIR\" | grep '^\\.screenShot-')\" ]"
(cd "$SMOKE_DIR" && WAYLAND_DISPLAY=wayland-0 "$SHOT" -q -f rel.png >"$SMOKE_DIR/rel.out" 2>>"$LOG")
expect "-f with a relative path prints it absolute" "[ \"\$(cat \"$SMOKE_DIR/rel.out\")\" = \"$SMOKE_DIR/rel.png\" ]"
(umask 022 && run_shot -f "$SMOKE_DIR/a.png" && [ "$STATUS" = 0 ])
expect "-f overwrites a file it is told to" "[ $? = 0 ]"
(umask 022 && run_shot -f "$SMOKE_DIR/new.png")
expect "-f: mode 644 under umask 022" "[ \$(stat -c %a \"$SMOKE_DIR/new.png\") = 644 ]"
(umask 077 && run_shot -f "$SMOKE_DIR/private.png")
expect "-f: mode 600 under umask 077" "[ \$(stat -c %a \"$SMOKE_DIR/private.png\") = 600 ]"
# What is there already keeps what it is: a private file stays private, a link stays a
# link and the picture goes where it points, a pipe gets the picture.
: >"$SMOKE_DIR/kept.png"
chmod 600 "$SMOKE_DIR/kept.png"
(umask 022 && run_shot -f "$SMOKE_DIR/kept.png")
expect "-f on a file of mode 600 keeps it 600" "[ \$(stat -c %a \"$SMOKE_DIR/kept.png\") = 600 ]"
expect "... and puts the picture in" "\"$PNGCMP\" same \"$SMOKE_DIR/kept.png\" \"$WALL\""
mkdir "$SMOKE_DIR/real"
: >"$SMOKE_DIR/real/x.png"
ln -s real/x.png "$SMOKE_DIR/link.png"
run_shot -f "$SMOKE_DIR/link.png"
expect "-f on a link: still a link" "[ -L \"$SMOKE_DIR/link.png\" ]"
expect "... the picture where it points" "\"$PNGCMP\" same \"$SMOKE_DIR/real/x.png\" \"$WALL\""
expect "... and that is the path printed" "[ \"\$OUT\" = \"$SMOKE_DIR/real/x.png\" ]"
expect "... no temporary file left there" "[ \"\$(ls -A \"$SMOKE_DIR/real\")\" = x.png ]"
ln -s real/y.png "$SMOKE_DIR/nowhere.png"
run_shot -f "$SMOKE_DIR/nowhere.png"
expect "-f on a link to nowhere: still a link" "[ -L \"$SMOKE_DIR/nowhere.png\" ]"
expect "... and the file it names is made" "\"$PNGCMP\" same \"$SMOKE_DIR/real/y.png\" \"$WALL\""
# Someone else's link, as one put into a shared folder, is not followed: it could point
# at ~/.bashrc. It is replaced, and what it points at stays as it was.
: >"$SMOKE_DIR/real/z.png"
ln -s real/z.png "$SMOKE_DIR/theirs.png"
LD_PRELOAD=$OTHERUID run_shot -f "$SMOKE_DIR/theirs.png"
expect "-f on someone else's link: replaced by the picture" \
	"[ ! -L \"$SMOKE_DIR/theirs.png\" ] && \"$PNGCMP\" same \"$SMOKE_DIR/theirs.png\" \"$WALL\""
expect "... and what it pointed at is untouched" "[ ! -s \"$SMOKE_DIR/real/z.png\" ]"
ln -s real/w.png "$SMOKE_DIR/theirs2.png"
LD_PRELOAD=$OTHERUID run_shot -f "$SMOKE_DIR/theirs2.png"
expect "... a link of theirs to nowhere makes nothing there" "[ ! -e \"$SMOKE_DIR/real/w.png\" ]"
LD_PRELOAD=$OTHERUID WAYLAND_DISPLAY=wayland-0 "$SHOT" -f /dev/stdout \
	>"$SMOKE_DIR/devout2.png" 2>>"$LOG"
expect "... but one to stdout, as /dev/stdout, is followed" \
	"\"$PNGCMP\" same \"$SMOKE_DIR/devout2.png\" \"$WALL\""
mkfifo "$SMOKE_DIR/their-pipe"
ln -s their-pipe "$SMOKE_DIR/theirs3.png"
LD_PRELOAD=$OTHERUID WAYLAND_DISPLAY=wayland-0 timeout "$TIMEOUT" "$SHOT" -q \
	-f "$SMOKE_DIR/theirs3.png" >/dev/null 2>>"$LOG"
expect "... and one to their pipe is not: replaced, nothing goes to them" \
	"[ $? = 0 ] && [ ! -L \"$SMOKE_DIR/theirs3.png\" ] && [ -p \"$SMOKE_DIR/their-pipe\" ]"
mkfifo "$SMOKE_DIR/pipe"
timeout "$TIMEOUT" cat "$SMOKE_DIR/pipe" >"$SMOKE_DIR/piped.png" &
reader=$!
run_shot -f "$SMOKE_DIR/pipe"
wait "$reader"
expect "-f on a named pipe: still a pipe" "[ -p \"$SMOKE_DIR/pipe\" ] && [ $STATUS = 0 ]"
expect "... whose reader gets the picture" "\"$PNGCMP\" same \"$SMOKE_DIR/piped.png\" \"$WALL\""
expect "... and no path on stdout" "[ -z \"\$OUT\" ]"
WAYLAND_DISPLAY=wayland-0 "$SHOT" -f /dev/stdout >"$SMOKE_DIR/devout.png" 2>>"$LOG"
expect "-f /dev/stdout: as -f -, the PNG and nothing else" \
	"\"$PNGCMP\" same \"$SMOKE_DIR/devout.png\" \"$WALL\""
WAYLAND_DISPLAY=wayland-0 "$SHOT" -f - >"$SMOKE_DIR/stdout.png" 2>>"$LOG"
expect "-f - writes the PNG to stdout" "\"$PNGCMP\" same \"$SMOKE_DIR/stdout.png\" \"$WALL\""
# printf only buffers the path: a full disk shows when it is flushed.
WAYLAND_DISPLAY=wayland-0 "$SHOT" -q -f "$SMOKE_DIR/full.png" >/dev/full 2>"$SHOT_ERR"
expect "the path onto a full disk: status 1" "[ $? = 1 ]"
expect "... saying so" "grep -q 'cannot write to stdout' \"$SHOT_ERR\""
expect "... and the picture is saved all the same" "\"$PNGCMP\" same \"$SMOKE_DIR/full.png\" \"$WALL\""
WAYLAND_DISPLAY=wayland-0 "$SHOT" -l >/dev/full 2>/dev/null
expect "-l onto a full disk: status 1" "[ $? = 1 ]"
"$SHOT" -v >/dev/full 2>/dev/null
expect "-v onto a full disk: status 1" "[ $? = 1 ]"
"$SHOT" -h >/dev/full 2>/dev/null
expect "-h onto a full disk: status 1" "[ $? = 1 ]"
run_shot -f "$SMOKE_DIR/no/such/dir/a.png"
expect "-f into a folder that is not there: status 1" "[ $STATUS = 1 ]"
expect "... and a message" "grep -q 'cannot write' \"$SHOT_ERR\""

run_shot -l
expect "-l lists the monitor" "[ \"\$OUT\" = 'HEADLESS-1 1280x720+0+0 scale 1' ]"
run_shot -o HEADLESS-1 -f "$SMOKE_DIR/o.png"
expect "-o HEADLESS-1" "[ $STATUS = 0 ] && \"$PNGCMP\" same \"$SMOKE_DIR/o.png\" \"$WALL\""
run_shot -o DP-9 -f "$SMOKE_DIR/o.png"
expect "-o with a name that is not there: status 1" "[ $STATUS = 1 ]"
expect "... which says how to find the names" "grep -q 'screenShot -l' \"$SHOT_ERR\""
run_shot -p -f "$SMOKE_DIR/p.png"
expect "-p: status 0" "[ $STATUS = 0 ]"
t0=$(ms)
run_shot -d 0.5 -f "$SMOKE_DIR/d.png"
t1=$(ms)
expect "-d 0.5: status 0" "[ $STATUS = 0 ]"
expect "-d 0.5 waits half a second first ($((t1 - t0)) ms)" "[ $((t1 - t0)) -ge 500 ]"

# ---- the pictures folder ----------------------------------------------------------------

run_shot
expect "no options: status 0" "[ $STATUS = 0 ]"
expect "no user-dirs.dirs: into ~/Pictures/Screenshots" \
	"[[ \"\$OUT\" == \"$HOME/Pictures/Screenshots/Screenshot_\"*.png ]] && [ -f \"\$OUT\" ]"
expect "named by the date and time" \
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
expect "XDG_PICTURES_DIR from user-dirs.dirs: ~/Bilder/Screenshots" \
	"[[ \"\$first\" == \"$HOME/Bilder/Screenshots/Screenshot_\"*.png ]]"
expect "three in a row are three files, none overwritten" \
	"[ \"\$(ls \"$HOME/Bilder/Screenshots\" | grep -c '^Screenshot_.*\\.png$')\" = 3 ]"
echo 'XDG_PICTURES_DIR="/abs/elsewhere/../x"' >"$XDG_CONFIG_HOME/user-dirs.dirs"
echo "XDG_PICTURES_DIR=\"$SMOKE_DIR/abs pics\"" >>"$XDG_CONFIG_HOME/user-dirs.dirs"
run_shot
expect "an absolute XDG_PICTURES_DIR, the last line wins, spaces are fine" \
	"[[ \"\$OUT\" == \"$SMOKE_DIR/abs pics/Screenshots/\"*.png ]]"

# A pictures folder on vfat or exFAT: no hard links there.
echo "XDG_PICTURES_DIR=\"$SMOKE_DIR/fat\"" >"$XDG_CONFIG_HOME/user-dirs.dirs"
for i in 1 2 3; do
	LD_PRELOAD=$NOLINK run_shot
	[ "$STATUS" = 0 ] || fail "without hard links: status $STATUS, $(cat "$SHOT_ERR")"
done
expect "without hard links: three in a row are three files" \
	"[ \"\$(ls \"$SMOKE_DIR/fat/Screenshots\" | grep -c '^Screenshot_.*\\.png$')\" = 3 ]"
expect "... and no temporary one left" "[ \"\$(ls -A \"$SMOKE_DIR/fat/Screenshots\" | wc -l)\" = 3 ]"
same_as_grim "... the picture is the screen" "$OUT"
rm "$XDG_CONFIG_HOME/user-dirs.dirs"

# ---- without WAYLAND_DISPLAY -------------------------------------------------------------

# As from an ssh login or a Claude session started outside the desktop.
OUT=$(env -u WAYLAND_DISPLAY "$SHOT" -q -f "$SMOKE_DIR/nodisp.png" 2>>"$LOG")
expect "without WAYLAND_DISPLAY it finds the compositor in XDG_RUNTIME_DIR" \
	"[ \"\$OUT\" = \"$SMOKE_DIR/nodisp.png\" ]"
same_as_grim "... and takes its screen" "$SMOKE_DIR/nodisp.png"
# Before wayland-0 in name order: a lock file, and the socket of a compositor that is
# gone, which refuses the connection. Both are passed over.
: >"$SMOKE_DIR/wayland-.lock"
python3 -c 'import socket, sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' \
	"$SMOKE_DIR/wayland-.dead" || fail "cannot make a dead socket"
OUT=$(env -u WAYLAND_DISPLAY "$SHOT" -l 2>>"$LOG")
expect "... past a lock file and a dead socket" \
	"[ \"\$OUT\" = 'HEADLESS-1 1280x720+0+0 scale 1' ]"
rm -f "$SMOKE_DIR/wayland-.lock" "$SMOKE_DIR/wayland-.dead"
WAYLAND_DISPLAY=wayland-9 "$SHOT" -l >/dev/null 2>"$SHOT_ERR"
expect "a WAYLAND_DISPLAY that is not there: status 1" "[ $? = 1 ]"
expect "... and it says which" "grep -q 'wayland-9' \"$SHOT_ERR\""
env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$SMOKE_DIR/home" "$SHOT" -l >/dev/null 2>"$SHOT_ERR"
expect "no compositor in the runtime directory: status 1" "[ $? = 1 ]"

# ---- the clipboard -------------------------------------------------------------------------

# Only this test's own: pgrep alone would also see a screenShot of the real session,
# for example the one holding its clipboard, and the box shares the host's processes.
test_shots() {
	local p
	for p in $(pgrep -x screenShot); do
		{ tr '\0' '\n' <"/proc/$p/environ"; } 2>/dev/null | grep -qx "SMOKE_DIR=$SMOKE_DIR" &&
			echo "$p"
	done
}
shot_count() { test_shots | wc -l; }
# For wait_for, which runs its command again each time: "$(shot_count)" as an argument
# would count only once.
one_shot() { [ "$(shot_count)" = 1 ]; }
no_shots() { [ "$(shot_count)" = 0 ]; }
run_shot -c
expect "-c: status 0, at once" "[ $STATUS = 0 ]"
expect "-c: nothing on stdout" "[ -z \"\$OUT\" ]"
expect "-c offers image/png" "[ \"\$(clip types)\" = image/png ]"
clip image/png >"$SMOKE_DIR/clip.png" 2>>"$LOG"
same_as_grim "-c: what comes out of the clipboard is the screen" "$SMOKE_DIR/clip.png"
clip image/png >"$SMOKE_DIR/clip2.png" 2>>"$LOG"
same_as_grim "it can be pasted again" "$SMOKE_DIR/clip2.png"
expect "one screenShot stays behind for the clipboard" "[ \"\$(shot_count)\" = 1 ]"
run_shot -c
wait_for "a new copy ends the one before" one_shot
clip image/png >"$SMOKE_DIR/clip3.png" 2>>"$LOG"
same_as_grim "and the new one serves" "$SMOKE_DIR/clip3.png"
# The one that stays may stay for hours: in /, so it keeps no directory busy, and with
# only the PNG, not the capture's buffer.
mkdir "$SMOKE_DIR/cwd"
(cd "$SMOKE_DIR/cwd" && run_shot -c)
wait_for "a copy from another directory" one_shot
holder=$(test_shots)
expect "the clipboard's process is in /" "[ \"\$(readlink /proc/$holder/cwd)\" = / ]"
expect "... and keeps no buffer of the capture" "! grep -q memfd:screenShot /proc/$holder/maps"
clip image/png >"$SMOKE_DIR/clip5.png" 2>>"$LOG"
same_as_grim "... and still serves" "$SMOKE_DIR/clip5.png"

# ---- the toast -----------------------------------------------------------------------------

# It sits at the top in the middle, 32 below the edge; 4 pixels in is its background.
# Below it the wallpaper's quarter is green.
TOAST_X=645 TOAST_Y=36
toast_shown() { [ "$(pixel_now $TOAST_X $TOAST_Y)" = 2b2b2b ]; }
toast_gone() { [ "$(pixel_now $TOAST_X $TOAST_Y)" = 00ff00 ]; }

clip image/png >/dev/null 2>&1 # the -c one above
kill $(test_shots) 2>/dev/null
wait_for "no screenShot left" no_shots
t0=$(ms)
OUT=$(WAYLAND_DISPLAY=wayland-0 "$SHOT" -f "$SMOKE_DIR/toast.png" 2>>"$LOG")
STATUS=$?
t1=$(ms)
expect "with a toast: status 0, the path" "[ $STATUS = 0 ] && [ \"\$OUT\" = \"$SMOKE_DIR/toast.png\" ]"
expect "\$(...) does not wait for the toast ($((t1 - t0)) ms)" "[ $((t1 - t0)) -lt 1500 ]"
wait_for "the toast shows" toast_shown
t2=$(ms)
expect "the screenshot has no toast in it" "[ \"\$(build/pngtool pixel \"$SMOKE_DIR/toast.png\" $TOAST_X $TOAST_Y)\" = 00ff00 ]"
wait_for "the toast goes" toast_gone
t3=$(ms)
expect "it was up about 3 seconds ($((t3 - t0)) ms)" "[ $((t3 - t0)) -ge 2500 ] && [ $((t3 - t0)) -le 5000 ]"
wait_for "and its process ends" no_shots

# The toast is its own process, forked before screenShot returns. None left behind
# means none will come.
WAYLAND_DISPLAY=wayland-0 "$SHOT" -q -f "$SMOKE_DIR/quiet.png" >/dev/null 2>>"$LOG"
expect "-q: no toast" "[ \"\$(shot_count)\" = 0 ] && toast_gone"
WAYLAND_DISPLAY=wayland-0 "$SHOT" -f - >"$SMOKE_DIR/pipe.png" 2>>"$LOG"
expect "-f -: no toast either" "toast_gone && [ \"\$(shot_count)\" = 0 ]"

WAYLAND_DISPLAY=wayland-0 "$SHOT" >/dev/null 2>>"$LOG"
wait_for "a toast also for the pictures folder" toast_shown
wait_for "... which goes again" toast_gone

t0=$(ms)
WAYLAND_DISPLAY=wayland-0 "$SHOT" -c 2>>"$LOG"
expect "-c with a toast: returns at once" "[ $(($(ms) - t0)) -lt 1500 ]"
wait_for "-c: the toast shows" toast_shown
clip image/png >"$SMOKE_DIR/tclip.png" 2>>"$LOG"
expect "-c: pasting works while the toast is up" "[ \"\$(build/pngtool pixel \"$SMOKE_DIR/tclip.png\" $TOAST_X $TOAST_Y)\" = 00ff00 ]"
wait_for "-c: the toast goes" toast_gone
clip image/png >"$SMOKE_DIR/tclip2.png" 2>>"$LOG"
expect "-c: and pasting still works after it" "\"$PNGCMP\" same \"$SMOKE_DIR/tclip.png\" \"$SMOKE_DIR/tclip2.png\""
expect "-c: one screenShot stays for the clipboard" "[ \"\$(shot_count)\" = 1 ]"
expect "-c: once the toast is gone, it keeps no buffer" \
	"! grep -q memfd:screenShot /proc/\$(test_shots)/maps"

stop_mwc
poll "the clipboard screenShot ends with the compositor" no_shots

# A reader that asks and never reads, like a hung application, gets left behind; the
# next paste still works. On noise, so the PNG is more than a pipe holds. The stuck one
# lives longer than clip's timeout, so only giving it up lets clip through.
"$PNGCMP" noise "$SMOKE_DIR/noise.png" 1280 720 || fail "cannot make the noise"
MWC_WALLPAPER=$SMOKE_DIR/noise.png begin
run_shot -c
expect "-c on noise: status 0" "[ $STATUS = 0 ]"
coproc STALL { WAYLAND_DISPLAY=wayland-0 exec timeout $((3 * TIMEOUT)) "$CLIPGET" -s image/png; }
read -r -t "$TIMEOUT" asked <&"${STALL[0]}"
expect "a paste that is never read" "[ \"\${asked:-}\" = asked ]"
clip image/png >"$SMOKE_DIR/clip4.png" 2>>"$LOG" || fail "a stuck reader blocks the next paste"
expect "... which is still stuck" "kill -0 \"\$STALL_PID\" 2>/dev/null"
same_as_grim "... doesn't block the next one" "$SMOKE_DIR/clip4.png"
expect "... a PNG larger than a pipe" "[ \$(stat -c %s \"$SMOKE_DIR/clip4.png\") -gt 1048576 ]"
kill "$STALL_PID" 2>/dev/null
wait "$STALL_PID" 2>/dev/null
stop_mwc

# ---- -r: a rectangle ---------------------------------------------------------------------

# start_mouse [width height]: vinput, and back only once mwc has its pointer. -r on a seat
# without one gives up at once.
start_mouse() {
	start_vinput "$@"
	do_input move 0 0
}

# region_start [args...]: screenShot -r in the background, until its still is up. The red
# quarter at 600,50 is dimmed then.
region_start() {
	WAYLAND_DISPLAY=wayland-0 "$SHOT" -q -r "$@" >"$SMOKE_DIR/r.out" 2>"$SHOT_ERR" &
	RPID=$!
	wait_for "-r: the still is up, dimmed" dimmed "${DIM_X:-600}" "${DIM_Y:-50}"
}

dimmed() { # <x> <y>: the red quarter at half brightness
	local got
	got=$(pixel_now "$1" "$2") && [[ $got == 7f0000 || $got == 800000 ]]
}

region_gone() { ! kill -0 "$RPID" 2>/dev/null; }

# region_end: waits for screenShot -r to end, its status in $STATUS, its stdout in $OUT.
region_end() {
	wait_for "-r ends" region_gone
	wait "$RPID"
	STATUS=$?
	OUT=$(cat "$SMOKE_DIR/r.out")
	cat "$SHOT_ERR" >>"$LOG"
}

begin
start_mouse
region_start -f "$SMOKE_DIR/r.png"
do_input move 100 100
do_input down left
do_input move 400 300
pixel_is "-r: within the rectangle the still is as it is" 200 150 ff0000
expect "-r: outside it stays dimmed" "dimmed 600 50"
do_input up left
region_end
expect "-r: status 0 after the drag" "[ $STATUS = 0 ]"
expect "-r: prints the path" "[ \"\$OUT\" = \"$SMOKE_DIR/r.png\" ]"
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
expect "-r: a click without a drag: status 0" "[ $STATUS = 0 ]"
same_as_grim "-r: a click takes the whole monitor" "$SMOKE_DIR/r3.png"

region_start -f "$SMOKE_DIR/esc.png"
do_input move 100 100
do_input down left
do_input move 300 300
do_input key Escape
region_end
do_input up left
expect "-r: Esc gives up, status 1" "[ $STATUS = 1 ]"
expect "... and says so" "grep -q 'given up' \"$SHOT_ERR\""
expect "... and writes nothing" "[ ! -e \"$SMOKE_DIR/esc.png\" ]"
pixel_is "-r: the still is gone after Esc" 600 50 ff0000

region_start -f "$SMOKE_DIR/right.png"
do_input move 100 100
do_input click right
region_end
expect "-r: the right button gives up, status 1" "[ $STATUS = 1 ] && [ ! -e \"$SMOKE_DIR/right.png\" ]"

region_start -c
do_input move 50 60
do_input down left
do_input move 250 160
do_input up left
region_end
expect "-r -c: status 0" "[ $STATUS = 0 ]"
clip image/png >"$SMOKE_DIR/rclip.png" 2>>"$LOG"
same_as_grim "-r -c: the rectangle in the clipboard" "$SMOKE_DIR/rclip.png" -g "50,60 200x100"
stop_vinput
stop_mwc

# ---- -p: the pointer -----------------------------------------------------------------------

# On the headless backend the pointer is a hardware cursor, so without -p it is not in
# the picture at all, and with -p the compositor paints it in. Its tip is at 400,300, in
# the red quarter.
begin
start_vinput
do_input move 400 300
run_shot -f "$SMOKE_DIR/nop.png"
expect "without -p: no pointer, only the wallpaper" "\"$PNGCMP\" same \"$SMOKE_DIR/nop.png\" \"$WALL\""
run_shot -p -f "$SMOKE_DIR/p.png"
expect "-p: status 0" "[ $STATUS = 0 ]"
expect "-p: the pointer is in the picture" "! \"$PNGCMP\" same \"$SMOKE_DIR/p.png\" \"$WALL\" >/dev/null"
"$PNGCMP" crop "$SMOKE_DIR/p.png" 300 200 200 200 "$SMOKE_DIR/p-near.png"
"$PNGCMP" crop "$WALL" 300 200 200 200 "$SMOKE_DIR/wall-near.png"
expect "... where the pointer is" "! \"$PNGCMP\" same \"$SMOKE_DIR/p-near.png\" \"$SMOKE_DIR/wall-near.png\" >/dev/null"
"$PNGCMP" crop "$SMOKE_DIR/p.png" 640 0 640 720 "$SMOKE_DIR/p-far.png"
"$PNGCMP" crop "$WALL" 640 0 640 720 "$SMOKE_DIR/wall-far.png"
expect "... and nowhere else" "\"$PNGCMP\" same \"$SMOKE_DIR/p-far.png\" \"$SMOKE_DIR/wall-far.png\""
same_as_grim "-p: the same as grim -c" "$SMOKE_DIR/p.png" -c
do_input move 900 500
run_shot -p -f "$SMOKE_DIR/p2.png"
"$PNGCMP" crop "$SMOKE_DIR/p2.png" 300 200 200 200 "$SMOKE_DIR/p2-near.png"
expect "-p: the pointer follows the mouse" "\"$PNGCMP\" same \"$SMOKE_DIR/p2-near.png\" \"$SMOKE_DIR/wall-near.png\""
same_as_grim "-p at its new place: the same as grim -c" "$SMOKE_DIR/p2.png" -c
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
expect "scale 2: every pixel of the monitor" "[ \"\$(\"$PNGCMP\" size \"$SMOKE_DIR/s.png\")\" = 1280x720 ]"
same_as_grim "scale 2: the same as grim" "$SMOKE_DIR/s.png"
run_shot -l
expect "-l at scale 2: logical size" "[ \"\$OUT\" = 'HEADLESS-1 640x360+0+0 scale 2' ]"
start_mouse 640 360
DIM_X=600 DIM_Y=50 region_start -f "$SMOKE_DIR/rs.png"
do_input move 100 100
do_input down left
do_input move 300 200
do_input up left
region_end
expect "-r at scale 2: every pixel of the rectangle" "[ \"\$(\"$PNGCMP\" size \"$SMOKE_DIR/rs.png\")\" = 400x200 ]"
# The pointer is at 100.5 logical, on pixel 201: screenShot cuts there, finer than grim -g.
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/full2.png" 2>>"$LOG"
"$PNGCMP" crop "$SMOKE_DIR/full2.png" 201 201 400 200 "$SMOKE_DIR/want2.png"
diff=$("$PNGCMP" same "$SMOKE_DIR/want2.png" "$SMOKE_DIR/rs.png") ||
	fail "-r at scale 2: on the pixels the pointer was on: $diff"
echo "ok: -r at scale 2: on the pixels the pointer was on, not rounded to the logical ones"
stop_vinput
stop_mwc

# The crosshair of -r at scale 4. A theme has only some sizes: Adwaita's nearest to
# 8 * 4 is 30x30, which 4 doesn't divide. Such a buffer scale is a protocol error for
# compositors that check; mwc only logs it, so the test reads what screenShot set from
# mwc's state. 3 is the largest that divides 30.
printf 'Mwm*HEADLESS-1*outputScale: 4\n' >"$XDG_CONFIG_HOME/mwc/mwmrc"
begin
start_mouse 320 180
XCURSOR_THEME=Adwaita XCURSOR_SIZE=8 region_start -f "$SMOKE_DIR/rc.png"
do_input move 100 100
do_input move 120 110
check "-r at scale 4: a 30x30 crosshair at buffer scale 3" \
	"[ \"\$(global cursor cw)x\$(global cursor ch)@\$(global cursor cscale)\" = 30x30@3 ]"
do_input key Escape
region_end
stop_vinput
stop_mwc

# wl_output only knows integer scales; -l must say the real one.
printf 'Mwm*HEADLESS-1*outputScale: 1.5\n' >"$XDG_CONFIG_HOME/mwc/mwmrc"
begin
run_shot -l
expect "-l at scale 1.5: the fractional scale" "[ \"\$OUT\" = 'HEADLESS-1 853x480+0+0 scale 1.5' ]"
stop_mwc

# The compositor cuts the logical width and height off separately: at 1.3 the monitor
# is 984x553, and 553 times 1280/984 is short of 720. grim loses that row, so here
# the reference is the frame itself, through ext-image-copy-capture. At 1.025 it is
# 1248x702, and 702 * (720 / 702.0) is just below 720: a click must still get that row.
capturec() { WAYLAND_DISPLAY=wayland-0 "$MWC_DIR/build/capturec" "$@"; }
for s in 1.3 1.025; do
	printf 'Mwm*HEADLESS-1*outputScale: %s\n' "$s" >"$XDG_CONFIG_HOME/mwc/mwmrc"
	begin
	capturec frame HEADLESS-1 "$SMOKE_DIR/frame.png" 2>>"$LOG" ||
		fail "scale $s: capturec failed"
	run_shot -f "$SMOKE_DIR/f.png"
	expect "scale $s: every row and column of the monitor" \
		"\"$PNGCMP\" same \"$SMOKE_DIR/frame.png\" \"$SMOKE_DIR/f.png\""
	run_shot -l
	logical=$(echo "$OUT" | cut -d' ' -f2 | cut -d+ -f1)
	start_mouse "${logical%x*}" "${logical#*x}"
	region_start -f "$SMOKE_DIR/fc.png"
	do_input move 300 300
	do_input click left
	region_end
	expect "scale $s: a click takes every row and column too" \
		"\"$PNGCMP\" same \"$SMOKE_DIR/frame.png\" \"$SMOKE_DIR/fc.png\""
	stop_vinput
	stop_mwc
done
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

export WLR_HEADLESS_OUTPUTS=2
begin
run_shot -l
expect "-l with two monitors" "[ \"\$(echo \"\$OUT\" | wc -l)\" = 2 ]"
run_shot -f "$SMOKE_DIR/two.png"
expect "two monitors side by side: one picture" "[ \"\$(\"$PNGCMP\" size \"$SMOKE_DIR/two.png\")\" = 2560x720 ]"
same_as_grim "two monitors: the same as grim" "$SMOKE_DIR/two.png"
run_shot -l
second=$(echo "$OUT" | sed -n 2p | cut -d' ' -f1)
run_shot -o "$second" -f "$SMOKE_DIR/second.png"
same_as_grim "-o $second: only that one" "$SMOKE_DIR/second.png" -o "$second"
start_mouse 2560 720
region_start -f "$SMOKE_DIR/rtwo.png"
do_input move 1200 100
do_input down left
do_input move 1400 300
do_input up left
region_end
expect "-r across two monitors: status 0" "[ $STATUS = 0 ]"
same_as_grim "-r across two monitors: the same as grim's" "$SMOKE_DIR/rtwo.png" -g "1200,100 200x200"
stop_vinput
stop_mwc

# One at scale 2, one at 1: the picture gets 2, the other is scaled up. Its edges keep
# their colour and don't fade into the black around it; the wallpaper's corners are red,
# green, blue and white.
printf 'Mwm*HEADLESS-1*outputScale: 2\n' >"$XDG_CONFIG_HOME/mwc/mwmrc"
begin
run_shot -l
one=$(echo "$OUT" | grep ' scale 1$') || fail "no monitor at scale 1: $OUT"
ox=$(echo "$OUT" | sed 's/.*x[0-9]*+\([0-9]*\)+.*/\1/' | sort -n | head -1)
oy=$(echo "$OUT" | sed 's/.*+\([0-9]*\) scale.*/\1/' | sort -n | head -1)
read -r w h x y <<<"$(echo "$one" | sed 's/^[^ ]* \([0-9]*\)x\([0-9]*\)+\([0-9]*\)+\([0-9]*\).*/\1 \2 \3 \4/')"
run_shot -f "$SMOKE_DIR/mixed.png"
expect "two scales: status 0" "[ $STATUS = 0 ]"
l=$(((x - ox) * 2)) t=$(((y - oy) * 2)) r=$(((x - ox + w) * 2 - 1)) b=$(((y - oy + h) * 2 - 1))
for corner in "$l $t ff0000" "$r $t 00ff00" "$l $b 0000ff" "$r $b ffffff"; do
	set -- $corner
	got=$(build/pngtool pixel "$SMOKE_DIR/mixed.png" "$1" "$2")
	expect "two scales: the scaled up corner at $1,$2 is $3 ($got)" "[ \"$got\" = $3 ]"
done
stop_mwc

# Both at 1.3, but 1280x720 and 1920x1080: their logical sizes are cut off to whole
# numbers, so the scales differ in the fourth place. Neither may be resampled for that:
# each piece of the picture is its monitor's frame, pixel for pixel.
printf 'Mwm*HEADLESS-%s*outputScale: 1.3\n' 1 2 >"$XDG_CONFIG_HOME/mwc/mwmrc"
printf 'Mwm*HEADLESS-2*outputMode: 1920x1080\n' >>"$XDG_CONFIG_HOME/mwc/mwmrc"
begin
run_shot -l
echo "$OUT" >"$SMOKE_DIR/layout"
run_shot -f "$SMOKE_DIR/same13.png"
expect "1.3 on two sizes: status 0" "[ $STATUS = 0 ]"
while read -r name box _; do
	capturec frame "$name" "$SMOKE_DIR/frame-$name.png" 2>>"$LOG" || fail "capturec $name"
	echo "$name $box $("$PNGCMP" size "$SMOKE_DIR/frame-$name.png")"
done <"$SMOKE_DIR/layout" >"$SMOKE_DIR/boxes"
# name WxH+X+Y MWxMH: where each frame lies, as compose() places it.
awk '{ split($2, a, /[x+]/); split($3, m, "x"); n[NR] = $1; x[NR] = a[3]; y[NR] = a[4]
	mw[NR] = m[1]; mh[NR] = m[2]; sx = m[1] / a[1]; sy = m[2] / a[2]
	if (sx > scx) scx = sx; if (sy > scy) scy = sy
	if (NR == 1 || a[3] < ox) ox = a[3]; if (NR == 1 || a[4] < oy) oy = a[4] }
	END { for (i = 1; i <= NR; i++) printf "%s %d %d %d %d\n", n[i],
		int((x[i] - ox) * scx + 0.5), int((y[i] - oy) * scy + 0.5), mw[i], mh[i] }' \
	"$SMOKE_DIR/boxes" >"$SMOKE_DIR/pieces"
while read -r name px py mw mh; do
	"$PNGCMP" crop "$SMOKE_DIR/same13.png" "$px" "$py" "$mw" "$mh" "$SMOKE_DIR/piece.png"
	expect "1.3 on two sizes: $name ($mw x $mh at $px,$py) is its frame" \
		"\"$PNGCMP\" same \"$SMOKE_DIR/piece.png\" \"$SMOKE_DIR/frame-$name.png\""
done <"$SMOKE_DIR/pieces"
stop_mwc
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
unset WLR_HEADLESS_OUTPUTS

echo "PASS: screenShot"
