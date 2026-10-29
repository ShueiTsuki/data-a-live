#!/usr/bin/env python3
"""An indirect buffer call to a freed BO: logged with who records it and who freed the BO, and left out.

post23 (kernel 0325-0329 hang dumps): the BR stopped in an IB2 at address 0. The stuck submission's IB 49 held, at
+421, a CP_INDIRECT_BUFFER to 0 with 202 dwords, where IB 51 (same layout) called 0x102e3a000+202: the packet was
written with address 0. tu_cs_emit_ib computes the address when it records the call, from the entry's BO, and the
Darwin backend zeroes a tu_bo as soon as Turnip finishes it (darwin_bo_finish; the kernel's free waits for the
fence). A call to a BO already finished therefore gets iova 0 plus the entry's offset (post15 0xed42000, post19
0x5362000, post18/20/23 0). This logs (syslog, the first 16) the recording's call stack and the BO's free (the last
256 finishes are kept with their stacks), as image+offset from dladdr, and records no call for such an entry: the
draws it holds are lost instead of the GPU hanging.

  pandora_bocheck.py MESA_DIR

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
     '''   assert(entry->offset % sizeof(uint32_t) == 0);

   tu_cs_emit_pkt7(cs, CP_INDIRECT_BUFFER, 3);
   tu_cs_emit_qw(cs, entry->bo->iova + entry->offset);''',
     '''   assert(entry->offset % sizeof(uint32_t) == 0);

#if defined(__APPLE__)
   /* data-a-live (tools/turnip/pandora_bocheck.py): a call to a BO already finished (its tu_bo zeroed) is
    * logged and left out. */
   if (!entry->bo->iova) {
      tu_darwin_stale_ib(cs, entry->bo, entry->offset, entry->size);
      return;
   }
#endif
   tu_cs_emit_pkt7(cs, CP_INDIRECT_BUFFER, 3);
   tu_cs_emit_qw(cs, entry->bo->iova + entry->offset);''')

edit('tu_cs.h',
     '''#include "tu_knl.h"
''',
     '''#include "tu_knl.h"

#if defined(__APPLE__)
/* data-a-live (tools/turnip/pandora_bocheck.py), tu_knl_darwin.cc */
void tu_darwin_stale_ib(const struct tu_cs *cs, const struct tu_bo *bo, uint64_t offset, uint32_t size);
#endif
''')

edit('tu_knl_darwin.cc',
     '''#include <unistd.h>
''',
     '''#include <unistd.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <syslog.h>
''')

edit('tu_knl_darwin.cc',
     '''   tu_debug_bos_del(dev, bo);
   tu_dump_bo_del(dev, bo);
''',
     '''   tu_debug_bos_del(dev, bo);
   tu_dump_bo_del(dev, bo);
   darwin_note_finish(bo);
''')

edit('tu_knl_darwin.cc',
     '''/* ---- buffer objects ---- */
''',
     '''/* ---- data-a-live (tools/turnip/pandora_bocheck.py): finished BOs and calls recorded to them ---- */

struct darwin_finished_bo {
   const struct tu_bo *bo;
   uint64_t iova, size, handle;
   const char *name;
   void *frames[10];
   int nframes;
};
#define DARWIN_FINISHED 256
static struct darwin_finished_bo darwin_finished[DARWIN_FINISHED];
static unsigned darwin_finished_next;
static pthread_mutex_t darwin_finished_lock = PTHREAD_MUTEX_INITIALIZER;

static void
darwin_note_finish(const struct tu_bo *bo)
{
   void *frames[10];
   const int n = backtrace(frames, 10);
   pthread_mutex_lock(&darwin_finished_lock);
   struct darwin_finished_bo *f = &darwin_finished[darwin_finished_next++ % DARWIN_FINISHED];
   f->bo = bo;
   f->iova = bo->iova;
   f->size = bo->size;
   f->handle = bo->darwin_handle;
   f->name = bo->name;
   memcpy(f->frames, frames, sizeof(frames));
   f->nframes = n;
   pthread_mutex_unlock(&darwin_finished_lock);
}

/* FRAMES as "image+offset" (symbol+offset where dladdr has one), from the second frame on. */
static void
darwin_describe(void *const *frames, int n, char *out, size_t size)
{
   size_t at = 0;
   out[0] = 0;
   for (int i = 1; i < n && at + 1 < size; i++) {
      Dl_info info = {};
      const char *image = "?";
      uintptr_t off = (uintptr_t)frames[i];
      if (dladdr(frames[i], &info) && info.dli_fname) {
         image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
         off = (uintptr_t)frames[i] - (uintptr_t)info.dli_fbase;
      }
      int w = info.dli_sname
         ? snprintf(out + at, size - at, " %s:%s+%#lx", image, info.dli_sname,
                    (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_saddr))
         : snprintf(out + at, size - at, " %s+%#lx", image, (unsigned long)off);
      if (w < 0)
         break;
      at += (size_t)w;
   }
}

void
tu_darwin_stale_ib(const struct tu_cs *cs, const struct tu_bo *bo, uint64_t offset, uint32_t size)
{
   static unsigned reports;
   if (p_atomic_inc_return(&reports) > 16)
      return;
   void *frames[12];
   const int n = backtrace(frames, 12);
   char where[1024];
   darwin_describe(frames, n, where, sizeof(where));
   syslog(LOG_ERR, "pandora-turnip: call to a finished BO %p +%#llx (%u bytes) left out of cs %p, recorded by%s",
          (const void *)bo, (unsigned long long)offset, size, (const void *)cs, where);
   pthread_mutex_lock(&darwin_finished_lock);
   const struct darwin_finished_bo *found = NULL;
   for (unsigned k = 1; k <= DARWIN_FINISHED && k <= darwin_finished_next; k++) {
      const struct darwin_finished_bo *f = &darwin_finished[(darwin_finished_next - k) % DARWIN_FINISHED];
      if (f->bo == bo) {
         found = f;
         break;
      }
   }
   if (found) {
      char freed[1024];
      darwin_describe(found->frames, found->nframes, freed, sizeof(freed));
      syslog(LOG_ERR, "pandora-turnip: BO %p was %s at %#llx+%#llx (handle %#llx), finished by%s", (const void *)bo,
             found->name ? found->name : "unnamed", (unsigned long long)found->iova, (unsigned long long)found->size,
             (unsigned long long)found->handle, freed);
   } else {
      syslog(LOG_ERR, "pandora-turnip: BO %p is not among the last %u finished", (const void *)bo, DARWIN_FINISHED);
   }
   pthread_mutex_unlock(&darwin_finished_lock);
}

/* ---- buffer objects ---- */
''')
