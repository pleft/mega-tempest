// Tempest 2000 — Mega CD Mode 1 cart, main entry point.
//
// Architecture: main = graphics + scene logic, sub = MOD music (see sub/).
// Web rendering + enemy sprites live in web.c; entity pool in entity.c;
// Mega CD comm-reg plumbing in mcd.c. This file owns VDP setup, joypad
// input, scene installs + handlers (TITLE / PLAYFIELD), and the main loop.

#include "res.h"
#include "entity.h"
#include "mcd.h"
#include "psg.h"
#include "web.h"
#include "hiscore.h"
#include <main/io.h>
#include <main/memmap.h>
#include <main/vdp.h>
#include <memory.h>
#include <system.h>

#define PLANE_A_ADDR    0x2000
#define PLANE_B_ADDR    0x4000
#define SPRITE_TBL_ADDR 0xb800

#define plane_xy(x, y) (to_vdp_addr(VDP_PLANE_POS(x, y, Width64) + PLANE_A_ADDR) | VRAM_W)

#if VIDEO == PAL
#define VIDEO_SIGNAL VDP_PAL_VIDEO
#else
#define VIDEO_SIGNAL 0
#endif

// ---- joypad ---------------------------------------------------------------

volatile u8 p1_hold, p1_single, p1_prev;

static void read_inputs(void)
{
  p1_hold = ~read_input_joypad(io_data1);
  p1_single = (p1_hold != p1_prev) ? (p1_hold & ~p1_prev) : 0;
  p1_prev = p1_hold;
}

// ---- VDP ------------------------------------------------------------------

u16 vdp_regs[18];   /* exposed: mcd.c reads vdp_regs[1] for DMA */
static u16 cram[64];

static void update_cram(void)
{
  vdp_ctrl_32 = to_vdp_addr(0) | CRAM_W;
  u16 * p = cram;
  for (u8 i = 0; i < 64; ++i) vdp_data = *p++;
}
static void clear_vram(void)
{
  vdp_ctrl_32 = to_vdp_addr(0) | VRAM_W;
  for (u16 i = 0; i < (0x10000 / 4); ++i) vdp_data_32 = 0;
}
static void clear_vsram(void)
{
  vdp_ctrl_32 = to_vdp_addr(0) | VSRAM_W;
  for (u16 i = 0; i < 40; ++i) vdp_data = 0;
}

volatile bool vblank_done;

__attribute__((interrupt)) void INT2_EXT(void)    { return; }
__attribute__((interrupt)) void INT4_HBLANK(void) { return; }
__attribute__((interrupt)) void INT6_VBLANK(void)
{
  read_inputs();
  vblank_done = true;
}


static u16 const default_vdp_regs[] = {
  VDP_REG_MODE1 | VDP_HICOLOR_ENABLE,
  VDP_REG_MODE2 | VDP_MD_DISPLAY_MODE | VDP_VBLANK_ENABLE | VIDEO_SIGNAL | VDP_DISPLAY_ENABLE,
  VDP_REG_PLA_ADDR | (PLANE_A_ADDR / 0x400),
  VDP_REG_WIN_ADDR | (0xa00 / 0x400),
  VDP_REG_PLB_ADDR | (PLANE_B_ADDR / 0x2000),
  VDP_REG_SPR_ADDR | (SPRITE_TBL_ADDR / 0x200),
  VDP_REG_SPR_ADDR2,
  VDP_REG_BGCOLOR,
  VDP_REG_HBLANK_COUNT,
  VDP_REG_MODE3 | VDP_EXTINT_ENABLE,
  VDP_REG_MODE4 | VDP_MASK_WIDTH_40CELL,
  VDP_REG_HS_ADDR | (0xbc00 / 0x400),
  VDP_REG_PL_ADDR2,
  VDP_REG_AUTOINC | 2,
  VDP_REG_PL_SIZE | VDP_PL_64x32,
  VDP_REG_WIN_HPOS,
  VDP_REG_WIN_VPOS};

static void print(char const * s, vdp_addr pos)
{
  /* priority=1 so plane-A text draws in front of the high-priority web. */
  vdp_ctrl_32 = pos;
  while (*s) vdp_data = (u16) (0x8000 | (u8) *s++);
}

static void plane_putc(u16 cx, u16 cy, char c)
{
  vdp_ctrl_32 = plane_xy(cx, cy);
  vdp_data = (u16) (0x8000 | (u8) c);
}

static void clear_play_area(void)
{
  /* Full plane-A wipe + fresh star scatter. Each scene's main_thread
   * calls this on its first frame to start from a clean background,
   * then overlays its own text. The web (plane B, priority=1) and the
   * sprites (priority=1 in emit_sprite_depth) draw IN FRONT of these
   * stars, so stars fill all the natural gaps around the web. */
  for (u8 y = 0; y < 28; ++y) {
    vdp_ctrl_32 = plane_xy(0, y);
    for (u8 x = 0; x < 40; ++x) vdp_data = ' ';
  }
  /* Sparse star scatter at 1/8 density across the whole plane. */
  u32 r = 0xCAFEF00Du;
  for (u8 cy = 0; cy < 28; ++cy) {
    for (u8 cx = 0; cx < 40; ++cx) {
      r ^= r << 13; r ^= r >> 17; r ^= r << 5;
      if ((r & 0x07) == 0) {                /* 1/8 density */
        u16 tile = (u16) (0x10 + ((r >> 3) & 0x03));
        vdp_ctrl_32 = plane_xy(cx, cy);
        vdp_data = tile;
      }
    }
  }
}

/* Live-starfield animation. Re-DMAs the 4 star tiles each shift step
 * with the white pixel moved by 1 px diagonally. All stars on screen
 * drift in lockstep — gives a "drifting through space" feel without
 * plane scrolling (which would also move the HUD).
 * 8-step cycle, 16 game frames per step ≈ 2.1 s per full cycle. */
static void update_starfield(void)
{
  extern u8 g_anim_frame;
  u8 const shift = (u8) ((g_anim_frame >> 3) & 0x7);   /* faster: ~1 s full cycle */
  static u8 last_shift = 0xFF;
  if (shift == last_shift) return;
  last_shift = shift;

  static const u8 BASE_XY[4][2] = { {2,1}, {6,3}, {2,6}, {4,4} };
  static u8 buf[4 * 32];
  for (u8 t = 0; t < 4; ++t) {
    u8 x = (u8) ((BASE_XY[t][0] + shift) & 7);
    u8 y = (u8) ((BASE_XY[t][1] + shift) & 7);
    u8 * tile = &buf[t * 32];
    for (u8 i = 0; i < 32; ++i) tile[i] = 0;
    tile[y * 4 + (x >> 1)] = (u8) ((x & 1) ? 0x01 : 0x10);
  }

  u16 const mode2_dma_on = vdp_regs[1] | VDP_DMA_ENABLE;
  vdp_ctrl = VDP_REG_AUTOINC | 2;
  vdp_ctrl = mode2_dma_on;
  vdp_dma_transfer((char const *) buf, to_vdp_addr(0x10 * 32) | VRAM_W,
                   (u16) (sizeof(buf) / 2));
  vdp_ctrl = vdp_regs[1];
}

// ---- Engine (doc 16) ------------------------------------------------------
//
// Three function pointers per scene:
//   always_vblank — runs every VBlank no matter what.
//   gated_vblank  — runs every VBlank unless paused.
//   main_thread   — runs on the main loop after gated_vblank fired this frame.

typedef void (*Handler)(void);

typedef struct {
  Handler always_vblank;
  Handler gated_vblank;
  Handler main_thread;
  u8      paused;
  u16     frame;          // u16 to avoid 32-bit libgcc divide/mod
} EngineState;

static EngineState g_engine;

u8  g_mcd_present;
static u8  g_music_playing;
static u8  g_music_idx;          /* gameplay tune slot (0..3); 0xFF = none */
static u8  g_scene_dirty;     // set by install_*; main loop redraws static text

// Forward decls — the scenes call each other across file order.
static void install_title(void);
static void title_main_thread(void);
static void play_main_thread(void);
static void install_gameover(void);
static void gameover_main_thread(void);
static void install_hiscore_entry(void);

// ---- Scene: TITLE ---------------------------------------------------------

/* Forward — g_anim_frame is defined in the play-scene section below. */
extern u8 g_anim_frame;

/* Title-screen attract loop — alternates every ATTRACT_PERIOD frames:
 *   0 = BANNER  : ASIC-pulsed MEGA TEMPEST banner + flashing START prompt
 *   1 = HISCORE : static MEGA TEMPEST text at top + hall-of-fame table
 * State + transition tracked here; the per-state rendering lives in
 * title_gated_vblank (ASIC kick gate) + title_main_thread (text). */
#define ATTRACT_PERIOD 480    /* 480 / 60 Hz = 8 s per state (Jag uses ~13 s; halved cycle here is fine for our 2-state loop) */
static u8  g_attract_state;
static u8  g_attract_prev_state;
static u16 g_attract_timer;

static void title_always_vblank(void) { return; }
static void title_gated_vblank (void)
{
  /* Keep the starfield drifting on the title screen too. update_starfield
   * gates on g_anim_frame, so tick it here as well as in play. */
  g_anim_frame++;
  update_starfield();

  /* Attract-loop state tick. */
  g_attract_timer++;
  if (g_attract_timer >= ATTRACT_PERIOD) {
    g_attract_timer = 0;
    g_attract_state ^= 1;
  }

  /* BANNER state only — pulse the ASIC banner. HISCORE state leaves
   * plane B alone (cleared by the BANNER→HISCORE transition in
   * title_main_thread); skipping the kick lets the table on plane A
   * show against the variant pipeline's last frame or a clean B. */
  if (g_mcd_present && g_attract_state == 0) {
    u8 t   = (u8) ((g_anim_frame >> 1) & 0x7F);
    u8 tri = (t < 64) ? t : (u8) (127 - t);
    s16 dx = (s16) (0x0800 + ((s16) tri - 32) * 0x30);
    mcd_render_asic_scale(0x4000, 0x280, 9, 1, dx);
  }
}

static void install_title(void)
{
  g_engine.always_vblank = title_always_vblank;
  g_engine.gated_vblank  = title_gated_vblank;
  g_engine.main_thread   = title_main_thread;
  g_engine.paused        = 0;
  g_scene_dirty          = 1;
  g_attract_state        = 0;       /* always start in BANNER mode */
  g_attract_timer        = 0;
  g_attract_prev_state   = 0xFF;

  if (g_mcd_present) {
    /* Stop whatever's playing, then upload + start the title theme
     * (tune13.mod — matches the Jaguar's "theme tune" at yak.s:1018). */
    if (g_music_playing) {
      mcd_stop_mod();
      mcd_wait_ack(CMD_STOP_MOD);
      g_music_playing = 0;
    }
    mcd_upload_mod(&res_tune13_mod);
    mcd_play_mod(res_tune13_mod.size);
    mcd_wait_ack(CMD_PLAY_MOD);
    g_music_playing = 1;
    g_music_idx     = 0xFF;       /* gameplay tune state invalidated */
  }
  web_clear_plane_b();

  /* Bake the "MEGA TEMPEST" banner into ASIC stamps. We paint the text
   * tiles into g_web_buf (which is the same buffer mcd_asic_pack_buf_to_stamps
   * reads from), then ship it to the stamp area in Word RAM. title_gated_vblank
   * fires per-frame CMD_RENDER_SCALE to pulse it. */
  if (g_mcd_present) {
    u8 * buf = web_get_buf();
    /* Clear the whole 20x20-cell source buffer first. */
    for (u16 i = 0; i < (20 * 20 * 32); ++i) buf[i] = 0;
    /* Paint "MEGA TEMPEST" at cells (4, 10) → (15, 10) — centred in the
     * 20-cell-wide source. Each glyph is one 32-byte font tile copied
     * straight from res_font into its cell. */
    static const char BANNER[] = "MEGA TEMPEST";
    u8 cy = 10;
    for (u8 i = 0; i < 12; ++i) {
      u8 cx  = (u8) (4 + i);
      u8 idx = (u8) (BANNER[i] - 0x20);
      u8 *       dst = buf + ((u16) cy * 20 + cx) * 32;
      u8 const * src = (u8 const *) res_font.data + (u16) idx * 32;
      for (u8 b = 0; b < 32; ++b) dst[b] = src[b];
    }
    mcd_asic_pack_buf_to_stamps();
  }
}

// ---- Scene: PLAYFIELD (web + player) -------------------------------------

static Entity * g_player;
static u8  g_player_lane;
static u8  g_left_tick;          // hold-to-repeat cooldowns
static u8  g_right_tick;
static u8  g_fire_cooldown;      // frames until next shot allowed
static u16 g_spawn_timer;        // frames until next flipper spawn
static u8  g_enemy_count;
static u16 g_score;
static u8  g_lives;              // remaining lives — game over at 0
u8         g_anim_frame;         // global frame counter — drives flipper rotation
static u32 g_rng = 0xCAFEF00Du;

/* ---- Wave system ---------------------------------------------------------
 * Each wave declares a per-type spawn quota + pacing. The spawn picker
 * draws from g_pool[] (random non-empty slot). When all pools are 0 AND
 * no enemies are alive, the wave is cleared and next_wave() advances.
 * Web shape rotates by g_wave_num % WEB_SHAPE_COUNT. */
typedef struct {
  u8  num_flippers;
  u8  num_tankers;
  u8  num_pulsars;
  u8  num_fuseballs;
  u8  num_spikers;
  u16 spawn_period;     // ticks between spawns (smaller = faster)
  u8  max_active;       // concurrent enemy cap
} WaveDef;

static const WaveDef WAVES[16] = {
  /* flip tank puls fuse spik  period  max  | wave  shape  */
  {   5,   0,   0,   0,   0,   150,    2 }, /*  W1   V       — gentle intro    */
  {   8,   0,   0,   0,   0,   140,    3 }, /*  W2   SQUARE                    */
  {   6,   2,   0,   0,   0,   130,    3 }, /*  W3   PLUS    — + tanker        */
  {   8,   2,   1,   0,   0,   120,    3 }, /*  W4   TRIANG  — + pulsar        */
  {   8,   2,   2,   1,   0,   120,    3 }, /*  W5   PENTA   — + fuseball      */
  {   8,   3,   2,   1,   1,   110,    4 }, /*  W6   STAR    — + spiker        */
  {  10,   3,   2,   2,   1,   100,    4 }, /*  W7   W                         */
  {  10,   3,   3,   2,   1,    90,    4 }, /*  W8   FAN                       */
  {  10,   4,   3,   2,   2,    90,    4 }, /*  W9   V (rep)                   */
  {  12,   4,   3,   3,   2,    80,    5 }, /* W10                             */
  {  12,   4,   4,   3,   2,    80,    5 }, /* W11                             */
  {  12,   5,   4,   3,   3,    75,    5 }, /* W12                             */
  {  14,   5,   4,   4,   3,    70,    5 }, /* W13                             */
  {  14,   5,   5,   4,   3,    70,    5 }, /* W14                             */
  {  16,   6,   5,   4,   4,    65,    6 }, /* W15                             */
  {  16,   6,   5,   5,   4,    60,    6 }, /* W16   FAN     — final           */
};

static u16 g_wave_num;          // 0-indexed wave (wraps mod 16 for shape)
static u8  g_pool[5];           // remaining-to-spawn: [flipper, tanker, pulsar, fuseball, spiker]
static u8  g_wave_clear_timer;  // counts up while wave-clear conditions hold
#define WAVE_CLEAR_DELAY 60     // 1 sec of empty-web pause before zoom fires

/* Fly-down-tube transition: after the wave-clear hold, scroll plane B
 * vertically each frame so the web slides off-screen. After ZOOM_OUT_FRAMES
 * the bake runs (next_wave). Plane B is the web only — plane A (stars,
 * HUD) and sprites are unaffected, so the player can still see the claw
 * at its rim position while the web vanishes underneath. */
static u8  g_zoom_out_frame;    // 0 = idle; 1..ZOOM_OUT_FRAMES while zooming
#define ZOOM_OUT_FRAMES 28      // 28 frames * 8 px = 224 px = off-screen

/* Wave-start splash: shows "WAVE NN — GET READY" centred for SPLASH_FRAMES
 * after each wave's bake finishes (incl. wave 0 from install_playfield).
 * Gated_vblank skips while > 0 — claw + enemies freeze; main_thread keeps
 * running so HUD + splash text render. */
static u16 g_splash_timer;
#define SPLASH_FRAMES 90

/* Bonus life: every BONUS_LIFE_EVERY waves cleared, +1 life (capped at
 * the HUD's single-digit ceiling). g_bonus_life_pending is consumed by
 * the wave splash to also draw a "1UP!" line. */
#define BONUS_LIFE_EVERY 4
static u8 g_bonus_life_pending;

/* Power-ups — dropped by killed tankers, drift outward to the rim.
 * Collected when reaching the rim on the player's lane.
 *   PUP_LASER → halves fire cooldown for LASER_FRAMES (~5 s).
 *   PUP_JUMP  → claw lifts above the rim, immune to rim deaths,
 *               for JUMP_FRAMES (~1 s).
 *   PUP_LIFE  → +1 life (capped at 9), instant.
 *   PUP_ZAP   → +1 superzapper charge (capped at 5), instant.
 *   PUP_SKIP  → instant wave clear — triggers the zoom-out transition.
 * g_pup_drop_count cycles through kinds on every tanker kill. */
#define PUP_LASER     0
#define PUP_JUMP      1
#define PUP_LIFE      2
#define PUP_ZAP       3
#define PUP_SKIP      4
#define PUP_DROID     5
#define PUP_KIND_COUNT 6
#define PUP_OUT_STEP  (FP_ONE >> 9)    /* ~8 s to drift the lane — slow enough to chase */
#define LASER_FRAMES  300              /* ~5 s of fast fire */
#define JUMP_FRAMES   60               /* ~1 s of claw lift + invulnerability */
#define SZ_CAP        5                /* stack up to 5 superzapper charges */
#define DROID_LIFE_FRAMES   1200       /* ~20 s before AI droid expires */
#define DROID_HOP_PERIOD    8          /* ticks between droid lane hops */
#define DROID_FIRE_MASK     0x17       /* fire every (mask+1) frames ≈ 24 */
static u16 g_laser_timer;
u16        g_jump_timer;    /* non-static: web.c reads it for claw-lift */
static u8  g_pup_drop_count;

/* Per-wave music cycle: every wave swaps in a new MOD from this 4-entry
 * pool. Matches the Jaguar's webtunes[] set (yak.s:19152) — rave4 /
 * tune7 / tune5 / tune12 — but cycles every wave instead of every 32
 * so a typical play session hears the full set. g_music_idx is declared
 * up by g_music_playing so install_title can reset it. */
static const DataChunk * const WAVE_MUSIC[4] = {
  &res_rave4_mod,
  &res_tune7_mod,
  &res_tune5_mod,
  &res_tune12_mod,
};

/* Superzapper: 1 charge per wave, B button triggers. Kills every live
 * enemy entity on screen at once. Spikes are NOT cleared (they're
 * terrain, not enemies). */
static u8 g_superzapper_charges;
#define SUPERZAPPER_PER_WAVE 1

/* Screen flash + per-kill spark — visual feedback for the zap. cram[0]
 * (background) goes white for ZAP_FLASH_FRAMES, and each killed enemy
 * leaves a stationary E_ZAPSPARK at its last position for ZAP_SPARK_LIFE. */
static u8 g_zap_flash;
#define ZAP_FLASH_FRAMES   4
#define ZAP_SPARK_LIFE     8
#define POOL_FLIPPER  0
#define POOL_TANKER   1
#define POOL_PULSAR   2
#define POOL_FUSEBALL 3
#define POOL_SPIKER   4

/* Debug switch — set to 1 to disable game-over (lives never decrement,
 * the player respawns indefinitely). Set to 0 for the actual game. */
#define INFINITE_LIVES     0
#define LIVES_START        3
#define LANE_HOLD_INITIAL 14     // frames before first repeat after first press
#define LANE_HOLD_REPEAT   4     // frames between subsequent repeats
#define FIRE_COOLDOWN      6     // frames between shots
#define SHOT_INWARD_STEP   (FP_ONE >> 5)   // 32 ticks rim->centre
#define FLIPPER_OUT_STEP   (FP_ONE >> 8)   // 256 ticks centre->rim (~4.3 sec)
#define TANKER_OUT_STEP    (FP_ONE >> 9)   // 512 ticks centre->rim (~8.5 sec) — slower
#define PULSAR_OUT_STEP    (FP_ONE >> 8)   // 256 ticks centre->rim (~4.3 sec) — same as flipper
#define FUSEBALL_STEP      (FP_ONE >> 9)   // slower base velocity (it flips direction often)
#define FUSEBALL_HOP_PERIOD 30             // ticks between random direction/lane changes
#define SPIKER_OUT_STEP    (FP_ONE >> 7)   // ~2 sec rim — fast painter
#define SPIKE_CUT_AMOUNT   (FP_ONE >> 2)   // shot trims this much off a spike's tip
#define SPIKE_KILL_THRESH  (FP_ONE - HIT_DEPTH_TOL)  /* spike-at-rim kills player */
#define FLIPPER_RIM_HOP    12              // frames between rim-walk hops
#define FLIPPER_SPAWN_PERIOD 150           // ~2.5 sec at 60 Hz
#define ENEMY_MAX_ACTIVE   3
#define HIT_DEPTH_TOL      (FP_ONE >> 4)   // collision threshold

/* Death-burst particle effect. When the player dies, 8 debris sprites spawn
 * at the claw's last screen position and fly outward in 8 cardinal +
 * diagonal directions for ~30 frames before despawning. g_respawn_timer
 * holds the player frozen (sprite hidden, input ignored) for the duration
 * of the burst, then lane 0 respawn happens once it hits zero. */
#define DEBRIS_LIFETIME    30
#define DEBRIS_DIRS         8
#define RESPAWN_DELAY      35      // a few frames longer than the burst
static const s8 DEBRIS_DX[DEBRIS_DIRS] = {  3,  2,  0, -2, -3, -2,  0,  2 };
static const s8 DEBRIS_DY[DEBRIS_DIRS] = {  0, -2, -3, -2,  0,  2,  3,  2 };
s16 g_death_x;                    // exposed to web.c's render_sprites
s16 g_death_y;
u8  g_respawn_timer;              // >0 = player is dead and counting down

/* Per-lane spike state — outer-edge depth (0 = no spike, FP_ONE = at rim).
 * Painted by spikers as they descend their lane; trimmed by shots. */
fp16 g_spike_depth[MAX_LANES];

/* Hit-flash counter per lane. Set on shot-cut; counts down each tick.
 * Render skips the spike marker every other frame while non-zero →
 * blink effect matching the Jag's flashcol behaviour. */
#define SPIKE_FLASH_FRAMES 8
u8 g_spike_flash[MAX_LANES];

static u16 lcg(void)
{
  g_rng = g_rng * 1103515245u + 12345u;
  return (u16) (g_rng >> 16);
}

static void spawn_shot(u8 lane)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_SHOT;
  e->lane         = lane;
  e->depth_fp     = FP_ONE;
  e->depth_vel_fp = -SHOT_INWARD_STEP;
  e->phase        = 0;
  if (g_mcd_present) mcd_play_sfx(0);    /* 0 = FIRE — PCM sample on Mega CD */
  else               sfx_fire();          /* PSG fallback when no Mega CD */
}

static Entity * spawn_flipper_at(u8 lane, fp16 depth_fp)
{
  Entity * e = entity_spawn();
  if (!e) return 0;
  e->type         = E_FLIPPER;
  e->lane         = lane;
  e->depth_fp     = depth_fp;
  e->depth_vel_fp = +FLIPPER_OUT_STEP;
  e->phase        = (depth_fp >= FP_ONE) ? 1 : 0;
  e->step_period  = FLIPPER_RIM_HOP;
  e->lifetime     = FLIPPER_RIM_HOP;
  g_enemy_count++;
  return e;
}

static void spawn_flipper(u8 lane) { (void) spawn_flipper_at(lane, 0); }

/* Super flipper — faster flipper variant (v0.2). Same tick + collision
 * code paths as regular flipper but ~50% faster descent and rim-walk.
 * Rendered with per-sprite palette select 1 → white body. */
static Entity * spawn_super_flipper_at(u8 lane, fp16 depth_fp)
{
  Entity * e = entity_spawn();
  if (!e) return 0;
  e->type         = E_SUPER_FLIPPER;
  e->lane         = lane;
  e->depth_fp     = depth_fp;
  e->depth_vel_fp = (fp16) (FLIPPER_OUT_STEP + (FLIPPER_OUT_STEP >> 1));
  e->phase        = (depth_fp >= FP_ONE) ? 1 : 0;
  e->step_period  = (u8) (FLIPPER_RIM_HOP - 4);
  e->lifetime     = (u8) (FLIPPER_RIM_HOP - 4);
  g_enemy_count++;
  return e;
}
static void spawn_super_flipper(u8 lane) { (void) spawn_super_flipper_at(lane, 0); }

/* Tanker variant kinds — stored in e->step_period (unused by tanker
 * tick code). The render pass picks per-sprite palette by this kind. */
#define TANKER_KIND_FLIPPER  0   /* default: splits into 2 flippers — pink */
#define TANKER_KIND_PULSAR   1   /* splits into 2 pulsars         — cyan  */
#define TANKER_KIND_FUSE     2   /* splits into 2 fuseballs       — green */

static void spawn_tanker_kind(u8 lane, u8 kind)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_TANKER;
  e->lane         = lane;
  e->depth_fp     = 0;
  e->depth_vel_fp = +TANKER_OUT_STEP;
  e->phase        = 0;
  e->step_period  = kind;
  e->lifetime     = 0;
  g_enemy_count++;
}

static void spawn_tanker(u8 lane) { spawn_tanker_kind(lane, TANKER_KIND_FLIPPER); }

static void spawn_pulsar_at(u8 lane, fp16 depth_fp)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_PULSAR;
  e->lane         = lane;
  e->depth_fp     = depth_fp;
  e->depth_vel_fp = +PULSAR_OUT_STEP;
  e->phase        = (depth_fp >= FP_ONE) ? 1 : 0;
  e->step_period  = 0;
  e->lifetime     = 0;
  g_enemy_count++;
}

static void spawn_pulsar(u8 lane) { spawn_pulsar_at(lane, 0); }

static void spawn_fuseball_at(u8 lane, fp16 depth_fp)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_FUSEBALL;
  e->lane         = lane;
  e->depth_fp     = depth_fp;
  e->depth_vel_fp = +FUSEBALL_STEP;
  e->phase        = (depth_fp >= FP_ONE) ? 1 : 0;
  e->step_period  = 0;
  e->lifetime     = FUSEBALL_HOP_PERIOD;
  g_enemy_count++;
}

static void spawn_fuseball(u8 lane) { spawn_fuseball_at(lane, 0); }

static void spawn_spiker(u8 lane)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_SPIKER;
  e->lane         = lane;
  e->depth_fp     = 0;
  e->depth_vel_fp = +SPIKER_OUT_STEP;
  e->phase        = 0;
  e->step_period  = 0;
  e->lifetime     = 0;
  g_enemy_count++;
}

/* Wave-aware pickers — called from the pool dispatch. Decide whether a
 * flipper / tanker spawn becomes a variant based on g_wave_num.
 *
 * Super flippers: from wave 6+ (g_wave_num >= 5) roughly 1/4 of flipper
 *   spawns are promoted to E_SUPER_FLIPPER.
 * Tanker kinds: from wave 7+ (g_wave_num >= 6) tanker spawns mix
 *   flipper-tanker + fuse-tanker; from wave 9+ all three kinds are in
 *   the rotation (flipper / pulsar / fuse). */
static void spawn_flipper_picked(u8 lane)
{
  if (g_wave_num >= 5 && (lcg() & 0x03) == 0) spawn_super_flipper(lane);
  else                                         spawn_flipper(lane);
}

static void spawn_tanker_picked(u8 lane)
{
  u8 kind;
  if (g_wave_num >= 8) {
    u8 r = (u8) (lcg() & 0x03);   /* 0..3; treat 3 as a re-roll to 0 */
    if (r == 3) r = 0;
    kind = r;                      /* 0=flipper, 1=pulsar, 2=fuse */
  } else if (g_wave_num >= 6) {
    kind = (lcg() & 0x01) ? TANKER_KIND_FUSE : TANKER_KIND_FLIPPER;
  } else {
    kind = TANKER_KIND_FLIPPER;
  }
  spawn_tanker_kind(lane, kind);
}

/* AI droid: spawned by PUP_DROID. Walks the rim toward the nearest
 * enemy + fires shots periodically. Lifetime tracked via a sibling u16
 * counter (g_droid_lifetime — Entity.lifetime is u8 and we want > 4 s). */
static u16 g_droid_lifetime;     /* > 0 = droid alive; frames remaining */

static void spawn_droid(u8 lane)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_DROID;
  e->lane         = lane;
  e->depth_fp     = FP_ONE;
  e->depth_vel_fp = 0;
  e->phase        = 0;
  e->step_period  = DROID_HOP_PERIOD;
  e->lifetime     = 0;
  g_droid_lifetime = DROID_LIFE_FRAMES;
}

/* Spawn a power-up that drifts outward to the rim. `kind` is PUP_LASER
 * or PUP_JUMP. Not an enemy — doesn't count against g_enemy_count. */
static void spawn_powerup(u8 lane, fp16 depth, u8 kind)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_POWERUP;
  e->lane         = lane;
  e->depth_fp     = depth;
  e->depth_vel_fp = +PUP_OUT_STEP;
  e->phase        = kind;        /* 0 = laser, 1 = jump */
  e->step_period  = 0;
  e->lifetime     = 0;
}

static void kill_enemy(Entity * e);    /* forward decl — defined below */

/* Fire the superzapper: kill every live enemy entity AND spawn a zapspark
 * marker at each kill site. Spikes (g_spike_depth) are terrain — left
 * alone. Also flashes the screen for a few frames. */
static void trigger_superzapper(void)
{
  g_superzapper_charges--;
  Entity * e = g_active_head;
  while (e) {
    Entity * next = e->next;
    if (e->type == E_FLIPPER || e->type == E_SUPER_FLIPPER ||
        e->type == E_TANKER  || e->type == E_PULSAR  ||
        e->type == E_FUSEBALL || e->type == E_SPIKER) {
      /* Spawn a zapspark at this enemy's lane+depth before killing. */
      Entity * s = entity_spawn();
      if (s) {
        s->type         = E_ZAPSPARK;
        s->lane         = e->lane;
        s->depth_fp     = e->depth_fp;
        s->depth_vel_fp = 0;
        s->phase        = 0;
        s->step_period  = 0;
        s->lifetime     = ZAP_SPARK_LIFE;
      }
      kill_enemy(e);
    }
    e = next;
  }
  g_zap_flash = ZAP_FLASH_FRAMES;
  /* ZAP = "Crackle" PCM (sfx 7 in Jag's samtab, file 08) — matches
   * the original superzapper sound (yak.s:9721). */
  if (g_mcd_present) mcd_play_sfx(3);
  else               sfx_hit();        /* PSG fallback — no zap synth */
}

/* Total enemies remaining in the spawn pool. */
static u16 pool_total(void)
{
  return (u16) (g_pool[0] + g_pool[1] + g_pool[2] + g_pool[3] + g_pool[4]);
}

/* Refill g_pool[] from WAVES[wave_idx] and set the wave's spawn period.
 * Per-wave shape rotation comes from g_wave_num % WEB_SHAPE_COUNT. */
static void load_wave_pool(u16 wave_idx)
{
  u8 t = (u8) (wave_idx & 0x0F);    /* WAVES table is 16 entries — wrap */
  g_pool[POOL_FLIPPER]  = WAVES[t].num_flippers;
  g_pool[POOL_TANKER]   = WAVES[t].num_tankers;
  g_pool[POOL_PULSAR]   = WAVES[t].num_pulsars;
  g_pool[POOL_FUSEBALL] = WAVES[t].num_fuseballs;
  g_pool[POOL_SPIKER]   = WAVES[t].num_spikers;
  g_spawn_timer = WAVES[t].spawn_period;
  g_superzapper_charges = SUPERZAPPER_PER_WAVE;
}

/* Swap to the gameplay MOD for the given wave (wave_num & 3 → WAVE_MUSIC).
 * No-op if the requested tune is already loaded — keeps the MOD position
 * intact when waves share a tune. */
static void switch_wave_music(u16 wave_num)
{
  if (!g_mcd_present) return;
  u8 idx = (u8) (wave_num & 0x3);
  if (idx == g_music_idx && g_music_playing) return;
  if (g_music_playing) {
    mcd_stop_mod();
    mcd_wait_ack(CMD_STOP_MOD);
  }
  mcd_upload_mod(WAVE_MUSIC[idx]);
  mcd_play_mod(WAVE_MUSIC[idx]->size);
  mcd_wait_ack(CMD_PLAY_MOD);
  g_music_playing = 1;
  g_music_idx     = idx;
}

/* Advance to the next wave: clear active enemies + spikes, swap web shape,
 * re-bake variants, refill pool. ~6-10 s freeze on the bake (no fly-down
 * transition yet — see project_megacd_port memory for future polish). */
static void next_wave(void)
{
  /* Kill all enemy entities (preserve player). */
  Entity * e = g_active_head;
  while (e) {
    Entity * next = e->next;
    if (e->type == E_FLIPPER || e->type == E_SUPER_FLIPPER ||
        e->type == E_TANKER  || e->type == E_PULSAR  ||
        e->type == E_FUSEBALL || e->type == E_SPIKER  ||
        e->type == E_DEBRIS) {
      entity_kill(e);
    }
    e = next;
  }
  g_enemy_count = 0;
  for (u8 k = 0; k < MAX_LANES; ++k) { g_spike_depth[k] = 0; g_spike_flash[k] = 0; }

  /* Bonus-life check before the wave counter ticks: completing wave N
   * (1-based) earns a life when N % BONUS_LIFE_EVERY == 0. The flag is
   * picked up by the wave splash. */
  u16 completed_wave = (u16) (g_wave_num + 1);
  if (completed_wave % BONUS_LIFE_EVERY == 0 && g_lives < 9) {
    g_lives++;
    g_bonus_life_pending = 1;
  }

  g_wave_num++;
  g_web_shape = (u8) (g_wave_num % WEB_SHAPE_COUNT);

  /* Re-bake the web for the new shape. Show LOADING + run the same
   * variant-bake pipeline install_playfield uses. */
  clear_play_area();
  print("LOADING...", plane_xy(15, 14));
  /* Wipe the sprite attribute table so leftover entities from the
   * previous frame don't linger on the LOADING screen during the bake. */
  vdp_ctrl_32 = to_vdp_addr(0xb800) | VRAM_W;
  vdp_data = 0; vdp_data = 0; vdp_data = 0; vdp_data = 0;
  web_init();
  g_player_lane = web_default_start_lane();
  web_player_snap_to(g_player_lane);
  if (g_player) g_player->lane = g_player_lane;
  g_vp_x = 0; g_vp_y = 0;
  if (g_mcd_present) {
    web_clear_plane_b();
    mcd_prebake_web_variants(4);
    web_project();
    mcd_dma_variant_to_vram(0);
    web_paint_plane_b();
  } else {
    web_render_main(4);
    web_dma_main_to_vram();
    web_paint_plane_b();
  }
  vdp_ctrl_32 = to_vdp_addr(1 * 2) | CRAM_W;
  vdp_data    = 0x0EEE;    /* reset cram[1] in case the bake pulsed it */

  load_wave_pool(g_wave_num);
  switch_wave_music(g_wave_num);   /* may or may not swap; no-op if same tune */
  g_wave_clear_timer = 0;
  g_zoom_out_frame   = 0;   /* web_clear_plane_b already reset VSRAM */
  g_splash_timer     = SPLASH_FRAMES;
  g_scene_dirty = 1;       /* repaint HUD next frame */
}

/* Shot hit a tanker — spawn 2 enemies on adjacent lanes at the tanker's
 * current depth and kill the tanker. What spawns depends on the tanker's
 * kind (stored in step_period): flippers / pulsars / fuseballs. */
static void split_tanker(Entity * t)
{
  u8   lane    = t->lane;
  fp16 depth   = t->depth_fp;
  u8   t_kind  = t->step_period;
  u8   l_lane  = web_lane_left(lane);
  u8   r_lane  = web_lane_right(lane);
  switch (t_kind) {
    case TANKER_KIND_PULSAR:
      spawn_pulsar_at(l_lane, depth);
      spawn_pulsar_at(r_lane, depth);
      break;
    case TANKER_KIND_FUSE:
      spawn_fuseball_at(l_lane, depth);
      spawn_fuseball_at(r_lane, depth);
      break;
    case TANKER_KIND_FLIPPER:
    default:
      spawn_flipper_at(l_lane, depth);
      spawn_flipper_at(r_lane, depth);
      break;
  }
  /* Drop a power-up at the tanker's lane. Kind cycles through all 6
   * types per tanker killed so the player sees each type over time. */
  u8 kind = g_pup_drop_count;
  while (kind >= PUP_KIND_COUNT) kind = (u8) (kind - PUP_KIND_COUNT);
  g_pup_drop_count = (u8) (g_pup_drop_count + 1);
  spawn_powerup(lane, depth, kind);
  g_enemy_count--;        /* the tanker itself */
  entity_kill(t);
}

static void kill_enemy(Entity * e)
{
  g_enemy_count--;
  entity_kill(e);
}

/* Spawn 8 debris particles at the current death point, one per direction.
 * Each particle reuses Entity fields: lane=dir, depth_fp=x-offset accum,
 * depth_vel_fp=y-offset accum, lifetime=frames to live. */
static void spawn_debris_burst(void)
{
  for (u8 i = 0; i < DEBRIS_DIRS; ++i) {
    Entity * e = entity_spawn();
    if (!e) return;     /* pool full — accept fewer particles, no big deal */
    e->type         = E_DEBRIS;
    e->lane         = i;          /* direction index */
    e->depth_fp     = 0;          /* x-offset accumulator */
    e->depth_vel_fp = 0;          /* y-offset accumulator */
    e->phase        = 0;
    e->step_period  = 0;
    e->lifetime     = DEBRIS_LIFETIME;
  }
}

static void play_always_vblank(void) { return; }
static void play_gated_vblank (void);

static void install_playfield(void)
{
  /* MC-T17: full game restored. The ASIC test rig (C/DOWN/UP) is still
   * present for visual verification — those buttons overwrite a slice
   * of the web with ASIC output. Next step is moving the web rendering
   * to the ASIC pipeline entirely. */
  g_engine.always_vblank = play_always_vblank;
  g_engine.gated_vblank  = play_gated_vblank;
  g_engine.main_thread   = play_main_thread;
  g_engine.paused        = 0;
  g_scene_dirty          = 1;

  /* Show LOADING immediately — the per-lane web pre-bake below takes
   * ~10 s and the title screen's text would otherwise stay frozen on
   * screen for that time. play_main_thread will clear+redraw on its
   * first tick after this function returns. The VBlank IRQ pulses
   * cram[1] while g_loading_pulse is set so the text breathes. */
  clear_play_area();
  print("LOADING...", plane_xy(15, 14));

  pool_init();
  g_vp_x = 0;
  g_vp_y = 0;
  /* Start at wave 0 — set shape BEFORE web_init so it reads the wave's
   * shape. Subsequent waves go through next_wave() which does its own
   * re-init. */
  g_wave_num  = 0;
  g_web_shape = (u8) (g_wave_num % WEB_SHAPE_COUNT);
  load_wave_pool(g_wave_num);
  web_init();
  /* Pick the bottom-centre lane as the spawn point and snap the claw
   * slide animation to it (web_init resets slide state to lane 0). */
  g_player_lane = web_default_start_lane();
  web_player_snap_to(g_player_lane);
  g_left_tick = g_right_tick = 0;
  g_fire_cooldown = 0;
  g_enemy_count = 0;
  g_score = 0;
  g_lives = LIVES_START;
  g_respawn_timer = 0;
  for (u8 k = 0; k < MAX_LANES; ++k) { g_spike_depth[k] = 0; g_spike_flash[k] = 0; }

  /* (Music continues to play through the LOADING bake — we only stop
   * the title theme right before swapping in rave4 at the end of this
   * function.) */

  if (g_mcd_present) {
    /* Clear plane B fully first (any stale tilemap entries from a
     * previous install would otherwise show web tiles at the wrong
     * cells once we DMA new tile data into the 0x280..0x40F range). */
    web_clear_plane_b();
    mcd_prebake_web_variants(4);
    g_vp_x = 0;
    g_vp_y = 0;
    web_project();
    mcd_dma_variant_to_vram(0);
    web_paint_plane_b();
  } else {
    /* No Mega CD: static software web. */
    web_render_main(4);
    web_dma_main_to_vram();
    web_paint_plane_b();
  }

  load_sprite_tiles_to_vram();

  /* Player entity. */
  g_player = entity_spawn();
  if (g_player) {
    g_player->type         = E_PLAYER;
    g_player->lane         = g_player_lane;
    g_player->depth_fp     = FP_ONE;
    g_player->depth_vel_fp = 0;
  }

  /* Reset cram[1] to bright white — the bake loop pulsed it. */
  vdp_ctrl_32 = to_vdp_addr(1 * 2) | CRAM_W;
  vdp_data    = 0x0EEE;

  /* Level is fully rendered — swap the title theme for the wave's tune. */
  switch_wave_music(g_wave_num);

  /* Arm the wave-start splash — freezes the gated_vblank for SPLASH_FRAMES
   * so the player gets a beat to read "WAVE 01 — GET READY". */
  g_splash_timer = SPLASH_FRAMES;
}

static void play_gated_vblank(void)
{
  /* Wave-start splash is gating everything below — count it down and bail.
   * Tick before the early return so the cycle still completes. */
  if (g_splash_timer) {
    g_splash_timer--;
    return;
  }

  g_anim_frame++;     /* drives flipper rotation (4 frames, 8 ticks each) */
  update_starfield(); /* drifts star dots through the cell once per ~2 s */

  /* Power-up effect timers. */
  if (g_laser_timer) g_laser_timer--;
  if (g_jump_timer)  g_jump_timer--;

  /* Superzapper screen flash — cram[0] (background) swaps to white for
   * ZAP_FLASH_FRAMES, restores to black when the counter hits zero. */
  if (g_zap_flash) {
    g_zap_flash--;
    vdp_ctrl_32 = to_vdp_addr(0) | CRAM_W;
    vdp_data    = g_zap_flash ? 0x0EEE : 0x0000;
  }

  /* Tick spike hit-flash counters. */
  for (u8 k = 0; k < MAX_LANES; ++k)
    if (g_spike_flash[k]) g_spike_flash[k]--;

  /* Input + fire gated by respawn timer — while dead, the player can't
   * move or shoot. Spawn loop and entity tick keep running so debris
   * animates and flippers keep approaching. */
  if (g_respawn_timer == 0) {
    // L/R lane hop with hold-to-repeat cooldown.
    if (p1_hold & PAD_LEFT) {
      if (g_left_tick == 0) {
        u8 new_lane = web_lane_left(g_player_lane);
        if (new_lane != g_player_lane) {
          g_player_lane = new_lane;
          web_lane_changed(g_player_lane, +1);   /* +1 = visual CCW = LEFT */
        }
        g_left_tick = (p1_single & PAD_LEFT) ? LANE_HOLD_INITIAL : LANE_HOLD_REPEAT;
      } else {
        g_left_tick--;
      }
    } else {
      g_left_tick = 0;
    }
    if (p1_hold & PAD_RIGHT) {
      if (g_right_tick == 0) {
        u8 new_lane = web_lane_right(g_player_lane);
        if (new_lane != g_player_lane) {
          g_player_lane = new_lane;
          web_lane_changed(g_player_lane, -1);   /* -1 = visual CW = RIGHT */
        }
        g_right_tick = (p1_single & PAD_RIGHT) ? LANE_HOLD_INITIAL : LANE_HOLD_REPEAT;
      } else {
        g_right_tick--;
      }
    } else {
      g_right_tick = 0;
    }
  }

  if (g_player) g_player->lane = g_player_lane;
  web_claw_tick(g_player_lane);

  // Fire on A (with cooldown). Also gated by respawn timer.
  if (g_respawn_timer == 0) {
    if (g_fire_cooldown) g_fire_cooldown--;
    if ((p1_single & PAD_A) && g_fire_cooldown == 0) {
      spawn_shot(g_player_lane);
      g_fire_cooldown = g_laser_timer ? (FIRE_COOLDOWN / 2) : FIRE_COOLDOWN;
    }
    /* Superzapper on B — instant kill of every live enemy. 1 charge per
     * wave. Spikes are terrain, not enemies; they survive. */
    if ((p1_single & PAD_B) && g_superzapper_charges > 0) {
      trigger_superzapper();
    }
  }

  // Spawn an enemy from the wave's pool. Per-wave spawn_period + max_active
  // are read from WAVES[g_wave_num & 0x0F]. Pick a random pool slot that
  // has remaining quota; if every slot is empty, no spawn (wave is winding
  // down — wave-clear check happens after the entity tick).
  u8 const wave_t = (u8) (g_wave_num & 0x0F);
  if (g_spawn_timer) g_spawn_timer--;
  if (g_spawn_timer == 0 &&
      g_enemy_count < WAVES[wave_t].max_active &&
      pool_total() > 0) {
    /* Pick a random lane in [0, lane_count). */
    u8 lane = (u8) (lcg() & 0x1F);
    u8 lc   = web_lane_count();
    while (lane >= lc) lane = (u8) (lane - lc);
    /* Pick a pool slot — random start, walk to first non-empty. */
    u8 start = (u8) (lcg() & 0x7);
    for (u8 i = 0; i < 5; ++i) {
      u8 slot = (u8) (start + i);
      while (slot >= 5) slot = (u8) (slot - 5);
      if (g_pool[slot] == 0) continue;
      g_pool[slot]--;
      switch (slot) {
        case POOL_FLIPPER:  spawn_flipper_picked(lane);  break;
        case POOL_TANKER:   spawn_tanker_picked(lane);   break;
        case POOL_PULSAR:   spawn_pulsar(lane);   break;
        case POOL_FUSEBALL: spawn_fuseball(lane); break;
        case POOL_SPIKER:   spawn_spiker(lane);   break;
      }
      break;
    }
    g_spawn_timer = WAVES[wave_t].spawn_period;
  }

  // Tick shots and flippers.
  Entity * e = g_active_head;
  while (e) {
    Entity * next = e->next;
    if (e->type == E_SHOT) {
      e->depth_fp += e->depth_vel_fp;
      if (e->depth_fp <= 0) entity_kill(e);
    } else if (e->type == E_FLIPPER || e->type == E_SUPER_FLIPPER) {
      if (e->phase == 0) {
        e->depth_fp += e->depth_vel_fp;
        if (e->depth_fp >= FP_ONE) {
          e->depth_fp     = FP_ONE;
          e->depth_vel_fp = 0;
          e->phase        = 1;
          e->lifetime     = e->step_period;
        }
      } else {
        if (e->lifetime) e->lifetime--;
        if (e->lifetime == 0) {
          if      (e->lane < g_player_lane) e->lane++;
          else if (e->lane > g_player_lane) e->lane--;
          e->lifetime = e->step_period;
        }
      }
    } else if (e->type == E_TANKER) {
      /* Tanker only descends — no rim-walk. On reaching the rim it sits
       * there (phase 1) and kills the player on contact. */
      if (e->phase == 0) {
        e->depth_fp += e->depth_vel_fp;
        if (e->depth_fp >= FP_ONE) {
          e->depth_fp     = FP_ONE;
          e->depth_vel_fp = 0;
          e->phase        = 1;
        }
      }
    } else if (e->type == E_PULSAR) {
      /* Same as tanker — descend, then sit at rim (phase 1). Kill check
       * is gated on the pulse animation peak so the player has a brief
       * safe window each cycle. */
      if (e->phase == 0) {
        e->depth_fp += e->depth_vel_fp;
        if (e->depth_fp >= FP_ONE) {
          e->depth_fp     = FP_ONE;
          e->depth_vel_fp = 0;
          e->phase        = 1;
        }
      }
    } else if (e->type == E_SPIKER) {
      /* Descend; paint the spike on this lane up to my current depth.
       * On reaching the rim, despawn — the spike persists. */
      e->depth_fp += e->depth_vel_fp;
      if (e->depth_fp > g_spike_depth[e->lane])
        g_spike_depth[e->lane] = e->depth_fp;
      if (e->depth_fp >= FP_ONE) kill_enemy(e);
    } else if (e->type == E_FUSEBALL) {
      /* Erratic — drifts in/out, hops to adjacent lane at random
       * intervals. lifetime ticks down between decision points. */
      if (e->phase == 0) {
        e->depth_fp += e->depth_vel_fp;
        if (e->depth_fp >= FP_ONE) {
          e->depth_fp     = FP_ONE;
          e->depth_vel_fp = 0;
          e->phase        = 1;       /* lock at rim */
        } else if (e->depth_fp < 0) {
          /* Bounced past centre — reverse direction. */
          e->depth_fp     = 0;
          e->depth_vel_fp = +FUSEBALL_STEP;
        } else {
          if (e->lifetime) e->lifetime--;
          if (e->lifetime == 0) {
            e->lifetime = FUSEBALL_HOP_PERIOD;
            u16 r = lcg();
            if (r & 0x0001) e->depth_vel_fp = -e->depth_vel_fp;
            if (r & 0x0002) {
              e->lane = (r & 0x0004) ? web_lane_left(e->lane)
                                     : web_lane_right(e->lane);
            }
          }
        }
      }
    } else if (e->type == E_DEBRIS) {
      /* Accumulate per-axis offset by direction's DX/DY each frame. */
      e->depth_fp     += DEBRIS_DX[e->lane];
      e->depth_vel_fp += DEBRIS_DY[e->lane];
      if (e->lifetime) e->lifetime--;
      if (e->lifetime == 0) entity_kill(e);
    } else if (e->type == E_ZAPSPARK) {
      /* Stationary; just count down to despawn. */
      if (e->lifetime) e->lifetime--;
      if (e->lifetime == 0) entity_kill(e);
    } else if (e->type == E_DROID) {
      /* Find the nearest enemy (highest depth_fp) and walk toward its
       * lane. Fire periodically. Despawn when g_droid_lifetime expires. */
      if (g_droid_lifetime == 0) { entity_kill(e); e = next; continue; }
      g_droid_lifetime--;
      u8   target_lane = e->lane;
      fp16 best_depth  = -1;
      for (Entity * en = g_active_head; en; en = en->next) {
        if (en->type == E_FLIPPER || en->type == E_SUPER_FLIPPER ||
            en->type == E_TANKER  || en->type == E_PULSAR  ||
            en->type == E_FUSEBALL || en->type == E_SPIKER) {
          if (en->depth_fp > best_depth) {
            best_depth  = en->depth_fp;
            target_lane = en->lane;
          }
        }
      }
      if (e->step_period) e->step_period--;
      if (e->step_period == 0) {
        if      (target_lane < e->lane) e->lane = web_lane_left(e->lane);
        else if (target_lane > e->lane) e->lane = web_lane_right(e->lane);
        e->step_period = DROID_HOP_PERIOD;
      }
      if ((g_anim_frame & DROID_FIRE_MASK) == 0) spawn_shot(e->lane);
    } else if (e->type == E_POWERUP) {
      /* Drift outward toward the rim. On reaching it, check if the
       * player is on the same lane → collect + activate. Otherwise
       * the power-up just expires. */
      e->depth_fp += e->depth_vel_fp;
      if (e->depth_fp >= FP_ONE) {
        if (e->lane == g_player_lane && g_respawn_timer == 0) {
          switch (e->phase) {
            case PUP_LASER: g_laser_timer = LASER_FRAMES;              break;
            case PUP_JUMP:  g_jump_timer  = JUMP_FRAMES;               break;
            case PUP_LIFE:  if (g_lives < 9) g_lives++;                break;
            case PUP_ZAP:   if (g_superzapper_charges < SZ_CAP)
                                g_superzapper_charges++;               break;
            case PUP_SKIP:  if (g_zoom_out_frame == 0)
                                g_zoom_out_frame = 1;                  break;
            case PUP_DROID: spawn_droid(g_player_lane);                break;
          }
          g_score += 5;
          if (g_mcd_present) mcd_play_sfx(1);    /* re-use HIT cue */
          else               sfx_hit();
        }
        entity_kill(e);
      }
    }
    e = next;
  }

  // Shot↔enemy collisions: same lane + close depth_fp.
  // Tanker hit → splits into 2 flippers (and scores double).
  Entity * s = g_active_head;
  while (s) {
    Entity * s_next = s->next;
    if (s->type == E_SHOT) {
      Entity * f = g_active_head;
      while (f) {
        Entity * f_next = f->next;
        if ((f->type == E_FLIPPER || f->type == E_SUPER_FLIPPER ||
             f->type == E_TANKER  || f->type == E_PULSAR  ||
             f->type == E_FUSEBALL || f->type == E_SPIKER)
            && f->lane == s->lane) {
          fp16 d = s->depth_fp - f->depth_fp;
          if (d < 0) d = -d;
          if (d <= HIT_DEPTH_TOL) {
            entity_kill(s);
            if      (f->type == E_TANKER)        { split_tanker(f); g_score += 2; }
            else if (f->type == E_PULSAR)        { kill_enemy(f);   g_score += 3; }
            else if (f->type == E_FUSEBALL)      { kill_enemy(f);   g_score += 4; }
            else if (f->type == E_SPIKER)        { kill_enemy(f);   g_score += 2; }
            else if (f->type == E_SUPER_FLIPPER) { kill_enemy(f);   g_score += 2; }
            else                                 { kill_enemy(f);   g_score++;    }
            if (g_mcd_present) mcd_play_sfx(1);    /* 1 = HIT — PCM */
            else               sfx_hit();           /* PSG fallback */
            break;
          }
        }
        f = f_next;
      }
    }
    s = s_next;
  }

  /* Shot↔spike trimming — separate pass because spikes aren't entities.
   * Each shot at a lane with a spike whose tip the shot has overtaken
   * trims the spike by SPIKE_CUT_AMOUNT and scores +1. */
  Entity * ss = g_active_head;
  while (ss) {
    Entity * ss_next = ss->next;
    if (ss->type == E_SHOT && g_spike_depth[ss->lane] > 0) {
      if (ss->depth_fp <= g_spike_depth[ss->lane]) {
        g_spike_depth[ss->lane] -= SPIKE_CUT_AMOUNT;
        if (g_spike_depth[ss->lane] < 0) g_spike_depth[ss->lane] = 0;
        g_spike_flash[ss->lane] = SPIKE_FLASH_FRAMES;     /* trigger blink */
        entity_kill(ss);
        g_score += 2;                                      /* Jag: +2 per cut */
        if (g_mcd_present) mcd_play_sfx(1);
        else               sfx_hit();
      }
    }
    ss = ss_next;
  }

  /* Spike-at-rim on player's lane → death. Handled before the enemy-rim
   * check so a spike alone is enough to kill (no enemy needed).
   * Jump power-up grants temporary invulnerability. */
  if (g_player && g_respawn_timer == 0 && g_jump_timer == 0 &&
      g_spike_depth[g_player_lane] >= SPIKE_KILL_THRESH) {
    g_death_x = web_pixel_x(g_player_lane, FP_ONE);
    g_death_y = web_pixel_y(g_player_lane, FP_ONE);
    spawn_debris_burst();
    g_respawn_timer = RESPAWN_DELAY;
#if !INFINITE_LIVES
    if (g_lives) g_lives--;
#endif
    /* Reset the spike so respawn isn't instant-death. */
    g_spike_depth[g_player_lane] = 0;
    if (g_mcd_present) mcd_play_sfx(2);
    else               sfx_death();
  }

  // Flipper-at-rim on player's lane → death (delayed respawn).
  // Guarded by g_respawn_timer so a second flipper on the same lane
  // can't re-trigger the death effect while the burst is playing.
  // Jump power-up grants temporary invulnerability.
  if (g_player && g_respawn_timer == 0 && g_jump_timer == 0) {
    Entity * f = g_active_head;
    while (f) {
      Entity * f_next = f->next;
      /* Pulsar only kills during its pulse peak — gives the player a
       * cyclic window to safely cross the lane. Peak is frame 2 of the
       * 4-step ping-pong animation (PULSAR_FRAME_MAP[2] = 2). */
      u8 const pulse_step = (u8) ((g_anim_frame >> 4) & 0x3);
      u8 const pulse_peak = (pulse_step == 2);
      if (((f->type == E_FLIPPER || f->type == E_SUPER_FLIPPER ||
            f->type == E_TANKER  || f->type == E_FUSEBALL) ||
           (f->type == E_PULSAR && pulse_peak)) &&
          f->phase == 1 && f->lane == g_player_lane) {
        /* Snapshot claw's outer-rim screen position — the burst origin. */
        g_death_x = web_pixel_x(g_player_lane, FP_ONE);
        g_death_y = web_pixel_y(g_player_lane, FP_ONE);
        spawn_debris_burst();
        kill_enemy(f);
        g_respawn_timer = RESPAWN_DELAY;       /* freeze + hide player */
#if !INFINITE_LIVES
        if (g_lives) g_lives--;
#endif
        if (g_mcd_present) mcd_play_sfx(2);    /* 2 = DEATH — PCM */
        else               sfx_death();         /* PSG fallback */
        break;
      }
      f = f_next;
    }
  }

  /* Wave-clear → 1 s pause → zoom-out → bake → next wave. */
  if (g_respawn_timer == 0 && pool_total() == 0 && g_enemy_count == 0 &&
      g_zoom_out_frame == 0) {
    g_wave_clear_timer++;
    if (g_wave_clear_timer >= WAVE_CLEAR_DELAY) {
      g_wave_clear_timer = 0;
      g_zoom_out_frame   = 1;            /* begin fly-down-tube */
    }
  } else if (g_zoom_out_frame == 0) {
    g_wave_clear_timer = 0;
  }

  /* Zoom-out via ASIC. Frame 1 sets up (bakes current web into stamps);
   * every frame thereafter sends a render-scale command with a growing
   * dx so the web visibly shrinks toward source centre. After
   * ZOOM_OUT_FRAMES, hand off to next_wave for the bake. */
  if (g_zoom_out_frame > 0 && g_mcd_present) {
    if (g_zoom_out_frame == 1) {
      /* One-time setup: bake current web shape into ASIC stamps + map.
       * Also wipe the sprite attribute table so the claw + enemies
       * vanish for the duration of the transition — only the web
       * should be visible while it zooms. */
      mcd_asic_load_web_stamps(4);
      vdp_ctrl_32 = to_vdp_addr(0xb800) | VRAM_W;
      vdp_data = 0; vdp_data = 0; vdp_data = 0; vdp_data = 0;
    }
    /* dx ramps from 0x0800 (identity) up by 0x200 per frame so the web
     * shrinks toward centre over the transition. */
    s16 dx = (s16) (0x0800 + (g_zoom_out_frame - 1) * 0x200);
    mcd_render_asic_scale(0x4000, 0x280, 10, 4, dx);
    g_zoom_out_frame++;
    if (g_zoom_out_frame > ZOOM_OUT_FRAMES) {
      next_wave();
      return;
    }
  } else if (g_zoom_out_frame > 0) {
    /* No Mega CD: just skip the zoom and bake the next wave. */
    next_wave();
    return;
  }

  /* Respawn timer countdown — on hitting zero, either bring the player
   * back to life at the default lane OR transition to game-over if all
   * lives are spent. */
  if (g_respawn_timer) {
    g_respawn_timer--;
    if (g_respawn_timer == 0) {
      if (g_lives == 0) {
        /* Hi-score qualifying run → initials entry first; otherwise
         * straight to GAME OVER. */
        if (hiscore_qualifies((u32) g_score)) install_hiscore_entry();
        else                                   install_gameover();
        return;
      }
      u8 start_lane = web_default_start_lane();
      g_player_lane = start_lane;
      if (g_player) g_player->lane = start_lane;
      web_player_snap_to(start_lane);
    }
  }
}

// ---- Scene main_thread bodies ---------------------------------------------

/* Format helpers for the hi-score table display. */
static void title_paint_hiscore_row(u8 row, u8 rank, const HiScoreEntry * e)
{
  /* "  N. ABC   NNNNNN  LV NN"  (rank, initials, 6-digit score, level) */
  char buf[25];
  for (u8 i = 0; i < 24; i++) buf[i] = ' ';
  buf[24] = 0;

  /* rank — 2 chars right-aligned, '.' after */
  u8 r = (u8) (rank + 1);
  if (r >= 10) { buf[0] = (char) ('0' + r / 10); buf[1] = (char) ('0' + r % 10); }
  else          { buf[1] = (char) ('0' + r); }
  buf[2] = '.';

  buf[4] = (char) e->initials[0];
  buf[5] = (char) e->initials[1];
  buf[6] = (char) e->initials[2];

  /* 6-digit score, leading zeros as spaces. Avoid u32 division (the
   * cart doesn't link libgcc __udivsi3); use a place-value subtraction
   * loop per digit. Powers of 10 up to 100000 fit in 32 bits. */
  static const u32 PLACE[6] = { 100000ul, 10000ul, 1000ul, 100ul, 10ul, 1ul };
  u32 s = e->score;
  if (s > 999999ul) s = 999999ul;
  u8 started = 0;
  for (u8 d = 0; d < 6; d++) {
    u32 p = PLACE[d];
    u8  digit = 0;
    while (s >= p) { s -= p; digit++; }
    if (digit == 0 && !started && d < 5) buf[10 + d] = ' ';
    else                                  { buf[10 + d] = (char) ('0' + digit); started = 1; }
  }

  buf[18] = 'L';
  buf[19] = 'V';
  buf[21] = (char) ('0' + (e->level / 10));
  buf[22] = (char) ('0' + (e->level % 10));

  print(buf, plane_xy(8, row));
}

static void title_paint_hiscores(void)
{
  /* Static title text at top — replaces the ASIC banner during this
   * attract state. */
  print("MEGA TEMPEST", plane_xy(14, 3));
  print("HALL OF FAME", plane_xy(14, 5));
  print("RK NAME    SCORE   LEVEL", plane_xy(8, 7));
  for (u8 i = 0; i < HISCORE_COUNT; i++) {
    title_paint_hiscore_row((u8) (9 + i), i, &g_hiscores.entries[i]);
  }
}

static void title_clear_hiscores(void)
{
  /* Erase the static text + table rows so the BANNER state shows
   * cleanly on its rendered area. */
  print("            ", plane_xy(14, 3));
  print("            ", plane_xy(14, 5));
  print("                        ", plane_xy(8, 7));
  for (u8 i = 0; i < HISCORE_COUNT; i++) {
    print("                        ", plane_xy(8, (u8) (9 + i)));
  }
}

static void title_main_thread(void)
{
  if (g_scene_dirty) {
    clear_play_area();
    /* Bottom-row tags persist across attract states. */
    print("v0.2 BETA", plane_xy(2, 27));
    print("MCD:",         plane_xy(28, 27));
    print(g_mcd_present ? "PRESENT" : "ABSENT ", plane_xy(33, 27));
    g_scene_dirty = 0;
    g_attract_prev_state = 0xFF;       /* force a paint on first frame */
  }

  /* Attract-state transitions: paint when entering a state, wipe
   * when leaving. */
  if (g_attract_state != g_attract_prev_state) {
    if (g_attract_state == 1) {
      /* BANNER → HISCORE: clear the ASIC-painted plane B + flashing
       * START prompt, then paint the table on plane A. */
      web_clear_plane_b();
      print("            ", plane_xy(13, 16));
      title_paint_hiscores();
    } else {
      /* HISCORE → BANNER: wipe the table; ASIC re-kick from
       * title_gated_vblank repaints plane B starting next frame. */
      title_clear_hiscores();
    }
    g_attract_prev_state = g_attract_state;
  }

  /* BANNER state: flash START = PLAY ~0.5 s on / 0.5 s off. */
  if (g_attract_state == 0) {
    static u8 prev_flash = 0xFF;
    u8 cur_flash = (u8) ((g_anim_frame >> 5) & 1);
    if (cur_flash != prev_flash) {
      if (cur_flash) print("START = PLAY", plane_xy(13, 16));
      else           print("            ", plane_xy(13, 16));
      prev_flash = cur_flash;
    }
  }

  if (p1_single & PAD_START) install_playfield();
}

static void play_main_thread(void)
{
  if (g_scene_dirty) {
    clear_play_area();
    print("SCORE:", plane_xy(28, 27));
    print("LIVES:", plane_xy( 2, 27));
    print("SZ:",    plane_xy(11, 27));     /* +1 col: spacer after LIVES digit */
    print("WAVE:",  plane_xy(16, 27));     /* +1 col: spacer after SZ digit    */
    g_scene_dirty = 0;
  }

  /* Wave-start splash. Painted once on rising edge of g_splash_timer,
   * wiped once on falling edge — no need to repaint every frame. */
  {
    static u8 prev_splash = 0;
    u8 cur_splash = (g_splash_timer > 0) ? 1 : 0;
    if (cur_splash && !prev_splash) {
      print("WAVE",      plane_xy(11, 14));
      u16 w = (u16) (g_wave_num + 1);
      plane_putc(17, 14, (char) ('0' + (w % 10)));
      plane_putc(16, 14, (char) ('0' + (w / 10)));
      print("GET READY", plane_xy(20, 14));
      if (g_bonus_life_pending) print("1UP!", plane_xy(18, 16));
    }
    if (!cur_splash && prev_splash) {
      print("                  ", plane_xy(11, 14));   /* WAVE line */
      print("      ",             plane_xy(18, 16));   /* 1UP line  */
      g_bonus_life_pending = 0;
    }
    prev_splash = cur_splash;
  }

  /* START toggles pause. g_engine.paused gates gated_vblank in the main
   * loop, so toggling it freezes input + entity ticks + spawn timers,
   * but main_thread still runs (we want to keep reading START for the
   * unpause edge). Music continues — it's on the Sub CPU and stopping
   * the MOD would lose its position. */
  if (p1_single & PAD_START) {
    g_engine.paused = !g_engine.paused;
    if (g_engine.paused) print("PAUSE", plane_xy(17, 14));
    else                 print("     ", plane_xy(17, 14));   /* wipe overlay */
  }

  // Score readout — 4 digits, no 32-bit divide needed.
  u16 s = g_score;
  plane_putc(37, 27, (char) ('0' + (s % 10))); s /= 10;
  plane_putc(36, 27, (char) ('0' + (s % 10))); s /= 10;
  plane_putc(35, 27, (char) ('0' + (s % 10))); s /= 10;
  plane_putc(34, 27, (char) ('0' + (s % 10)));

  // Lives readout — single digit (capped to 9 for safety).
  plane_putc(9, 27, (char) ('0' + (g_lives > 9 ? 9 : g_lives)));

  // Superzapper charge — single digit.
  plane_putc(14, 27, (char) ('0' + g_superzapper_charges));

  /* Active power-up indicator at col 24-25 (between WAVE digits and SCORE). */
  plane_putc(24, 27, g_laser_timer ? 'L' : ' ');
  plane_putc(25, 27, g_jump_timer  ? 'J' : ' ');

  // Wave readout — 2 digits, 1-based display.
  u16 w = (u16) (g_wave_num + 1);
  plane_putc(23, 27, (char) ('0' + (w % 10))); w /= 10;
  plane_putc(22, 27, (char) ('0' + (w % 10)));


  /* Pick pre-baked variant matching player's target lane (after slide).
   * Set g_vp to that variant's camera, project rim mids so sprites match,
   * DMA variant tile data to plane B. ~7ms DMA fits comfortably in 60Hz.
   * Skipped during the ASIC zoom-out — that pipeline owns plane B's
   * tile content (col-major) and we'd otherwise overwrite it with the
   * variant's row-major data, producing a 90° transposed leftover. */
  if (g_mcd_present && g_zoom_out_frame == 0) {
    /* DIAG isolated the bug to per-lane variant data (variant 0 is
     * clean; one specific variant K has bad pixels). Re-enabled so we
     * can identify K via the LANE readout below — navigate to the
     * glitch position and report the printed lane number. */
    u8 variant_k = g_player_lane;
    /* Reconstruct variant K's vp from lane K's outer-rim world offset
     * (must match mcd_prebake_web_variants' calculation). */
    g_vp_x = 0;
    g_vp_y = 0;
    web_project();
    s16 lane_off_x = (s16) (web_pixel_x(variant_k, FP_ONE) - 160);
    s16 lane_off_y = (s16) (web_pixel_y(variant_k, FP_ONE) - 112);
    g_vp_x = (s16) (lane_off_x >> 2);
    g_vp_y = (s16) (lane_off_y >> 2);
    web_project();
    mcd_dma_variant_to_vram(variant_k);
  } else {
    g_vp_x = 0;
    g_vp_y = 0;
  }

  /* Skip sprites during the ASIC zoom — sprite table was cleared at
   * zoom start; re-rendering would re-show the (frozen) claw. */
  if (g_zoom_out_frame == 0) render_sprites();

}

// ---- Scene: HI-SCORE ENTRY (post-game-over, if qualifying) ----------------
//
// After death + lives == 0, if g_score qualifies for the table, this scene
// runs BEFORE the GAME OVER scene. Three-letter initials entry:
//   D-pad ↑↓ : cycle current letter A..Z (wraps)
//   D-pad ←→ : move to previous / next letter slot (wraps 0..2)
//   A        : advance one slot; if at last slot, save + jump to GAME OVER
//   START    : same as A on the last slot (one-press save shortcut)

static u8  g_hiscore_initials[HISCORE_INITIALS_LEN];
static u8  g_hiscore_slot;          /* current slot 0..2 */

static void hiscore_entry_main_thread(void);

static void install_hiscore_entry(void)
{
  g_engine.always_vblank = 0;
  g_engine.gated_vblank  = 0;
  g_engine.main_thread   = hiscore_entry_main_thread;
  g_engine.paused        = 0;
  g_scene_dirty          = 1;

  /* Stop music — the gameover scene normally does this; we do it now
   * since we run before it. */
  if (g_mcd_present && g_music_playing) {
    mcd_stop_mod();
    mcd_wait_ack(CMD_STOP_MOD);
    g_music_playing = 0;
  }

  /* Wipe leftover sprites + reset initials buffer. */
  vdp_ctrl_32 = to_vdp_addr(0xb800) | VRAM_W;
  vdp_data = 0; vdp_data = 0; vdp_data = 0; vdp_data = 0;
  for (u8 i = 0; i < HISCORE_INITIALS_LEN; i++) g_hiscore_initials[i] = (u8) 'A';
  g_hiscore_slot = 0;
}

static void hiscore_entry_repaint_letters(void)
{
  /* Three letters, centred at col 18. Underline (`_`) under the
   * currently-selected slot. */
  for (u8 i = 0; i < HISCORE_INITIALS_LEN; i++) {
    plane_putc((u8) (18 + i * 2), 14, (char) g_hiscore_initials[i]);
    plane_putc((u8) (18 + i * 2), 15, (i == g_hiscore_slot) ? '_' : ' ');
  }
}

static void hiscore_entry_main_thread(void)
{
  if (g_scene_dirty) {
    clear_play_area();
    print("NEW HIGH SCORE",     plane_xy(13, 9));
    print("ENTER YOUR INITIALS", plane_xy(11, 11));
    print("U D - L R - A",      plane_xy(14, 19));
    print("CYCLE  MOVE  SAVE",  plane_xy(12, 20));
    hiscore_entry_repaint_letters();
    g_scene_dirty = 0;
  }

  /* Cycle current slot's letter. Letters are 'A'..'Z' (26 values). */
  if (p1_single & PAD_UP) {
    g_hiscore_initials[g_hiscore_slot] = (u8) (g_hiscore_initials[g_hiscore_slot] + 1);
    if (g_hiscore_initials[g_hiscore_slot] > (u8) 'Z') g_hiscore_initials[g_hiscore_slot] = (u8) 'A';
    hiscore_entry_repaint_letters();
  }
  if (p1_single & PAD_DOWN) {
    g_hiscore_initials[g_hiscore_slot] = (u8) (g_hiscore_initials[g_hiscore_slot] - 1);
    if (g_hiscore_initials[g_hiscore_slot] < (u8) 'A') g_hiscore_initials[g_hiscore_slot] = (u8) 'Z';
    hiscore_entry_repaint_letters();
  }

  /* Move slot. Avoid 32-bit %; cart doesn't link libgcc __modsi3. */
  if (p1_single & PAD_LEFT) {
    g_hiscore_slot = g_hiscore_slot ? (u8) (g_hiscore_slot - 1)
                                    : (u8) (HISCORE_INITIALS_LEN - 1);
    hiscore_entry_repaint_letters();
  }
  if (p1_single & PAD_RIGHT) {
    g_hiscore_slot = (g_hiscore_slot >= HISCORE_INITIALS_LEN - 1)
                       ? (u8) 0 : (u8) (g_hiscore_slot + 1);
    hiscore_entry_repaint_letters();
  }

  /* A = advance slot; on the final slot, save + go to GAME OVER. */
  if (p1_single & PAD_A) {
    if (g_hiscore_slot < HISCORE_INITIALS_LEN - 1) {
      g_hiscore_slot++;
      hiscore_entry_repaint_letters();
    } else {
      hiscore_insert((u32) g_score, g_hiscore_initials, (u8) (g_wave_num + 1));
      install_gameover();
    }
  }
  /* START = save immediately, regardless of slot. */
  if (p1_single & PAD_START) {
    hiscore_insert((u32) g_score, g_hiscore_initials, (u8) (g_wave_num + 1));
    install_gameover();
  }
}

// ---- Scene: GAME OVER -----------------------------------------------------

static void install_gameover(void)
{
  g_engine.always_vblank = 0;
  g_engine.gated_vblank  = 0;
  g_engine.main_thread   = gameover_main_thread;
  g_engine.paused        = 0;
  g_scene_dirty          = 1;

  /* Stop music. */
  if (g_mcd_present && g_music_playing) {
    mcd_stop_mod();
    mcd_wait_ack(CMD_STOP_MOD);
    g_music_playing = 0;
  }

  /* Clear sprite attribute table so leftover entities from the play
   * scene don't linger on screen. */
  vdp_ctrl_32 = to_vdp_addr(0xb800) | VRAM_W;
  vdp_data = 0; vdp_data = 0; vdp_data = 0; vdp_data = 0;
}

static void gameover_main_thread(void)
{
  if (g_scene_dirty) {
    clear_play_area();
    print("GAME OVER",   plane_xy(15, 12));
    print("SCORE",       plane_xy(15, 15));
    print("PRESS START", plane_xy(14, 19));
    g_scene_dirty = 0;
  }

  /* Score digits sit right after "SCORE " on the same row. */
  u16 s = g_score;
  plane_putc(24, 15, (char) ('0' + (s % 10))); s /= 10;
  plane_putc(23, 15, (char) ('0' + (s % 10))); s /= 10;
  plane_putc(22, 15, (char) ('0' + (s % 10))); s /= 10;
  plane_putc(21, 15, (char) ('0' + (s % 10)));

  if (p1_single & PAD_START) install_title();
}

// ---- main -----------------------------------------------------------------

void main(void)
{
  disable_interrupts();
  vblank_done = false;
  p1_hold = p1_single = p1_prev = 0;
  g_engine.frame = 0;
  memcpy16((u16 *) default_vdp_regs, vdp_regs, 18);

  u16 mode2_display_off = VDP_REG_MODE2 | VDP_MD_DISPLAY_MODE | VDP_VBLANK_ENABLE | VIDEO_SIGNAL;
  u16 mode2_dma_enable  = mode2_display_off | VDP_DMA_ENABLE;
  for (u8 i = 0; i < 18; ++i)
    vdp_ctrl = (i == 1) ? mode2_display_off : vdp_regs[i];
  clear_vram();
  clear_vsram();

  /* Palette: black bg, white text/UI, red enemies, yellow web. */
  for (u8 i = 0; i < 64; ++i) cram[i] = 0;
  cram[0]  = 0x0000;          // 0 transparent / black
  cram[1]  = 0x0EEE;          // 1 white   — text, UI, player
  cram[2]  = 0x000E;          // 2 red     — flipper sprites
  cram[3]  = 0x0E0E;          // 3 magenta — tanker ("Pink Thang" per obj2d.s)
  cram[4]  = 0x0E80;          // 4 electric blue — web outline (matches Jag T2K)
  /* slot 9 set below (after blue-gradient block to keep slots grouped) */
  /* Web lane fill gradient: 4 bands from deep (inner, far) to bright
   * (outer rim, near). All in the blue/purple family for that T2K vibe. */
  /* 8-step blue gradient — every valid xBGR intensity from 0x2 to 0xE.
   * Inner web fill bands use 5,6,7,8; outer bands use 12,13,14,15.
   * (See WEB_FILL_BANDS in web.c.) */
  cram[5]  = 0x0200;
  cram[6]  = 0x0400;
  cram[7]  = 0x0600;
  cram[8]  = 0x0800;
  cram[9]  = 0x0EE0;          // 9 cyan    — pulsar sprite
  cram[10] = 0x00E0;          // 10 green  — fuseball sprite
  cram[11] = 0x00EE;          // 11 yellow — player claw (moved off slot 4)
  cram[12] = 0x0A00;          // 12 web fill band 4
  cram[13] = 0x0C00;          // 13 web fill band 5
  cram[14] = 0x0E00;          // 14 web fill band 6 — bright blue
  cram[15] = 0x0E00;          // 15 web fill band 7 — pure blue, outline (0x0E80) pops above it

  /* Per-sprite palette variants for enemy sub-types (v0.2). Same tile
   * bytes render in different colours when the sprite-attribute palette
   * select (bits 13-14 of word 2) picks palette 1 or 2 instead of 0.
   *
   * Palette 1 (CRAM 16..31): cyan at slot 3 (pulsar-tanker, tile uses
   *   palette index 3 = same as flipper-tanker) and white at slot 2
   *   (super-flipper, flipper tile uses palette index 2 = red in pal 0).
   * Palette 2 (CRAM 32..47): green at slot 3 (fuse-tanker).
   *
   * Other slots in palettes 1/2 stay zero (transparent) — only the
   * sub-types' body colours need to be set. */
  cram[16 + 2] = 0x0EEE;       /* pal 1 slot 2 = white  (super flipper) */
  cram[16 + 3] = 0x0EE0;       /* pal 1 slot 3 = cyan   (pulsar tanker) */
  cram[32 + 3] = 0x00E0;       /* pal 2 slot 3 = green  (fuse  tanker)  */
  update_cram();

  init_joypads();

  // Upload font into VRAM tile area starting at glyph 0x20 (' ').
  vdp_ctrl = mode2_dma_enable;
  vdp_dma_transfer(res_font.data, to_vdp_addr(tile_offset(0x20)),
                   (u16) (res_font.size / 2));
  vdp_ctrl = mode2_display_off;

  /* Star tile data — 4 variants with the bright pixel in different cell
   * positions so a scatter on plane A looks like an actual starfield
   * rather than a regular grid. All use palette index 1 (white). */
  static const char STAR_TILES[4 * 32] = {
    /* tile 0x10 — dot near top-left */
    0x00,0x00,0x00,0x00,  0x00,0x10,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,
    /* tile 0x11 — dot near centre-right */
    0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x10,
    0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,
    /* tile 0x12 — dot near bottom */
    0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x10,0x00,0x00,  0x00,0x00,0x00,0x00,
    /* tile 0x13 — dot near centre */
    0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x10,0x00,
    0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,  0x00,0x00,0x00,0x00,
  };
  vdp_ctrl = mode2_dma_enable;
  vdp_dma_transfer(STAR_TILES, to_vdp_addr(tile_offset(0x10)),
                   (u16) ((sizeof STAR_TILES) / 2));
  vdp_ctrl = mode2_display_off;

  /* Star scatter on plane A is now painted per-scene by clear_play_area
   * — full-screen scatter with the web + sprites drawing in front via
   * priority=1. No boot-time paint needed. */

  g_mcd_present = detect_mega_cd();
  if (g_mcd_present) mcd_init();
  g_music_playing = 0;

  /* Load default hi-score table into RAM. BRAM persistence is parked
   * for v0.3 (Mode 1 cart can't reach BIOS BURAM safely) — table is
   * session-only for v0.2. */
  hiscore_init();

  psg_init();
  install_title();

  vdp_ctrl = vdp_regs[1];        // display on
  enable_interrupts();

  while (1) {
    while (!vblank_done) asm("nop");
    vblank_done = false;
    g_engine.frame++;

    psg_tick();
    if (g_engine.always_vblank) g_engine.always_vblank();
    if (!g_engine.paused && g_engine.gated_vblank) g_engine.gated_vblank();
    if (g_engine.main_thread)   g_engine.main_thread();
  }
}
