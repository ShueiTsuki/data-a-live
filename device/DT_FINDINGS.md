# 17 Pro 设备树查询结果（进行中）

来源：device/pandora-live.dtb（2026-10-21 从手机 /sys/firmware/fdt 导出），查询工具 tools/dt.py。
状态：做到一半被打断（2026-10-21），下面"未查"的还没做。

## 已查到

| 项目 | 结果 | 和 ios-sweet（红米）比 |
|---|---|---|
| CPU | 8 核两组：cpu@0–500（6 个，capacity 0x700）+ cpu@10000、10100（2 个超大核，0x766）；compatible 只写 "arm,armv8"，核心型号要读 /proc/cpuinfo | 红米 6×A55 + 2×A76 |
| 启动其他核心 | /psci：arm,psci-1.0，method = "smc" | 相同 |
| 中断控制器 | GICv3：GICD 0x17000000（0x10000），GICR 0x17080000（0x200000），redistributor-stride 0x40000，带 ITS 0x17040000 | 红米 GICD 0x17a00000、GICR 0x17a60000、stride 0x20000 |
| 计时器 | arm,armv8-timer，19.2 MHz；PPI 13/14/11/10 → INTID 29/30/27/26（虚拟计时器 27，标准编号） | 红米是高通特殊编号（虚拟计时器 19） |
| UFS | /soc/ufshc@1d84000，qcom,ufshc；reg：ufs_mem 0x1d84000、ice 0x1d88000、mcq_sqd 0x1da5000、mcq_vs 0x1da4000；**有 iommus**（phandle 107，SID 0x60）；中断 SPI 265 | 红米 UFSHCI 2.1，没有 iommus，没有 MCQ |
| 电源管理总线 | /soc/arbiter@c400000，qcom,canoe-spmi-pmic-arb；两条总线 spmi@c426000、spmi@c437000；调试节点是 pmic-arb-debug-v8 | 红米是 v5 |
| 电源键 | pmk8850@0 / pon_hlos@1300（qcom,pmk8350-pon），reg 0x1300、0x800 | — |
| 重启原因 | /soc/qcom,msm-imem@14680000/restart_reason@65c | — |
| 主屏 | 面板 qcom,mdss_dsi_q200_42_02_0a_p20_prim_dsc_cmd：**命令模式**，DSC 压缩，1220×2656，30 bpp，24–120 Hz | 红米是视频模式 |
| 背屏 | qcom,mdss_dsi_q200_42_06_0a_sec_dsc_cmd：命令模式，572×904，LTPO | — |
| 屏幕缓冲区 | /reserved-memory/splash_region（label cont_splash_region）：0xfc800000，0x2b00000 | 红米 0x9c000000 |

## 有用的旁证
- Mu-Silicium 里有同芯片（SM8850，KaanapaliPkg）的一加机型 infiniti（unixnu_review/uefi-sheng/Platforms/OnePlus/infinitiPkg）：
  - 它的内存表里 Display_Reserved = 0xFC800000 / 0x02B00000，和我们的 splash_region 一模一样。
  - 它不用自己的显示驱动（USE_CUSTOM_DISPLAY_DRIVER = 0），直接往引导程序留下的缓冲区画 1272×2772 的画面。如果那台的屏幕也是命令模式，说明 ABL 交接后屏幕还会自动刷新——**未核实**（没查一加那台的面板模式，也没真机验证）。
  - 它的内存表可以当 SM8850 受保护区域的参考（HYP 0x80000000、PIL_Reserved 0x8AA00000 等）。

## 手机上只读实测（2026-10-21）
- /data：f2fs，挂在 /dev/block/dm-62 上（下面是 sda34 = userdata），挂载选项含 inlinecrypt。ro.crypto.state=encrypted，ro.crypto.type=file，ro.crypto.metadata.enabled=true：既有逐文件加密，又有整分区的元数据加密。所以存储上的原始块全是密文。
- 系统里没有 f2fs_io（/system、/vendor、/system_ext、/product 都没有）。电脑上有 NDK 27.2（D:\Android\ndk\27.2.12479018），可以自己编一个。
- CPU：8 个核心 CPU implementer 0x51（高通自研），part 0x002；核心 0–5 variant 0x2，核心 6–7 variant 0x3（两个超大核）。
- CPU 特性含 paca pacg（指针签名）、bti、sve2、sme、lrcpc3 等，是 ARMv9 级别。16K 页支不支持读不到，要第 1 步在小引导程序里查。

## 固定文件测试（第 2 小步，2026-10-22）
- 用 tools/pinfile 建了 /data/ios_pandora_test/test.bin（64 MB）：固定成功（lsattr 显示 C，即 f2fs 的固定标记），27 段，每段 2 MB 或其整数倍，块位置存在 device/test-extents.json。
- /data 的加密层（dm-62，名字 userdata）：`default-key, aes-xts-plain64`，目标设备 259:18（= sda34），偏移 0，覆盖 0–1000869848 扇区，即和 sda34 一一对应；密钥是硬件包装的（wrappedkey_v0）。所以 FIEMAP 给的位置就是 sda34 上的字节位置。
- compare：通过文件系统读到的全是零，分区上同一位置的原始数据 16384 块全都不同、没有一块是全零 → 原始数据是密文，证实了推测：iOS 镜像只能绕过加密层直接写到这些位置。
- 注意：F2FS_IOC_GET_PIN_FILE 返回的是"GC 挪动失败次数"，不是"是否固定"（Linux 6.12 fs/f2fs/file.c f2fs_ioc_get_pin_file）；判断是否固定要看 FS_NOCOW_FL。工具已改。

## 第二轮查询（2026-10-22）

内存：
- FDT 头里没有 memreserve 条目。
- 候选 A：0x8_c000_0000 起 5 GB；候选 B：0xa_0000_0000 起 8 GB。两段都完整落在 /memory 里，没有任何固定保留区；重叠的只有 debug_kinfo_region、dump_mem_region 两个"由 Linux 自己在全内存范围内挑位置"的动态区，我们不跑 Linux，不用管。和 ios-fuxi 在小米 13 上核对 0x8c0000000 那段的结论相同。

屏幕：
- 显示控制器 /soc/qcom,mdss_mdp@9800000：mdp_phys 0x9800000（0x1c8800）、vbif 0x9b16000、regdma 0x9b60000。
- 图层管线（qcom,sde-sspp-off，相对 0x9800000）：VIG 0x2c000/0x35000/0x3e000/0x47000，DMA 0x98000/0xa1000/0xaa000/0xb3000/0xbc000/0xc5000。CTL 0x20000 起 6 个，INTF 0x18e000 起 4 个，DSC 0x182000 起。
- 小米 13 上 ABL 实际扫描的地址 = 设备树 splash_region 地址（ios-fuxi 的 mkadt-fuxi.py 强制两者相同），并在引导程序里读 SSPP 寄存器核对：源地址 +0x14、每行字节 +0x24、尺寸 +0x00、格式 +0x30。17 Pro 的 splash_region 是 0xfc800000，大小和小米 13 一样（0x2b00000）。
- 每行字节数、像素格式要第 1 步在手机上读 SSPP 寄存器才能确定；按 1220 宽、每像素 4 字节估计是 4880 字节/行（未验证）。
- 注意（ios-fuxi probe.c 注释）：读显示控制器顶层寄存器（小米 13 上的 0xae00000）时，如果那部分供电时钟没开会让整个芯片重启；只读 SSPP 这些子块。

复位：
- 设备树里没有 PS_HOLD 节点；重启原因写在 IMEM 0x14680000 + 0x65c。重启用 PSCI SYSTEM_RESET（ios-sweet 在红米上验证可用）。

按键：
- 电源键：PMK8850 PON（pon_hlos@1300，SPMI 总线 c426000，SID 0），linux code 116。
- 音量减：同一个 PON 的 resin，code 114。
- 音量加：gpio_keys / vol_up，TLMM（pinctrl@f000000，qcom,canoe-tlmm）GPIO 101，低电平有效，code 115。

触摸（两个芯片，安卓里都有驱动在用）：
- 敦泰 focaltech_fts：SPI，QUP3 的 spi@1988000（spi15），中断 TLMM 201，avdd/iovdd 供电。
- 新思 synaptics_tcm：SPI，QUP4 的 spi@1a80000（spi19），中断 TLMM 158。
- 一个是主屏、一个是背屏，还没确定是哪个。办法：看 /proc/interrupts 里两个中断的计数，只摸主屏时哪个在涨。

存储地址转换：
- UFS 的 iommus 指向 /soc/apps-smmu@15000000（qcom,qsmmu-v500），流 ID 0x60；节点标了 dma-coherent 和 qcom,iommu-dma = fastmap。红米上没有这一层；UniXNU 的 SmmuTakeover.h 处理过同类问题。

## 还没确定的
- 屏幕缓冲区每行字节数和像素格式（第 1 步读 SSPP 寄存器）。
- 主屏用的是哪个触摸芯片。
- ABL 把我们的程序放在哪个地址；16K 页支不支持（第 1 步）。

## 存储探测结果（2026-10-22，真机，补丁 0114 `pandora_ufs_probe=1`，只读）
- apps SMMU（0x15000000）：sCR0=0x00210406（总开关开，USFCFG=1：没登记的设备一律拦截）；IDR1=0x60000051（4K 页，全局 128 页，81 个上下文）；IDR2=0x5211（输入/输出地址 36 位）；IDR7=0x24；sGFSR=0。
- 7 条有效登记。**SMR0 = 存储 0x60（mask 0），S2CR=0 → 上下文 0**；CB0：CBAR=0x1f000，CBA2R=1（64 位），SCTLR=0x0a9f00e0（M=0：不翻译，直通），TTBR0=0，TCR=0。其他：0x540→CB1，0x800/mask2（屏幕）→CB2，0x40→CB3，0xc0→CB4，0xb6/0xa3→CB5。
  → 引导程序已经给存储留了直通通道，存储控制器可以直接用物理地址（36 位以内，即 64 GB 以下）做 DMA，不用改 SMMU。
- UFS 主机（0x1d84000）：CAP=0xd5c7031f（32 个请求槽，支持 64 位地址，支持 MCQ）；MCQCAP=0x20e0071f；VER=0x400（UFSHCI 4.0）；HCS=0x10f（设备在、各列表就绪）；HCE=1；IS=0x400（UIC 命令完成位未清）；IE=0；UTRLBA=0xc25b1000（引导程序的请求表）；DBR=0；RSR=1；**MEM_CFG=0（没开 MCQ，用传统单门铃方式）**；MCQ_CFG=0x1f00。
  → 时钟开着（读寄存器没卡住），控制器已开启、就绪、没有未完成的请求。现有驱动的传统方式可以直接用。

## 存储交接状态（2026-10-22，真机，补丁 0116–0118 诊断）
- 时钟（Linux 主线 gcc-kaanapali.c 的偏移；设备树写 qcom,canoe-gcc，寄存器范围 0x1f4200 与驱动一致）：UFS GDSC=0xfa22f000、物理层 GDSC=0xfa222800（都开着）；aggre_axi/ahb/axi/aux/rx0/rx1/tx0/unipro=0x00000001、ice=0x00004001（都在运行，不是硬件自动开关）。
- 连接：交接时 M-PHY 4 条通道全部 HIBERN8（txfsm/rxfsm=1），PA_PWRMODE=0x11（双向快速模式），2 条通道，HS-G5；AHIT=0（自动休眠关）。所以门铃按下后命令一直不发出（DBR 一直为 1，CNR=0，IS=0）。之前看到的 CNR=1 是 UTRLCLR 取消造成的。
- 修法 1（0118）：DME_HIBERNATE_EXIT → UCCS、UHXS、UPMCRS=PWR_LOCAL，通道变成 5（HS burst）。
- 之后 LUN 0 的 TEST UNIT READY 回 CHECK CONDITION，sense 70/key 2 NOT READY/ASC 04 ASCQ 02（initializing command required）：存储芯片本身也被留在省电状态。
- 修法 2（0119）：对设备管理 WLUN 0xD0 发 START STOP UNIT，电源状态 ACTIVE（1b 00 00 00 10 00），和 Linux 恢复、Fuxi 初始化一样。

## /data 加密层与原始分区（2026-10-22，安卓里只读检查）
- 内核 6.12.69-android16 GKI 4k；CONFIG_BLK_DEV_WRITE_MOUNTED=y（允许写正在使用的分区）、CONFIG_BLK_INLINE_ENCRYPTION=y、CONFIG_DM_DEFAULT_KEY=y。
- /data = dm-62 `userdata`：`0-1000869848: default-key aes-xts-plain64 ... 259:18 0 ... sector_size:4096 iv_large_sectors wrappedkey_v0`（覆盖整个 sda34，无偏移）。f2fs 挂载选项含 inlinecrypt、discard、background_gc=on。
- sda34 的第二个占用者 dm-63 `cust`：`0-5533696: linear, 259:18 3186688`——小米把 cust 放在 userdata 内部一段连续区域，绕过 default-key 直接线性映射原始块（libfiemap 机制，/data/gsi/{dsu,ota,remount} 也属于它）。说明"userdata 里钉住文件 + 直接访问其原始块"是这台手机系统自己在用的做法。
- root 能以读写模式打开 /dev/block/sda34（只打开立即关闭，未写）。

## 触摸（2026-10-22，安卓里只读：/proc/bus/input/devices、getevent -p、/sys/bus/spi、设备树）
- 主屏 = **Synaptics TCM**（`synaptics,tcm-spi`，驱动 synaptics_tcm_spi），SPI 0x1a80000（qupv3_4，spi19），irq GPIO 158、reset GPIO 66；设备树 max-x 1220、max-y 2656、super-resolution-factor 100；安卓输入设备 `Xiaomi_Touch_Input_0`（ABS_MT 0..121999 × 0..265599）。
- 背屏 = FocalTech（`focaltech,fts`，驱动 focaltech_ts），SPI 0x1988000（qupv3_3，spi15），irq GPIO 201、reset GPIO 185；display-coords 90400×57200；`Xiaomi_Touch_Input_1`（904×572 ×100）。
- 所以 ios-fuxi 的 fuxi_touch（FTS）在 17 Pro 上对应背屏；主屏要新写 Synaptics TouchComm 驱动（第 7 步）。
- 按键：gpio-keys（音量+）、pmk8850 pon_hlos@1300 的 pwrkey / resin（电源、音量-），与之前设备树结论一致。
