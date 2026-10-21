# XNU patches

`patches/` currently holds the unchanged ios-sweet series (0001–0081 plus its three
staging scripts), copied from https://github.com/dipa-studio/ios-sweet at commit
425b25102b6a9ee4b68fb2b9ca5d6a086b8a4116. The first CI run only checks that this
series builds in our repository (machine config SWEET, xnu-8796 via
blacktop/darwin-xnu-build, macOS 13.5 KDK, Xcode 15 on macos-14).

Next: a PANDORA machine config with 4 KiB pages (no `__ARM_16K_PG__`), UniXNU's
native-4k patches, ios-fuxi's SM8550 patches as reference, and 17 Pro values.
