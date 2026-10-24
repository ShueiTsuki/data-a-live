# 开机菜单（boot_b）

不接电脑也能在开机时选 iOS 或安卓。

## 开机时会怎样

- 正常开机：小米开机画面下方出现一行 `Volume up  iOS   5`，倒数 5 秒。
  - 这 5 秒里按音量上：进 iOS。
  - 不按：这一行被擦掉，开机画面恢复原样，进安卓。
- 插着充电器关机充电、静默开机等其他情况：不显示这一行，直接进安卓。
- iOS 里重启或长按电源强制重启：回到开机画面，默认进安卓。

注意：开机时一直按着音量上，会被引导程序当成进 recovery。要等这一行出现后再按。

## 写到哪里

只有当前槽的 `boot_b` 一个分区。里面依次是：菜单程序、iOS（和 `fastboot boot` 用的同一种载荷，去掉测试用的定时重启和抓屏）、原装安卓内核（从原装 boot_b 原样拷贝）、原装签名信息（安全补丁日期 2026-10-01、系统版本 16，原样保留）。

安卓的其他分区（init_boot 里的 Magisk、vendor_boot、dtbo、vbmeta）一个都不动。

## 做镜像（只在电脑上，不碰手机）

```
bootmgr/build.sh                 # 正式镜像：只在正常开机时显示
bootmgr/build.sh --any-mode      # 测试镜像：fastboot boot 时也显示
python -I bootmgr/test/fdt_test.py   # 用手机的设备树测"认开机模式"那段代码
```

输出 `bootmgr/out/boot-menu.img`（和分区一样大）和 `bootmgr/out/layout.txt`。

## 先不写入地测试

`fastboot boot bootmgr/out/boot-menu.img` 只放进内存，重启就没了。安卓和 iOS 两条路都先这样测通，再考虑写入。

## 写入和撤销

- 写入：`fastboot flash boot_b bootmgr/out/boot-menu.img`
- 撤销：`fastboot flash boot_b device/stock/boot_b.img`（原装备份，sha256 `560618ac…`，和 `backup/sde.img` 里的 boot_b 一致）
- 写入后如果安卓起不来：音量下 + 电源进 fastboot（引导程序没动，这条路一直在），执行撤销命令。
- 系统更新会换槽并写入新的 boot，菜单自然消失，安卓照常。

## 保护

iOS 的系统盘、日志、固件包这三个文件的位置是做镜像时固定进去的。文件被删、重建或被挪动后再进 iOS，会写坏安卓的数据。所以：

1. 三个文件加"不可改动"标记（`chattr +i`），安卓里删不掉、改不了。主会话的工具（`pinfile rawwrite`）是直接写存储块的，不受影响。
2. 看门模块 `guard/`（Magisk 模块，`python -I bootmgr/guard/build_module.py` 打包成 `out/pandora_bootmgr_guard.zip`）：开机时和之后每分钟核对三个文件还钉着、位置的校验值和 boot 里设置页记的一样。对不上就把设置页的"允许进 iOS"改成 0、写上原因，菜单之后只显示 `iOS off: 原因`，直接进安卓。它不会再打开，要重新做镜像写入。日志在 `/data/adb/modules/pandora_bootmgr_guard/guard.log`。
   电脑上的模拟测试：`MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -- bash /mnt/d/DeepSeekHarness/claude/ios_pandora/bootmgr/test/guard_test.sh`
3. 还没做（主会话的内核）：写日志、挂载系统盘之前核对开头的标志。
