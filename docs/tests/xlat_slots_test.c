/*
 * Host-side validation of the RADV_XCLIPSE_LATENCY slot bookkeeping.
 *
 * The instrument's whole value is attribution: which recording a timestamp belongs to, and whether a
 * zero means "not finished" or "not observable". Both were got wrong by the first scheme (slots
 * indexed rec % capacity, "non-zero" read as "this recording completed"), so these cases are pinned
 * here rather than discovered on a device.
 *
 *   cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/xlat_slots_test docs/tests/xlat_slots_test.c
 *   /tmp/xlat_slots_test
 */
#include <stdio.h>
#include <string.h>

#include "../../src/amd/vulkan/radv_xclipse_latency_slots.h"

static int failures;

#define CHECK(expr)                                                                                          \
   do {                                                                                                      \
      if (!(expr)) {                                                                                         \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                     \
         failures++;                                                                                         \
      }                                                                                                      \
   } while (0)

/* A plausible CP timestamp: low, and bit 63 clear. */
#define TICK(v) ((uint64_t)(v))

static void
test_classification(void)
{
   /* Untouched memory is not "retired": the buffer is zeroed once, before any submission. */
   CHECK(!xlat_slot_written(0));
   CHECK(!xlat_slot_pending(0));

   /* A pending marker is never a timestamp, for every recording in range. */
   for (uint64_t rec = 1; rec < 8; rec++) {
      const uint64_t marker = xlat_pending_value(rec);
      CHECK(xlat_slot_pending(marker));
      CHECK(!xlat_slot_written(marker));
      CHECK((marker & XLAT_PENDING_BIT) != 0);
   }

   /* A timestamp is never a marker, including one whose low bits would look like a recording id. */
   CHECK(xlat_slot_written(TICK(1)));
   CHECK(xlat_slot_written(TICK(XLAT_REC_CAP - 1)));
   CHECK(xlat_slot_written(TICK(0x7fffffffffffffffull)));
   CHECK(!xlat_slot_pending(TICK(0x7fffffffffffffffull)));

   /* The regression this replaces: a stale timestamp must not be usable as "this recording
    * finished". A slot that still holds the marker is unambiguously unfinished, and one that holds a
    * timestamp is finished only because the index can no longer name a different recording. */
   const uint64_t stale_from_previous_generation = TICK(1234567);
   CHECK(xlat_slot_written(stale_from_previous_generation)); /* the value itself is a timestamp */
   CHECK(!xlat_slot_written(xlat_pending_value(XLAT_REC_CAP - 1))); /* but the marker rules win */
}

static void
test_capacity(void)
{
   CHECK(!xlat_rec_in_range(0));                     /* 0 is "no recording" */
   CHECK(xlat_rec_in_range(1));
   CHECK(xlat_rec_in_range(XLAT_REC_CAP - 1));
   /* Past the capacity the recording is refused, never aliased onto recording rec - capacity. */
   CHECK(!xlat_rec_in_range(XLAT_REC_CAP));
   CHECK(!xlat_rec_in_range(XLAT_REC_CAP + 1023));
   CHECK(!xlat_rec_in_range(UINT64_MAX));
}

static void
test_offsets(void)
{
   CHECK(xlat_rec_offset(1) == XLAT_STRIDE_B);
   CHECK(xlat_rec_offset(XLAT_REC_CAP - 1) == (uint64_t)(XLAT_REC_CAP - 1) * XLAT_STRIDE_B);

   /* No two recordings in range may share a slot pair: the aliasing that made a stale timestamp look
    * current was exactly this property being false. Sampling is enough, exhaustively it is 64k. */
   for (uint64_t rec = 1; rec < 4096; rec++)
      CHECK(xlat_rec_offset(rec) != xlat_rec_offset(rec - 1));

   /* The pair is addressable and the completion slot is the second one. */
   CHECK(xlat_rec_offset(7) + XLAT_ARRIVAL * 8 + 8 == xlat_rec_offset(7) + XLAT_COMPLETION * 8);
   CHECK(xlat_rec_offset(7) + XLAT_COMPLETION * 8 + 8 == xlat_rec_offset(8));
}

static void
test_snapshot_lookup(void)
{
   const uint64_t base = 1000, n = 16;
   CHECK(xlat_snapshot_index(base, n, 999) == XLAT_SNAPSHOT_MISSING); /* older than the window */
   CHECK(xlat_snapshot_index(base, n, base) == 0);
   CHECK(xlat_snapshot_index(base, n, base + n - 1) == n - 1);
   CHECK(xlat_snapshot_index(base, n, base + n) == XLAT_SNAPSHOT_MISSING); /* newer than the window */
   CHECK(xlat_snapshot_index(base, 0, base) == XLAT_SNAPSHOT_MISSING);     /* nothing in flight */
   /* A missing snapshot must not be comparable to a timestamp by accident. */
   CHECK(XLAT_SNAPSHOT_MISSING == UINT64_MAX);
   CHECK(!xlat_slot_written(XLAT_SNAPSHOT_MISSING));
}

static void
test_submit_recall(void)
{
   xlat_sub_entry e;
   memset(&e, 0, sizeof e);
   uint64_t out[XLAT_IDS_MAX + 4];
   const uint64_t recs[4] = {11, 12, 13, 14};

   /* Unwritten entry: unresolved, not "no recordings". */
   CHECK(xlat_recall_from_entry(&e, 700, out, XLAT_IDS_MAX) == 0);

   xlat_store_entry(&e, 700, recs, 4);
   CHECK(xlat_recall_from_entry(&e, 700, out, XLAT_IDS_MAX) == 4);
   CHECK(out[0] == 11 && out[3] == 14);

   /* A different sequence that hashes to the same ring slot must not return these recordings. */
   CHECK(xlat_recall_from_entry(&e, 701, out, XLAT_IDS_MAX) == 0);

   /* Overwritten by a newer submission: unresolved, never the older list. */
   const uint64_t newer[2] = {21, 22};
   xlat_store_entry(&e, 701, newer, 2);
   CHECK(xlat_recall_from_entry(&e, 700, out, XLAT_IDS_MAX) == 0);
   CHECK(xlat_recall_from_entry(&e, 701, out, XLAT_IDS_MAX) == 2 && out[1] == 22);

   /* Torn write: an odd tag means the entry is being rewritten, so it must be refused. */
   __atomic_store_n(&e.tag, xlat_sub_tag_writing(702), __ATOMIC_RELAXED);
   CHECK(xlat_recall_from_entry(&e, 702, out, XLAT_IDS_MAX) == 0);

   /* Sequence 0 is "no submission", never a lookup. */
   CHECK(xlat_recall_from_entry(&e, 0, out, XLAT_IDS_MAX) == 0);

   /* More recordings than fit: the count is capped, the tag arithmetic still matches. */
   uint64_t many[XLAT_IDS_MAX + 4];
   for (unsigned i = 0; i < XLAT_IDS_MAX + 4; i++)
      many[i] = 100 + i;
   xlat_store_entry(&e, 703, many, XLAT_IDS_MAX + 4);
   CHECK(xlat_recall_from_entry(&e, 703, out, XLAT_IDS_MAX) == XLAT_IDS_MAX);
   CHECK(out[XLAT_IDS_MAX - 1] == 100 + XLAT_IDS_MAX - 1);
}

static void
test_tag_arithmetic(void)
{
   /* Tags are submit * 2 + 1 / + 2: distinct for distinct submissions, odd while writing. */
   for (uint64_t s = 1; s < 1000; s++) {
      CHECK(xlat_sub_tag_writing(s) % 2 == 1);
      CHECK(xlat_sub_tag_stable(s) % 2 == 0);
      CHECK(xlat_sub_tag_stable(s) == xlat_sub_tag_writing(s) + 1);
      CHECK(xlat_sub_tag_stable(s) != xlat_sub_tag_stable(s + 1));
   }
}

int
main(void)
{
   test_classification();
   test_capacity();
   test_offsets();
   test_snapshot_lookup();
   test_submit_recall();
   test_tag_arithmetic();

   if (failures) {
      fprintf(stderr, "%d check(s) failed\n", failures);
      return 1;
   }
   printf("xlat slot bookkeeping: all checks passed (cap %u, stride %u B, %u KiB)\n", XLAT_REC_CAP, XLAT_STRIDE_B,
          XLAT_REC_CAP * XLAT_STRIDE_B / 1024);
   return 0;
}
