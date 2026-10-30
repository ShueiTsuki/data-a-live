#!/usr/bin/env python3
"""darwin_bo_finish zeroes the tu_bo before its kernel slot can be freed, as msm_bo_gem_close does.

The Darwin backend keeps its tu_bo structs in the device's sparse array indexed by the kernel's buffer slot
(tu_device_lookup_bo(dev, gem_handle), gem_handle = the slot + 1), and zeroed a finished BO's struct only at the end
of darwin_bo_finish, after unlocking the retire queue. The darwin_reap_locked call just before that frees the BO's
kernel slot at once when its fence has already passed (BoFree), and the kernel hands the lowest free slot to the next
BoAlloc. A thread that allocates in that window (it waits on the retire lock in darwin_collect_retired, and the
unlock wakes it) gets the same slot, so darwin_bo_init fills the same struct with its new BO; the finishing thread's
memset then zeroes that new BO under its owner. The owner's command stream keeps writing through its own pointers,
but every address taken from the struct afterwards is 0 plus an offset: post23's CP_INDIRECT_BUFFER to 0+202,
post26's submitted IB with handle 0 at 0+772, post27's draw states at 0x2d4..0x384 and its fault at 0x200, and the
earlier hangs in an IB2 at 0. msm_bo_gem_close copies the GEM handle, zeroes the struct, then closes the handle.

This moves the memset into the retire lock, after the BO's fields are copied into its retire entry and before the
reap that may free its slot; with the queue full (the device is lost and the slot kept) it is zeroed there too.

  pandora_bo_finish_order.py MESA_DIR

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


edit('tu_knl_darwin.cc',
     '''      retire->entries[slot] = {bo->darwin_handle, bo->iova, bo->size, bo->map, bo->darwin_token, fence};
      ++retire->count;
      ++retire->enqueued;
      darwin_reap_locked(dev, retire);
   } else {
      /* No reusable range is reported on error. The kernel still owns the handle
       * and view; connection teardown must drain them, even if this record cannot fit. */
      retire->failure = vk_device_set_lost(&dev->vk, "deferred BO queue remains full; retaining native allocation");
   }
   pthread_mutex_unlock(&retire->lock);

   /* The BO structs live in a sparse array: zero the entry to mark it free. */
   memset(bo, 0, sizeof(*bo));
''',
     '''      retire->entries[slot] = {bo->darwin_handle, bo->iova, bo->size, bo->map, bo->darwin_token, fence};
      ++retire->count;
      ++retire->enqueued;
      /* data-a-live (tools/turnip/pandora_bo_finish_order.py): the BO structs live in a sparse array indexed by
       * the kernel slot: zero the entry before the reap below can free that slot, which the next BoAlloc of any
       * thread may get and fill this same struct with (msm_bo_gem_close zeroes before GEM_CLOSE likewise). */
      memset(bo, 0, sizeof(*bo));
      darwin_reap_locked(dev, retire);
   } else {
      /* No reusable range is reported on error. The kernel still owns the handle
       * and view; connection teardown must drain them, even if this record cannot fit. */
      retire->failure = vk_device_set_lost(&dev->vk, "deferred BO queue remains full; retaining native allocation");
      memset(bo, 0, sizeof(*bo));
   }
   pthread_mutex_unlock(&retire->lock);
''')
