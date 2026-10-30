"""nmsym.py FILE OFFSET...: nearest preceding symbol (llvm-nm -n, demangled) for each offset (run in WSL)."""
import bisect
import subprocess
import sys

out = subprocess.run(['llvm-nm', '-n', '-C', sys.argv[1]], capture_output=True, text=True).stdout
addrs, names = [], []
for ln in out.splitlines():
    p = ln.split(' ', 2)
    if len(p) == 3 and p[1] in 'tT' and p[0].strip():
        addrs.append(int(p[0], 16))
        names.append(p[2])
for a in sys.argv[2:]:
    v = int(a, 16)
    i = bisect.bisect_right(addrs, v) - 1
    print('%s %s+%#x' % (a, names[i] if i >= 0 else '?', v - addrs[i] if i >= 0 else 0))
