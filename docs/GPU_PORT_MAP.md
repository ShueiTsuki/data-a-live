# UniXNU Adreno 栈 → 我们的 iOS 16 / Adreno 840 移植对照（只读调研）

日期 2026-10-23。只读了代码，没碰手机，没改任何克隆仓库。
标记：**[读到]** = 亲眼读到的代码或数据，附 文件:行；**[推断]** = 由读到的东西推出来的，没在机器上验证。

路径简写：
- `U/` = `D:\DeepSeekHarness\claude\unixnu_review\gpu\`（七个组件的独立克隆；NativeAdreno 最新提交 b686b90，2026-10-23）
- `S/` = `D:\DeepSeekHarness\claude\unixnu_review\UniXNU-sheng\package\`（sheng 板级包，**真正跑通 GMU 的代码在这里**）
- `LX/` = 上次调研的 Linux 源码 `...\0c6fe11f-...\scratchpad\lx\`（`a6xx_gmu.c`、`a6xx_hfi.c` 等；`LX/fetched/` 有 gpucc/gcc/gxclkctl-kaanapali.c、kaanapali.dtsi 在 `LX/`）
- `KG/` = 高通 KGSL 源 `...\scratchpad\kgsl_src\gfx-kernel.lnx.16.0.r14-rel.*`
- `DT` = 我们手机的安卓设备树 `D:\DeepSeekHarness\claude\ios_pandora\device\pandora-live.dts`

---

## 0. 先说结论

1. **UniXNU 的 GMU 冷启动不是由 `NativeAdrenoService.cpp` 跑的。** 那个文件里 `gpu_context_ready_` 只被初始化为 false，从不置 true（`U/NativeAdreno/NativeAdrenoService.cpp:66,420`），所以它的 CP 路径不会走到。真正在 sheng 上跑通的是板级包里的 `S/GpuPowerProbe.cpp`：一个按启动参数 `sheng-gpu-power=1..9` 分级的内核线程，由 `S/LegacyDisplayProbe.cpp:492` 在桌面起来后调用 [读到]。我们要借的主要是这条路径和它调用的几个纯 C 小库。
2. **GMU 启动这一层，A740 → A840 的差别比想象的小。** GMU 的大部分寄存器（CM3_SYSRESET、FW_INIT_RESULT、HFI_* 、ITCM/DTCM 起点、HOST2GMU、AO 中断）在 gen8 上偏移不变；变的是：不再由 CPU 写 RSC/PDC 序列、不写 TCM_RET_CNTL、FAL 和 GENERAL_8/9/10 换了地址、AHB 围栏下限 0x8a0→0x8c0、GBIF 数值不同、芯片号不同；以及第一条 HFI 消息从"性能表 id 4"换成"通用表 id 15"（见第 3 节）。
3. **真正要重写的是"上电和时钟"那一层**：SM8850 的 GPUCC 寄存器偏移、PLL 类型、GDSC 位置全变了（第 3.3 节）。这部分 UniXNU 的 `S/GpuPowerBoard.h` 只能当"写法模板"，数值一个都不能用。
4. **第一步建议**：一个内核补丁，启动参数 `pandora_gpu=<级别>` 分级，第一次上机只开级别 1（纯读）。等读出来的状态看过了，再用同一个补丁的级别 2–4 依次打开 CX 电源/时钟、GMU 时钟、GMU 冷启动 + 第一条 HFI 回复（第 4 节）。

---

## 1. 各组件：依赖什么、能不能直接用

| 组件 | 许可 | 依赖（UniXNU 专有 / Darwin 23 / iPadOS 17） | 归类 |
|---|---|---|---|
| **ArmSmmu4K**（`U/ArmSmmu4K/*.c`） | MIT | 只依赖 `<stdint.h>/<stddef.h>`，MMIO 通过回调传入（`ArmSmmu4KProgram.h` 的 `ArmSmmu4KIo`）[读到 `U/ArmSmmu4K/ArmSmmu4K.h:4-5`，`ArmSmmu4KProgram.h:11-19`] | **基本可直接用**。它是通用的 ARM SMMUv2 4K 页表/上下文/流匹配库，不含 A740 数据。我们的内核也用了 UniXNU 的原生 4K 页补丁（`xnu/patches/0103..0105`），页大小前提一致。要改的只有板级参数：ACTLR，手机 DT 给 GPU SMMU 的 SID 0/1/2/4/5/7 都是 `0x32b`（DT:22453），sheng 用 `0x301`（`S/GpuPowerProbe.cpp` gpuSetActlr 一段）。 |
| **AdrenoFirmware**（`U/AdrenoFirmware/`） | BSD-2-Clause-Patent | 纯 C，AArch64 小端，无内核 API | 分三块看：<br>• `AdrenoFirmware.c`（解析 GMU 固件的 block_header）**可直接用**：格式就是 Linux/KGSL 共用的 `{addr,size,type,value,data}`，gen8 不变 [读到 `AdrenoFirmware.c:10-69`，对照 `KG/adreno_gen8_gmu.c:398-455`]。<br>• `AdrenoHfi.c`（HFI 三条队列的收发）**可直接用**：队列表头 `168/6/12/3/3`、队列头 `type=0xa00|q`、消息头 `seq<<20 | size<<8 | id` 都与 Linux a8xx 共用的 `a6xx_hfi_init()` 一致 [读到 `AdrenoHfi.c:29-45` 对照 `LX/a6xx_hfi.c:1031-1102`、`LX/a6xx_hfi.h:57-66`]。注意它把回包第 3 个字当"错误码"（`AdrenoHfi.c:91`），对普通 ACK 正确，但对 KGSL 的 GET_VALUE 回包（`{hdr, req_hdr, data[16]}`，`KG/adreno_hfi.h:759-763`）会误判，所以第一条消息别选 GET_VALUE。<br>• `AdrenoHfiStartup.c`（启动消息序列）**要为 gen8 改**：头文件自己写明"A8xx 表格式不在范围内"（`AdrenoHfiStartup.h:8`）。性能表要换成 MSG_TABLE（见 3.2）；带宽表（id 3）、CORE_FW_START（14）、START（10）格式不变。<br>• `AdrenoHfiVotes.c`（ARC 档位和 BCM 带宽票的算法）**可直接用**，算法与 Linux `a6xx_gmu_rpmh_arc_votes_init` / `a6xx_generate_bw_table` 一致；gen8 只是多一列 MX 依赖票（`LX/a6xx_gmu.c` `a6xx_gmu_rpmh_dep_votes_init`：`(0x3fff<<14)|(index<<8)|0xff`），要新加一个小函数。 |
| **sheng 板级 GMU 代码**（`S/ShengGmu.c/.h`、`ShengGmuVotes.c`、`ShengGmuCommandDb.c`、`GpuPowerBoard.h`、`GpuPowerProbe.cpp`） | BSD-2-Clause-Patent | `ShengGmu.c` 纯 C，但依赖**生成头** `ShengGmuSource.h`，由 `UniXNU-sheng/scripts/prepare-sheng-gmu.py` 从固定版本的 Linux `a6xx_gmu.c`/XML/catalog/DT 抽出来（`prepare-sheng-gmu.py:9-10,63-91`）；`GpuPowerProbe.cpp` 用 IOKit（`IODeviceMemory::withRange`、`IOMemoryMap`）、`kernel_thread_start`、`PE_parse_boot_argn`、`SHA256_*`，以及 UniXNU 的引导进度行 `boot_progress_ext` | **流程和结构可照搬，A740 数据全部要换**：<br>• `ShengGmu.c` 的 A740 专有处：`count!=15`（固件块数，`:98`）、19 条 RSC/PDC 写（`:113-116`，gen8 不需要）、`TCM_RET_CNTL` 检查和写 1（`:104-105,120-122`，gen8 不写）、FAL 用 A6XX 变体地址（`:118-119`）、`GENERAL_8/10` 用 A7XX 变体地址（`:169-170`）、围栏下限 `0x8a0`（`:167,183`）、`GBIF_QSB_SIDE0..3=0x71620`（`:171`）、芯片号 `0x43050a01` 与固件哈希/大小（`:70,88`）、五个窗口（含 PDC `0x0b280000`）的物理地址固定在生成头里（`:275-276`）。<br>• `GpuPowerBoard.h` 全是 SM8550 的 GCC/GPUCC 偏移和 Lucid OLE PLL 配置（`:14-49,222-300`），**要重写**。<br>• `ShengGmuCommandDb.c` 解析 AOP cmd-db 的逻辑**可用**（地址 `0x81c60000`、大小 `0x20000` 与我们手机 DT:394-397 相同）；我们自己的 `pandora_adsp.c` 里也已有 `cmddb_find()`（`xnu/patches/0144:207-232`）。<br>• `ShengGmuVotes.c` 的 OPP 表来自 sheng DT，要换成我们手机的 GPU/GMU 频点。 |
| **NativeAdreno**（`U/NativeAdreno/`） | MIT | IOKit C++（`IOService`、`IOUserClient`、`IOBufferMemoryDescriptor::inTaskWithPhysicalMask`、`OSSharedPtr`）；私有内核符号 `pmap_set_cache_attributes`、`pmap_cache_attributes`、`phystokv`、`ml_io_map_unmappable`、`mac_iokit_check_open` 等。这些在我们 xnu-8796 树里都有 [读到：WSL `/opt/work/tree` 中 `osfmk/arm64/machine_routines.c:1579`、`osfmk/device/iokit_rpc.c` 等]。依赖 iPadOS 17 的部分：`NativeAdrenoAccelerator.cpp` 继承 `IOGPUFamily` 的 `IOAcceleratorES`、类名故意叫 `AGXAccelerator`/`AGXDeviceUserClient` 以通过 iPadOS 17 的沙箱配置（`NativeAdrenoAccelerator.cpp:2-25`，`NativeAdrenoPublish.cpp:10-11,663-672`） | 分层：<br>• 内存/VM/BO/GMU 内存布局/SMMU 上下文（`NativeAdrenoMemory`、`NativeAdrenoVm`、`NativeAdrenoGmuMemory`、`NativeAdrenoGmuContext`、`NativeAdrenoSmmuContext`、`NativeAdrenoGpuContext`）**基本可用，需小改**（板级参数、ACTLR）。GMU 地址布局（icache 0x4000、dummy 0x60000000、debug 0x60400000、log/hfi 紧跟 dummy）与 gen8 一致 [读到 `NativeAdrenoGmuMemory.cpp:24-30`，对照 `LX/a6xx_gmu.c:2326-2347`、`KG/adreno_gen8_gmu.c:27-52`]。<br>• CP/环/中断/计时器（`NativeAdrenoRing.cpp:13-18`、`NativeAdrenoCpInit.cpp:14`、`NativeAdrenoIrq.cpp:11-15`、`NativeAdrenoTimer.cpp:14-17`、`NativeAdrenoA740HwInit.h`、`NativeAdrenoPwrupRegs.h`）**要按 gen8 重写寄存器**：例如 `RBBM_STATUS 0x210→0x012`、`RBBM_INT_0_MASK 0x38→0x62`、`CP_SQE_CNTL 0x808→0x815`、`CP_SQE_INSTR_BASE 0x830→0x816`、`CP_ALWAYS_ON_COUNTER 0x980→0x8e7`，中断状态拆成 GLOBAL/PIPE 两组 [读到 `KG/gen8_reg.h:44,54-57,758-812,847`]。<br>• 用户接口与 Metal 发现（`NativeAdrenoUserClient`、`NativeAdrenoPublish`、`NativeAdrenoAccelerator`）**要按 iOS 16 适配**；我们树里已有 ios-fuxi 带来的 `iokit/Kernel/arm/FuxiAdrenoUserClient.cpp`（NativeAdreno ABI v2 的一部分，只在 `FUXI` 宏下编译）可作起点。 |
| **AdrenoPM4**（`U/AdrenoPM4/`） | MIT | 纯 C | `AdrenoPM4.c`（type4/type7 包头编码）**可直接用** [推断：gen8 仍用同样的 PKT4/PKT7 包头，KGSL gen8 未见改动，没逐位核对]。`AdrenoConstants/Raster/Shader/VPC.c` 都只接受 A740 芯片号（例如 `AdrenoShader.c:35`、`AdrenoVPC.c:29`），**要为 gen8 重写或不用**（运行时着色器状态由 Turnip 生成，这几个主要服务离线路径）。 |
| **AirNir**（`U/AirNir/`） | MIT（含 SoftFloat 许可） | 主机工具，依赖 Mesa（`MesaBackend.c:24-27` 用 `fd_dev_id{chip_id}` 选 GPU） | 离线编译器，不是运行时路径（README）。**换芯片号即可试**，前提是所用 Mesa 支持 gen8 [推断]。第一阶段用不上。 |
| **NativeMetal**（`U/NativeMetal/`） | MIT；metal2vulkan 是 GPL/LGPL | iPadOS 17 的 `MTLAddDevice`、IOGPU/IOAccelerator 私有接口、QuartzCore 私有调用 | **要按 iOS 16 适配**；具体差异已在 `GPU_PLAN.md` 第 3 节和 ios-fuxi N1 的对照里。与本次"第一步"无关。 |
| **NativeMobileDisplay**（`U/NativeMobileDisplay/`） | BSD-2-Clause-Patent | 继承苹果的 `IOMobileFramebuffer`（`NativeMobileDisplay.cpp:7`），配合 UEFI GOP 帧缓冲 | 我们已有自己的 `SweetFramebuffer.cpp`/AppleCLCD 路线；**只作参考**，以后做"GPU 画好交给显示"时看它的 swap 回收逻辑。 |

---

## 2. NativeAdreno 作为 XNU 驱动的结构，以及怎么对到我们的构建

### 2.1 UniXNU 怎么编、怎么加载 [读到]

- 不是 kext 加载，是**编进内核**。`Info.plist` 只是记录（`U/NativeAdreno/Info.plist:10-22` 写了三个 personality：`NativeAdrenoGpuService`、`NativeAdrenoSmmuService`、`NativeAdrenoGmuService`，都匹配 `IOPlatformDevice`）。文档原话：内核用的是编进 `iokit/KernelConfigTables.cpp` 的 personality（`src/docs/drivers/xnu-drivers.md` "Source layout" 与 "Add a personality" 两节）。
- 编法：脚本读内核构建留下的 `.cpo.json` 编译记录，把源文件替换成驱动源文件、沿用同一套编译参数，编成 `.o`，`ar` 成静态库，链接内核时 `-Wl,-force_load`（`xnu-drivers.md` "Compile against the kernel ABI"、"Archive"、"Link into the kernel"）。
- sheng 的板级清单里，内核步骤依次跑 `build-arm-driver.py NativeAdreno`、`build-native-adreno-kits.py`、`build-native-adreno-accelerator.py`（`UniXNU-sheng/board.toml:56-58`）。`build-native-adreno-kits.py` 把 `ShengGmu*.c`、`AdrenoHfi*.c`、`AdrenoFirmware.c`、全部 `ArmSmmu4K*.c`、`AdrenoPM4.c` 和大部分 `NativeAdreno*.cpp` 编进 `libNativeAdreno.a`（`:10-19`），**还把 GMU、SQE、zap 三个固件用 `.incbin` 直接嵌进内核**（`:55-69`）。
- `NativeAdrenoAccelerator.cpp` 单独编，它的静态初始化被挪出 `__mod_init_func`，等原版 `IOGPUFamily` kext 加载后再由板级探测手动调用（`NativeAdrenoAccelerator.cpp:5-7`，`build-native-adreno-kits.py:20`）。
- 实际的 GPU 上电/GMU 启动由 `S/GpuPowerProbe.cpp` 里的 `sheng_gpu_power_probe()`（`:1904`）起一个内核线程 `probeThread`（`:1874`）完成，它在桌面起来约 10 秒后运行，结果写到屏幕上的诊断行。调用点是 `S/LegacyDisplayProbe.cpp:492`。

### 2.2 IOService 类

| 类 | 文件 | 作用 |
|---|---|---|
| `NativeAdrenoGpuService : IOService` | `NativeAdrenoService.cpp:55-89` | 匹配 `compatible` 含 `qcom,adreno`（`:91-105`）；映射前两个 reg 窗口；查找 GMU/SMMU 服务；在 `sheng-gpu-bringup=1` 时走一遍 SMMU 上下文 + GMU 冷启动（`:302-516`）；没起 CP 就拒绝发布（`:517-524`） |
| `NativeAdrenoGmuService : IOService` | `NativeAdrenoGmuService.cpp` | 匹配 `qcom,adreno-gmu`，只映射窗口、不访问 |
| `NativeAdrenoSmmuService : IOService` | `NativeAdrenoSmmuService.cpp` | 发布 SMMU 窗口和 `iommu-cells` |
| `NativeAdrenoGpuClient : IOUserClient` | `NativeAdrenoService.cpp:542-631` | 14 个外部方法（`:600-614`），转给 `NativeAdrenoUserClient` 核心 |
| `AGXAccelerator : IOAcceleratorES`、`AGXDeviceUserClient` | `NativeAdrenoAccelerator.cpp:16-25`、`NativeAdrenoPublish.cpp:147` | 给 Metal 发现和沙箱用的"冒名"类（iPadOS 17 专有） |

它依赖 UniXNU 的 `ArmDTPlatform` 把设备树节点发布成 `IOPlatformDevice` 才能匹配。**我们的平台（`SweetPlatformExpert`）不发布这些节点**，所以走 IOService 匹配这条路要另做；而 sheng 真正跑通的 `GpuPowerProbe.cpp` 根本不靠匹配，直接用写死的物理地址（`S/GpuPowerProbe.cpp:257,282`）[读到]。

### 2.3 对到我们的构建 [读到我们的树 + 推断做法]

- 我们的驱动直接放进内核源码树：C 文件进 `bsd/dev/` 并登记在 `bsd/conf/files`（例如 `pandora_adsp.c` 在第 115 行），C++ 文件进 `iokit/Kernel/arm/` 并登记在 `iokit/conf/files`；personality 写在 `iokit/KernelConfigTables.cpp`（PANDORA 宏下）[读到 WSL `/opt/work/tree`]。功能用启动参数开关、从 `sweet_ufs_init()` 里起内核线程（`xnu/patches/0143` 末尾：`pandora_adsp_init()` 读 `pandora_adsp` 启动参数后 `kernel_thread_start`）。
- 所以**照 sheng 的 `GpuPowerProbe` 方式最省事**：不用 IOService 匹配，新增 `bsd/dev/pandora_gpu.c`（C，起线程、分级、打日志），纯 C 小库（ShengGmu 的 gen8 版、AdrenoHfi、AdrenoFirmware、AdrenoHfiVotes、ArmSmmu4K）放 `bsd/dev/adreno/` 并登记进 `bsd/conf/files`。需要 IOKit 内存对象（GMU 用的物理连续内存）时，再加一个 `iokit/Kernel/arm/PandoraAdrenoMem.cpp`，或直接搬 `NativeAdrenoMemory.cpp` + `NativeAdrenoGmuMemory.cpp` + `NativeAdrenoGmuContext.cpp` + `NativeAdrenoSmmuContext.cpp` + `NativeAdrenoVm.cpp`（都是标准 IOKit，见第 1 节）。
- **固件不嵌进内核**：我们已有固件包机制，`pandora_fw_read()` 按 `PANDFW01` 包读文件（`xnu/patches/0144:312-350`）。`gen80200_gmu.bin` 放进这个包即可，公开仓库里没有固件字节。这一点比 UniXNU 的 `.incbin` 好。
- 我们树里已经有 ios-fuxi 的 A740 探测代码 `bsd/dev/gpu.c`（文件头写明是"分级 GMU 探测，移植自 Linux 并对照 UniXNU-sheng 的 GpuPowerBoard.h"），它被 `fuxi_adreno.c:172` `#include`，只在 `FUXI` 宏下编译 [读到]。可以作为第二份参考，但它带的 `gpu-a740.h` 来自 Linux，是 GPL-2.0-only（见 `GPU_PLAN.md` 第 8 节）。

---

## 3. GMU 冷启动 + 第一条 HFI：A740 的做法，以及 gen8 变了什么

### 3.1 sheng（A740）实际执行顺序 [读到]

外层是 `S/GpuPowerProbe.cpp` 的分级（每一级都要求上一级读到"前提成立"才继续）：

1. `probeThread`（`:1874`）映射 GCC 两页、GPUCC、SMMU 全局页、GPU 一页，调用 `gpb_run`（`S/GpuPowerBoard.h:106`）：
   - 读 GCC 的 `gcc_gpu_cfg_ahb_clk` 等（`:111-114`），级别 ≥2 时如果使能位是 0 就置 1（`:116-122`）；
   - 只有 cfg_ahb 开着才读 GPUCC 的 CX/GX GDSCR、AHB、SMMU 投票时钟、GMU 时钟（`:124-134`）；
   - 级别 ≥3 且 CX 关着时，照 Linux `gdsc.c` 打开 `gpu_cc_cx_gdsc`，再开 `gpu_cc_ahb_clk` 和 `gpu_cc_hlos1_vote_gpu_smmu_clk`（`:138-181`）；
   - 只有 CX 开、SMMU 投票时钟在跑，才读 SMMU 全局空间 0 和全部 SMR（`:185-208`）；
   - 只有 CX 和 GX 都开，才读 `RBBM_STATUS`（`:210-215`）。
2. 级别 4：`gmuContext`（`:214`）调用 `NativeAdrenoGmuContextBringup`（`U/NativeAdreno/NativeAdrenoGmuContext.cpp:92`）——读 SMMU ID2 → 分配 GMU 五块内存 + 1027 页页表池 → 编好上下文页表 → 写上下文寄存器（bank 1 / ASID 1）→ 写 SMR+S2CR（slot 2，**GMU 流号 5**）→ 把五块内存映射进去（`NativeAdrenoGmuContext.cpp:18-89`；常量在 `GpuPowerProbe.cpp:204`）。此时 GMU 还没启动。
3. 级别 5–6：`gmuPrerequisites`（`:1770`）读 GMU 时钟树；级别 6 按 Linux 顺序打开 GPLL0 投票、配 PLL0/PLL1、hub 200 MHz、gmu 220 MHz、开 7 个分支时钟（`gpb_gmu_clocks_on`，`GpuPowerBoard.h:342-400`）；拷贝并解析 AOP cmd-db，生成 HFI 投票表（`ShengGmuCommandDbInputs` + `ShengGmuBuildStartup`）；只有 GMU 时钟在跑才读 GMU 状态四个字：SYSRESET / TCM_RET / RSC 忙 / HFI 状态。
4. 级别 7：只有前面全对、且 GMU 读到"复位中、空闲"（1/0/0/0）才 `gmuColdStart`（`:1695`）：
   - 映射五个窗口（GMU 核心、RSCC、PDC、GPU、CX_MISC），清 AO 中断、只放开致命中断（`:1712-1735` 一段）；
   - 调 `ShengGmuColdStart`（`S/ShengGmu.c:229`）= `ShengGmuLoadAndStartHfi` + `AdrenoHfiStartupRun`。
5. 级别 8：`gpuOn`（`:1637`）发 HFI `GX_BW_PERF_VOTE`（id 30，`:1659`），再写 `HOST2GMU_INTR_SET` 第 30 位做 OOB GPU_SET（`:1664`），等第 31 位应答，GMU 自己把 GX 上电。
6. 级别 9：GPU 自己的 SMMU 上下文、SQE、CP_ME_INIT、CP_MEM_WRITE、zap（不在本次范围）。

`ShengGmuLoadAndStartHfi`（`S/ShengGmu.c:91-203`）内部顺序：

| 步 | 代码 | 内容 |
|---|---|---|
| 校验 | `:66-90,95-99` | 固件整份 SHA-256 与固定值比对；按区域切块，**要求恰好 15 块** |
| 前提读 | `:101-110` | `CM3_SYSRESET==1`、`CX_MISC_TCM_RET_CNTL==0`、`RSCC_SEQ_BUSY_DRV0==0`、`HFI_CTRL_STATUS` 第 0 位为 0、无致命中断 |
| RSC/PDC | `:113-117` | 19 条写（生成头 `sheng_gmu_rsc_setup`，来自 Linux `a6xx_gmu_rpmh_init` 的 A740 分支，`prepare-sheng-gmu.py:63-85`） |
| FAL/TCM | `:118-122` | `CX_FALNEXT_INTF=1`、`CX_FAL_INTF=1`、`TCM_RET_CNTL=1` 并读回 |
| 清内存 | `:125-130` | icache/dummy/debug/log 清零；`AdrenoHfiInitializeStopped` 建队列表 |
| 装固件 | `:132-161` | ITCM/DTCM 块写寄存器，icache 块写内存；逐字读回比对 |
| 配置 | `:163-173` | `FW_INIT_RESULT=0`、`BOOT_CONFIG=2`、`HFI_QTBL_ADDR`、`HFI_QTBL_INFO=1`、`AHB_FENCE_RANGE_0=0x80000000|(0x32<<18)|0x8a0`、`CM3_CFG=0x4052`、`GENERAL_10=芯片号`、`GENERAL_8=log iova|3`、`GBIF_QSB_SIDE0..3=0x71620`、`SYS_BUS/ICACHE/DCACHE_CONFIG=1` |
| 放复位 | `:174-189` | 读 DTCM+0xff8 定完成值（≤0x20010004 用 0xbabeface，否则 0x100/掩码 0x1ff）；`SYSRESET=1`、`GENERAL_9=0`；配置寄存器逐个读回；`SYSRESET=0` |
| 等就绪 | `:191-200` | 轮询 `FW_INIT_RESULT`；写 `HFI_CTRL_INIT=1`；轮询 `HFI_CTRL_STATUS` 第 0 位 |

然后 `AdrenoHfiStartupRun`（`U/AdrenoFirmware/AdrenoHfiStartup.c:109-157`）依次发：性能表（id 4，58 字，`:58-68`）→ 带宽表（id 3，159 字，`:69-77`）→ [ACD 开关 id 11 + ACD 表 id 7，`:78-82`] → [IFPC 开关 id 11，`:83`] → CORE_FW_START（id 14，`:84`）→ START（id 10，`:85`），每条都等匹配序号的 ACK。sheng 关了 IFPC（`NativeAdrenoService.cpp:396-398` 注释说明原因）。

这与 Linux 的 `a6xx_gmu_fw_start()`（`LX/a6xx_gmu.c:929-1060`）+ `a6xx_gmu_start()`（`:291-331`）+ `a6xx_gmu_hfi_start()`（`:333-345`）+ `a6xx_hfi_start()`（`LX/a6xx_hfi.c:972-1009`）一一对应。

### 3.2 gen8 上能看到的差别

**寄存器偏移**（dword，相对 GPU 基址 `0x3d00000`）[读到 `LX/fetched/a6xx_gmu.xml` 与 `KG/gen8_reg.h`]：

| 寄存器 | A740（sheng 用的） | gen8 | 来源 |
|---|---|---|---|
| CM3_ITCM_START / DTCM_START | 0x1b400 / 0x1c400 | 不变 | xml:45-46，gen8_reg.h:1403-1404 |
| CM3_SYSRESET / BOOT_CONFIG / FW_INIT_RESULT / CM3_CFG | 0x1f800 / 0x1f801 / 0x1f81c / 0x1f82d | 不变 | gen8_reg.h:1416-1421 |
| HFI_CTRL_STATUS / QTBL_INFO / QTBL_ADDR / CTRL_INIT | 0x1f980 / 984 / 985 / 986 | 不变 | gen8_reg.h:1588-1591 |
| GMU2HOST_INTR_INFO / HOST2GMU_INTR_SET | 0x1f992 / 0x1f994 | 不变 | gen8_reg.h:1594-1596 |
| ICACHE / DCACHE / SYS_BUS_CONFIG | 0x1f400 / 401 / 40f | 不变 | gen8_reg.h:1405-1407 |
| AO_HOST_INTERRUPT_STATUS / AHB_FENCE_RANGE_0 | 0x23b05 / 0x23b11 | 不变 | gen8_reg.h:1607,1623 |
| **CX_FAL_INTF / CX_FALNEXT_INTF** | 0x1f8f0 / 0x1f8f1 | **0x1f7ec / 0x1f7ed** | xml:135-138，gen8_reg.h:1414-1415 |
| **GENERAL_8 / 9 / 10** | 0x1f9cd / ce / cf | **0x1f9c8 / c9 / ca**（另有 GENERAL_11 0x1f9cb） | xml:183-192，gen8_reg.h:1599-1602 |
| **RPMH_POWER_STATE** | 0x1f8ec | **0x1f7e9** | xml:133-134，gen8_reg.h:1412 |
| GX 电源状态 | `GMU_SPTPRAC_PWR_CLK_STATUS` 0x1f8d0（`GpuPowerProbe.cpp` kGxStatus） | **`GMUCX_GFX_PWR_CLK_STATUS` 0x1f7e8** | gen8_reg.h:1411 |
| RSCC 位置 | DT 里单独的 rscc 窗口 | GPU 基址 + 0x50000（即 0x3d50000，手机 DT 的 rscc 窗口也是它，DT:11932） | `LX/a6xx_gmu.c:2409-2416` |

**启动步骤的差别**：

1. **不写 RSC/PDC**：Linux `a6xx_gmu_rpmh_init()` 遇 a8xx 直接返回，注释说 "On A8x and above, RPMH/PDC configurations are entirely configured in AOP"（`LX/a6xx_gmu.c:679-681`）。KGSL gen8 也没有这一步。→ 删掉 `ShengGmu.c:113-117`，也不需要 PDC 窗口。
2. **不写 TCM_RET_CNTL**：Linux 只在 a7xx 写（`LX/a6xx_gmu.c:950-952`），a8xx 只用它判断冷/热启动（`:1350-1352`）；KGSL `gen8_gmu_register_config()` 也不写（`KG/adreno_gen8_gmu.c:841-922`）。→ `ShengGmu.c:120-122` 去掉写，`:104-105` 的"必须为 0"可保留为只读检查。
3. **围栏下限**：a8xx `fence_range_lower=0x8c0`（`LX/a6xx_gmu.c:977-979`；KGSL 写的是 `GEN8_CP_CONTEXT_SWITCH_CNTL`=0x8c0，`KG/gen8_reg.h:816`、`KG/adreno_gen8_gmu.c:866-868`）。
4. **GENERAL_8/10 换地址**，值的算法不变（`LX/a6xx_gmu.c:1014-1018`）。芯片号用 catalog 的 `gmu_chipid = 0x8020100`（`LX/a6xx_catalog.c:2443,2465`），手机原厂驱动也是 `0x08020100`（`GPU_PLAN.md` 第 2 节）。
5. **GBIF**：A740 是 `QSB_SIDE0..3=0x71620`；a840 是 `QSB_SIDE0..3=0x71e20` 外加 `GBIF_CX_CONFIG=0x20023000`（`LX/a6xx_catalog.c:2212-2219`），并且 a8xx 还要写 `GMU_MRC_GBIF_QOS_CTRL=0x33`（`LX/a6xx_gmu.c:1035-1037`，地址 0x1f50b）。
6. **KGSL 额外做、Linux 不做的**（`KG/adreno_gen8_gmu.c:841-922`）：启动前 `RPMH_POWER_STATE=0xF`；写 `GBIF_HALT`(0x3c45) 清掉可能残留的停顿；可选 `GENERAL_11`=VRB 地址；`GENERAL_9`=上次保存的日志写指针（冷启动为 0）。第一次照 Linux 做，KGSL 这几项作为失败后的备选 [推断]。
7. **放复位与等待**：KGSL gen8 等 `FW_INIT_RESULT` 的 `[8:0]==0x100`（`KG/adreno_gen8_gmu.c:271-297`），正好是 `ShengGmu.c:174-175` 的"新固件"分支（手机固件核心版本 `0x50030027` > `0x20010004`），这段逻辑不用改。HFI 就绪等待相同（`:300-314`）。
8. **安全调用 GPU_INIT_REGS**：Linux 在 a750 与 a8xx 的 `a6xx_gmu_secure_init()` 里调用 `qcom_scm_gpu_init_regs(ALWAYS_EN | TSENSE_EN)`（`LX/a6xx_gmu.c:1200-1245`，调用在 :1231）；KGSL 在打开时钟后、装固件前调用 `gen8_scm_gpu_init_cx_regs()`（`KG/adreno_gen8_gmu.c:1546-1570`，函数在 `KG/adreno_gen8.c:1865-1895`）。调用号：SIP，服务 `0x28`，命令 `0x01`，1 个参数（`LX/fetched/qcom_scm.c:1944-1955`、`qcom_scm.h:158-159`）。参数位值 `ALWAYS_EN=bit0`、`TSENSE_EN=bit1` 我没在本地源码里找到定义 [推断，需上网核对]。sheng（A740）不调用它。
9. **启动前投票**：Linux 在启动 GMU 前用 OPP 投一次带宽（`a6xx_gmu_set_initial_bw`，`LX/a6xx_gmu.c:1342`）；KGSL 投最低 DDR 带宽（`KG/adreno_gen8_gmu.c:1599`）。sheng 什么都没投也起来了。我们第一次可以不投，失败再加 [推断]。
10. **ACD 的 AOP 消息**：KGSL 每次首启都向 AOP 发 `{class: gpu, res: acd, val: 0/1}`（`KG/adreno_gen8_gmu.c:1527-1545,1555`）。Linux 只在开 ACD 时发。我们关 ACD，第一次不发 [推断]；我们已有 `qmp_send()`（`xnu/patches/0144`），需要时可加。

**HFI 消息的差别**：

- **第一条消息**：a8xx 的性能表不再是 id 4 的固定结构，而是 `HFI_H2F_MSG_TABLE`（id 15），内容 `{version, type=HFI_TABLE_GPU_PERF(1), entry{count,stride=3,data[gx_arc, dep_arc, freq_khz]...}, entry{count,stride=2,data[cx_arc, freq_khz]...}}`（`LX/a6xx_hfi.c:280-325`，结构在 `LX/a6xx_hfi.h:261-283`）。`dep_arc` 是 MX 依赖票，第 0 档固定 `0xffffffff`（`LX/a6xx_gmu.c` `a6xx_gmu_rpmh_dep_votes_init`）。GX 的第二条轨：cmd-db 里有 `gmxc.lvl` 就用它，否则用 `mx.lvl`（同文件 `a6xx_gmu_rpmh_votes_init`）。KGSL 的硬件调度路径也是先发性能表、再发带宽表（`KG/adreno_gen8_hwsched_hfi.c:2755-2760`）。
- **带宽表**（id 3）格式不变；a840 的 BCM 是 SH0（buswidth 16）、MC0（4）、ACV（fixed，perfmode bit2，perfmode_bw 10687500），外加 CN0（`LX/a6xx_catalog.c:2466-2477` 一带的 `.bcms`；生成算法 `LX/a6xx_hfi.c:357-397`）。`AdrenoHfiBuildBandwidth` 可直接算，只换策略数值。
- CORE_FW_START（14）、START（10）、FEATURE_CTRL（11）、GX_BW_PERF_VOTE（30）编号不变（`LX/a6xx_hfi.h:204-261`）。
- 回包：GMU 回 ACK 的第 2 个字是原命令头，序号在 `>>20`；`AdrenoHfiPoll` 已按这个规则匹配（`AdrenoHfi.c:84`）。`GPU_PLAN.md` 7.6 第 2 条 ios-fuxi 的同一教训也对得上。

### 3.3 上电/时钟层：SM8550 → SM8850 全换 [读到 `LX/fetched/gpucc-kaanapali.c`、`gcc-kaanapali.c`、`gxclkctl-kaanapali.c`，手机 DT]

| 项 | sheng（SM8550，`S/GpuPowerBoard.h`） | 我们（mainline kaanapali；手机 DT 叫 canoe） |
|---|---|---|
| GPUCC 基址 | 0x3d90000 | 0x3d90000，大小 0x9800（DT:8675-8677） |
| CX GDSCR / gds_hw_ctrl | 0x9108 / 0x953c | **0x9080 / 0x9094**（gpucc-kaanapali.c:373-383，VOTABLE + RETAIN_FF） |
| gpu_cc_ahb_clk | 0x911c | **0x90bc**（:180） |
| gpu_cc_cx_gmu_clk | 0x913c | **0x90d4**（:211） |
| gpu_cc_cxo_clk / hub_cx_int_clk | 0x9144 / 0x9148 | **0x90e4 / 0x90e8** |
| SMMU 投票时钟 | 0x7000 | 0x7000（:281，同） |
| GMU RCG / HUB RCG | 0x9318 / 0x93ec | 0x9318 / **0x93f0**（:125,149） |
| GMU 时钟可选频率 | 220 MHz（PLL1/2） | **19.2 / 475 / 575 / 700 / 725 / 750 MHz**，非 XO 档都来自 PLL0 even（:114-121） |
| HUB 时钟 | 200 MHz（GPLL0/3） | 150 MHz（GPLL0 div /2）或 200 MHz（GPLL0/3）等（:140-145） |
| PLL | PLL0 + PLL1，Lucid OLE | **只有 PLL0，Taycan EKO T**，950 MHz，L=0x31，cal_l 0x48，alpha 0x7aaa（:44-52）——配置序列要重写 |
| GX GDSC | GPUCC 0x905c | **移到 gxclkctl：0x3d64000+0x4024 = 0x3d68024**（gxclkctl-kaanapali.c:22；手机 DT:8688-8690）；其上电函数是"什么都不做"（:28），由 GMU 负责 |
| GCC cfg_ahb / gemnoc_gfx / GPLL0 投票 | 0x71004 / 0x71010 / 0x52000 b15,16 | 同（gcc-kaanapali.c:3466 列为常开，:1549-1587） |
| GMU 窗口 | 0x3d6a000（偏移 0x6a000） | 手机 DT **0x3d37000，大小 0x68000**，另有 `gmu_ao_blk_dec0` 0x3d40000（DT:38283-38299）；mainline 是 0x3d6c000（kaanapali.dtsi:2862-2865）。寄存器都按"GPU 基址 + dword×4"算，绝对地址相同 |
| GMU 的 SMMU 流号 | 5 | 5（DT:38288 `iommus = <0x386 0x5 0x0>`） |
| 中断 | — | GPU SPI 300，cx_host SPI 80；GMU hfi SPI 304、gmu SPI 305（DT:11930,38296） |

另外两处我们手机特有、桌面查不清的 [读到 DT，含义推断]：
- GMU 在手机 DT 里挂了三个电源域 `cx`、`gmu_cx`、`gx`（`power-domains = <0x50 0x0 0x50 0x2 0x481 0x0>`，DT:38294），SMMU 挂的是 `<0x50 0x1>`（DT:22448）。mainline 的 kaanapali gpucc 只有一个 GDSC（`GPU_CC_CX_GDSC`）。canoe 的第 1、2 号电源域是什么，要上机读 GPUCC 后才能确定。
- 手机 DT 的 GPU SMMU 节点和 GPU 节点都带 `interconnects`（DT:11920, 22449），说明安卓在访问前投了总线带宽。sheng 没投也能读；我们要在只读阶段看清楚。

---

## 4. 我们的第一个硬件步骤（一个内核补丁，分级）

**补丁名建议**：`0162-pandora-gpu-gmu-probe.patch`。新增 `bsd/dev/pandora_gpu.c` + `bsd/dev/adreno/*.c`，在 `sweet_ufs_init()` 里像 `pandora_adsp_init()` 一样按启动参数起线程。启动参数 `pandora_gpu=<1..4>`，默认关。每一级都只在上一级读到"前提成立"时才继续，所有读数写进内核日志（我们的 log 文件），一次启动拿到整条链。

### 级别 1：纯读（第一次上机只开这一级）

按 sheng `gpb_run` 的门控顺序（`S/GpuPowerBoard.h:106-216`），换成我们的地址：
1. GCC：读 `0x171004`（cfg_ahb，看 bit0 使能，**不要看 CLK_OFF**，sheng 读到过 `0x80000003` 实际在跑，`GpuPowerBoard.h:90-93`）、`0x171010`、`0x152000`。
2. 仅当 cfg_ahb 使能：读 GPUCC `0x9080`、`0x9094`、`0x90bc`、`0x90d4`、`0x90e4`、`0x90e8`、`0x7000`、`0x9318/+4`、`0x93f0/+4`、PLL0 MODE（`+0x0`）。再把 `0x9000..0x95e8` 里所有疑似 GDSCR 的字整体扫一遍，用来回答"canoe 第 1、2 号电源域是哪个寄存器"（gpucc regmap 上限 0x95e8，gpucc-kaanapali.c:431-437）。
3. 仅当 CX GDSCR 第 31 位为 1 且 `0x7000` 的 CLK_OFF 为 0：读 SMMU `0x3da0000` 的 sCR0、ID0/1/2、sGFSR，然后**一个一个**读 SMR/S2CR（ios-fuxi 一次读完全部 SMR 时出过同步异常导致复位，`GPU_PLAN.md` 7.6 第 1 条）。
4. 仅当 CX 开、gpu_cc_ahb 和 cx_gmu 时钟都在跑：读 GMU 状态 `CM3_SYSRESET`(0x1f800)、`HFI_CTRL_STATUS`(0x1f980)、`FW_INIT_RESULT`(0x1f81c)、`AO_HOST_INTERRUPT_STATUS`(0x23b05)、`GFX_PWR_CLK_STATUS`(0x1f7e8)、`RPMH_POWER_STATE`(0x1f7e9)、RSCC `0x3d50000+0x101*4`、CX_MISC `0x3d9e000+0x39*4`（TCM_RET）。
5. 不需要任何电：拷贝 cmd-db（`0x81c60000`，`0x20000`），解析出 `gfx.lvl / mx.lvl / cx.lvl / gmxc.lvl / SH0 / MC0 / ACV / CN0` 的地址和 aux 长度并打印。
6. **不读**：GPU 寄存器窗口（GX 没开）、gxclkctl `0x3d68024`（它的父电源包括 GFX 轨，GX 关时读它是否安全不清楚）。

可借用：`GpuPowerBoard.h` 的门控结构与 `gpb_rd` 记账写法；`ShengGmuCommandDb.c` 或我们自己的 `cmddb_find()`。要新写：地址表（上表）、日志输出。

### 级别 2：CX 电源和 SMMU 时钟（写 GPUCC，不碰 GMU）

- cfg_ahb 未使能则置位（Linux gcc 驱动探测时就这样做，`GpuPowerBoard.h:116-122`）。
- CX GDSC 关着就按 `gdsc.c` 打开：清 SW_COLLAPSE → 轮询 `0x9094` 第 31 位 → 置 RETAIN_MEM/PERIPH → 置 RETAIN_FF（照 `GpuPowerBoard.h:138-160` 的写法，换偏移）。
- 开 `gpu_cc_ahb_clk`(0x90bc) 和 SMMU 投票时钟(0x7000)，等 CLK_OFF 清零，然后重读 SMMU。
- 可借用：上面那段写法几乎原样。要新写：只有偏移。

### 级别 3：GMU 时钟 + 安全调用

- GPLL0 的 GPU 投票位（`0x152000` bit15/16）；PLL0 没在跑时按 Taycan EKO T 配置并锁定（**新写**，参考 `LX/fetched/clk-alpha-pll.c` 的 taycan_eko_t 函数）；hub RCG 设 150 MHz（源选 6 = GPLL0 div，分频字段 3 = /2）；gmu RCG 设 475 MHz（源选 2 = PLL0 even，分频字段 1 = 不分频；分频字段按 `F()` 宏为 2×分频−1，sheng 的 `gpb_rcg_set(...,5,5)` 就是 GPLL0/3）；按 Linux 列表开分支：ahb、cx_gmu、cxo、hub_cx_int，以及 GCC `gemnoc_gfx`（kaanapali.dtsi:2872-2876 的 GMU clocks）。可借用 `gpb_rcg_set`、`gpb_set_bit0` 的写法（`GpuPowerBoard.h:312-341`）。
- `SCM GPU_INIT_REGS`：用我们 `pandora_adsp.c` 里已经做通的 `scm_smc()`，`SCM_FN(0x28, 0x01)`、1 个参数（位值先核对）。需要把 `scm_smc()` 从 `pandora_adsp.c` 挪成公用函数。
- 结束后再读一次级别 1 第 4 条的 GMU 状态，期望 `SYSRESET=1, TCM_RET=0, RSCC 忙=0, HFI=0`（sheng 要求的就是这组，`GpuPowerProbe.cpp` gmuPrerequisites 末尾 `idle` 判断）。

### 级别 4：GMU 的 SMMU 上下文 + 冷启动 + 第一条 HFI 回复

1. GMU 的 SMMU 上下文：搬 `NativeAdrenoGmuContext` + `NativeAdrenoGmuMemory` + `NativeAdrenoSmmuContext` + `NativeAdrenoVm` + `NativeAdrenoMemory` + `ArmSmmu4K*`（C++ 放 `iokit/Kernel/arm/`）。bank/ASID/SMR 槽位要选级别 1 读到的空闲项；SID 5；ACTLR 按手机 DT 设 `0x32b`。
2. 冷启动：把 `ShengGmu.c` 复制成 gen8 版（建议叫 `PandoraGmu.c`），按 3.2 节逐条改：
   - 窗口改为两个就够：GMU `0x3d37000/0x68000`（覆盖 GMU 核心、AO、RSCC 0x3d50000），CX_MISC `0x3d9e000/0x2000`；GPU 窗口 `0x3d00000/0x40000` 只用于写 GBIF（Linux 在 GMU 启动前就写这几个，属 CX 侧 [推断]）。`core_offset` 的减数改为 `0x37000`。
   - 删 RSC/PDC 19 写；TCM_RET 只读不写；FAL、GENERAL_8/9/10 换 gen8 地址；围栏 `0x8c0`；GBIF 改 a840 值并加 `GBIF_CX_CONFIG`、`MRC_GBIF_QOS_CTRL=0x33`；芯片号 `0x08020100`；固件块数改成按 `gen80200_gmu.bin` 实际值（`GPU_PLAN.md` 第 5 节：4 块数据 + 若干零长元数据），哈希/大小改成手机这份；固件从 `pandora_fw_read` 读，不嵌内核。
3. **第一条 HFI = 性能表 MSG_TABLE（id 15）**，按 `LX/a6xx_hfi.c:280-325` 编。投票值来自级别 1 解析的 cmd-db：GX 用 `gfx.lvl` + (`gmxc.lvl` 或 `mx.lvl`)，CX 用 `cx.lvl` + `mx.lvl`，依赖票用 `mx.lvl`（`AdrenoHfiArcVotes` 可直接算）。频点用手机 DT：GMU `qcom,gmu-freq-table` = 475/575/700/750 MHz（DT:38290），GPU 频点取手机 GPU OPP 表（本次未逐条抄，需从 DT 抽）。**成功标志**：收到序号匹配、错误字为 0 的 ACK。
   - 为了把"第一步"缩到最小：只发这一条就停，不发带宽表、CORE_FW_START、START；GMU 收到 ACK 后保持运行、不请求 GX。下一步再接着发剩下几条（顺序 3.2 节已列）。
   - 不选 GET_VALUE 当第一条，原因见第 1 节 AdrenoFirmware 一行（回包格式不同，现成的收包代码会误判）。
4. 日志：照 sheng 那样记录"阶段号 / FW_INIT_RESULT / HFI 状态 / AO 与 CM3 故障字 / ACK 头与错误字"，失败时写 `CM3_SYSRESET=1` 并读回（`ShengGmu.c:58-63`）。

### 这一步借用 / 新写 一览

| 借用（保留原版权声明） | 新写 |
|---|---|
| `AdrenoFirmware.c`、`AdrenoHfi.c`、`AdrenoHfiVotes.c`（BSD-2-Clause-Patent） | gen8 的 MSG_TABLE 编码 + 依赖票函数 |
| `ShengGmu.c` 的流程骨架、校验、读回、失败复位（BSD-2-Clause-Patent） | `PandoraGmu.c` 里的 gen8 寄存器与数值（3.2 节） |
| `GpuPowerBoard.h` 的分级门控写法、GDSC/RCG/分支时钟函数结构（BSD-2-Clause-Patent） | SM8850 GPUCC 地址表、Taycan EKO T PLL 配置 |
| `ArmSmmu4K*`、`NativeAdrenoGmuContext/GmuMemory/SmmuContext/Vm/Memory`（MIT） | 板级参数：SMR 槽位、bank、ACTLR `0x32b` |
| 我们自己的 `scm_smc`、`cmddb_find`、`pandora_fw_read`、`qmp_send`（0143–0146） | GPU_INIT_REGS 调用、`pandora_gpu.c` 分级线程和日志 |

---

## 5. 许可

- MIT（NativeAdreno、ArmSmmu4K、AdrenoPM4、AirNir、NativeMetal）：复制时保留 `Copyright (c) 2026 <组件名> contributors` 与许可全文。我们树里已有 `iokit/Kernel/arm/LICENSE.NativeAdreno`，可沿用并补上 ArmSmmu4K。
- BSD-2-Clause-Patent（AdrenoFirmware、NativeMobileDisplay、sheng 板级包 `ShengGmu*`、`GpuPowerBoard.h`、`GpuPowerProbe.cpp`）：版权方写的是 "ArmVirtPlatform contributors"（`U/AdrenoFirmware/LICENSE:1-3`）；保留每个文件的 `SPDX-License-Identifier` 行和版权声明，分发时附许可全文。
- UniXNU 的生成头（`ShengGmuSource.h` 等）是从 Linux GPL 代码里抽出的寄存器值和序列（`prepare-sheng-gmu.py:9-10`）。我们按同样做法只取事实（地址、数值、顺序），寄存器名和偏移优先从 Mesa/Linux 的 XML（MIT）生成。KGSL 是 GPL-2.0-only，只作事实来源，不复制代码。
- 固件（`gen80200_gmu.bin`、sqe、zap）不进公开仓库，走固件包。

---

## 6. 在手机上会导致整机复位的访问（风险清单）

1. **在 cfg_ahb 关时读 GPUCC**、**在 CX GDSC 关或 SMMU 投票时钟停时读 `0x3da0000`**、**在 CX/ahb/cx_gmu 时钟没开时读 GMU 窗口**：总线可能直接挂死而不是报错（`S/GpuPowerBoard.h:5-7` 的说明）。严格按级别 1 的门控。
2. **在 GX 没开时读 GPU 寄存器窗口**（`RBBM_STATUS` 等）。GX 只能由 GMU 在 OOB 请求后打开；我们第一步不碰。gxclkctl `0x3d68024` 第一步也不读。
3. **一次读完全部 SMR**：ios-fuxi 在 SM8550 上因此同步异常复位（`GPU_PLAN.md` 7.6 第 1 条）。逐个读，每读一个记一次进度。
4. **大块 `ml_io_map`**：ios-fuxi 一次映射 32 MiB 撑爆 IO 子映射导致 panic（7.6 第 6 条）。按块映射：GMU 0x68000、GPUCC 0x9800、SMMU 0x40000（级别 1 只映射 GR0 一页即可）。
5. **GMU 一旦放出复位就锁死**（7.6 第 7 条），且 `ShengGmu` 的设计是"一个设备只允许尝试一次、失败就标记作废"（`ShengGmu.c:48-71`）。冷启动失败后要整机重启才能再试，所以级别 4 只在级别 1–3 的读数都对上后才开。
6. **SMMU 上下文写错**：GMU 访问未映射地址会触发上下文故障，可能挂住总线。级别 4 先写上下文、读回比对（`NativeAdrenoGmuContext.cpp:53-62` 的 mismatch 掩码），比对不过就停。
7. **安全上下文**：GPU SMMU 的 secure 上下文可能归安全世界管，我们只用非安全 bank，并避开级别 1 读到已有效的 SMR。
8. **IFPC 必须关**（`NativeAdrenoService.cpp:396-398`）：否则 GMU 可能在两步之间把 GX 断电，之后访问 GPU 寄存器就会出事。

## 7. 上机前仍未定的问题

1. canoe GPUCC 第 1、2 号电源域（SMMU 用 1，GMU 用 0、2）分别对应哪个寄存器（级别 1 的整段扫描回答）。
2. fastboot 启动后 GPUCC 的 PLL0、GMU/HUB RCG、CX GDSC 是开是关（级别 1 回答）。
3. GPU SMMU 是否已有有效 SMR、哪个 bank 空闲（级别 1 回答）。
4. `GPU_INIT_REGS` 的参数位值（桌面可查，上网核对 `include/linux/firmware/qcom/qcom_scm.h`）。
5. 不投带宽、不发 ACD 的 AOP 消息时，GMU 能否起来（级别 4 回答；失败就按 KGSL 顺序补上）。
