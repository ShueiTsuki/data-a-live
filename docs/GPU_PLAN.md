# 真正的 GPU：让 iOS 16 的 Metal 跑在 17 Pro 的 Adreno 840 上（方案）

状态：调研完成，未开始实现。没有动手机（只从安卓侧只读取了 GPU 型号和固件文件名）。
每条事实后面标了来源：**[实测]** 在我们手机或镜像上直接看到的；**[读到]** 从别人的代码、文档或苹果二进制里读到的；**[推断]** 我们自己的判断。

## 一句话结论

可行，而且不是从零开始：UniXNU 已经在 Adreno 740 上做出了 GPU 合成的桌面（iPadOS 17），ios-fuxi 已经在同一个 iOS 16 上把 GPU 和 Metal 设备跑通到"Metal 测试程序和独立的 CoreAnimation 测试程序用 GPU 出画面"。我们要做的是：把这套东西搬到 Adreno 840（新一代 gen8）上，并把 ios-fuxi 卡住的最后一段（真正的 backboardd 用 GPU 合成）做完。

## 1. 手机上的 GPU [实测]

| 项目 | 值 | 来源 |
| --- | --- | --- |
| 型号 | Adreno 840 v2 | `/sys/class/kgsl/kgsl-3d0/gpu_model`，设备树 `qcom,gpu-model` |
| 设备树 compatible | `qcom,adreno-gpu-gen8-2-1`，`qcom,kgsl-3d0` | `/proc/device-tree/soc/qcom,kgsl-3d0@3d00000` |
| 芯片编号 | `0x44050a01` | 设备树 `qcom,chipid` |
| 频率 | 160–1200 MHz，18 档 | `gpu_available_frequencies` |
| GPU 寄存器 | `0x3d00000`，长 `0x40000`；另有 rscc、cx_dbgc、cx_misc | 设备树 `reg`/`reg-names` |
| GMU | `0x3d37000`，compatible `qcom,gen8-gmu` | `qcom,gmu@3d37000` |
| GPU 的 SMMU | `kgsl-smmu@3da0000`（qsmmu-v500 / adreno-smmu，安卓由 `arm-smmu` 驱动绑定） | 设备树、`/sys/bus/platform/devices` |
| GPU 时钟控制器 | `0x3d90000`（SMMU 的供应者之一） | `/sys/bus/platform/devices/3da0000.kgsl-smmu` |
| UBWC | 版本 6 | `qcom,ubwc-mode` |
| 固件 | `/vendor/firmware/gen80200_sqe.fw`、`gen80200_gmu.bin`、`gen80200_aqe.fw`；zap：`/odm/firmware/gen80200_zap.mbn` | 文件列表 + 安卓 `msm_kgsl.ko` 里的字符串 `adreno_gpu_core_gen8_2_1` |

## 2. 已有的开源工作

### Linux [读到]
主线 `drivers/gpu/drm/msm/adreno/` 有 `a8xx_gpu.c`、`a8xx_preempt.c`，`a6xx_catalog.c` 里有一条正好对应我们芯片的记录：`chip_ids = 0x44050a01`，`ADRENO_8XX_GEN2`，固件 `gen80200_sqe.fw / gen80200_gmu.bin / gen80200_aqe.fw`，GMEM 18 MiB，`max_slices = 3`，`gmu_chipid = 0x8020100`。这是 gen8 上电、GMU、HFI 的权威参考（Qualcomm 自己提交，Linux 6.19 起）。

SM8850（代号 Kaanapali）这颗芯片本身在主线里也有完整描述 [读到，torvalds master]：
- 时钟驱动 `drivers/clk/qcom/gpucc-kaanapali.c`、`gxclkctl-kaanapali.c`、`gcc-kaanapali.c`。
- 设备树 `arch/arm64/boot/dts/qcom/kaanapali.dtsi`：
  - GPU 节点 `gpu@3d00000`，`compatible = "qcom,adreno-44050a01"`，带 zap 节点；
  - GMU 节点 `gmu@3d6c000`（长 `0x68000`，`qcom,adreno-gmu-840.1`），中断是 SPI 304（HFI）和 SPI 305（GMU）；要开的时钟是 GPUCC 的 AHB、CX_GMU、CXO、HUB_CX_INT；电源域是 `GPU_CC_CX_GDSC` 和 `GX_CLKCTL_GX_GDSC`；
  - GX 时钟控制器 `gxclkctl@3d64000`，挂在 RPMh 的 `RPMHPD_GFX` 电压域上；GPU 时钟控制器 `gpucc@3d90000`；
  - GPU 的 SMMU `iommu@3da0000` 是标准的 `arm,mmu-500`（`qcom,kaanapali-smmu-500`）。GPU 用流编号 0 和 1，GMU 用流编号 5。

注意：安卓设备树里 GMU 写在 `0x3d37000`（`reg-names` 是 `gmu`、`gmu_ao_blk_dec0`），主线写在 `0x3d6c000`，两边对寄存器区域的划分不同，移植时要逐个核对偏移。主线这几段原文存在调研笔记 `kaanapali_gpu_dt.txt` 里。

### Mesa / Turnip（Vulkan）[读到]
- Mesa 主线 `src/freedreno/common/freedreno_devices.py` 已有 `Adreno (TM) 840`（`chip_id=0xffff44050A31`，a8xx_gen2，6 个 CCU）。
- Linux 会把有效分片数写进芯片编号：`0x44050a01` 报给用户态时变成 `0x44050a31`（a8xx_gpu.c）。我们的内核驱动报编号时要照做，否则 Mesa 认不出。
- UniXNU 固定的 Mesa 版本 `85fda3b`（26.2.0-devel）已经带 A8XX 的 Turnip 和 A840 表项，不需要社区的 gen8 补丁（whitebelyash/turnip-gen8 主要补 810/825/829）。[读到，子代理核对]

### metal2vulkan [读到]
`github.com/steelbrain/metal2vulkan`：读取 Metal 的 AIR（metallib 里的 LLVM 中间代码），编成 SPIR-V 和 Vulkan 管线，还带一个"Metal 形状"的运行时。Rust，GPL-2.0+ 或 LGPL-3.0+ 二选一（ios-fuxi 选 LGPL-3.0，做成单独的动态库）。这是最难的"着色器翻译"那一块，已经有人做好。

### UniXNU（iPadOS 17，Adreno 740 / 610）[读到]
`unixnu_review/src/docs/drivers/native-adreno.md`、`docs/devices/sheng.md`：

链路：Metal → NativeMetal 插件 → metal2vulkan（AIR→SPIR-V）→ Turnip（SPIR-V→NIR→IR3）→ NativeAdreno 内核驱动 → GMU、CP、SMMU、命令环 → Adreno 740 → GPU 合成到屏幕。

状态表里 GMU 冷启动、HFI、CP 命令、4K SMMU、Metal 设备发现、render server 合成、GPU 合成桌面都标"works"。代码（BSD-2-Clause-Patent；NativeAdreno 接口部分另有 MIT 声明）：`drivers/NativeAdreno`（约 1.1 万行）、`drivers/NativeMetal`（约 1.2 万行）、`kits/AdrenoFirmware`、`kits/AdrenoPM4`、`kits/ArmSmmu4K`、`kits/AirNir`、`patches/mesa/turnip-darwin-kmd.patch`（给 Turnip 加 Darwin 内核接口，约 1100 行）。

注意 [读到]：真正在 sheng 上跑通的上电顺序在板级文件 `UniXNU-sheng/package/GpuPowerProbe.cpp`，不在通用的 `NativeAdrenoService.cpp`；时钟表、RSC/PDC、HFI 表格式、hw_init、上电寄存器表都是 A740/SM8550 专用。插件 `NativeAdrenoMetalPlugin.mm` 里写死了 iPadOS 17 的 QuartzCore 地址并往 backboardd 内存里写（第 125–127 行），iOS 16 上必须删掉。

### ios-fuxi（iOS 16，小米 13，Adreno 740）[读到，他们自己的文档]
`ref/ios-fuxi`（最新 95e34e5）`docs-lanes/gpu.md`、`metal*.md`、`nativemetal-n1.md`、`turnip-t1.md`、`gpu-k1.md`：

- 真机通过：GMU 启动、HFI、GPU 上电、SMMU、CP 执行命令、zap（之后 2D 引擎才写得进内存）；内核用 GPU 做显示拷贝（约 1.58 ms/帧）；Turnip 填充/计算/三角形测试；`MTLCreateSystemDefaultDevice` 找到他们的设备；Metal 冒烟测试 0–6 阶段；独立的 CoreAnimation 测试进程 1080×2400、60 帧、GPU 约 3.6 ms/帧。
- 没过：真正的 backboardd 用 GPU 时被三个功能连续拒绝（mip 层数 10、memoryless 的 RGBA16Float、第二个颜色附件），每次退回 CPU。所以他们的主屏仍是 CPU 合成。之后的 NativeMetal iOS 16 移植（N1）、Turnip 九补丁（T1）、内核 K1 只在 Linux 上验证过。
- iOS 16 特有：`_MTLDevice` 实例大小 528（iPadOS 17 是 536）；设备发现走 bundle 回退，内核补一个空的 `IOAcceleratorES` 类即可；iOS 16 的 QuartzCore 只带半精度着色器，`supportsNativeHardwareFP16` 要回 YES；UniXNU 的"改 backboardd 内存"换成在设备发现前写环境变量和偏好（关帧缓冲压缩、IOSurface 压缩、DeepFramebuffer）；backboardd 以 uid 501 运行，内核要专门放行它打开 GPU。
- 我们已经带着他们较早的内核补丁：0096（`fuxi_adreno.c`）、0098（NativeAdreno 接口）、0101，其中 0096、0101 与他们最新版逐字节相同；他们新增的 0100/0098 K1（BO 表 1024、时间戳和状态调用）、0102（放行 backboardd）我们还没有。A740 专用的部分（`gpu-a740.h`、写死的 SM8550 地址、`0x43050a01`）不能直接用于 840。

### ios-sweet [读到]
`ref/ios-sweet/site/future.html` 只说 GPU 加速"可能"做，没人给 Adreno 在 iOS 下做过 Metal。

### Turnip 的 Darwin 接口用在 840 上要改什么 [读到，UniXNU `patches/mesa/turnip-darwin-kmd.patch` 和 Mesa 85fda3b；详见调研笔记 `turnip_darwin_a840.md`]
- Mesa 这一侧基本不挑 GPU 代际：芯片编号、GMEM 大小和地址、UBWC 参数都从内核的设备信息调用读取，命令生成由 Turnip 自己按代际处理，85fda3b 已有完整的 gen8 分支（清屏、拷贝、绘制、计算）。要改的只有：去掉把 `0x43050a01` 当作 740 的特例；先用 `TU_DEBUG=sysmem,nocb` 关掉并行分箱（UniXNU、ios-fuxi 和社区 gen8 分支都碰到过 BV/BR 互相卡住）；补上 A8XX 无绑定描述符失效的修复（上游 MR 45129）；确认 CPU 缓存行确实是 64 字节。
- 内核报给用户态的芯片编号必须是 `0x44050a31`（Mesa 只认这个），另报 18 MiB GMEM、GMEM 起始地址 64 MiB。
- A740 专用的东西集中在内核的命令环（UniXNU `NativeAdrenoRing.cpp`）：BR/BV 包装和 Linux A8XX 用的一样，但几个寄存器编号要换成 A8XX 的。例如：写栅栏号 `0x885 → 0x82c`；CP 计时器 `0x980 → 0x8e7`；GMU 计时器 `0x1f888 → 0x1f840`（还要按 GMU 基地址重新算偏移）；性能计数器的清零和状态寄存器也要换；另外新设 BV 读指针 `0x808`。非上下文寄存器、UCHE、UBWC、分片数按 Linux `a8xx_hw_init` 写。

## 3. iOS 16 这一侧怎么接入 [读到，苹果二进制]

（详细地址和符号见调研笔记。）

- 内核：iPhone X 的 GPU 驱动类继承链 `AGXAcceleratorG10P_B0 → … → AGXAccelerator → IOGPU → IOAcceleratorES → IOService`。AGXG10P 的配置写着 `MetalPluginName = AGXMetalA11`，启动后补写 `MetalPluginClassName = AGXA11Device`。
- Metal.framework（`_MTLRegisterDevices`）：`IOServiceMatching("IOAcceleratorES")` 列出 GPU 服务 → 读 `MetalPluginName` 拼出 `/System/Library/Extensions/<名字>.bundle` → 读 `MetalPluginClassName`（没有就用 bundle 的 principalClass）→ 要求是 `_MTLDevice` 的子类 → `initWithAcceleratorPort:` 建设备 → `_MTLAddDevice`。**没有白名单，不要求苹果签名**，只需要过系统的代码签名（加进我们自己的信任列表）。
- 苹果自己的驱动建在 IOGPU.framework 这层通用代码上（设备连接、共享内存、资源、提交、事件），内核侧有 IOGPUFamily 的 50 个通用调用。我们不走这套：我们的插件用自己的内核接口（NativeAdreno 接口），只需要 Metal 能发现并加载插件。
- CoreAnimation（QuartzCore）：必须加载 `default.metallib`（里面有 141 段 AIR [实测]）。预编译管线按设备的 `targetDeviceInfo.name` 找 `default.pipelinelib/pipelines.<名字>`；名字不认识就自动改为从 AIR 现场编译（苹果自己的虚拟机也走这条路）。之后还会查询 GPU 家族、FP16、YCbCr 格式等能力。
- 现在手机走的是软件回退：Metal 和 GLES 上下文都建不起来时，由 `CA::WindowServer::Server::sw_renderer()` 用 CPU 光栅化。主屏墙纸的 HEIC/YUV 图层画不出来、图标画不全，都发生在这条 CPU 路径上 [推断]。

## 4. 三条路线比较

### (a) Metal → Vulkan（Turnip）→ 我们内核里的 Adreno 驱动 —— 推荐
组成：内核 Adreno 840 驱动（上电、GMU/HFI、SMMU、命令环、栅栏、zap、用户接口）+ Turnip 的 Darwin 后端 + metal2vulkan + NativeMetal 插件（iOS 16 版）+ 把 GPU 画好的画面交给我们的显示驱动。
- 已有：除了"gen8 的上电和寄存器"以外，每一层都有在苹果系统上跑通过的实现（UniXNU 在 iPadOS 17，ios-fuxi 在 iOS 16）。
- 最难的点：
  1. **gen8 上电**：SM8850 的 GPU 时钟、电源、RPMh 投票、GMU 固件格式、HFI 表格式都和 A740 不同，只能照 Linux a8xx 代码重写；读到没上电的模块会让整机重启。
  2. **真正的 backboardd**：ios-fuxi 卡在三个功能上（mip 层数、memoryless RGBA16Float、第二个颜色附件）。这三次拒绝来自他们自己写的较小前端 FuxiMetal：`tools/fuxi-metal/Frontend.mm` 里 `textureDescriptorAccepted()` 用白名单检查格式、mip 层数、存储方式 [读到]。UniXNU 的 NativeMetal 本身支持 memoryless 渲染目标（`bridge/NativeMetalObjects.rs:317,431`）、mip（`NativeBridgeMetalFrontend.mm:200`）和多个颜色附件（`bridge/quartzcore_blur_host.mm:43`）[读到]，iPadOS 17 的 backboardd 正是靠它跑通的。ios-fuxi 最新的 N1 就是把 NativeMetal 移植到 iOS 16（`tools/build-fuxi-nativemetal.sh`），但还没上过真机。**所以选 NativeMetal（经 N1 的 iOS 16 移植），不选 FuxiMetal**；backboardd 这一段的风险主要剩下 iOS 16 与 17 的差别。
  3. **构建环境**：插件（Objective-C++）、Rust 库、Mesa 都要编成 iOS 16 arm64 的二进制，需要 Xcode 工具链（GitHub 的 macOS 机器上有）。

### (b) 模拟苹果 GPU（让苹果原装 AGX 驱动跑，内核把 AGX 命令翻译成 Adreno 命令）—— 不推荐
- A11 的 GPU（G10P）命令格式和着色器指令都没有公开资料。Asahi Linux 的逆向针对 M1 及以后（G13+），和 G10 有差异 [推断]。
- 就算知道格式，也要在内核里把苹果 GPU 的着色器机器码实时翻译成 Adreno 指令，比路线 (a) 的"从 AIR 中间代码翻译"难得多。
- 唯一的好处是不用写 Metal 驱动，但 (a) 的 Metal 驱动已经有现成实现，这个好处不存在了。

### (c) 更小的切入点（作为 (a) 的前几步，而不是替代）
1. **只让 GPU 做显示拷贝**：内核用 GPU 的 2D 引擎把 backboardd 画好的画面拷到屏幕，ios-fuxi 已经在真机上这样做过。不改善 CPU 绘制本身，但能最早验证"GPU 能在我们内核里干活"。
2. **独立的 Metal/CoreAnimation 测试程序**：先不碰 backboardd，用一个测试进程验证 Metal 设备、着色器翻译和 GPU 出图，问题都能单独定位。

## 5. 移植清单（按依赖顺序）

不碰手机的准备：
1. 整理 gen8 资料：Linux `a8xx_gpu.c`、`a6xx_gmu.c` 和 `a6xx_hfi.c` 里的 gen8 分支、`a6xx_catalog.c` 的 a840 表（上电寄存器表、GBIF、保护寄存器）；SM8850 设备树里 GPU、GMU、GPUCC、RPMh 投票相关节点（安卓 `/proc/device-tree` 只读导出，存到 `device/`，不进 git）。
2. ~~离线解析 `gen80200_gmu.bin` 的分块布局~~ 已完成 [实测]：固件从手机只读复制到 `device/gpufw/`（被 git 忽略，不提交）。它用的就是 Linux `a6xx_gmu.c` 的 `block_header` 格式（地址、长度、类型、值，后面跟"长度"个字节；长度为 0 的项是开头的目录，装载时跳过）。实际数据只有 4 块：
   - `0x0`，长 `0x1fc` → GMU 指令内存（ITCM，0–16 KB）；
   - `0x4000`，长 `0x1b40c` → Linux 为 gen8 分配的 icache 区（GMU 地址 `0x4000` 起，16 MB 减 16 KB）；
   - `0x10007f98`，长 `0x28`，以及 `0x10007fdc`，长 `0x18` → GMU 数据内存（DTCM，`0x10004000` 起）。

   目录里类型 7 的预留区 `0x60000000`、`0x60001000`、`0x60003000`，落在 Linux 的 dummy 区（`0x60000000`，16 KB）里。结论：Linux 现成的装载规则可以直接照搬。前提是 GMU 通过 SMMU 的流编号 5 访问这些地址，所以 GMU 启动前要先配好 SMMU。
3. 寄存器头文件尽量从 Mesa 的寄存器 XML（MIT）生成，避开 GPL-only 的来源。

手机上的步骤（每一步都要先问用户再上机）：
4. **只读探测**：内核里读 GPUCC 和 GMU 时钟状态、kgsl-smmu 的 ID 寄存器和 SMR，确认 SMMU 能不能由我们直接编程（有被 Gunyah 占用的可能）。只读常开电源域里的寄存器。
5. **第一个可验证点：GMU 冷启动 + HFI 握手**。加载 `gen80200_gmu.bin`，看到 `CM3_FW_INIT_RESULT` 和 `HFI_CTRL_STATUS` 就绪，再发一条 HFI 消息拿到应答。复用我们 ADSP 那套 SCM、QMP、RPMh 投票代码（补丁 0143–0146）。
6. GPU 上电（OOB）→ 装 SQE → `CP_ME_INIT` → 发一条 `CP_MEM_WRITE`，CPU 读回确认。
7. zap 着色器（PAS 13，用我们已有的 SCM 调用，不自己建 SHM bridge）→ 让 GPU 退出安全模式。
8. 发布用户接口（NativeAdreno 接口 2 + ios-fuxi 的 K1），报芯片编号 `0x44050a31`、18 MiB GMEM、3 个分片。
9. Turnip（Darwin 后端，iOS 16 arm64）在一个测试进程里跑填充、计算、三角形。
10. NativeMetal（用 ios-fuxi 的 iOS 16 移植，删掉写 backboardd 内存的代码）+ metal2vulkan 动态库，加进信任列表；内核补 `IOAcceleratorES` 类和 `MetalPluginName`；Metal 测试程序出图。
11. 独立 CoreAnimation 测试进程用 GPU 出图。
12. 真正的 backboardd：解决 ios-fuxi 卡住的三个功能，放行 backboardd 打开 GPU；GPU 画好的画面交给我们的 AppleCLCD（处理缓存一致性）。

## 6. 第一个可验证小步骤

**在我们的内核里让 GMU 冷启动并完成 HFI 握手。** 成功的标志是内核日志里打印出 GMU 固件初始化结果和 HFI 状态就绪，并收到一条 HFI 回复。这一步不需要 Metal、Turnip 或任何用户态改动，只验证"我们能让这块 GPU 的电源管理核心在苹果内核下跑起来"。在它之前，先做第 4 步的只读探测。

## 7. 主要风险

- **整机重启**：访问没上电的 GPU 模块会直接让手机重启（不伤数据，但每次都要重新进 iOS）。上电顺序必须照 Linux gen8 代码，先只读、后写。
- **SMMU 归属**：安卓侧 [实测] `3da0000.kgsl-smmu` 由安卓内核自己的 `arm-smmu` 驱动绑定（compatible `qcom,qsmmu-v500`、`qcom,adreno-smmu`，带 `qcom,use-3-lvl-tables`、`qcom,actlr`），GMU（`3d37000`）和 GPU 的 user/secure/lpac 三个地址空间都是它的使用者，所以上下文页表由普通内核直接编程，不是完全交给虚拟化层 Gunyah。安全上下文（secure）仍可能由安全世界管。风险从"可能整个用不了"降为"要照安卓的做法配置"。
- **gen8 资料只有 Linux 一份**：UniXNU 和 ios-fuxi 都只做过 A740，840 的每个寄存器都要对照 Linux 核实。
- **backboardd 最后一段没人做通过**：ios-fuxi 卡在这里；UniXNU 在 iPadOS 17 上通过，但用了写内存的办法，iOS 16 不能照搬。
- **构建链**：需要 Xcode 工具链编 iOS 16 的插件、Rust 和 Mesa。ios-fuxi 的做法 [读到 `.github/workflows/fuxi-build.yml`]：Metal 插件、metal2vulkan 桥接库在 GitHub 的 `macos-14` 机器上编（带 cargo 缓存），其余在 `ubuntu-latest` 上编。我们的仓库是公开的，编译不限额，可以照做。
- **墙纸**：7400 墙纸的图片是 HEIC（HEVC 视频编码）[读到 ios-fuxi]。HEVC 解码和 GPU 是两块不同的硬件。有了 GPU，CoreAnimation 可以在着色器里直接画 YUV 图片 [推断]，但如果问题出在解码这一步，GPU 解决不了，要另外确认。

## 8. 许可

- UniXNU：BSD-2-Clause-Patent；NativeAdreno 接口另有 MIT 声明。
- metal2vulkan：GPL-2.0+ 或 LGPL-3.0+，按 LGPL 做成单独的动态库，分发时附源码和重建方法。
- ios-fuxi 的 `gpu-a740.h` 来自 Linux，是 GPL-2.0-only，已经在我们的 0096 里；ios-fuxi 自己没有顶层 LICENSE，对外分发他们写的代码前最好先问作者。
- 固件（gen80200_*、zap）和苹果文件一律不进公开仓库。

## 来源

- 调研笔记（含地址、符号、文件:行号）：会话共享笔记目录里的 `ios_metal_driver_notes.md`、`unixnu_gpu_port_notes.md`、`ios_fuxi_gpu_status.md`。
- [Linux 6.19 加入 Adreno 8xx（Phoronix）](https://www.phoronix.com/news/Qualcomm-X2-Elite-GPU-Linux-619)
- [drm/msm/adreno: Introduce Adreno 8xx family support（补丁 v4）](http://www.mail-archive.com/freedreno@lists.freedesktop.org/msg41578.html)
- [Mesa 26.0 加入 Adreno Gen 8 初步支持（Phoronix）](https://www.phoronix.com/news/Freedreno-Lands-Adreno-Gen-8)
- [Mesa freedreno_devices.py（主线）](https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/src/freedreno/common/freedreno_devices.py)
- [whitebelyash turnip/gen8](https://github.com/whitebelyash/mesa-unified/tree/turnip/gen8)
- [metal2vulkan](https://github.com/steelbrain/metal2vulkan)
- [UniXNU](https://github.com/UniXNU/UniXNU)（本地 `unixnu_review/src`）
- ios-fuxi（本地 `ref/ios-fuxi`，最新 origin/main 95e34e5）
