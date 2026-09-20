# Beneath a Steel Sky — a BBS door

Revolution Software's 1994 cyberpunk adventure, with art and the intro comic by
Dave Gibbons, running on ScummVM's `sky` engine compiled to WebAssembly and
drawn on the caller's own machine through TRACE.

Revolution released the game as freeware with its source in 2003.

## How it works

```
        the BBS                                 the caller's PC
 ┌──────────────────────┐                 ┌───────────────────────────┐
 │ steelskydoor         │  TRACE APCs     │ TERMinator                │
 │  - sends the game    │────────────────►│   steelsky.wasm           │
 │    once, by hash     │                 │    - ScummVM's sky engine │
 │  - keeps the saves   │◄── saves ───────│    - 320x200, own menus   │
 └──────────────────────┘                 └───────────────────────────┘
```

Unlike the Micropolis door, the game runs **on the caller's machine**. A
point-and-click animates the whole screen constantly, so there are no cheap
deltas to send — shipping pictures down the wire is the problem the TRACE engine
exists to avoid.

**ScummVM is invisible.** There is no launcher and no ScummVM UI: the module
starts the `sky` engine directly, so the player sees the Gibbons intro and then
the game's own menus and its own save screen, exactly as on a 1994 PC.

## Controls

The game is a point-and-click, but it does **not** require `mouse=1`, so it
still plays on an older TRACE client.

| | |
|---|---|
| Mouse | move the pointer; left walks/uses, right looks |
| Arrows | move the same pointer (Shift moves faster) |
| Enter | left click — walk / use |
| Space | right click — look at |
| F5 | the game's own control panel: save, load, options, quit |

## The download

The CD version is sent, for the speech and the longer intro. That is **72 MB on
the first call** — roughly six minutes on a slow link, with a progress bar.
TERMinator caches it by SHA-256, so it happens once per machine and the game
starts straight away after that.

`sky.dsk` is 72 MB and TERMinator caps a single asset at 64 MB, so the door
splits it into 32 MB parts and the module's file layer stitches them back
together as the engine seeks. The engine never sees the seam.

The floppy version is only 8.8 MB and would work identically without the speech,
if six minutes ever proves too long for your callers.

## Saved games live on the BBS

A TRACE module has no disk, so saves travel over the wire and are kept under the
caller's account in `door/saves/<handle>-<usernum>/`. They follow the player to
whatever machine they dial in from.

Chunks land in a `.part` file and are renamed into place only once the whole save
has arrived, so a dropped carrier cannot leave a truncated save that the game
would later fail to load.

## Build

```sh
sh tools/get_scummvm.sh      # sparse clone of ScummVM at a pinned commit,
                             # then applies tools/patches/*.patch
sh tools/get_game.sh         # the freeware game data (checks sky.cpt's size)
make -C module               # steelsky.wasm  (needs wasi-sdk in ~/tools)
make -C door                 # steelskydoor
make -C door install         # copies the wasm + game data into door/
```

Verify without a BBS or Windows:

```sh
python3 door/test_door.py                  # a fake TERMinator on a pty
python3 door/test_door.py --bin            # binary frames
python3 door/test_door.py --nomouse        # an older client
python3 door/test_door.py --upload         # make it really send the 72 MB
cc -o /tmp/ts door/test_saves.c door/files.c && /tmp/ts   # the save round-trip
```

`tools/run_native.cpp` builds the module natively against stubbed TRACE
functions and dumps frames to .ppm, which is how the renderer was checked
without Windows — `gamesandbox_probe` is Windows-only.

## Things worth knowing

- **`sky.cpt` is not game data.** It is ScummVM's own engine file and the engine
  refuses to start unless it is exactly 419,427 bytes. It ships inside the CD
  archive and also lives at `dists/engine-data/sky.cpt` in ScummVM.
- **ScummVM needs RTTI** — `common/stream.cpp`, `common/file.cpp` and
  `audio/mixer.cpp` all use `dynamic_cast`. Never build it with `-fno-rtti`.
- **System headers must come before any ScummVM header.** `common/forbidden.h`
  poisons libc names and mangles `<time.h>` if it is included afterwards.
- **A module must do five things ScummVM's `main()` normally does**, or the
  engine dies at startup: supply a filesystem factory, init the plugin manager,
  set an active config domain, register the engine's keymaps from the
  MetaEngine, and make `delayMillis()` really sleep. Without that last one the
  engine spins at thousands of frames a second instead of 49.
- **`FILES_CHUNK` (door) and `kChunk` (module) must stay equal**, both 3000.

## Patches to ScummVM

`tools/patches/` holds changes applied to the ScummVM tree after checkout.
`get_scummvm.sh` applies them and is safe to re-run — an already-applied patch
is reported and skipped rather than failing. At present there is one:

| patch | what it does |
|---|---|
| `0001-shorten-intro-logos.patch` | Cuts the three publisher logo screens before the intro (`Intro::_mainIntroSeq`) from about 16 seconds to about 3. The intro itself is untouched. |

## Licence

ScummVM is GPL v3 — see `COPYING`. The patches above modify ScummVM's own
sources and are covered by the same licence. Beneath a Steel Sky is © Revolution Software,
released as freeware by its authors; art and intro by Dave Gibbons. The game data
is not in this repository; `tools/get_game.sh` fetches it from ScummVM's own
download site.
