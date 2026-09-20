# Retro Recomp — Dreamcast recomp library launcher

A game library menu for the native Dreamcast recompilations, styled after
rexmenu (the Xbox 360 family menu): same palette, same dashboard icon rail,
same pad-first navigation.

Every game in the rail is a **100% native port**: the original SH4 binary
recompiled to C and built for the host CPU, rendered through
raylib/OpenGL. There is no emulator anywhere in the stack. The launcher
itself is one small C++17/raylib binary.

## Features

- **Four-screen library**: `Home` (Play / Game files / Settings / About /
  Quit), `Files` (per-game import status for every game on the rail,
  with Import / Clear / Back actions), `Settings`, and `About` (per-game
  100% native details).
- The rail is built dynamically from the candidate games; any whose
  `launch.sh` is missing on disk is dropped (no "Game launcher not found"
  surprise when you press Play). Cover art loads from
  `recomp-launcher/assets/<id>.png`, then `icon.png`, then any `.png` in
  the game dir; missing art falls back to a letter tile so the rail has
  no holes.
- Dashboard-style game rail with cover art (or letter tiles), pad, mouse
  and keyboard navigation; LB/RB and mouse wheel switch the active game on
  the Home, Files and About screens.
- **Global settings**: FPS cap, FPS overlay, renderer (OpenGL via raylib
  or Vulkan via SDL3), controller mode and pad device are shared by every
  game in the rail — one launcher.conf under
  `~/.config/recomp-launcher/`. Every change auto-saves. Each game caps
  the cycle at its `max_fps` so MSR can't be pushed past its native 30.
- **Per-game import queue**: pick a `.gdi` for any game from the Files
  screen, then navigate to another game and pick its `.gdi` while the
  first import is still on disk; the launcher runs one import per game
  in parallel. The status line on every screen reports "Importing…" while
  the active game's import is running.
- **Clear imported data** per game (Files → Clear imported data), so a
  fresh dump can be re-imported. Play refuses to start a game with no
  content imported.
- Play forks the game's own `launch.sh` with the recomp environment and
  logs to `~/.local/share/recomp-launcher/logs/`. The MSR host gets
  `TZ=UTC` so the in-game clock isn't dragged by the host's zone.

## Building

Requirements: a C++17 compiler, raylib 4.5+ built as a static library,
python3 (for the importer), and optionally zenity (GTK file picker — the
Browse/Import rows fall flat without it).

```sh
make                      # expects raylib at ../raylib-src
make RAYLIB_DIR=/opt/raylib
./recomp-launcher
```

`make test` builds and runs the settings round-trip tests
(`settings.hpp` is unchanged by the launcher rewrite).

## Game layout

Games live in sibling directories of your home folder, one per port:

```
~/powerstone-native/     launch.sh, <game>_host, disc/
~/powerstone2-native/
~/msr-native/
```

The candidate table (id, title, directory, host binary, max_fps, how-it-
runs lines for the About block) is at the top of `main()` in
`src/main.cpp` — add a row per port. A row is dropped from the rail if
its `launch.sh` is missing. `src/recomp_input.h` is the shared
input-conventions header the game hosts use.

## Installing a game's content from a .gdi

You must supply your own dump of each game — no game content is, or may
be, distributed with this repository.

In the GUI: pick the game, **Files → Import GDI image…**, choose the
`.gdi` file (or the `.zip` containing the whole dump; only the needed
data track is unpacked, to a temp file next to the destination). The
status line reports progress on every screen; failures point at a log in
`~/.local/share/recomp-launcher/logs/`.

From the command line, same thing without the GUI:

```sh
tools/import_gdi.py "Power Stone v1.001 (1999)(Capcom)(US)[!].zip" ~/powerstone-native/disc
tools/import_gdi.py game.gdi ~/msr-native/disc --list   # just show the filesystem
```

The importer is pure python3 stdlib: it parses the `.gdi` manifest, cooks
the raw 2352-byte data track down to ISO 9660 sectors, handles the GD-ROM
absolute-LBA addressing quirk (session base 45000), extracts every file,
and writes `gdmap.txt` + `ISO_META.BIN` so the ports can answer guest
sector reads straight from the extracted files.

**Files → Clear imported data** wipes a game's `disc/` folder (after a
confirmation) so a different dump can be imported. Play refuses to start
a game with no content imported (`1ST_READ.BIN` and `gdmap.txt` both
present).

## Legal

The launcher and importer contain no Sega, Capcom or Bizarre Creations
code or data. Dump your own GD-ROMs; importing someone else's dump of a
game you don't own is piracy in most jurisdictions.

## License

MIT (see LICENSE).