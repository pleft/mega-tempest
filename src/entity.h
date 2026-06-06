// Entity pool (doc 10) — fixed-size pool, doubly-linked free + active lists.
// Plus the fp16 type used for depth + velocity.

#ifndef TEMPEST_ENTITY_H
#define TEMPEST_ENTITY_H

#include <types.h>

typedef s32 fp16;
#define FP_ONE         ((fp16) 0x10000)
#define FP_FROM_INT(i) (((s32)(i)) << 16)
#define FP_INT(fp)     ((s16) ((fp) >> 16))

typedef struct Entity Entity;
typedef enum {
  E_PLAYER  = 1,
  E_SHOT    = 2,
  E_FLIPPER = 3,
  E_DEBRIS  = 4,    // death-burst particle; reuses existing fields:
                    //   lane         = direction index 0..7
                    //   depth_fp     = accumulated screen-x offset from spawn
                    //   depth_vel_fp = accumulated screen-y offset from spawn
                    //   lifetime     = frames remaining (auto-kill at 0)
  E_TANKER  = 5,    // big slow enemy; splits on hit. Field reuse:
                    //   lane, phase, depth_fp, depth_vel_fp — same as
                    //     E_FLIPPER (phase 0=desc / 1=rim).
                    //   step_period = tanker kind (0=flipper-tanker
                    //     → 2 flippers; 1=pulsar-tanker → 2 pulsars;
                    //     2=fuse-tanker → 2 fuseballs).
                    //   Render pass picks the sprite palette by kind
                    //     so the variants are colour-coded.
                    //   Tankers don't rim-walk — they sit at the rim
                    //   on reaching it and kill the player on contact.
  E_PULSAR  = 6,    // descends, cycles 3-frame pulse animation (driven
                    // by g_anim_frame). When pulse hits peak (frame 2)
                    // and pulsar is at the rim on the player's lane,
                    // kills the player. Reuses lane/depth_fp/depth_vel_fp
                    // /phase same as E_FLIPPER.
  E_FUSEBALL = 7,   // erratic enemy — moves in/out and hops between
                    // adjacent lanes at random intervals. lifetime =
                    // ticks until next direction/lane change.
                    // phase 0 = roaming, 1 = at rim (kills like flipper).
  E_SPIKER   = 8,   // small fast enemy that paints a spike onto its lane
                    // as it descends. The spike lives in g_spike_depth[]
                    // (not an entity); spiker despawns on reaching rim.
                    // Uses lane/depth_fp/depth_vel_fp same as flipper.
  E_ZAPSPARK = 9,   // superzapper kill marker — stationary at a lane+depth
                    // for a few frames, then vanishes. Reuses lane/depth_fp
                    // for screen position, lifetime for countdown.
  E_POWERUP  = 10,  // dropped by killed tankers; travels outward to the
                    // rim like a flipper. Player collects by being on the
                    // same lane when it arrives. Field reuse:
                    //   lane, depth_fp, depth_vel_fp     — standard
                    //   phase (0=LASER, 1=JUMP, ...)     — power-up kind
  E_DROID    = 11,  // AI sidekick spawned by PUP_DROID. Walks the rim
                    // toward the nearest enemy's lane and fires shots
                    // periodically. Field reuse:
                    //   lane           = current rim lane
                    //   depth_fp       = FP_ONE (always at rim)
                    //   step_period    = ticks until next lane hop
                    //   lifetime       = u8 — wraps; combined with a u16
                    //                    g_droid_life_timer for the real
                    //                    countdown (~20 s).
  E_SUPER_FLIPPER = 12,  // promoted flipper variant — same descent +
                    // rim-walk behaviour as E_FLIPPER but ~1.5× faster
                    // and rendered in white (per-sprite palette 1 slot
                    // 2). Spawned probabilistically from the flipper
                    // pool from wave 6+. Same field semantics as
                    // E_FLIPPER throughout the tick + collision paths.
  E_PULSAR_SPARK = 13,  // Spawned by a pulsar when it reaches the rim
                    // (one going left, one going right). Walks the rim
                    // every PSPARK_HOP_PERIOD ticks; kills player on
                    // same-lane rim contact; killable by shot (flipper
                    // score). Field reuse:
                    //   lane          = current rim lane
                    //   depth_fp      = FP_ONE (always at rim)
                    //   phase         = direction (0 = left, 1 = right)
                    //   step_period   = ticks until next lane hop
                    //   lifetime      = u8 countdown — auto-despawn so
                    //                   sparks don't circle forever
} EntityType;

struct Entity {
  u8       type;
  u8       alive;
  u8       lane;           // 0..NUM_LANES-1 — which radial lane
  u8       phase;          // flipper: 0=descending, 1=rim-walking
  fp16     depth_fp;       // 0 = centre (vanishing point), FP_ONE = rim
  fp16     depth_vel_fp;   // per-tick depth_fp delta (negative = inward)
  u8       step_period;    // flipper: frames between rim-walk hops
  u8       lifetime;       // flipper: countdown to next hop
  Entity * prev;
  Entity * next;
};

#define ENTITY_POOL_SIZE 32

extern Entity   g_entity_pool[ENTITY_POOL_SIZE];
extern Entity * g_active_head;
extern Entity * g_free_head;
extern u16      g_active_count;

void pool_init(void);
Entity * entity_spawn(void);
void entity_kill(Entity * e);

#endif
