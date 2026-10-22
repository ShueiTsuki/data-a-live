# native_prelink

`native_prelink.py`, `emit_prelink.py` and `LICENSE` are copied unchanged from UniXNU's
`kits/NativePrelink` (github.com/UniXNU, commit deada14e, BSD-2-Clause-Patent, UniXNU
contributors). They copy selected original kexts out of an Apple kernelcache into the empty
`__PRELINK_*` segments of a natively built xnu and rebind their pointers by symbol name.

`tools/mkprelink.py` drives them for PANDORA and adds what this port needs on top.
