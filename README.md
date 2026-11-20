# screenShot

screenShot takes a screenshot into a file or the clipboard: the whole screen across
all monitors, one monitor, or a rectangle drawn with the mouse. It is made for
[mwc](https://github.com/reygnn/mwc) and runs on any Wayland compositor with
`wlr-screencopy` (version 3), `ext-data-control-v1` for the clipboard and
`wlr-layer-shell` for `-r`. One C file on libwayland and Cairo, no window of its own.

    screenShot              into Screenshots/ in the pictures folder, the path to stdout
    screenShot -c           into the clipboard, as image/png
    screenShot -f FILE      into that file, - for stdout
    screenShot -r           a rectangle, dragged with the mouse
    screenShot -o NAME      only that monitor, screenShot -l lists the names
    screenShot -d SECONDS   wait that long first
    screenShot -p           with the mouse pointer
    screenShot -q           no toast afterwards
    screenShot -l           list the monitors
    screenShot -v           print the version and the commit built

`-l` and `-v` stand alone. Exit status 0 on success, 1 on an error (message on stderr),
2 for bad options.


## Build

On Ubuntu 26.04:

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libcairo2-dev
    make
    sudo make install

`make install` puts `screenShot` in `/usr/local/bin`.


## Run

In mwc, Print runs `screenShot` and Shift+Print `screenShot -c`. Other commands go in
`~/.config/mwc/mwmrc`, for example a rectangle on Print:

    Mwm*screenshot:            screenShot -r
    Mwm*clipboardScreenshot:   screenShot -c

The pictures folder is `XDG_PICTURES_DIR` from `user-dirs.dirs` in `$XDG_CONFIG_HOME`
(by default `~/.config`), else `~/Pictures`; `Screenshots` in it is created when needed.
New files get the mode the umask allows, 0644 under the usual 022. Files are named
`Screenshot_2026-11-08_14-03-22.png`, a second one within the same second `..._2.png`.
Nothing there is overwritten; `-f` replaces the file it is given. A file only shows up
complete, and one that was there keeps its mode. A link given to `-f` stays a link and
the picture goes where it points; a named pipe or `/dev/stdout` gets the PNG and no
path is printed.

Several monitors end up in one picture, laid out as they are. Rotated and flipped
monitors come out the right way up.

With `-r` screenShot shows the screen as a dimmed still on every monitor. Drag a
rectangle with the left button; letting go takes it. A click without a drag takes the
monitor under the pointer, Esc or another button gives up with status 1. The rectangle
may span monitors and its edges are on the pixels the pointer is on, also on a scaled
monitor. `-r` combines with `-c`, `-f`, `-o`, `-d` and `-p`.

Afterwards a toast at the top of the screen shows a thumbnail and the file name, or
"in the clipboard", for three seconds. It lets clicks and keys through and runs in the
background, so `$(screenShot)` gets the path at once. `-q` and `-f -` leave it out.

Without `WAYLAND_DISPLAY`, from an ssh login or a program started outside the session,
screenShot takes the first `wayland-N` socket in `$XDG_RUNTIME_DIR` (else
`/run/user/<uid>`) whose compositor offers screencopy.


## For Claude sessions

A Claude Code session on this machine, also one over Remote Control, can take a
screenshot and read the PNG:

    screenShot -f /tmp/screen.png        # prints the path
    screenShot -o DP-1 -f /tmp/dp1.png   # one monitor
    screenShot -d 2 -f /tmp/screen.png   # let something open first

The session's shell needn't run inside mwc, see above. `-r` waits for a person at the
mouse. What the session sees is in CAVEATS.


## How this is built

screenShot is developed with Claude (Anthropic) as a coding assistant. I set the
scope and the design and review the changes; Claude writes most of the code and the
tests, see Testing. Known limits are written down in CAVEATS.


## Testing

    make test

runs `tests/build.sh`, which checks the Makefile, then screenShot in a headless mwc
and compares its pictures pixel for pixel with grim's: one monitor, all eight
rotations and flips, scale 2, two monitors, the clipboard and `-r` with mwc's
simulated mouse and keyboard. At scales 1.3 and 1.025, where grim loses a row, it
compares with the frame itself, taken through ext-image-copy-capture. It also checks
the toast, the pictures folder, one without hard links, file modes, `-f` on links and
pipes, stdout onto a full disk, starting without `WAYLAND_DISPLAY` past a dead socket,
a paste that is never read, the edges of a monitor scaled up next to
one at scale 2, `-l` at scale 1.5 and the `-r` crosshair at scale 4. It needs a built
mwc in `MWC_DIR` (by default `../mwc`) with `build/vinput`, `build/pngtool` and
`build/capturec`, and grim. mwc's development box has all of it:

    distrobox enter mwc-dev -- make test


## License

MIT, see [LICENSE](LICENSE).
