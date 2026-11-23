/*
 * Copyright © 2026 JimVulkan
 *
 * SPDX-License-Identifier: MIT
 *
 * Slot bookkeeping for RADV_XCLIPSE_LATENCY, kept in its own header so it can be exercised on the
 * host (docs/tests/xlat_slots_test.c) without a device, a driver build or a GPU.
 *
 * The instrument writes two CP timestamps per command-buffer recording. The identity of a timestamp
 * must be settled before the value can be believed, and the earlier scheme could not settle it: slots
 * were indexed rec % 1024, so after 1024 recordings a slot belonged to a different recording while
 * still holding the previous one's timestamp, and "non-zero" was read as "this recording completed".
 * These are the rules that replace it:
 *
 *  - the slot index is the recording instance itself (never a modulus), so a slot cannot be reused
 *    while the GPU may still write it, and an index can only name the recording that owns it;
 *  - before the recording is submitted the host writes a pending marker, which the GPU then
 *    overwrites with the timestamp; "written" is therefore decidable without a clock comparison;
 *  - the marker's top bit is set, and a CP timestamp never has it set, so a marker can never be
 *    mistaken for a timestamp, and a timestamp can never be mistaken for a marker;
 *  - recordings past the capacity are refused and counted, rather than silently aliasing.
 */
#ifndef RADV_XCLIPSE_LATENCY_SLOTS_H
#define RADV_XCLIPSE_LATENCY_SLOTS_H

#include <stdbool.h>
#include <stdint.h>

/* Recordings instrumented per device lifetime. Observed volume is ~7 recordings per frame, so this
 * covers roughly 9300 frames (~5 minutes at 30 fps) in 1 MiB of GTT, with no reuse inside a run. */
#define XLAT_REC_CAP 65536u

#define XLAT_SLOT_DW 2u                     /* arrival, completion */
#define XLAT_STRIDE_B (XLAT_SLOT_DW * 8u)   /* bytes per recording */
#define XLAT_ARRIVAL 0u
#define XLAT_COMPLETION 1u

/* Set in the pending marker only. A CP timestamp is well below 2^63 for any plausible uptime. */
#define XLAT_PENDING_BIT (1ull << 63)

static inline uint64_t
xlat_pending_value(uint64_t rec)
{
   return XLAT_PENDING_BIT | (rec & ~XLAT_PENDING_BIT);
}

/* The GPU wrote this slot: not zero (never written) and not the pending marker. */
static inline bool
xlat_slot_written(uint64_t value)
{
   return value != 0 && (value >> 63) == 0;
}

static inline bool
xlat_slot_pending(uint64_t value)
{
   return (value >> 63) != 0;
}

/* Recording 0 is never handed out (the counter is pre-incremented), so 0 stays "no recording". */
static inline bool
xlat_rec_in_range(uint64_t rec)
{
   return rec > 0 && rec < XLAT_REC_CAP;
}

static inline uint64_t
xlat_rec_offset(uint64_t rec)
{
   return rec * XLAT_STRIDE_B;
}


/* ---- wait-start snapshot lookup ---------------------------------------------------------------
 *
 * A wait snapshots the slots of the recordings that could still be in flight. Reading that snapshot
 * for recording `rec` must distinguish "the slot held X" from "this recording was not in the window",
 * because the second one cannot answer whether it was finished and must not be reported as "no".
 */
#define XLAT_SNAPSHOT_MISSING UINT64_MAX

static inline uint64_t
xlat_snapshot_index(uint64_t base, uint64_t n, uint64_t rec)
{
   if (rec < base)
      return XLAT_SNAPSHOT_MISSING;
   const uint64_t i = rec - base;
   return i < n ? i : XLAT_SNAPSHOT_MISSING;
}

/* ---- submission -> recording instances, recalled by sequence ----------------------------------
 *
 * Which recordings a submission carried has to survive to the wait that blocks on it, and it has to
 * be recalled without a shared lock (a lock here once blocked a recording thread behind a
 * multi-second wait). A seqlock does it: the tag is odd while the entry is being written and equals
 * submit * 2 + 2 once stable, so a reader either sees a complete entry for the sequence it asked for
 * or reports "unresolved". It never returns another submission's recordings.
 */
#define XLAT_IDS_MAX 8u

typedef struct {
   uint64_t tag; /* written and read only through __atomic_* on a plain word */
   uint64_t submit;
   uint64_t recs[XLAT_IDS_MAX];
   unsigned n;
} xlat_sub_entry;

static inline uint64_t
xlat_sub_tag_writing(uint64_t submit)
{
   return submit * 2 + 1;
}

static inline uint64_t
xlat_sub_tag_stable(uint64_t submit)
{
   return submit * 2 + 2;
}

/* Returns the number of recording instances recalled, 0 = unresolved or nothing recorded. */
static inline unsigned
xlat_recall_from_entry(xlat_sub_entry *e, uint64_t submit, uint64_t *recs, unsigned cap)
{
   if (!submit)
      return 0;
   if (__atomic_load_n(&e->tag, __ATOMIC_ACQUIRE) != xlat_sub_tag_stable(submit))
      return 0;
   const unsigned n = e->n < cap ? e->n : cap;
   for (unsigned i = 0; i < n; i++)
      recs[i] = e->recs[i];
   /* Re-read after copying: a writer that started during the copy must discard the result rather
    * than let a half-old, half-new list be attributed to one submission. */
   if (__atomic_load_n(&e->tag, __ATOMIC_ACQUIRE) != xlat_sub_tag_stable(submit))
      return 0;
   return n;
}

static inline void
xlat_store_entry(xlat_sub_entry *e, uint64_t submit, const uint64_t *recs, unsigned n)
{
   __atomic_store_n(&e->tag, xlat_sub_tag_writing(submit), __ATOMIC_RELAXED);
   e->submit = submit;
   for (unsigned i = 0; i < XLAT_IDS_MAX; i++)
      e->recs[i] = i < n ? recs[i] : 0;
   e->n = n < XLAT_IDS_MAX ? n : XLAT_IDS_MAX;
   __atomic_store_n(&e->tag, xlat_sub_tag_stable(submit), __ATOMIC_RELEASE);
}

#endif /* RADV_XCLIPSE_LATENCY_SLOTS_H */
