#!/usr/bin/env python3
"""air_specialize.py <module.ll> <function> <out.ll> name:type:hex ...

Specializes Apple's AIR text the way Metal does before compiling: each function constant's
initializer global gets its value, `air.is_function_constant_defined` folds to whether a value was
given, the predicate normalizer is the identity, and LLVM's `opt` (globalopt evaluates the static
initializer; then -O2) folds the rest. Prints which of the function's parameters survive in use.
Run in WSL only: the input is Apple's code."""
import re, subprocess, sys, os
src, fn, out = sys.argv[1], sys.argv[2], sys.argv[3]
vals = {}
for tok in sys.argv[4:]:
    name, ty, hx = tok.rsplit(':', 2)
    vals[name] = bytes.fromhex(hx)
t = open(src).read()
defined = set()
def init(m):
    g, name, lty = m.group(1), m.group(2), m.group(3)
    if name not in vals:
        return m.group(0)
    defined.add(g)
    b = vals[name]
    if lty == 'i8':
        v = 'i8 %d' % b[0]
    else:
        n = int(re.match(r'<(\d+) x i8>', lty).group(1))
        v = '%s <%s>' % (lty, ', '.join('i8 %d' % b[i] if i < len(b) else 'i8 0' for i in range(n)))
    return m.group(0).replace('externally_initialized constant %s undef' % lty, 'constant ' + v)
t = re.sub(r'^(@_Z\d+(\w+)\.MTL_FC_INIT_\w+) = internal [^\n]*?externally_initialized constant (i8|<\d+ x i8>) undef[^\n]*$', init, t, flags=re.M)
def isdef(m):
    return '%s = or i1 %s, false' % (m.group(1), 'true' if m.group(2) in defined else 'false')
t = re.sub(r'(%\d+) = tail call i1 @air\.is_function_constant_defined\(ptr addrspace\(2\) captures\(none\) (@[\w.]+)\)( #\d+)?', isdef, t)
t = re.sub(r'(%\d+) = tail call i8 @air\.normalize_function_constant_predicate\.i8\(i8 (%\d+)\)( #\d+)?', r'\1 = or i8 \2, 0', t)
missing = sorted(set(re.findall(r'@_Z\d+(\w+)\.MTL_FC_INIT_\w+ = ', t)) - set(vals))
t = re.sub(r'^target triple = .*$', 'target triple = "spir64-unknown-unknown"', t, flags=re.M)
tmp = out + '.pre.ll'
open(tmp, 'w').write(t)
T = os.environ.get('LLVM_BIN', '')
r = subprocess.run([os.path.join(T, 'opt'), '-S', '-passes=globalopt,default<O2>', tmp, '-o', out], capture_output=True, text=True)
if r.returncode:
    sys.exit('opt: ' + r.stderr[:2000])
o = open(out).read()
m = re.search(r'^define [^\n]*@%s\((.*)\)[^\n]*\{\n(.*?)^\}' % re.escape(fn), o, re.M | re.S)
params, body = m.group(1), m.group(2)
names = re.findall(r'(%\d+)(?:,|$)', params + ',')
used = [p for p in names if re.search(re.escape(p) + r'\b', body)]
print('constants not given:', ' '.join(missing) or 'none')
print('parameters used:', ' '.join(used))
print('instructions:', body.count('\n'))
dest = {'%31': 'dest_0(c0)', '%32': 'dest_1(c1)', '%33': 'dest_2(c2)'}
print('reads framebuffer:', ' '.join(dest[p] for p in used if p in dest) or 'none')
slots = re.findall(r'insertvalue <\{[^}]*\}> [%\w]+, <4 x half> ([^,]+), (\d)', body)
print('returns:', ' '.join('color%s=%s' % (i, 'unwritten(NaN)' if 'splat (half 0xH7E00)' in v else v.strip()) for v, i in slots))
