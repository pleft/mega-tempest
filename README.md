# MEGA TEMPEST — Sega Mega CD (Mode 1 Cart)

A from-scratch port of Atari Jaguar's **Tempest 2000** (Llamasoft / Atari, 1994) to the **Sega Mega Drive + Mega CD**. Plays as a **Mode 1 cartridge** — code lives on a Mega Drive cart, but uses the Mega CD's Sub CPU, Word RAM, and RF5C164 PCM chip for audio and graphics work the standalone Mega Drive can't do alone.

No CD-ROM disc required. Just a Mega Drive + Mega CD attached.

[![Watch the demo on YouTube — real Mega Drive + Mega CD hardware](https://img.youtube.com/vi/EKqgXYvCzAo/maxresdefault.jpg)](https://www.youtube.com/watch?v=EKqgXYvCzAo)

▶ **[Watch the demo on YouTube](https://www.youtube.com/watch?v=EKqgXYvCzAo)** — gameplay captured on real Mega Drive + Mega CD hardware.

> **Status: v0.2.1-beta (released 2026-06-01).** Patch release on top of v0.2-beta with the real fix for the rave4 voice-sample echo on real hardware (the v0.2 attempt only handled half the failure mode). v0.2's headline content — enemy variants (fuse-tanker, pulsar-tanker, super-flipper) and a 10-entry Hall of Fame — is unchanged. Verified working on **real Mega Drive + Mega CD hardware** + [ares](https://ares-emu.net/); BlastEm has known Mode 1 quirks. See [CHANGELOG](CHANGELOG.md) for what shipped in each version and [Known limitations](#known-limitations) below.

---

## Why Mode 1 cart?

The Mega CD is best known for FMV games, but its real gift is *hardware*: a second 68000 CPU, 256 KB of shared "Word RAM", a dedicated PCM audio chip, and an ASIC graphics engine. **Mode 1** lets you exploit all of that **without** the CD-ROM constraints — boot from a cartridge, but still call into the Mega CD silicon for anything you can offload.

For a port of Tempest 2000 — a 60 Hz vector-3D shooter with full MOD-tracker music — that turns out to be exactly the architecture you want.

---

## The architecture

```
                         ┌─────────────────────────┐
   ┌────────────┐        │     MEGA DRIVE          │
   │  CART ROM  │◄───────│  Main 68000  ─────►  VDP│   (sprites, planes, web)
   │   ~615 KB  │        │      │                  │
   └────────────┘        └──────┼──────────────────┘
                                │ gate-array comm regs
                                ▼
                         ┌─────────────────────────┐
                         │       MEGA CD           │
                         │  Sub 68000 ────► RF5C164│   (MOD playback + SFX)
                         │      │                  │
                         │      ▼                  │
                         │  PRG RAM (512 KB)       │   MOD data lives here
                         │                         │
                         │  Word RAM (256 KB) ◄───►│   shared with Main
                         │      │                  │
                         │      ▼                  │
                         │  ASIC stamp/map engine  │   wave-transition zoom
                         └─────────────────────────┘
```

**Main CPU (cart)** runs the game loop:

- VDP sprite + tile rendering, plane priority, palette
- Player input, web shape geometry, entity pool, collision
- Wave system, scoring, lives, game state machine
- Drives the Sub CPU via gate-array communication registers

**Sub CPU (Mega CD)** is a dedicated audio + special-effects engine:

- Decodes a 4-channel MOD tracker file and streams samples to the RF5C164
- Plays one-shot PCM SFX (fire / hit / death) on a 5th channel
- Drives the ASIC stamp engine for the wave-transition zoom

**Word RAM** is the shared canvas:

- 16 pre-rasterised "lane variants" of the web (~12.8 KB each), one per player position around the rim. Per-vertex 1/Z perspective baked in.
- Per frame, the variant matching the player's current lane gets DMA'd to VRAM — gives true 3D web parallax at 60 Hz without per-frame software rasterising.

**PRG RAM** holds the active MOD file (one of `rave4 / tune7 / tune5 / tune12` for gameplay, `tune13` for the title screen — all five lifted from the Jag source's tune table).

---

## Game features

- **Animated title screen** — drifting stars, flashing START prompt, "MEGA TEMPEST" banner rendered + pulsed by the Mega CD ASIC every frame
- **All 5 core enemies** from the Jaguar original: Flipper, Tanker (splits into 2 flippers), Pulsar (pulse-gated kill), Fuseball (erratic lane-hopper), Spiker + Spike obstacles
- **Per-wave progression** — 16 waves, each with its own enemy quota and shape, then loops forever with escalating difficulty
- **"WAVE N — GET READY" splash** between waves, freezing the action for ~1.5 s
- **Bonus life every 4 waves** (capped at 9), celebrated with a "1UP!" on the splash
- **Superzapper power-up** — 1 charge per wave; clears every live enemy with a screen-flash + per-kill spark + the Jag's own Crackle PCM
- **Six power-ups** — dropped by killed tankers: LASER (rapid fire), JUMP (immune to rim deaths), extra life, extra superzapper charge, wave-skip, and an AI droid sidekick that walks the rim and fires at the nearest enemy
- **Enemy variants** — green fuse-tankers split into 2 fuseballs, cyan pulsar-tankers split into 2 pulsars, white super-flippers (≈1.5× faster) — introduced across waves 6-9 to ramp difficulty
- **Hall of Fame** — 10-entry hi-score table modelled on the Jag's, displayed on the title screen via an 8-second attract-loop swap with the banner; after game-over the player enters 3-letter initials if they qualify (session-only — persistence to Backup RAM is a v0.3 item)
- **Web shape rotation** — 8 shapes from the Jag (V, square, plus, triangle, pentagon, star, W, fan) extracted directly from `yak.s`
- **3-life player loop** → game over → back to title; START pauses
- **Iconic fly-down-tube wave transition** rendered by the Mega CD ASIC
- **MOD music** — separate title theme + 4 gameplay tracks (`rave4`, `tune7`, `tune5`, `tune12`) cycled per wave, matching the Jaguar's own `webtunes[]` set. Music keeps playing continuously through scene transitions.
- **4 PCM SFX** — FIRE, HIT, DEATH, ZAP — all extracted from the Jaguar sample bank

---

## Known limitations

This list reflects the current v0.2.1-beta release. The core gameplay loop is complete and stable; the following are known and intentional gaps:

- **Loops at wave 16.** The Jag original has 99 levels with bonus stages and a warp mechanic between them. The port currently cycles the 16 wave definitions endlessly with escalating difficulty. Bonus stages + the level-99 victory + Beastly NG+ mode are scoped but not implemented.
- **Hi-score table is session-only.** Backup-RAM persistence between power cycles is a v0.3 item — the Mode 1 cart's bootstrap doesn't initialise the BIOS state that the BURAM calls need, so saves currently hang. Table + qualifying detection + initials entry all work in-session.
- **No options screen or level select.**
- **Real-hardware behaviour.** Verified booting + playing end-to-end on real Mega Drive + Mega CD as of 2026-05-30. **One known hardware-only audio issue**: a faint echo on some `rave4.mod` samples that does not reproduce in ares — suspected PCM-RAM allocation overlap with SFX, pending fix.
- **Emulator support.** Developed against [ares](https://ares-emu.net/), which models the Mega CD ASIC and Word RAM accurately. **BlastEm has known Mode 1 sub-CPU / Word-RAM issues** — graphics will glitch or hang.
- **Tested with PAL TVs / 50 Hz timing**: only US/JP 60 Hz mode has been driven through the wave cycle. PAL should boot but level pacing is untested.

Bug reports and feedback are very welcome.

---

## Technical highlights

### Per-vertex 3D web at 60 Hz
The Jag rasterises the web with per-vertex 1/Z perspective every frame on its GPU. We can't do that on the Mega Drive — there's no FPU and tile-rendering 12.8 KB per frame would blow the budget. So instead we **pre-bake every camera angle**: at level install, software-render 16 web variants (one per player lane, each with that lane's perspective shift) into Word RAM. Per frame, DMA the variant matching the player's lane to VRAM. **Result: per-vertex parallax at 60 Hz** with ~7 ms per-frame DMA cost.

### Fly-down-tube transition via the Mega CD ASIC
Between waves the web shrinks to a vanishing point — the iconic T2K transition. We can't scale plane B on the VDP, so we revive the otherwise-dormant Mega CD ASIC stamp engine. At transition start, the current web shape is baked into 5×5 ASIC stamps. The Sub CPU then renders the web into Word RAM via a trace vector with growing scale; the result DMAs to VRAM. About 28 frames of pure ASIC zoom, then the variant pipeline takes back over for the new wave.

### Drifting starfield with plane priority layering
Plane A holds a sparse pseudo-random scatter of single-pixel stars across the full screen. The 4 star tiles are re-DMA'd each frame with the bright pixel shifted by 1, so all stars drift diagonally over a ~1 s cycle. Plane B (web) and sprites both run at priority=1; plane A stars at priority=0 — so the web naturally "floats" in space without painting around the V's exact silhouette.

### Entity pool with field reuse
Every game object — flipper, tanker, pulsar, fuseball, spiker, debris particle, superzapper spark — is one entry in a fixed 32-slot pool. Each new entity type **overloads existing struct fields** with per-type semantics rather than widening the struct. This is a hard-won rule: an earlier session lost ~10 hours to a struct-widening bug that turned out to be a memory-corruption-class issue we couldn't isolate, and never reproduced after reverting.

### ROM size: 16 MB → ~615 KB
The megadev linker script anchors `.data` at `0xFF0000`, which makes objcopy pad the output binary to that offset. We trim back to `_rom_end` (read from the sym file) after the build — cart binary down from a default 16 MB to actually-used ~615 KB (most of which is the 5 MOD files).

### Color fidelity
The web uses an 8-step pure-blue gradient across CRAM slots 5-8 + 12-15 (every valid `xBGR` blue intensity from `0x2` to `0xE`), with the outline at `0x0E80` to match the Jag's electric-blue rim. The player claw still uses yellow on slot 11.

### Hand-designed pixel font
On-screen text uses a hand-designed 6×7 chunky-italic pixel font. Glyphs live inline in `tools/extract_font.py`, which emits the 4bpp tile blob DMA'd to VRAM at boot.

---

## Build

```sh
./build.sh            # outputs ../mega-tempest-<version>.bin
```

`build.sh` is self-bootstrapping — run it from a fresh checkout on any
machine. On the first run it will:

1. check for the three tools it can't install for you: **`git`**, **`docker`**, **`python3`**;
2. fetch the Tempest 2000 game assets and convert them (see *Assets* below);
3. clone the [megadev](https://github.com/drojaazu/megadev) framework next to this repo;
4. build the megadev m68k toolchain Docker image (one-time, ~1–2 min);
5. build the Sub CPU module (`sub/spx.smd`), then the cart, and copy the result to `../mega-tempest-<version>.bin`.

Network access is needed on the first run (the clones + the Docker image
build). Subsequent runs reuse everything and just rebuild. The version is
pinned at the top of `build.sh` — bump it on each tagged release.

### Assets

The music (`*.mod`), font, sprite shapes, and sound effects are fetched from
the [mwenge/tempest2k](https://github.com/mwenge/tempest2k) source tree and
converted at build time by `fetch_assets.sh` (which calls the
`tools/extract_*.py` scripts). Both upstreams are pinned to specific commits
so the build is reproducible. To regenerate the assets without building:

```sh
./fetch_assets.sh
```

## Run

Grab `mega-tempest-v0.2.1-beta.bin` from the [releases page](https://github.com/pleft/mega-tempest/releases) (or build from source) and load it in an emulator that supports Mega CD Mode 1 cart booting. **[ares](https://ares-emu.net/)** is recommended — accurate Sub CPU + Word RAM + ASIC. BlastEm has known quirks with Mode 1 Sub-CPU WR ownership.

```
ares mega-tempest-v0.2.1-beta.bin
```

On real hardware: flash to a flashcart that supports Mode 1, attach to a Mega CD, boot.

## Controls

| Button | Title | Playfield | Game over |
|--------|-------|-----------|-----------|
| **D-pad ◀ ▶** | — | walk lanes | — |
| **A** | — | fire | — |
| **B** | — | superzapper (1 charge per wave) | — |
| **START** | begin play | **pause / resume** | back to title |

### Power-ups

Killed tankers drop a glyph that drifts to the rim — grab it by being on the same lane:

| Glyph | Effect |
|-------|--------|
| **L** | LASER — rapid fire for 5 s |
| **J** | JUMP — claw lifts above the lane, immune to rim deaths for 1 s |
| **1** | extra life (+1, capped at 9) |
| **Z** | extra superzapper charge (+1, capped at 5) |
| **S** | force the wave-end transition (instant skip to next wave) |
| **D** | AI droid sidekick — walks the rim and fires at the nearest enemy for ~20 s |

---

## Credits + sources

- **Original game**: Tempest 2000 — Llamasoft / Atari, Jaguar (1994), by Jeff Minter and team.
- **Reverse-engineered Jag source**: [mwenge/tempest2k](https://github.com/mwenge/tempest2k) — invaluable reference for enemy AI, wave structure, polygon data, and palette decisions.
- **Mega CD development framework**: [megadev](https://github.com/drojaazu/megadev) — provided the Mode 1 boot setup, Sub CPU build pipeline, and VDP helpers.
- **MOD player on RF5C164**: based on the Chilly Willy lineage via [matteusbeus/ModPlayer](https://github.com/matteusbeus/ModPlayer).
