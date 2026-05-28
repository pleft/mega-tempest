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
  E_TANKER  = 5,    // big slow enemy; splits into 2 flippers on hit.
                    // Same field semantics as E_FLIPPER (lane, phase,
                    // depth_fp, depth_vel_fp). Tankers don't rim-walk —
                    // they sit at the rim on reaching it and kill the
                    // player when the claw passes through that lane.
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
                    //   phase (0=LASER, 1=JUMP)          — power-up kind
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
