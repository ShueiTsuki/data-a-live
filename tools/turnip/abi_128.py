#!/usr/bin/env python3
"""NativeAdrenoABI.h (version 1, UniXNU NativeAdreno b686b90) with 128 indirect buffers per submission
instead of 16, for both the kernel (wsl/adreno/NativeAdrenoABI.h) and Turnip (.github/workflows/turnip.yml).

  abi_128.py HEADER

With 16, Turnip's Darwin backend splits a larger submission into kernel submissions of 16, and the kernel
frames each one (thread sync, wait for idle, BV timestamp, BR wait). bb20: the A840 then stopped with BR
and BV each inside a user IB, 57 fences pending, the last ones in runs of 16, 16, 6. Linux a7xx_submit
puts all of a submission's IBs in one frame; 128 keeps Turnip from splitting what backboardd submits.
The submission grows from 416 to 3104 bytes (still inband: under 4096). Exact text edits, asserted.
"""
import pathlib
import sys

p = pathlib.Path(sys.argv[1])
s = p.read_bytes().decode()
for old, new in (
        ('    NativeAdrenoAbiMaxBuffers = 16,', '    NativeAdrenoAbiMaxBuffers = 128,   /* ios-pandora: 16 upstream (tools/turnip/abi_128.py) */'),
        ('static_assert(sizeof(NativeAdrenoAbiSubmit) == 416, "ABI submit layout");',
         'static_assert(sizeof(NativeAdrenoAbiSubmit) == 3104, "ABI submit layout");'),
):
    assert s.count(old) == 1, old
    s = s.replace(old, new)
p.write_bytes(s.encode())
print('edited', p)
