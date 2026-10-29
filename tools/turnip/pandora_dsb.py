#!/usr/bin/env python3
"""A DSB SY where Turnip hands command streams to the GPU: at vkEndCommandBuffer, and before each kernel submission.

Turnip's Darwin backend maps buffers write-combined (Normal non-cacheable): stores to them may reach memory in any
order, and the GPU reads memory, not the CPU's write buffers. The command streams are written on the thread that
records the command buffer and submitted from another (metal2vulkan's scheduler); the kernel's barriers before it
rings the GPU run on the submitting core. post15 (two clusters, kernel 0320: recording on the prime cores, the
scheduler on the others) hung on a CP_INDIRECT_BUFFER whose address had its low word and size but not its high
word (the IB2 at 0xed42000 in backboardd's window at 0x1_00000000): the GPU had seen part of the packet. A DSB SY
on the recording thread when it ends the command buffer completes all its stores before anything is handed on;
one on the submitting thread before the call does the same for the submission's own streams.

  pandora_dsb.py MESA_DIR

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


edit('tu_cmd_buffer.cc',
     '''   tu_cs_end(&cmd_buffer->draw_epilogue_cs);

   return vk_command_buffer_end(&cmd_buffer->vk);
}
TU_GENX(tu_EndCommandBuffer);''',
     '''   tu_cs_end(&cmd_buffer->draw_epilogue_cs);

#if defined(__APPLE__) && defined(__aarch64__)
   /* data-a-live (tools/turnip/pandora_dsb.py): the streams are in write-combined memory and another thread may
    * submit them; complete this thread's stores before the command buffer leaves it. */
   __asm__ volatile("dsb sy" ::: "memory");
#endif

   return vk_command_buffer_end(&cmd_buffer->vk);
}
TU_GENX(tu_EndCommandBuffer);''')

edit('tu_knl_darwin.cc',
     '''   for (unsigned attempt = 0; attempt < 64; attempt++) {
      NativeAdrenoAbiSubmitReply reply = {};
      kern_return_t kr = dw_call(NativeAdrenoMethodSubmit, &req, sizeof(req), &reply, sizeof(reply));''',
     '''   /* data-a-live (tools/turnip/pandora_dsb.py): complete this thread's stores to the write-combined streams first. */
   __asm__ volatile("dsb sy" ::: "memory");
   for (unsigned attempt = 0; attempt < 64; attempt++) {
      NativeAdrenoAbiSubmitReply reply = {};
      kern_return_t kr = dw_call(NativeAdrenoMethodSubmit, &req, sizeof(req), &reply, sizeof(reply));''')
print('pandora_dsb: 2 edits')
