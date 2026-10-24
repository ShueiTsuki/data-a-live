# 开机菜单（boot_b）

不接电脑也能在开机时选 iOS 或安卓。菜单只写一次；以后换 iOS 只换"格子"里的东西。

## 开机时会怎样

- 正常开机：小米开机画面下方出现一行 `Volume up  iOS   5`，倒数 5 秒。
  - 这 5 秒里按音量上：进 iOS。
  - 不按：这一行被擦掉，开机画面恢复原样，进安卓。
- 插着充电器关机充电、静默开机等其他情况：不显示这一行，直接进安卓。
- iOS 里重启或长按电源强制重启：回到开机画面，默认进安卓。
- 格子里没有 iOS、iOS 入口被看门模块关掉、或格子内容损坏：这一行显示 `iOS off: 原因`，然后进安卓。

注意：开机时一直按着音量上，会被引导程序当成进 recovery。要等这一行出现后再按。

## boot_b 里放什么

引导程序每次开机把 boot_b 里的内核部分整个读进内存交给菜单。这部分依次是：

1. 菜单程序和设置页（倒数几秒、格子在哪）。
2. 原装安卓内核，2 MiB 处，从原装 boot_b 原样拷贝。
3. iOS 格子（约 54 MiB）：一页格子头（标记、是否允许进 iOS、大小、校验值、三个文件位置的指纹），后面是用电脑 `fastboot boot` 启动的那一整个东西（`shim/loader/out/loader-boot.img` 里的启动小程序 + iOS）。

菜单不看格子里是哪个版本：按了音量上，核对格子头和校验值，就像引导程序那样跳进去。用电脑能启动的版本，放进格子就能从菜单启动。

最后是原装签名信息（安全补丁日期 2026-10-01、系统版本 16，原样保留）。安卓的其他分区（init_boot 里的 Magisk、vendor_boot、dtbo、vbmeta）一个都不动。

## 换 iOS（菜单不动）

```
bootmgr/update_ios.sh                  # 只检查，不写
bootmgr/update_ios.sh --yes            # 把 shim/loader/out/loader-boot.img 放进格子
bootmgr/update_ios.sh --loader 文件 --yes
```

手机在安卓里、有 root。它会：

1. 读手机上格子的位置和三个文件现在的位置。
2. 核对这个 iOS 版本记的三个文件位置和手机上的一致，不一致就拒绝。
3. 去掉测试用的启动参数（定时重启、抓屏）。
4. 写入：先清空格子头，再写内容，最后写格子头。中途断了，格子就是空的，进安卓不受影响。
5. 读回来核对；格子以外的部分写前写后校验值必须一样。

格子比现在的 iOS 大约 2.3 MiB。以后装不下时，在格子里放"自带解压的小程序 + 压缩后的 iOS"，菜单不用改。

## 做镜像（只在电脑上，不碰手机）

```
bootmgr/build.sh --slot-from bootmgr/out/loader-snapshot.img      # 正式镜像，格子里先放一个 iOS
bootmgr/build.sh --slot-from 文件 --any-mode --wait-ms 9000         # 测试镜像：fastboot boot 时也显示，并显示判断用的数字
```

输出 `bootmgr/out/boot-menu.img`（和分区一样大）和 `bootmgr/out/layout.txt`。

电脑上的测试：

```
python -I bootmgr/test/fdt_test.py     # 认开机模式（用手机的设备树）
python -I bootmgr/test/sum_test.py     # 菜单和打包工具算的校验值一致
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash /mnt/d/DeepSeekHarness/claude/ios_pandora/bootmgr/test/guard_test.sh
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash /mnt/d/DeepSeekHarness/claude/ios_pandora/bootmgr/test/update_test.sh
```

## 先不写入地测试

`fastboot boot bootmgr/out/boot-menu.img` 只放进内存，重启就没了。用 `--any-mode` 的测试镜像。

## 写入和撤销

- 写入：`fastboot flash boot_b bootmgr/out/boot-menu.img`
- 撤销：`fastboot flash boot_b device/stock/boot_b.img`（原装备份，sha256 `560618ac…`，和 `backup/sde.img` 里的 boot_b 一致）
- 写入后如果安卓起不来：音量下 + 电源进 fastboot（引导程序没动，这条路一直在），执行撤销命令。
- 系统更新会换槽并写入新的 boot，菜单自然消失，安卓照常。

## 保护

格子里的 iOS 会读写三个文件（系统盘、日志、固件包）所在的存储块，位置是编译时记下的。文件被删、重建或被挪动后再进 iOS，会写坏安卓的数据。所以：

1. 三个文件加"不可改动"标记（`chattr +i`），安卓里删不掉、改不了。主会话的工具（`pinfile rawwrite`）是直接写存储块的，不受影响。
2. 看门模块 `guard/`（Magisk 模块，`python -I bootmgr/guard/build_module.py` 打包成 `out/pandora_bootmgr_guard.zip`）：开机时和之后每分钟核对三个文件还钉着、位置的指纹和格子头里记的一样。对不上就把格子头的"允许进 iOS"改成 0、写上原因。之后菜单只显示 `iOS off: 原因`，直接进安卓。再用 `update_ios.sh` 放一个和现在位置相符的 iOS 进去，就重新打开。日志在 `/data/adb/modules/pandora_bootmgr_guard/guard.log`。
3. 还没做（主会话的内核）：写日志、挂载系统盘之前核对开头的标志。
