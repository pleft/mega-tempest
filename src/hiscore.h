// Hi-score table (v0.2): 10 entries, persisted to Mega CD Backup RAM
// via mcd_hiscore_load/save. Faithful to the Jaguar T2K hall-of-fame
// (10 slots, 3-letter initials, sorted descending by score) but
// extended with the Jaguar's "level reached" field so the table can
// be displayed alongside ranks. See reference_t2k_progression.md.

#ifndef TEMPEST_HISCORE_H
#define TEMPEST_HISCORE_H

#include <types.h>

#define HISCORE_COUNT          10
#define HISCORE_INITIALS_LEN   3      /* AAA..ZZZ */

typedef struct {
  u32 score;
  u8  initials[HISCORE_INITIALS_LEN];      /* 'A'..'Z' (ASCII) */
  u8  level;                                /* 1-based wave reached */
} HiScoreEntry;

/* Header: magic + checksum + entry count. Followed by the 10 entries
 * back-to-back. Total = 8 + 10*8 = 88 bytes — well under the 256-byte
 * shared slot in PRG-RAM. */
typedef struct {
  u8           magic[4];     /* "MEGT" — magic word for Sega format */
  u16          checksum;     /* sum of all entry bytes (mod 65536) */
  u16          entry_count;  /* always HISCORE_COUNT — reserved */
  HiScoreEntry entries[HISCORE_COUNT];
} HiScoreTable;

extern HiScoreTable g_hiscores;

/* Boot — try to load from Backup RAM; on miss / bad checksum / MCD
 * not present, populate with defaults. */
void hiscore_init(void);

/* True if `score` would qualify (i.e. ≥ the table's lowest entry). */
u8   hiscore_qualifies(u32 score);

/* Insert `score / initials / level` into the table at the rank
 * implied by score, dropping the previous bottom. Persists via
 * mcd_hiscore_save. No-op if score doesn't qualify. */
void hiscore_insert(u32 score, const u8 initials[HISCORE_INITIALS_LEN], u8 level);

#endif
