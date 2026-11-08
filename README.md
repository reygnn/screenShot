# msc – Screenshot in eine Datei oder ins Clipboard (für mwc)

Eine C-Datei, nur libwayland und cairo. Nimmt den ganzen Bildschirm auf, über alle
Monitore oder einen davon:

    msc               nach Screenshots/ im Bilder-Ordner, z. B.
                      ~/Bilder/Screenshots/Screenshot_2026-11-08_14-03-22.png,
                      der Pfad kommt auf stdout
    msc -c            ins Clipboard, als image/png
    msc -f DATEI      in diese Datei, - für stdout
    msc -o NAME       nur dieser Monitor, msc -l zeigt die Namen
    msc -d SEKUNDEN   erst so lange warten
    msc -p            mit Mauszeiger
    msc -v            Version

## Bauen

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libcairo2-dev
    make
    sudo make install        # -> /usr/local/bin/msc

## In mwc

**Print** ruft `msc`, **Shift+Print** `msc -c`. Andere Befehle stellt
`~/.config/mwc/mwmrc` ein:

    Mwm*screenshot:            msc
    Mwm*clipboardScreenshot:   msc -c

Mit Verzögerung, etwa damit ein Menü noch aufgehen kann: `msc -d 3`.

msc braucht `wlr-screencopy` (Version 3), fürs Clipboard `ext-data-control-v1`. mwc hat
beides, andere wlroots-Compositoren meist auch.

## Verhalten

- **Bilder-Ordner:** `XDG_PICTURES_DIR` aus `~/.config/user-dirs.dirs`, wie
  `xdg-user-dir PICTURES` ihn nennt, sonst `~/Pictures`. `Screenshots` darin wird bei
  Bedarf angelegt.
- **Nichts wird überschrieben:** Ein zweiter Screenshot in derselben Sekunde heißt
  `…_2.png`. Mit `-f` wird die angegebene Datei ersetzt. Die Datei erscheint immer erst
  fertig, nie halb geschrieben.
- **Mehrere Monitore** landen so in einem Bild, wie sie im Layout liegen. Lücken bleiben
  schwarz. Bei verschiedenen Skalierungen bekommt das Bild die größte, damit kein Monitor
  Auflösung verliert. Gedrehte und gespiegelte Monitore stehen richtig herum.
- **Clipboard:** Unter Wayland hält das Clipboard keine Daten, nur ein Versprechen. Wer
  kopiert hat, liefert beim Einfügen. `msc -c` kehrt deshalb sofort zurück, lässt aber
  einen Prozess im Hintergrund, bis etwas anderes kopiert wird oder der Compositor
  endet.
- **Ohne `WAYLAND_DISPLAY`**, etwa aus einem ssh-Login, einer Konsole oder einem
  Programm, das außerhalb der Sitzung gestartet wurde, sucht msc selbst in
  `$XDG_RUNTIME_DIR` (sonst `/run/user/<uid>`) nach dem ersten `wayland-N`-Socket, dessen
  Compositor Screencopy anbietet.
- **Exit-Status:** 0 bei Erfolg, 1 bei einem Fehler (Meldung auf stderr), 2 bei falschen
  Optionen.

## Für Claude-Sessions

Eine Claude-Code-Session auf diesem Rechner, auch eine über Remote Control vom Handy, kann
msc direkt aufrufen und sich das Bild danach ansehen:

    msc -f /tmp/screen.png        # druckt den Pfad, dann das PNG mit Read öffnen
    msc -o DP-1 -f /tmp/dp1.png   # nur ein Monitor; msc -l zeigt die Namen
    msc -d 2 -f /tmp/screen.png   # erst etwas aufgehen lassen

Die Shell der Session muss dafür nicht in mwc gestartet sein. Fehlt `WAYLAND_DISPLAY`,
findet msc den Compositor selbst (siehe oben). Die Session sieht dann genau das, was auf
dem Bildschirm steht, auch offene Mails oder Passwortmanager.

## Tests

    make test

fährt msc unter einem headless laufenden mwc. Jedes Bild wird Pixel für Pixel mit dem von
grim verglichen: ein Monitor, alle acht Drehungen und Spiegelungen, Skalierung 2, zwei
Monitore nebeneinander, dazu das Clipboard, der Bilder-Ordner und der Start ohne
`WAYLAND_DISPLAY`. Es braucht einen gebauten mwc in `MWC_DIR` (sonst `../mwc`) und
`grim`. Die `distrobox.ini` von mwc hat alles dafür.

## Lizenz

MIT, siehe [LICENSE](LICENSE).
