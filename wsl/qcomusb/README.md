# qcomusb

USB 串口日志（DWC3 设备模式，CDC ACM）用的内核代码，编进内核的位置是 `bsd/dev/qcomusb/`，入口在 `wsl/pandora_usb.c`（启动参数 `pandora_usb=2 qcom-usb-debug=1`）。

## 来源

- 上游：[xiaoleGun/QcomXnuUsbDebug](https://github.com/xiaoleGun/QcomXnuUsbDebug)，MIT。
- 实际拷的是 [Lux12138/nabu-usb-cdc](https://github.com/Lux12138/nabu-usb-cdc) 里的 `module/`（提交 627c83a，许可证见 `LICENSE.nabu-usb-cdc`）。它在上游基础上加了“沿用固件的直通上下文”的 DMA 方式（`soc/QcomUsbBypass.c`）和上电准备步骤（`soc/QcomUsbPrepare.c`）。
- `usb/` 下的 DWC3 代码最早来自 m1n1，见 `usb/LICENSE.m1n1`。

拷贝用 `tools/qcomusb_import.py`：只拷内核用的 `include/ kernel/ soc/ usb/`，把引号形式的 `#include` 改成相对文件自己的路径，这样不需要往内核编译参数里加 `-I`。

## 我们改了什么

除了导入脚本做的 include 改写，源码里有四处改动，都标了 “Data A Live”：

1. `soc/Sm8850.h`：新文件，本机的平台配置。每个值的出处写在文件头里：设备树、主线 `gcc-kaanapali.c`，以及 usb1、spk6 两次实测。
2. `include/QcomUsbSoc.h` 和 `kernel/QcomXnuUsbDebug.c`：初始化里“关着中断等电脑识别”的时间改成 `QUD_INIT_ENUM_WAIT_MS`，默认还是 5 秒。我们是在系统已经跑起来以后才从线程里启动它，所以设成 0，识别交给 `pandora_usb.c` 的轮询线程完成。
3. `soc/QcomUsbBypass.c`：
   - SMMU 寄存器区的大小检查改成按映射窗口算。本机是 128 个全局页加 81 个上下文，共 0xd1000 字节，超过了原来写死的 0xd0000。
   - 固件留给流号 0x40 的上下文，只要翻译关着（SCTLR.M=0）、类型是 S1_TRANS_S2_BYPASS，就原样使用，不再改写成 SCTLR=0、CBAR=0x10000。
4. `kernel/QcomXnuUsbDebug.c`、`include/QcomXnuUsbDebug.h`：新增 `qcom_usb_debug_map()`，在开着中断时先把时钟、DWC3、SMMU 三个寄存器窗口映射好；初始化时发现已经映射过就跳过。原来的初始化在关中断的状态下调用 `ml_io_map`，会申请内存，系统跑起来以后内核断言不允许，usb2 那次就在 0.19 秒重启了。

## 本机的交接状态（usb1，`fastboot boot` 之后）

- 五个 USB 时钟全开，电源域开着，复位没被按住。
- DWC3 是设备模式，已经停下（RUN_STOP=0，DEVCTRLHLT=1）。
- SMMU：流号 0x40 指向第 3 号上下文，翻译关着。

只有用 `fastboot boot` 启动时，引导程序才会把 USB 留在这个状态。从开机菜单正常进 iOS 时还没有测过。
