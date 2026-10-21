# 17 Pro 原版启动镜像分析（2026-10-22）

来源：backup/sde.img（已整体哈希校验）里按分区表截出的 B 槽分区，存在 device/stock/。
工具：tools/gptextract.py（读分区表、截分区）、tools/bootimg_info.py（解析头部）。

| 分区 | SHA-256 前 16 位 | 结论 |
|---|---|---|
| boot_b | 560618ac1c253cea | 启动镜像头 v4，只装内核，无内存盘，命令行为空 |
| init_boot_b | 764dc9d56c24c630 | v4，只装 2.4 MB 的通用内存盘（Magisk 改过的就在这里） |
| vendor_boot_b | cd0c076def366241 | 厂商启动头 v4，装厂商内存盘 22 MB + 设备树合集 6.5 MB + bootconfig |
| dtbo_b | 844f05856b81aa86 | 设备树覆盖表（魔数 d7b7ab1e），ABL 按机型挑一个叠到设备树上 |

## boot_b 里的内核
- 未压缩的 arm64 Image，41.5 MB。头部：text_offset 0，image_size 40 MiB，flags 0xa = 4K 页、可放在内存任意位置、小端。
- 版本串：`Linux version 6.12.69-android16-6-g43b5ab7e247d-abogki561813157-4k`，2026-09-08 编译，和手机当前运行的内核一致（uname 相同）。
- 镜像末尾有 AVB 签名信息；bootloader 已解锁，临时启动时不会因为签名拒绝。

## vendor_boot_b
- 头里的加载地址：内核 0x8000、内存盘 0x1000000、tags 0x100、设备树 0x1f00000（都是相对 base 0 的值；红米上实测 ABL 不管这些，固定把内核放在 0x80080000，17 Pro 要实测）。
- 命令行：`video=vfb:640x400,bpp=32,memsize=3072000 erofs.reserved_pages=64 swinfo.fingerprint=pandora:16/OS4.0.1.0.XBLCN:user bootconfig`。
- 设备树合集另存为 device/stock/vendor_boot_b.dtb（ABL 从中挑一个再叠 dtbo，交给内核的就是我们之前从手机导出的 pandora-live.dtb）。

## 对第 1 步的意义
- `fastboot boot` 时只换 boot 镜像；vendor_boot、dtbo、init_boot 仍由 ABL 从手机上读。所以 ABL 交给我们程序的 x0 就是拼好的设备树，和 pandora-live.dtb 应该一样。
- 我们的启动镜像照 ios-fuxi 的做法（shim/fuxi-loader/build.sh）：
  - 只放一个"内核"：开头是标准 arm64 Image 头（偏移 56 处魔数 `ARM\x64`），image_size 必须覆盖整个文件；苹果内核和设备树作为数据直接嵌在这个 Image 里面（.incbin），不靠内存盘传。
  - `mkbootimg --header_version 4 --kernel Image`，不带内存盘。ios-fuxi 写了 `--os_version 16.0.0 --os_patch_level 2026-08`；原版 boot_b 的 os_version 字段是 0。
  - 第 1 步的程序很小，不嵌苹果内核，只在屏幕上打字。
- 待实测：ABL 把我们的 Image 放在哪个物理地址；fastboot 一次能传多大（ios-sweet 说红米的 ABL 用 `fastboot getvar` 会卡住，要谨慎）。
