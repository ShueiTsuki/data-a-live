#!/usr/bin/env python3
"""Turnip's Darwin backend (UniXNU's tu_knl_darwin.cc) waits for a fence in calls of at most 100 ms each, not 1 ms.

dw_wait_fence_raw asked the kernel (NativeAdrenoMethodWaitFence) for 1 ms at a time: a wait of a frame's GPU work
(about 10 ms while the screen moves) took a call through IOKit for each millisecond, each a Mach message made, signed
and freed and a thread woken; r2n counted about 420 such polls a second timing out in backboardd. The kernel sleeps
until the GPU's fence interrupt or the timeout (patch 0216), as Linux's MSM_WAIT_FENCE, which Turnip's msm backend
gives the whole timeout at once. Each call now asks for the rest of the wait, at most 100 ms: the kernel's sleep is
uninterruptible, and 100 ms is the bound metal2vulkan's own waits keep (WAIT_SLICE). A wait already past its deadline
still looks once (0).

  pandora_wait.py MESA_DIR

Exact text edits, asserted; run once on a patched tree (local: the local Turnip build; CI: turnip.yml).
"""
import pathlib
import sys

p = pathlib.Path(sys.argv[1]) / 'src/freedreno/vulkan/tu_knl_darwin.cc'
s = p.read_text()
old = '''   for (;;) {
      uint64_t now = os_time_get_nano();
      uint64_t slice = 1000000; /* 1 ms */
      if (abs_timeout_ns != UINT64_MAX && abs_timeout_ns <= now)
         slice = 0;
'''
new = '''   for (;;) {
      uint64_t now = os_time_get_nano();
      /* data-a-live (tools/turnip/pandora_wait.py): the rest of the wait, at most 100 ms a call; the kernel sleeps
       * until the fence's interrupt or the timeout, as MSM_WAIT_FENCE takes the whole timeout at once. */
      uint64_t slice = 100000000; /* 100 ms */
      if (abs_timeout_ns != UINT64_MAX)
         slice = abs_timeout_ns > now ? MIN2(abs_timeout_ns - now, slice) : 0;
'''
assert s.count(old) == 1, ('dw_wait_fence_raw', s.count(old))
p.write_text(s.replace(old, new))
print('patched', p)
