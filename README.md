# screenShot – a screenshot into a file or the clipboard (for mwc)

One C file, libwayland and cairo only, no window of its own. Captures the whole screen,
across all monitors or just one of them, or a rectangle drawn with the mouse:

    screenShot              into Screenshots/ in the pictures folder, e.g.
                            ~/Pictures/Screenshots/Screenshot_2026-11-08_14-03-22.png,
                            the path goes to stdout
    screenShot -c           into the clipboard, as image/png
    screenShot -f FILE      into that file, - for stdout
    screenShot -r           a rectangle, dragged with the mouse
    screenShot -o NAME      only that monitor, screenShot -l lists the names
    screenShot -d SECONDS   wait that long first
    screenShot -p           with the mouse pointer
    screenShot -v           print the version

## Build

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libcairo2-dev
    make
    sudo make install        # -> /usr/local/bin/screenShot

## In mwc

**Print** runs `screenShot`, **Shift+Print** `screenShot -c`. Other commands go in
`~/.config/mwc/mwmrc`:

    Mwm*screenshot:            screenShot
    Mwm*clipboardScreenshot:   screenShot -c

With a delay, for example to leave time to open a menu: `screenShot -d 3`. For a
rectangle on Print instead of the whole screen:

    Mwm*screenshot:            screenShot -r

screenShot needs `wlr-screencopy` (version 3), `ext-data-control-v1` for the clipboard
and `wlr-layer-shell` for `-r`. mwc has all three, and so do most other wlroots
compositors.

## Behavior

- **Pictures folder:** `XDG_PICTURES_DIR` from `~/.config/user-dirs.dirs`, as
  `xdg-user-dir PICTURES` reports it, otherwise `~/Pictures`. `Screenshots` inside it is
  created when needed.
- **Nothing is overwritten:** a second screenshot within the same second is named
  `…_2.png`. With `-f` the given file is replaced. A file only ever shows up complete,
  never half written.
- **Several monitors** end up in one picture, laid out as they are in the layout. Gaps
  stay black. With different scales the picture gets the largest one, so no monitor
  loses resolution. Rotated and flipped monitors come out the right way up.
- **Rectangle (`-r`):** screenShot captures the screen first and shows that still,
  dimmed, on every monitor. Drag a rectangle with the left button: within it the still
  shows undimmed, with a white border around it. Letting go takes it. A click without a
  drag takes the whole monitor under the pointer. Esc or another button gives up, with
  exit status 1. Since the picture is a still, menus and tooltips stay as they were, and
  the overlay is never in it. Edges go to the pixels the pointer is on, also on a scaled
  monitor, and a rectangle may span several monitors. `-r` combines with `-c`, `-f`,
  `-o`, `-d` and `-p`.
- **Clipboard:** on Wayland the clipboard holds no data, only a promise: whoever copied
  hands the data over when someone pastes. So `screenShot -c` returns at once but leaves
  a process in the background until something else is copied or the compositor exits.
- **Without `WAYLAND_DISPLAY`**, for example from an ssh login, a console or a program
  started outside the session, screenShot looks in `$XDG_RUNTIME_DIR` (otherwise
  `/run/user/<uid>`) for the first `wayland-N` socket whose compositor offers
  screencopy.
- **Exit status:** 0 on success, 1 on an error (message on stderr), 2 for bad options.

## For Claude sessions

A Claude Code session on this machine, including one over Remote Control from a phone,
can run screenShot directly and then look at the picture:

    screenShot -f /tmp/screen.png        # prints the path, then open the PNG with Read
    screenShot -o DP-1 -f /tmp/dp1.png   # only one monitor; -l lists the names
    screenShot -d 2 -f /tmp/screen.png   # let something open first

`-r` waits for a person with a mouse, so a session uses it only when someone is at the
screen to drag the rectangle. The session's shell doesn't have to be started inside
mwc: without `WAYLAND_DISPLAY`, screenShot finds the compositor by itself (see above).
The session then sees exactly what is on the screen, including open mail or a password
manager.

## Tests

    make test

runs screenShot under a headless mwc. Every picture is compared pixel for pixel with
grim's: one monitor, all eight rotations and flips, scale 2, two monitors side by side,
plus the clipboard, the pictures folder and starting without `WAYLAND_DISPLAY`. `-r` is
driven with mwc's simulated mouse and keyboard. It needs a built mwc in `MWC_DIR`
(otherwise `../mwc`) with `build/vinput` and `build/pngtool`, and `grim`. mwc's
`distrobox.ini` has everything for it.

## License

MIT, see [LICENSE](LICENSE).
