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
typedef enum { E_PLAYER = 1, E_SHOT = 2, E_FLIPPER = 3 } EntityType;

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
