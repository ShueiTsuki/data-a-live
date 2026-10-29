#!/usr/bin/env python3
"""A submitted IB in a finished BO: logged with who submits it and who freed the BO, and left out of the submission.

post26 (kernel 0327's handle check): backboardd's submission had IB 39 of 41 at address 0+772 bytes with handle 0 (a
tu_bo already zeroed by darwin_bo_finish); the kernel refused the whole submission (an IB in no buffer) and Turnip
reported the device lost, so backboardd stopped drawing. pandora_bocheck.py only covers calls recorded into a
command stream (tu_cs_emit_ib); the IBs of a submission come from the command buffers' entries
(darwin_submit_add_entries). This applies the same check there: an entry whose BO has iova 0 is logged through
tu_darwin_stale_ib (the submitting call stack and, from the last 256 finishes, the stack that finished the BO) and
left out, so the rest of the submission still runs.

  pandora_bocheck_submit.py MESA_DIR      (after pandora_bocheck.py)

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
     '''   NativeAdrenoAbiSubmitBuffer *cmds = (NativeAdrenoAbiSubmitBuffer *)
      util_dynarray_grow(&submit->commands, NativeAdrenoAbiSubmitBuffer, num_entries);
   for (unsigned i = 0; i < num_entries; i++) {
      cmds[i] = (NativeAdrenoAbiSubmitBuffer) {
         .handle = entries[i].bo->darwin_handle,
         .iova = entries[i].bo->iova + entries[i].offset,
         .bytes = entries[i].size,
      };
   }
}''',
     '''   NativeAdrenoAbiSubmitBuffer *cmds = (NativeAdrenoAbiSubmitBuffer *)
      util_dynarray_grow(&submit->commands, NativeAdrenoAbiSubmitBuffer, num_entries);
   unsigned kept = 0;
   for (unsigned i = 0; i < num_entries; i++) {
      /* data-a-live (tools/turnip/pandora_bocheck_submit.py): an entry in a BO already finished is logged and
       * left out; the kernel would refuse the whole submission. */
      if (!entries[i].bo->iova) {
         tu_darwin_stale_ib(NULL, entries[i].bo, entries[i].offset, entries[i].size);
         continue;
      }
      cmds[kept++] = (NativeAdrenoAbiSubmitBuffer) {
         .handle = entries[i].bo->darwin_handle,
         .iova = entries[i].bo->iova + entries[i].offset,
         .bytes = entries[i].size,
      };
   }
   submit->commands.size -= (num_entries - kept) * sizeof(NativeAdrenoAbiSubmitBuffer);
}''')
