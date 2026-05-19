#include "entity.h"

Entity   g_entity_pool[ENTITY_POOL_SIZE];
Entity * g_active_head;
Entity * g_free_head;
u16      g_active_count;

void pool_init(void)
{
  for (u8 i = 0; i < ENTITY_POOL_SIZE; ++i) {
    g_entity_pool[i].alive = 0;
    g_entity_pool[i].prev_cx = -1;
    g_entity_pool[i].prev_cy = -1;
    g_entity_pool[i].prev = 0;
    g_entity_pool[i].next = (i + 1 < ENTITY_POOL_SIZE) ? &g_entity_pool[i + 1] : 0;
  }
  g_free_head    = &g_entity_pool[0];
  g_active_head  = 0;
  g_active_count = 0;
}

Entity * entity_spawn(void)
{
  if (!g_free_head) return 0;
  Entity * e = g_free_head;
  g_free_head = e->next;

  e->alive = 1;
  e->prev_cx = -1;
  e->prev_cy = -1;
  e->prev = 0;
  e->next = g_active_head;
  if (g_active_head) g_active_head->prev = e;
  g_active_head = e;
  g_active_count++;
  return e;
}

void entity_kill(Entity * e)
{
  if (!e->alive) return;
  e->alive = 0;
  if (e->prev) e->prev->next = e->next;
  else         g_active_head = e->next;
  if (e->next) e->next->prev = e->prev;
  e->next = g_free_head;
  e->prev = 0;
  g_free_head = e;
  g_active_count--;
}
