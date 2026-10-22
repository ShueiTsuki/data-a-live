"""Split QuartzCore default.pipelinelib/functions into its embedded one-function MTLB records."""
import os, struct, sys
src, out = sys.argv[1], sys.argv[2]
b = open(src, 'rb').read()
os.makedirs(out, exist_ok=True)
pos, n = 0, 0
while True:
    i = b.find(b'MTLB', pos)
    if i < 0:
        break
    size, = struct.unpack_from('<Q', b, i + 0x10)
    if size < 0x58 or i + size > len(b):
        pos = i + 4
        continue
    open(os.path.join(out, f'rec{n:03d}.metallib'), 'wb').write(b[i:i + size])
    n += 1
    pos = i + size
print(n, 'records')
