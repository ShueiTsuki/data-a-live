#!/usr/bin/env python3
"""Finishes of a BO already finished, and addresses taken from one: logged with both call stacks.

post27: the GPU faulted at 0x200 (a UCHE read) and draw state groups in backboardd's IB2 pointed at 0x2d4..0x384:
addresses taken from tu_bo structs that were already zeroed (iova 0 plus the offset), in a command buffer still being
recorded. The Darwin backend keeps its tu_bo structs in a table indexed by the kernel's buffer slot and zeroes one at
darwin_bo_finish; the kernel gives the slot to the next buffer once the deferred free ran. A finish through a stale
pointer (a second finish of a BO) then zeroes whatever buffer holds the slot now, under its owner. This logs (syslog,
the first 16 of each kind; the last 256 finishes are kept with their stacks):
  - a finish of a tu_bo that is already zeroed (it is not counted down again), with the finishes of that struct
    before it: the first finish of a pair is legitimate, the later one is the stale caller;
  - a draw state (tu_cs_end_draw_state) or a sub-allocation (tu_cs_alloc) whose BO has iova 0, with the finishes of
    that struct (the latest is the one that zeroed it under its owner).

  pandora_bocheck_finish.py MESA_DIR      (after pandora_bocheck.py and pandora_bocheck_submit.py)

Exact text edits, asserted; run once on a patched tree (local: the local Turnip build; CI: turnip.yml).
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1]) / 'src/freedreno/vulkan'


def edit(name, old, new):
    p = root / name
    s = p.read_text()
    assert s.count(old) == 1, (name, old[:60])
    p.write_text(s.replace(old, new))


edit('tu_cs.h',
     '''void tu_darwin_stale_ib(const struct tu_cs *cs, const struct tu_bo *bo, uint64_t offset, uint32_t size);
#endif''',
     '''void tu_darwin_stale_ib(const struct tu_cs *cs, const struct tu_bo *bo, uint64_t offset, uint32_t size);
/* data-a-live (tools/turnip/pandora_bocheck_finish.py): WHAT took an address from a BO already finished. */
void tu_darwin_stale_use(const char *what, const struct tu_bo *bo, uint64_t offset);
#endif''')

edit('tu_cs.h',
     '''   struct tu_cs_entry entry = tu_cs_end_sub_stream(cs, sub_cs);
   return (struct tu_draw_state) {''',
     '''   struct tu_cs_entry entry = tu_cs_end_sub_stream(cs, sub_cs);
#if defined(__APPLE__)
   if (entry.bo && !entry.bo->iova)
      tu_darwin_stale_use("draw state", entry.bo, entry.offset);
#endif
   return (struct tu_draw_state) {''')

edit('tu_cs.cc',
     '''   memory->map = (uint32_t *) bo->map + offset;
   memory->iova = bo->iova + offset * sizeof(uint32_t);''',
     '''   memory->map = (uint32_t *) bo->map + offset;
#if defined(__APPLE__)
   if (!bo->iova)
      tu_darwin_stale_use("sub-allocation", bo, offset * sizeof(uint32_t));
#endif
   memory->iova = bo->iova + offset * sizeof(uint32_t);''')

edit('tu_knl_darwin.cc',
     '''void
tu_darwin_stale_ib(const struct tu_cs *cs, const struct tu_bo *bo, uint64_t offset, uint32_t size)
{''',
     '''/* The finishes of the struct BO among the last DARWIN_FINISHED, newest first (at most two), to syslog. */
static void
darwin_report_finishes(const struct tu_bo *bo)
{
   unsigned shown = 0;
   pthread_mutex_lock(&darwin_finished_lock);
   for (unsigned k = 1; k <= DARWIN_FINISHED && k <= darwin_finished_next && shown < 2; k++) {
      const struct darwin_finished_bo *f = &darwin_finished[(darwin_finished_next - k) % DARWIN_FINISHED];
      if (f->bo != bo)
         continue;
      char freed[1024];
      darwin_describe(f->frames, f->nframes, freed, sizeof(freed));
      syslog(LOG_ERR, "pandora-turnip: BO %p %s finish (%u finishes ago) was %s at %#llx+%#llx (handle %#llx), by%s",
             (const void *)bo, shown ? "earlier" : "latest", k, f->name ? f->name : "unnamed",
             (unsigned long long)f->iova, (unsigned long long)f->size, (unsigned long long)f->handle, freed);
      shown++;
   }
   if (!shown)
      syslog(LOG_ERR, "pandora-turnip: BO %p is not among the last %u finished", (const void *)bo, DARWIN_FINISHED);
   pthread_mutex_unlock(&darwin_finished_lock);
}

void
tu_darwin_stale_use(const char *what, const struct tu_bo *bo, uint64_t offset)
{
   static unsigned reports;
   if (p_atomic_inc_return(&reports) > 16)
      return;
   void *frames[12];
   const int n = backtrace(frames, 12);
   char where[1024];
   darwin_describe(frames, n, where, sizeof(where));
   syslog(LOG_ERR, "pandora-turnip: %s at a finished BO %p +%#llx, taken by%s", what, (const void *)bo,
          (unsigned long long)offset, where);
   darwin_report_finishes(bo);
}

/* A finish of a struct already zeroed; DARWIN_FINISH_LOGGED of them get the stacks. */
static void
darwin_double_finish(const struct tu_bo *bo)
{
   static unsigned reports;
   if (p_atomic_inc_return(&reports) > 16)
      return;
   void *frames[12];
   const int n = backtrace(frames, 12);
   char where[1024];
   darwin_describe(frames, n, where, sizeof(where));
   syslog(LOG_ERR, "pandora-turnip: finish of a BO already finished %p (refcnt %d), by%s", (const void *)bo,
          bo->refcnt, where);
   darwin_report_finishes(bo);
}

void
tu_darwin_stale_ib(const struct tu_cs *cs, const struct tu_bo *bo, uint64_t offset, uint32_t size)
{''')

edit('tu_knl_darwin.cc',
     '''darwin_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   assert(bo->gem_handle);
''',
     '''darwin_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   /* data-a-live (tools/turnip/pandora_bocheck_finish.py): a second finish through a stale pointer. */
   if (!bo->gem_handle) {
      darwin_double_finish(bo);
      return;
   }
   assert(bo->gem_handle);
''')
