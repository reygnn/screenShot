# Caveats

Limits of screenShot that are known and left as they are.


## Pictures

A screenshot taken while a toast is up has that toast in it.

Gaps between monitors stay black. With different scales the picture gets the largest
one, so no monitor loses resolution and the others are scaled up.

`-r` works on a still taken first, so menus and tooltips stay as they were, and the
overlay is never in the picture.


## Clipboard

On Wayland the clipboard holds a promise, not data: whoever copied hands it over at
each paste. So `screenShot -c` returns at once but leaves a process in the background
until something else is copied or the compositor exits.


## Cursor

The crosshair of `-r` comes in the theme's sizes only. When the scale doesn't divide
that size, it gets the largest buffer scale that does and the compositor scales it up.
It is loaded once for the largest scale; the overlay is short lived.


## Privacy

A Claude session that runs screenShot sees exactly what is on the screen, also open
mail or a password manager. The toast shows the person at the screen each time; `-q`
hides it.
