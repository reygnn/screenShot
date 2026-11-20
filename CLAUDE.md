# screenShot

Project rules. Everything else: see the global CLAUDE.md, section "C projects".


## Build and test

`make` builds on the host. `make test` needs a built mwc (by default `../mwc`, else
`MWC_DIR`) with `build/vinput`, `build/pngtool` and `build/capturec`, plus grim; run it in
mwc's box:
`distrobox enter mwc-dev -- bash -lc 'cd ~/data/C-Projects/screenShot && make test'`.

| Target | |
|---|---|
| `make` | screenShot |
| `make build-test` | `tests/build.sh`: CPPFLAGS on every compile line, protocol files follow their XML |
| `make test` | `make build-test`, then `tests/test.sh`: a headless mwc, log in `build/test.log`, a failing pair of pictures in `build/fail-*.png` |
| `make lint` | C lines over 100 columns, tabs as 8 |

In `tests/test.sh`, `expect` checks once and works without mwc; `check`, `wait_for` and
`pixel_is` come from mwc's `tests/lib.sh` and poll.


## Rules

- One C file, `screenShot.c`, no window of its own.
- Tests compare against grim pixel for pixel.
- Tests count and stop only their own screenShot processes (by `SMOKE_DIR` in their
  environment), never the running session's, for example the one holding its clipboard.
- A test client screenShot needs belongs in mwc's `tests/`, not here.
- C lines at most 100 columns, tabs as 8, as in mwc.
