// Hi-score table — boot/save/insert. See hiscore.h for layout.

#include "hiscore.h"
#include "mcd.h"

extern u8 g_mcd_present;

HiScoreTable g_hiscores;

/* Default table — Easter-egg homage to the Jaguar T2K defaults
 * (yak.s:23807-23828, animal initials counting down from 500 000).
 * Values scaled down ~100× to fit our 4-digit HUD score readout in
 * v0.2; bump back up once scoring is rescaled in a follow-up. */
static const HiScoreEntry DEFAULT_ENTRIES[HISCORE_COUNT] = {
  { 5000ul, {'Y','A','K'}, 15 },
  { 4500ul, {'E','W','E'}, 14 },
  { 4000ul, {'C','O','W'}, 13 },
  { 3500ul, {'G','N','U'}, 12 },
  { 3000ul, {'O','X',' '}, 11 },
  { 2500ul, {'E','L','K'}, 10 },
  { 2000ul, {'D','O','E'},  9 },
  { 1500ul, {'M','O','O'},  8 },
  { 1000ul, {'B','A','A'},  7 },
  {  500ul, {'F','U','R'},  6 },
};

static const u8 MAGIC[4] = { 'M', 'E', 'G', 'T' };

/* Sum of every byte in the entries[] array, mod 65536. Used for
 * validation of saved blobs. The header bytes themselves are NOT
 * included (so the checksum is computed BEFORE the header is stamped). */
static u16 compute_checksum(const HiScoreEntry * entries)
{
  u16 sum = 0;
  const u8 * p = (const u8 *) entries;
  for (u16 i = 0; i < sizeof(HiScoreEntry) * HISCORE_COUNT; i++) {
    sum = (u16) (sum + p[i]);
  }
  return sum;
}

static void load_defaults(void)
{
  for (u8 i = 0; i < 4; i++) g_hiscores.magic[i] = MAGIC[i];
  g_hiscores.entry_count = HISCORE_COUNT;
  for (u8 i = 0; i < HISCORE_COUNT; i++) g_hiscores.entries[i] = DEFAULT_ENTRIES[i];
  g_hiscores.checksum = compute_checksum(g_hiscores.entries);
}

static u8 validate(void)
{
  for (u8 i = 0; i < 4; i++) {
    if (g_hiscores.magic[i] != MAGIC[i]) return 0;
  }
  if (g_hiscores.entry_count != HISCORE_COUNT) return 0;
  if (g_hiscores.checksum != compute_checksum(g_hiscores.entries)) return 0;
  return 1;
}

/* BRAM persistence is DISABLED in v0.2 — Mode 1 cart can't reach the
 * BIOS BURAM functions without initialising state the cart-bootstrap
 * doesn't set up. mcd_hiscore_load/save would hang the sub. The table
 * still works in RAM for the session — display + qualifying detection +
 * initials entry all function — but resets on power off. v0.3 backlog
 * item: either init BIOS state ourselves or hit BRAM hardware directly.
 *
 * The plumbing (CMD_BRAM_LOAD / _SAVE on sub, mcd_hiscore_* on main)
 * is kept for a future fix; just not called. */

void hiscore_init(void)
{
  load_defaults();
}

u8 hiscore_qualifies(u32 score)
{
  return score > g_hiscores.entries[HISCORE_COUNT - 1].score;
}

void hiscore_insert(u32 score, const u8 initials[HISCORE_INITIALS_LEN], u8 level)
{
  if (!hiscore_qualifies(score)) return;

  /* Find rank (first slot whose score is strictly less than ours). */
  u8 rank = HISCORE_COUNT - 1;
  for (u8 i = 0; i < HISCORE_COUNT; i++) {
    if (g_hiscores.entries[i].score < score) { rank = i; break; }
  }

  /* Shift the lower entries down by one. */
  for (u8 i = HISCORE_COUNT - 1; i > rank; i--) {
    g_hiscores.entries[i] = g_hiscores.entries[i - 1];
  }

  /* Write the new entry. */
  HiScoreEntry * e = &g_hiscores.entries[rank];
  e->score = score;
  for (u8 i = 0; i < HISCORE_INITIALS_LEN; i++) e->initials[i] = initials[i];
  e->level = level;

  g_hiscores.checksum = compute_checksum(g_hiscores.entries);

  /* BRAM save disabled in v0.2 — see comment near hiscore_init. */
}
