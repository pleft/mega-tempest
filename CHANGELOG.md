# Changelog

All notable changes to **MEGA TEMPEST** are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project loosely follows [Semantic Versioning](https://semver.org/), pre-1.0.

## [Unreleased] — v0.3-beta (in development)

### Changed
- **Scoring is now on the Jaguar scale.** `g_score` promoted from `u16` to `u32`. Per-enemy points match the Jaguar's `doscore` table (yak.s:22772-22785): flipper / super-flipper / spiker = 100, pulsar = 10, fuseball = 250, spike segment = 200, tanker break itself = 0 (children score on their own kills). All score increments go through a new `award_score()` helper that also runs the bonus-life threshold check.
- **Wave-completion bonus** awarded on each wave clear, computed via the Jag's formula `2600 + 200·L + L³` (yak.s:9067-9075). Displayed as `BONUS NNNNNN` on the WAVE-GET-READY splash.
- **Bonus life mechanic swapped** from "every 4 waves" to the Jaguar's "every 10 000 points" threshold-cross (yak.s:18380-18383). Lives just appear silently in the HUD as the score passes 10k boundaries (no more "1UP!" splash line).
- **HUD score readout widened** from 4 digits to 6, with leading zeros rendered as spaces. The `SCORE:` label moved left to accommodate. Same widening on the game-over screen.
- **Hi-score table defaults** restored to the Jaguar's animal-named values (YAK 500 000 down to FUR 50 000) now that scoring is on the right scale.

### Added
- **EXCELLENT! splash** on a no-deaths wave clear. Centred text overlay during the 1 s wave-clear hold before the fly-down-tube transition.
  *Adapted trigger*: the Jag's EXCELLENT (yak.s:4193, 4703) only fires on **warp-tunnel** or **bonus-stage** completions, which require the warp mechanic + the 3 bonus-level game scenes we haven't ported yet. Our closest faithful trigger is "wave cleared without dying" — keeps the flourish meaningful instead of cheapening it on every wave. When the warp / bonus-stage scenes land (v0.4+), revisit this trigger to match the Jag exactly. Audio (voice sample SFX #21 at two pitches) is also pending — needs sub-ROM budget audit + sample extraction.

### Changed (SFX architecture)
- **SFX byte bank moved from sub binary → cart ROM + PRG-RAM**. The sub used to bake all SFX bytes into its `spx.smd` binary inside the 56 KB `MODULE_ROM` region, so adding a new SFX always meant trimming an existing one. Now the cart `.incbin`s the bank as `res_sfx_data` and `mcd_upload_sfx_bank()` copies it to PRG-RAM at boot (`$01100`) along with a metadata table (`$0F000`); the sub dereferences both directly at `sfx_play` time. Removing SFX from the sub binary shrank it from ~57 KB → **~11 KB**.
- **MODULE_ROM relocated** from `$10000-$1DFFF` (56 KB) to `$14000-$1DFFF` (40 KB). The freed `$01100-$13FFF` region (~75 KB) is now the SFX bank, large enough to hold all current SFX at full Jag-original sizes with headroom for ~10 more.
- **DEATH / ZAP / EXC restored to full Jag-original sizes** — previously trimmed (12 KB / 7 KB / 13.5 KB) to fit the old 56 KB bank, now stored at 21 KB / 17 KB / 23 KB. DEATH plays its full crash decay; ZAP plays the full Crackle; EXC plays the trailing reverb of "Excellent!" instead of cutting off after the "t".

### Changed (on-screen font)
- New hand-designed 6×7 chunky-italic pixel font, replacing the Jaguar-extracted cfont which didn't translate cleanly to 1bpp tiles.

### Removed
- `BONUS_LIFE_EVERY` + `g_bonus_life_pending` (superseded by the 10k threshold).
- Sub-side `sub/src/sfx_data.{c,h}` (SFX bytes live in the cart now).

## [0.2.1-beta] — 2026-06-01

### Fixed
- **rave4 "Play"-voice echo on real hardware (real fix)**. The v0.2-beta attempt at this only filled the streaming ring from the write pointer forward, which assumed the chip's read pointer was *behind* the write pointer. On real silicon the chip can overrun the write pointer by a tick under certain pitches, leaving the chip reading stale end-of-sample bytes BEHIND the fill point and re-playing them on each loop-wrap (audible as "lay lay lay" looping the last syllable of "Play"). v0.2.1 blankets the *entire* ring with `$FF` on end-of-source so no matter where the chip is, the next byte read triggers a wrap to ring_start (also `$FF`) → silent infinite loop. Confirmed silent on real Mega Drive + Mega CD. (Builds on `f58c723` from v0.1-beta and the stack-init fix from v0.2-beta.)

## [0.2-beta] — 2026-06-01

### Added
- **Enemy variants** — `fuse-tanker` (green, splits into 2 fuseballs), `pulsar-tanker` (cyan, splits into 2 pulsars), and `super-flipper` (white, ≈1.5× faster). Introduced across waves 6-9 to ramp difficulty. Tanker kind stored in unused `step_period` field; super-flipper is a new entity type sharing flipper tick/collision code. Per-sprite palette select renders the same sprite tiles in different colours. (MC-T49, `6fcdbe1`)
- **Hi-score hall of fame** — 10-entry table modelled on the Jaguar T2K original (yak.s:5598-5742). Title-screen attract loop alternates every 8 s between the ASIC-pulsed banner and a `HALL OF FAME` table view. Game-over flow: if the player's score qualifies, a `NEW HIGH SCORE — ENTER YOUR INITIALS` scene captures three letters via D-pad before returning to the GAME OVER scene. Defaults are the Jag's animal-named entries scaled to fit the current 4-digit HUD range. **Persistence to Mega CD Backup RAM is parked for v0.3** — the BIOS `BURAM` calls hang in Mode 1 cart context. (MC-T50, `6928491`)
- **MOD sample extractor** (`tools/extract_mod_samples.py`) — debugging helper that emits each instrument in a Protracker MOD as a standalone WAV. (`af7e5f6`)

### Fixed
- **Streaming-ring echo on end-of-source** — when a streamed MOD sample exhausted its source bytes, the old code wrote `$FF` at `ring[0]` only. The chip would keep reading sequentially through the ring and play *stale bytes from earlier ring passes of the same sample* before reaching the loop wrap — audible on real silicon as a faint echo of the sample's start. (Identified specifically as rave4's "tempest tune 4" voice sample bleeding under gameplay music.) ares masked the bug because of different chip-drain pacing. Fix fills the ring tail with `$FF` from the current write position outward so the chip hits end-of-sample before reaching stale data; the RF5C68A datasheet confirms `$FF` is treated as loop-stop and the address resets silently to `LSH/LSL`. (`f58c723`, pending real-hardware verification)

## [0.1-beta] — 2026-05-29

First public release. Tagged at commit `b95ccb4`; verified working on real Mega Drive + Mega CD hardware 2026-05-30.

### Added
- Complete title → 16-wave play → death → game-over → title loop
- 5 core enemies — flipper, tanker, pulsar, fuseball, spiker (+ spike obstacles)
- 6 power-ups dropped by killed tankers — LASER, JUMP, +life, +superzapper, wave-skip, AI droid sidekick
- Superzapper with Jaguar "Crackle" PCM sample
- Bonus life every 4 waves; 3-life player loop
- ASIC fly-down-tube wave transition (Mega CD ASIC stamp engine)
- True per-vertex 3D web at 60 Hz via pre-baked Word RAM camera variants
- Title screen with ASIC-pulsed MEGA TEMPEST banner, drifting starfield, flashing START prompt
- Title theme `tune13.mod` + 4 gameplay MODs cycled per wave (`rave4 / tune7 / tune5 / tune12`, matching the Jaguar's `webtunes[]` set)
- Music continuity through scene transitions (no music drop on the LOADING bake)
- 4 PCM SFX — FIRE / HIT / DEATH / Crackle — all extracted from the Jaguar sample bank
- Pause via START button
- 8-shape web rotation per wave (V / square / plus / triangle / pentagon / star / W / fan) — polygons lifted from `yak.s`
- 8-step blue gradient web palette matching the Jaguar's electric-blue rim
- T2K cfont used for all on-screen text
- "WAVE N — GET READY" splash on every wave start
- Drifting starfield with VDP plane-priority layering (sprites > web > stars)

### Known limitations
- Game loops at wave 16 (no bonus stages, no 99-level progression, no Beastly mode)
- No high-score persistence
- No options screen / level select
- PAL timing untested
- BlastEm has Mode 1 Sub-CPU / Word-RAM issues — use ares

[Unreleased]: https://github.com/pleft/mega-tempest/compare/v0.2.1-beta...HEAD
[0.2.1-beta]: https://github.com/pleft/mega-tempest/releases/tag/v0.2.1-beta
[0.2-beta]: https://github.com/pleft/mega-tempest/releases/tag/v0.2-beta
[0.1-beta]: https://github.com/pleft/mega-tempest/releases/tag/v0.1-beta
