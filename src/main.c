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

static u16 vdp_regs[18];
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
  vdp_ctrl_32 = pos;
  while (*s) vdp_data = *s++;
}

static void plane_putc(u16 cx, u16 cy, char c)
{
  vdp_ctrl_32 = plane_xy(cx, cy);
  vdp_data = (u8) c;
}

static void clear_play_area(void)
{
  for (u8 y = 4; y < 27; ++y) {
    vdp_ctrl_32 = plane_xy(0, y);
    for (u8 x = 0; x < 40; ++x) vdp_data = ' ';
  }
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
static u8  g_flipper_count;
static u16 g_score;
static u32 g_rng = 0xCAFEF00Du;
#define LANE_HOLD_INITIAL 14     // frames before first repeat after first press
#define LANE_HOLD_REPEAT   4     // frames between subsequent repeats
#define FIRE_COOLDOWN      6     // frames between shots
#define SHOT_INWARD_STEP   (FP_ONE >> 5)   // 32 ticks rim->centre
#define FLIPPER_OUT_STEP   (FP_ONE >> 7)   // 128 ticks centre->rim (~2 sec)
#define FLIPPER_RIM_HOP    12              // frames between rim-walk hops
#define FLIPPER_SPAWN_PERIOD 90            // ~1.5 sec at 60 Hz
#define FLIPPER_MAX_ACTIVE   4
#define HIT_DEPTH_TOL      (FP_ONE >> 4)   // collision threshold

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

static void spawn_flipper(u8 lane)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_FLIPPER;
  e->lane         = lane;
  e->depth_fp     = 0;
  e->depth_vel_fp = +FLIPPER_OUT_STEP;
  e->phase        = 0;
  e->step_period  = FLIPPER_RIM_HOP;
  e->lifetime     = FLIPPER_RIM_HOP;
  g_flipper_count++;
}

static void kill_flipper(Entity * e)
{
  g_flipper_count--;
  entity_kill(e);
}

static void play_always_vblank(void) { return; }
static void play_gated_vblank (void);

static void install_playfield(void)
{
  g_engine.always_vblank = play_always_vblank;
  g_engine.gated_vblank  = play_gated_vblank;
  g_engine.main_thread   = play_main_thread;
  g_engine.paused        = 0;
  g_scene_dirty          = 1;

  pool_init();
  web_init();
  g_player_lane = 0;
  g_left_tick = g_right_tick = 0;
  g_fire_cooldown = 0;
  g_spawn_timer  = FLIPPER_SPAWN_PERIOD;
  g_flipper_count = 0;
  g_score = 0;

  // Kick off music for the round (Mega CD only — gracefully silent on plain MD).
  if (g_mcd_present) {
    if (g_music_playing) {
      mcd_stop_mod();
      mcd_wait_ack(CMD_STOP_MOD);
      g_music_playing = 0;
    }
    mcd_upload_mod(&res_rave4_mod);
    mcd_play_mod(res_rave4_mod.size);
    mcd_wait_ack(CMD_PLAY_MOD);
    g_music_playing = 1;

    web_render_main(4);              /* yellow web */
    web_dma_main_to_vram();
    web_paint_plane_b();
  }

  load_sprite_tiles_to_vram();

  g_player = entity_spawn();
  if (g_player) {
    g_player->type         = E_PLAYER;
    g_player->lane         = g_player_lane;
    g_player->depth_fp     = FP_ONE;
    g_player->depth_vel_fp = 0;
  }
}

static void play_gated_vblank(void)
{
  // L/R lane hop with hold-to-repeat cooldown.
  if (p1_hold & PAD_LEFT) {
    if (g_left_tick == 0) {
      g_player_lane = (u8) ((g_player_lane + NUM_LANES - 1) % NUM_LANES);
      web_lane_changed(g_player_lane, +1);   /* +1 = visual CCW = LEFT */
      g_left_tick = (p1_single & PAD_LEFT) ? LANE_HOLD_INITIAL : LANE_HOLD_REPEAT;
    } else {
      g_left_tick--;
    }
  } else {
    g_left_tick = 0;
  }
  if (p1_hold & PAD_RIGHT) {
    if (g_right_tick == 0) {
      g_player_lane = (u8) ((g_player_lane + 1) % NUM_LANES);
      web_lane_changed(g_player_lane, -1);   /* -1 = visual CW = RIGHT */
      g_right_tick = (p1_single & PAD_RIGHT) ? LANE_HOLD_INITIAL : LANE_HOLD_REPEAT;
    } else {
      g_right_tick--;
    }
  } else {
    g_right_tick = 0;
  }

  if (g_player) g_player->lane = g_player_lane;
  web_claw_tick(g_player_lane);

  // Fire on A (with cooldown).
  if (g_fire_cooldown) g_fire_cooldown--;
  if ((p1_single & PAD_A) && g_fire_cooldown == 0) {
    spawn_shot(g_player_lane);
    g_fire_cooldown = FIRE_COOLDOWN;
  }

  // Spawn a flipper every spawn_period frames, capped.
  if (g_spawn_timer) g_spawn_timer--;
  if (g_spawn_timer == 0 && g_flipper_count < FLIPPER_MAX_ACTIVE) {
    spawn_flipper((u8) (lcg() & 0x0F));
    g_spawn_timer = FLIPPER_SPAWN_PERIOD;
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
    }
    e = next;
  }

  // Shot↔flipper collisions: same lane + close depth_fp.
  Entity * s = g_active_head;
  while (s) {
    Entity * s_next = s->next;
    if (s->type == E_SHOT) {
      Entity * f = g_active_head;
      while (f) {
        Entity * f_next = f->next;
        if (f->type == E_FLIPPER && f->lane == s->lane) {
          fp16 d = s->depth_fp - f->depth_fp;
          if (d < 0) d = -d;
          if (d <= HIT_DEPTH_TOL) {
            entity_kill(s);
            kill_flipper(f);
            g_score++;
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

  // Flipper-at-rim on player's lane → respawn player at lane 0.
  if (g_player) {
    Entity * f = g_active_head;
    while (f) {
      Entity * f_next = f->next;
      if (f->type == E_FLIPPER && f->phase == 1 && f->lane == g_player_lane) {
        g_player_lane = 0;
        g_player->lane = 0;
        web_player_snap_to(0);    /* match visual to logical respawn position */
        kill_flipper(f);
        if (g_mcd_present) mcd_play_sfx(2);    /* 2 = DEATH — PCM */
        else               sfx_death();         /* PSG fallback */
        break;
      }
      f = f_next;
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
    g_scene_dirty = 0;
  }

  // Score readout — 4 digits, no 32-bit divide needed.
  u16 s = g_score;
  plane_putc(37, 27, (char) ('0' + (s % 10))); s /= 10;
  plane_putc(36, 27, (char) ('0' + (s % 10))); s /= 10;
  plane_putc(35, 27, (char) ('0' + (s % 10))); s /= 10;
  plane_putc(34, 27, (char) ('0' + (s % 10)));


  /* All entities (player, shots, flippers) render via VDP hardware sprites. */
  render_sprites();

  if (p1_single & PAD_B) install_title();
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
  cram[2]  = 0x000E;          // 2 red     — enemy sprites
  cram[4]  = 0x00EE;          // 4 yellow  — web lines
  cram[15] = 0x0AAA;          // 15 gray   — dim accent
  update_cram();

  init_joypads();

  // Upload font into VRAM tile area starting at glyph 0x20 (' ').
  vdp_ctrl = mode2_dma_enable;
  vdp_dma_transfer(res_basic_font.data, to_vdp_addr(tile_offset(0x20)),
                   (u16) (res_basic_font.size / 2));
  vdp_ctrl = mode2_display_off;

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
