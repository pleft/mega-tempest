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
  /* Sparse star scatter at 1/4 density across the whole plane. */
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

static u8  g_mcd_present;
static u8  g_music_playing;
static u8  g_scene_dirty;     // set by install_*; main loop redraws static text

// Forward decls — the scenes call each other across file order.
static void install_title(void);
static void title_main_thread(void);
static void play_main_thread(void);
static void install_gameover(void);
static void gameover_main_thread(void);

// ---- Scene: TITLE ---------------------------------------------------------

static void title_always_vblank(void) { return; }
static void title_gated_vblank (void) { return; }

static void install_title(void)
{
  g_engine.always_vblank = title_always_vblank;
  g_engine.gated_vblank  = title_gated_vblank;
  g_engine.main_thread   = title_main_thread;
  g_engine.paused        = 0;
  g_scene_dirty          = 1;

  if (g_mcd_present && g_music_playing) {
    mcd_stop_mod();
    mcd_wait_ack(CMD_STOP_MOD);
    g_music_playing = 0;
  }
  web_clear_plane_b();
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

static void spawn_tanker(u8 lane)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_TANKER;
  e->lane         = lane;
  e->depth_fp     = 0;
  e->depth_vel_fp = +TANKER_OUT_STEP;
  e->phase        = 0;
  e->step_period  = 0;
  e->lifetime     = 0;
  g_enemy_count++;
}

static void spawn_pulsar(u8 lane)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_PULSAR;
  e->lane         = lane;
  e->depth_fp     = 0;
  e->depth_vel_fp = +PULSAR_OUT_STEP;
  e->phase        = 0;
  e->step_period  = 0;
  e->lifetime     = 0;
  g_enemy_count++;
}

static void spawn_fuseball(u8 lane)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_FUSEBALL;
  e->lane         = lane;
  e->depth_fp     = 0;
  e->depth_vel_fp = +FUSEBALL_STEP;
  e->phase        = 0;
  e->step_period  = 0;
  e->lifetime     = FUSEBALL_HOP_PERIOD;
  g_enemy_count++;
}

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
    if (e->type == E_FLIPPER || e->type == E_TANKER ||
        e->type == E_PULSAR  || e->type == E_FUSEBALL ||
        e->type == E_SPIKER) {
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
  /* HIT sample = "enemy killed" — closest match to the zap killing many. */
  if (g_mcd_present) mcd_play_sfx(1);
  else               sfx_hit();
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

/* Advance to the next wave: clear active enemies + spikes, swap web shape,
 * re-bake variants, refill pool. ~6-10 s freeze on the bake (no fly-down
 * transition yet — see project_megacd_port memory for future polish). */
static void next_wave(void)
{
  /* Kill all enemy entities (preserve player). */
  Entity * e = g_active_head;
  while (e) {
    Entity * next = e->next;
    if (e->type == E_FLIPPER || e->type == E_TANKER ||
        e->type == E_PULSAR  || e->type == E_FUSEBALL ||
        e->type == E_SPIKER  || e->type == E_DEBRIS) {
      entity_kill(e);
    }
    e = next;
  }
  g_enemy_count = 0;
  for (u8 k = 0; k < MAX_LANES; ++k) { g_spike_depth[k] = 0; g_spike_flash[k] = 0; }

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
  g_wave_clear_timer = 0;
  g_zoom_out_frame   = 0;   /* web_clear_plane_b already reset VSRAM */
  g_scene_dirty = 1;       /* repaint HUD next frame */
}

/* Shot hit a tanker — spawn 2 flippers on adjacent lanes at the tanker's
 * current depth and kill the tanker. */
static void split_tanker(Entity * t)
{
  u8 lane = t->lane;
  spawn_flipper_at(web_lane_left(lane),  t->depth_fp);
  spawn_flipper_at(web_lane_right(lane), t->depth_fp);
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

  /* Stop any leftover music NOW so the LOADING screen is silent.
   * The new MOD is uploaded + started AFTER all rendering finishes
   * (further down) so music starts at the same moment the player sees
   * the game ready. */
  if (g_mcd_present && g_music_playing) {
    mcd_stop_mod();
    mcd_wait_ack(CMD_STOP_MOD);
    g_music_playing = 0;
  }

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

  /* Level is fully rendered — kick off music now (Mega CD only;
   * gracefully silent on plain MD). */
  if (g_mcd_present) {
    mcd_upload_mod(&res_rave4_mod);
    mcd_play_mod(res_rave4_mod.size);
    mcd_wait_ack(CMD_PLAY_MOD);
    g_music_playing = 1;
  }
}

static void play_gated_vblank(void)
{
  g_anim_frame++;     /* drives flipper rotation (4 frames, 8 ticks each) */
  update_starfield(); /* drifts star dots through the cell once per ~2 s */

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
      g_fire_cooldown = FIRE_COOLDOWN;
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
        case POOL_FLIPPER:  spawn_flipper(lane);  break;
        case POOL_TANKER:   spawn_tanker(lane);   break;
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
    } else if (e->type == E_FLIPPER) {
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
        if ((f->type == E_FLIPPER || f->type == E_TANKER ||
             f->type == E_PULSAR  || f->type == E_FUSEBALL ||
             f->type == E_SPIKER)
            && f->lane == s->lane) {
          fp16 d = s->depth_fp - f->depth_fp;
          if (d < 0) d = -d;
          if (d <= HIT_DEPTH_TOL) {
            entity_kill(s);
            if      (f->type == E_TANKER)   { split_tanker(f); g_score += 2; }
            else if (f->type == E_PULSAR)   { kill_enemy(f);   g_score += 3; }
            else if (f->type == E_FUSEBALL) { kill_enemy(f);   g_score += 4; }
            else if (f->type == E_SPIKER)   { kill_enemy(f);   g_score += 2; }
            else                            { kill_enemy(f);   g_score++;    }
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
   * check so a spike alone is enough to kill (no enemy needed). */
  if (g_player && g_respawn_timer == 0 &&
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
  if (g_player && g_respawn_timer == 0) {
    Entity * f = g_active_head;
    while (f) {
      Entity * f_next = f->next;
      /* Pulsar only kills during its pulse peak — gives the player a
       * cyclic window to safely cross the lane. Peak is frame 2 of the
       * 4-step ping-pong animation (PULSAR_FRAME_MAP[2] = 2). */
      u8 const pulse_step = (u8) ((g_anim_frame >> 4) & 0x3);
      u8 const pulse_peak = (pulse_step == 2);
      if (((f->type == E_FLIPPER || f->type == E_TANKER || f->type == E_FUSEBALL) ||
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
        install_gameover();
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

static void title_main_thread(void)
{
  if (g_scene_dirty) {
    clear_play_area();
    print("TEMPEST 2000",       plane_xy(14, 6));
    print("MEGA CD PORT",       plane_xy(14, 8));
    print("MCD:",               plane_xy(2, 12));
    print(g_mcd_present ? "PRESENT" : "ABSENT ", plane_xy(7, 12));
    print("START = PLAY",       plane_xy(13, 16));
    print("    C = NEXT SHAPE", plane_xy(13, 17));
    print("WEB:",               plane_xy(2, 20));
    g_scene_dirty = 0;
  }
  print(WEB_SHAPE_NAMES[g_web_shape], plane_xy(7, 20));

  if (p1_single & PAD_START) install_playfield();
  if (p1_single & PAD_C) {
    g_web_shape = (u8) ((g_web_shape + 1) % WEB_SHAPE_COUNT);
  }
}

static void play_main_thread(void)
{
  if (g_scene_dirty) {
    clear_play_area();
    print("SCORE:", plane_xy(28, 27));
    print("LIVES:", plane_xy( 2, 27));
    print("SZ:",    plane_xy(10, 27));
    print("WAVE:",  plane_xy(15, 27));
    g_scene_dirty = 0;
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
  plane_putc(13, 27, (char) ('0' + g_superzapper_charges));

  // Wave readout — 2 digits, 1-based display.
  u16 w = (u16) (g_wave_num + 1);
  plane_putc(22, 27, (char) ('0' + (w % 10))); w /= 10;
  plane_putc(21, 27, (char) ('0' + (w % 10)));


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

  /* C button — debug: force the wave transition right now (zoom + bake
   * next wave). Useful for iterating on the ASIC zoom effect without
   * having to clear a wave normally. */
  if ((p1_single & PAD_C) && g_zoom_out_frame == 0 && g_respawn_timer == 0) {
    g_zoom_out_frame = 1;
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
  cram[4]  = 0x00EE;          // 4 yellow  — web outline lines
  /* slot 9 set below (after blue-gradient block to keep slots grouped) */
  /* Web lane fill gradient: 4 bands from deep (inner, far) to bright
   * (outer rim, near). All in the blue/purple family for that T2K vibe. */
  cram[5]  = 0x0200;          // 5 darkest navy — innermost band
  cram[6]  = 0x0412;          // 6 dark blue
  cram[7]  = 0x0624;          // 7 medium blue
  cram[8]  = 0x0846;          // 8 brightest blue-purple — outermost band
  cram[9]  = 0x0EE0;          // 9 cyan    — pulsar sprite
  cram[10] = 0x00E0;          // 10 green  — fuseball sprite
  cram[15] = 0x0AAA;          // 15 gray   — dim accent
  update_cram();

  init_joypads();

  // Upload font into VRAM tile area starting at glyph 0x20 (' ').
  vdp_ctrl = mode2_dma_enable;
  vdp_dma_transfer(res_basic_font.data, to_vdp_addr(tile_offset(0x20)),
                   (u16) (res_basic_font.size / 2));
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
