# Caveats

Limits of screenShot that are known and left as they are.


## Pictures

A screenshot taken while a toast is up has that toast in it.

Gaps between monitors stay black. With different scales the picture gets the largest
one, so no monitor loses resolution and the others are scaled up.

`-r` works on a still taken first, so menus and tooltips stay as they were, and the
overlay is never in the picture.

Only 8 bit shm formats are taken (XRGB, ARGB, XBGR, ABGR8888). A compositor that
offers only 10 bit ones gets "no shm format".

The stride the compositor gives for a buffer is taken as it is. One of less than four
bytes a pixel would have screenShot read past the buffer.

Little endian only: wl_shm's formats are little endian, and screenShot reads them as
cairo's, which are native. On a big endian machine the colours come out swapped.


## Clipboard

On Wayland the clipboard holds a promise, not data: whoever copied hands it over at
each paste. So `screenShot -c` returns at once but leaves a process in the background
until something else is copied or the compositor exits.

A paste whose reader takes no data for 5 seconds is given up, so a hung application
can't block the next one. The reader gets a cut off PNG.

Without `WAYLAND_DISPLAY` the sockets are tried in name order, so `wayland-10` comes
before `wayland-2`.


## Cursor

The crosshair of `-r` comes in the theme's sizes only. When the scale doesn't divide
that size, it gets the largest buffer scale that does and the compositor scales it up.
It is loaded once for the largest scale; the overlay is short lived.


## -r

mwc and most wlroots compositors have wp_viewporter. Without it the still is shown at
the output's scale rounded to an integer, unchecked against its size: on a fractional
scale it covers only part of the monitor, and a compositor that checks may reject an
odd size.

The overlay is drawn again on pointer motion. A second configure, or both buffers
still held by the compositor, shows only once the pointer moves; wlroots does neither.

A drag shorter than one logical pixel in both directions counts as a click.


## Errors

A compositor that breaks the connection during startup is reported as one without
wlr-screencopy.


## Privacy

A Claude session that runs screenShot sees exactly what is on the screen, also open
mail or a password manager. The toast shows the person at the screen each time; `-q`
hides it.
