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
| GMU | `0x3d37000`，长 `0x68000`（到 `0x3d9f000`，覆盖 Linux 清单里最高的 `0x3d8ec50`）；另有 `gmu_ao_blk_dec0` 在 `0x3d40000`，长 `0x10000`；compatible `qcom,gen8-gmu` | `qcom,gmu@3d37000` 的 `reg` |
| GPU 其他区域 | rscc `0x3d50000`（= GPU 基地址 + `0x50000`，和 Linux a8xx 一致），cx_dbgc `0x3d61000`，cx_misc `0x3d9e000`（长 `0x2000`） | `qcom,kgsl-3d0` 的 `reg` |
| GPU 的 SMMU | `kgsl-smmu@3da0000`（qsmmu-v500 / adreno-smmu，安卓由 `arm-smmu` 驱动绑定） | 设备树、`/sys/bus/platform/devices` |
| GPU 时钟控制器 | `0x3d90000`（SMMU 的供应者之一） | `/sys/bus/platform/devices/3da0000.kgsl-smmu` |
| UBWC | 版本 6 | `qcom,ubwc-mode` |
| zap 保留内存 | `gpu_microcode_region@9959a000`，长 `0x2000`，no-map；在 4 GB 以下，不在给 iOS 的 5 GB（`0x8c0000000` 起）里，不冲突 | zap-shader 的 `memory-region` → `/reserved-memory` |
| 频率表 | 主线 `kaanapali.dtsi` 的 `gpu_opp_table`（160 MHz 起），和速度档 `gpu_speed_bin` 有关 | 主线设备树 |
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

### 着色器翻译实验（不用手机，在 WSL 里实际跑过）[实测]
目的：确认 iOS 16 的 CoreAnimation 着色器能不能被 metal2vulkan 翻译成 Vulkan 能用的 SPIR-V。脚本和说明在 `tools/gpu_m2v_census/`。
- 输入：我们根盘里的 `QuartzCore.framework/default.metallib`（SHA-256 `11c74cc4…`，和 ios-fuxi 记录的是同一个文件）有 141 个入口函数（39 顶点、95 片元、7 计算）；`default.pipelinelib/functions` 拆出 451 条已特化的单函数记录。
- 工具：metal2vulkan（a2c440b）的命令行，Rust 1.99.1 编译；每个输出都用 `spirv-val`（SPIRV-Tools 2025.1）按 Vulkan 1.2 和 1.4 校验。
- 结果：

| 设备描述 | 主库 141 个 | 预编译库 451 条 | "帧缓冲读取"拒绝 |
| --- | --- | --- | --- |
| 不带"按光栅化顺序访问附件"扩展（vk12-floor） | 未补参数时 65 通过 | 290 通过 | 20 + 161 = 181 |
| 带这个扩展（lavapipe，和 Turnip 一样） | **141 全部通过** | **451 全部通过** | 0 |

  其余失败都是测试参数的问题，补齐后就通过：顶点和片元要配对得上；部分片元要第二、第三个颜色附件（`path_blit` 用 BGRA8、BGRA8、R32Float）；6 个万能着色器要给出函数常量（顶点 4 到 5 个，片元 38 个，实验里全填 0）。
- Turnip 对所有 Adreno 都无条件声明了 `VK_EXT_rasterization_order_attachment_access`（颜色、深度、模板三种），主线和 85fda3b 都是 [读到 `src/freedreno/vulkan/tu_device.cc`]。ios-fuxi 统计里那 181 个"帧缓冲读取"拒绝，正是因为他们用的设备描述没开这个扩展。
- **再往下一层：用 Turnip 编成 Adreno 840 的机器码 [实测]。** 用 Mesa 主线（64245a2）编出 Turnip 和 freedreno 的 drm-shim。drm-shim 在电脑上冒充一块 GPU，Mesa 官方 CI 也这样用。我在本地副本里给它加了一条 840（`chip_id 0x44050a31`、18 MiB GMEM），Turnip 认出的设备是 "Adreno (TM) 840"，Vulkan 1.4。再用 `vulkaninfo --json` 从这台模拟设备导出 Turnip 的真实能力描述，用它重新翻译全部着色器；然后 `tools/gpu_m2v_census/vk_pipeline.py` 按每条管线的 meta 建真正的 Vulkan 管线，Turnip 在建管线时把着色器编成 840 的指令。通过 `VK_KHR_pipeline_executable_properties` 读到编译后的指令数，例如 axial_shading：顶点 78 条、片元 189 条。

| 用 Turnip 在 840 上的真实能力描述 | 结果 |
| --- | --- |
| metal2vulkan 翻译 | 592 个里 **590** 个通过 |
| Turnip 编成 840 机器码 | 这 590 个**全部成功**，0 失败，0 崩溃 |
| 唯一的缺口 | `inplace_copy_lpf` / `inplace_copy_lph` 两个：用了 Metal 的光栅顺序组（raster order groups），翻译到 Vulkan 需要片元着色器像素互锁，Turnip 不支持 |

- **将来实际要用的 Mesa 版本也做了同样的测试 [实测]**：UniXNU 和 ios-fuxi 固定用 Mesa `85fda3b36405bcaea8bffc64aa4f2651dc90770d`（26.2.0-devel），我在它上面按 UniXNU 记录的顺序（`scripts/data/adreno-compiler-inputs.json`）打上全部 9 个补丁，包括 `turnip-darwin-kmd.patch`，全部打上。这份源码自带 840 条目，在 Linux 上按 msm 版本编出来，同样把这 590 个着色器全部编成 840 的机器码，0 失败。Darwin 后端只在指定 `darwin` 内核接口时才编译，要在 macOS 机器上编，这里没测。另外，这组补丁在 Mesa 主线上大多已经打不上了，所以短期应该继续用 85fda3b。
- **编译开销** [实测，电脑上]：592 个着色器的翻译（8 个并行，包括反复尝试配置）约 18 秒；590 条管线交给 Turnip 编成 840 指令（每条单独起一个进程，8 个并行）约 10.5 秒。UniXNU 在真机 A740 上冷启动时编了 77 到 103 条管线，约 40 秒，之后靠缓存（`native-adreno.md` 的 "Shader compilation" 一节）。推断：开机第一次会有几十秒的着色器编译，之后缓存命中，不是长期负担。
- 关于这个缺口：iOS 16 的 CoreAnimation 会先查询设备支不支持光栅顺序组 [读到 ios-fuxi `metal-m6.md:29`，"Observed CA capability queries"]。UniXNU 的 NativeMetal 按 Turnip 的真实能力回答"不支持"（`bridge/NativeMetalObjects.rs:426`），iPadOS 17 的 backboardd 照样用 GPU 合成跑通了桌面。所以 CoreAnimation 很可能在不支持时就不用 `inplace_copy`，改走别的路 [推断，要真机确认]。
- 早先用 lavapipe 的描述时，`inplace_copy` 会被翻译成"互锁 + 片元写图像"的方案（lavapipe 支持互锁），Turnip 编译它时崩溃。这说明**测试必须用目标驱动的真实能力描述**，用别的驱动的描述会得出错误结论。
- 没验证到的：着色器在 840 上的执行结果是否正确（drm-shim 不执行任何命令）；函数常量全填 0 的那 6 个万能着色器，CoreAnimation 运行时实际会用别的常量组合。

### 全系统计算内核普查 [实测，脚本 `tools/gpu_m2v_census/census_kernels.sh`]
iOS 根盘上一共 138 个 metallib，入口函数里有 7620 个计算内核（另有 1762 个片元、1075 个顶点函数没做普查）。全部用 Turnip 在 840 上的真实能力描述交给 metal2vulkan 翻译，缺的函数常量按类型逐个填 0，通过的再交给 Turnip 编成 840 机器码：

| | 数量 |
| --- | --- |
| 计算内核总数 | 7620 |
| 翻译通过，并且 Turnip 编成 840 机器码 | **7170（94.1%）**，Turnip 编译 0 失败 |
| 其中 Espresso（Core ML 的神经网络库，常量很多，改用 `census_lib_parallel.sh` 按内核并行测） | 1981 个全部通过 |
| 没通过 | 450 |没通过的里面，真正属于 metal2vulkan 功能缺口的都在和主屏画面无关的库里：MPSNeuralNetwork 的卷积内核 235 个（采样器的一种用法还没实现）、MTLReplayer.app（开发者用的 GPU 回放工具）56 个（内建函数签名不匹配）、NRFV3 相机降噪 13 个（imageblock 等）、CoreRE（AR）4 个、其他 2 个。其余失败都是测试输入问题：缺链接函数、可见函数表、stage input、存储纹理格式、tile 附件。QuartzCore 的 4 个就是 tile 函数。**和主屏、墙纸、小组件、图标相关的库里没有发现翻译缺口。**

### Linux 的 a840 数据和这台手机原厂驱动的对照 [实测，详见调研笔记 `kgsl_vs_linux_a840.md`]
从手机只读复制了安卓的 `msm_kgsl.ko`（带符号），解析出 `adreno_gpu_core_gen8_2_1` 和它指向的各张表，逐条和 Linux 的 a840 条目比对：
- **一致**：固件名（sqe、aqe、gmu；zap 在 KGSL 里叫 `gen80200_zap.mbn`）、GMEM 18 MiB、highest bank bit 16、GMU chip id `0x08020100`、54 段保护寄存器、48 项上电恢复列表和 31 项按管线的扩展表、UCHE 和 GMEM 基地址等。`gen8_2_1` 和 `gen8_2_0` 两个结构只差版本号，表格共用。
- **不一致，写驱动时以原厂为准**：非上下文寄存器里有几项不同，`RB_RBP_CNTL` 原厂 `0x60`、Linux `0x20`，`SP_CHICKEN_BITS` 和 `SP_CHICKEN_BITS_2` 也不同，两边还各有几项对方没有；GBIF 原厂多一项 `GMUCX_MRC_GBIF_QOS_CTRL=0x33`；时钟门控 `GMUAO_CGC_MODE_CNTL` 原厂 `0x02220000`；UBWC 的 `RB_CMP_NC_MODE_CNTL` 原厂 `0x28040`、Linux `0x28046`。安卓设备树里 `qcom,min-access-length` 是 32（不是 64）[实测]，所以 UBWC 模式寄存器里和它有关的那一位保持默认。
- **通信方式**：原厂驱动用"硬件调度"模式，命令由 GMU 接收；Linux 用老式 HFI，命令走 CP 命令环。固件里的版本字段 [实测]：手机这份 `gen80200_gmu.bin` 的 HFI 版本是 `0x20070001`（固件块类型 6，定义见 KGSL `kgsl_gmu_core.h`，`GMU_BLK_TYPE_HFI_VER`），和 linux-firmware 里的 `gen80200_gmu.bin` **完全相同**；GMU 核心版本手机是 `0x50030027`，linux-firmware 是 `0x50030017`，手机的稍新。Linux 主线正是用 linux-firmware 这份、按老式 HFI 跑 840 的，所以协议版本这一层兼容。后备方案：linux-firmware 的 `gen80200_*` 可公开再分发，AQE 和手机上的完全相同（SHA-256 一致），GMU、SQE 版本稍旧。

### CPU 和 GPU 之间的缓存一致 [读到]
Linux 的 a840 条目带 `ADRENO_QUIRK_HAS_CACHED_COHERENT`（`a6xx_catalog.c`），主线设备树里 GPU 的 SMMU 标了 `dma-coherent`，安卓设备树的 kgsl-smmu 节点也有 `dma-coherent`。Linux 因此允许 `MSM_BO_CACHED_COHERENT` 缓冲区（`msm_gem.c:1211-1220`）：GPU 读写 CPU 缓存里的数据时，由硬件保证一致。推断：只要 SMMU 页表把这些映射设成可缓存、可共享的属性，iOS 的 IOSurface 交给 GPU 读写、再交给我们的 AppleCLCD 显示时，不必每次手动刷缓存。ios-fuxi 目前在每次提交前手动刷缓存，那是更保守、更慢的做法。

## 3. iOS 16 这一侧怎么接入 [读到，苹果二进制]

（详细地址和符号见调研笔记。）

- 内核：iPhone X 的 GPU 驱动类继承链 `AGXAcceleratorG10P_B0 → … → AGXAccelerator → IOGPU → IOAcceleratorES → IOService`。AGXG10P 的配置写着 `MetalPluginName = AGXMetalA11`，启动后补写 `MetalPluginClassName = AGXA11Device`。
- Metal.framework（`_MTLRegisterDevices`）：`IOServiceMatching("IOAcceleratorES")` 列出 GPU 服务 → 读 `MetalPluginName` 拼出 `/System/Library/Extensions/<名字>.bundle` → 读 `MetalPluginClassName`（没有就用 bundle 的 principalClass）→ 要求是 `_MTLDevice` 的子类 → `initWithAcceleratorPort:` 建设备 → `_MTLAddDevice`。**没有白名单，不要求苹果签名**，只需要过系统的代码签名（加进我们自己的信任列表）。
- 苹果自己的驱动建在 IOGPU.framework 这层通用代码上（设备连接、共享内存、资源、提交、事件），内核侧有 IOGPUFamily 的 50 个通用调用。我们不走这套：我们的插件用自己的内核接口（NativeAdreno 接口），只需要 Metal 能发现并加载插件。
- CoreAnimation（QuartzCore）：必须加载 `default.metallib`（里面有 141 段 AIR [实测]）。预编译管线按设备的 `targetDeviceInfo.name` 找 `default.pipelinelib/pipelines.<名字>`；名字不认识就自动改为从 AIR 现场编译（苹果自己的虚拟机也走这条路）。之后还会查询 GPU 家族、FP16、YCbCr 格式等能力。
- 现在手机走的是软件回退：Metal 和 GLES 上下文都建不起来时，由 `CA::WindowServer::Server::sw_renderer()` 用 CPU 光栅化。主屏墙纸的 HEIC/YUV 图层画不出来、图标画不全，都发生在这条 CPU 路径上 [推断]。

### NativeMetal 的 iOS 16 移植（ios-fuxi N1）和我们系统库的对照 [详见调研笔记 `n1_private_api_check.md`]
检查对象：UniXNU NativeMetal 固定版本 a96fece，打上 ios-fuxi 的三个补丁（都能干净打上）。
- **都存在** [读自我们的 dyld 共享缓存]：N1 子类化或查找的私有类、协议（`_MTLDevice`、MTLDevice、MTLDeviceSPI 等），用到的 C 函数（`sys_dcache_flush`、IOSurface、IOKit 等）。N1 没有链接任何 Metal 私有 C 函数。
- **`_MTLDevice` 实例大小 528 字节**，直接从我们的系统库读出，和 N1 写死的值一致。
- **iOS 16 的设备登记流程** [读自二进制]：`_MTLAddDevice` 只检查两个协议，再调 `initLimits` 和 `initFeatureQueries`。N1 两样都有。
- **UniXNU 的 iPadOS 17 专用做法**（读 QuartzCore 内部字段、vm_read 等）在 N1 里都已经去掉 [读自源码]。
- **缺口**：CoreAnimation 判断图像格式（能否生成 mip、IOSurface 能否当纹理）时，会通过 Metal 的格式查询问设备 `isMsaa32bSupported`、`isFloat32FilteringSupported`、`supportsASTCTextureCompression`、`supportsBCTextureCompression`（偶尔还有 `retainedLabel`）。N1 都没实现。设备收到没实现的方法会算一次失败，backboardd 会被记过。上机前要给它们补上固定答案。另一个没实现的 `newRenderPipelineStateWithTileDescriptor:...` 受 `supportsTileShaders` 控制，N1 回答"否"，会跳过。
- **行为差异** [推断]：N1 的 Rust 桥接层按 iPad 的 A10X 回答 GPU 家族（Apple1–3），而我们的系统是 iPhone X（A11，属于 Apple4）。系统有些判断可能会走和原机不同的分支。
- **别的 Metal 使用者** [读自二进制，详见同一份笔记的 "Other Metal clients"]：
  - **CoreImage 是最大的缺口**：图标服务、CoreUI、墙纸渲染都要经过它。它编译滤镜有三条路，默认的 `newLibraryWithImageFilterFunctionsSPI:…`、函数拼接（`newDagStringWithGraphs:`、`newLibraryWithDescriptorSPI:error:`、动态库），以及运行时编译源代码 `newLibraryWithSource:options:error:`。N1 一个都没实现，UniXNU 的 NativeMetal 也没有；metal2vulkan 的覆盖表里，源代码编译明确不在支持范围内，函数拼接还没做（`docs/METAL-COVERAGE.md:58,1419-1422`）。
  - **MPS**：第一次用时会问 5 个 N1 没回答的特性（`supportsArrayOfTextures`、`supportsQuadShufflesAndBroadcast`、`supportsSIMDShufflesAndBroadcast`、`supportsReadWriteTextureArgumentsTier2`、`supportsNorm16BCubicFiltering`）。
  - **PhotosUICore**：`limits` 返回空时调用方会直接崩溃，只有用到照片渲染的墙纸才会碰到。
  - **没有缺口的**：PaperBoardUI、PosterBoard、WallpaperKit、CoreMaterial、SpringBoardHome、ImageIO、MPSImage；小组件相关框架不直接调用设备方法。
  - **怎么应对** [推断]：UniXNU 的做法是 Metal 设备默认只发给渲染服务（backboardd），别的进程要列进名单才有（`--publish` / `--clients`），没拿到设备的进程照旧走 CPU。所以第一阶段只给 backboardd，CoreImage 的缺口碰不到。核实过 [读自二进制]：QuartzCore 只链接了 Metal、没有链接 CoreImage，引用的类里也没有任何 `CIContext`、`CIImage`、`CIFilter`；`/usr/libexec/backboardd` 链接的是 QuartzCore 和 Metal，同样没有 CoreImage。以后要让图标、墙纸也用 GPU，再补这几个方法。
  - **CoreImage 内部怎么做的** [读自二进制，详见调研笔记 `coreimage_metal_library.md`]：选哪条路由 CoreImage 自己的数据和环境变量 `CI_ENABLE_METAL_DAG`、`CI_ENABLE_FUNCTION_STITCHING` 决定（默认都开），不问设备。`_MTLDevice` 的通用实现把每个函数的 AIR 打包，经 XPC 送给 MTLCompilerService；那边用 LLVM 读入，再调 `libComposeFilters.dylib` 的 `composeImageFilterFunctionsFromModulesSPI` 组合出新的 AIR 内核 `ciKernelMain`。全程是 AIR 进、AIR 出，不碰 GPU 后端。
  - **iOS 16 的 CoreImage 没有 CPU 兜底**：只有 Metal 和 GL 两种上下文。所以哪个进程拿到我们的 GPU 设备，它的 CoreImage 绘制就会坏。这也是第一阶段只给 backboardd 的理由之一。
  - **我们的插件拿不到苹果的通用实现**：N1 把基类方法都转发掉了，输入、输出对象的类型也不兼容。
  - **最小的实现办法** [推断，未验证]：在插件进程内，用系统自带的 `libLLVM.dylib` 的 C 接口（`LLVMParseBitcodeInContext2`、`LLVMLinkModules2`、`LLVMWriteBitcodeToMemoryBuffer`）读入 CoreImage 函数的 AIR，调 `libComposeFilters` 组合出 `ciKernelMain`，再交给 metal2vulkan。同时在这些进程里设 `CI_ENABLE_FUNCTION_STITCHING=0`，让内置滤镜都走这条路。
  - **已经验证的部分** [实测]：CoreImage 自带的计算内核（`default.metallib`，以及 `photo_style`、`portrait_filters`、`portrait_filters_fullsize`、`redeye_repair` 几个 archive，共 367 个）用 Turnip 的真实能力翻译，**364 个通过，并且全部被 Turnip 编成了 840 的机器码**，0 失败。剩下 3 个只是缺函数常量。组合出来的 `ciKernelMain` 只能在真机上生成，没测。

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

**在我们的内核里让 GMU 冷启动，并收到它对第一条 HFI 消息的回复。** 这一步不需要 Metal、Turnip 或任何用户态改动，只验证"我们能让这块 GPU 的电源管理核心在苹果内核下跑起来"。在它之前，先做第 4 步的只读探测。

按 Linux gen8 的顺序（完整的寄存器级清单在调研笔记 `gen8_bringup_sequence.md`，每一项都标了 Linux 文件和行号）：
1. 投 RPMh 的 CX 电压和总线带宽（解析 cmd-db，复用 0143–0146 的代码），打开 GPUCC 的 PLL、GMU 时钟源、hub 时钟和各分支时钟，打开 `GPU_CC_CX_GDSC`。**在这之前不能碰 GMU、GBIF、RSCC、cx_misc 和 SMMU 的任何寄存器，否则整机重启。**
   Linux 启动 GMU 前的顺序是 [读到 `a6xx_gmu_resume()`]：打开电源域 → GMU 核心时钟设 200 MHz、hub 时钟设 150 或 200 MHz 并打开 → `a6xx_gmu_set_initial_bw()` 按 GPU 频率表投一次内存总线带宽 → 装固件、启动 GMU → HFI。所以**带宽要在 GMU 启动前投好**。840 要投的总线节点是 catalog 里列的 SH0、MC0、ACV，都是 RPMh 的 BCM 投票，和我们给 ADSP 做的 RPMh 投票走同一个硬件通道。我们内核里已有现成实现 [读到]：ios-fuxi 的 Wi-Fi 补丁 0099 里的 `wl_bcm_lookup`、`wl_bcm_bandwidth`、`wl_bcm_encode` 按 Linux 规则查 cmd-db、编码并提交 SH0、MC0、ACV 的带宽投票（0099 第 385–534 行），GPU 可以照搬。
2. SMC 调用 `GPU_INIT_REGS`：750 和 a8xx 上 Linux 都会请求常开电源和温度传感器 [读到 `a6xx_gmu.c:1211-1235`]。
3. 配置 GPU 的 SMMU：流 5 给 GMU，把 GMU 地址空间映射好（icache `0x4000`，dummy `0x60000000`，debug `0x60400000`，log 和 hfi 两块各 16 KB）。
4. 按 `block_header` 格式装入 `gen80200_gmu.bin`（见第 5 节第 2 条），启动 GMU，等它报告初始化完成。
5. **成功标志**：发出第一条 HFI 消息并收到回复。a8xx 上第一条不是 MSG_INIT，而是性能表 `MSG_TABLE`（a8xx 专用格式）[读到 `a6xx_hfi.c:280-337, 972-1006`]；后面依次是带宽表、ACD、IFPC、CORE_FW_START、START。

GMU 的寄存器地址都按"GPU 基地址 `0x3d00000` 加偏移"换算，所以安卓（GMU 在 `0x3d37000`）和主线（GMU 在 `0x3d6c000`）的两种设备树写法算出来的绝对地址是一样的 [推算，见笔记]。

第一次上机建议的简化：只用一个命令环、不开抢占、不开 IFPC（防止 GMU 在两步之间把 GX 断电）、不开 ACD、从低频档开始。RSC/PDC 由 AOP 配置，我们不碰 [读到 `a6xx_gmu.c:679-681`]。GX（真正干活的 GPU 核心）的电由 GMU 固件在收到 OOB 请求后自己打开，我们不直接投 GFX 电压。

## 7. 主要风险

- **整机重启**：访问没上电的 GPU 模块会直接让手机重启（不伤数据，但每次都要重新进 iOS）。上电顺序必须照 Linux gen8 代码，先只读、后写。
- **SMMU 归属**：安卓侧 [实测] `3da0000.kgsl-smmu` 由安卓内核自己的 `arm-smmu` 驱动绑定（compatible `qcom,qsmmu-v500`、`qcom,adreno-smmu`，带 `qcom,use-3-lvl-tables`、`qcom,actlr`），GMU（`3d37000`）和 GPU 的 user/secure/lpac 三个地址空间都是它的使用者，所以上下文页表由普通内核直接编程，不是完全交给虚拟化层 Gunyah。安全上下文（secure）仍可能由安全世界管。风险从"可能整个用不了"降为"要照安卓的做法配置"。
- **gen8 资料只有 Linux 一份**：UniXNU 和 ios-fuxi 都只做过 A740，840 的每个寄存器都要对照 Linux 核实。
- **backboardd 最后一段没人做通过**：ios-fuxi 卡在这里；UniXNU 在 iPadOS 17 上通过，但用了写内存的办法，iOS 16 不能照搬。
- **构建链**：需要 Xcode 工具链编 iOS 16 的插件、Rust 和 Mesa。ios-fuxi 的做法 [读到 `.github/workflows/fuxi-build.yml`]：Metal 插件、metal2vulkan 桥接库在 GitHub 的 `macos-14` 机器上编（带 cargo 缓存），其余在 `ubuntu-latest` 上编。我们的仓库是公开的，编译不限额，可以照做。
- **墙纸**：7400 墙纸的图片是 HEIC（HEVC 视频编码）[读到 ios-fuxi]。HEVC 解码和 GPU 是两块不同的硬件。有了 GPU，CoreAnimation 可以在着色器里直接画 YUV 图片 [推断]，但如果问题出在解码这一步，GPU 解决不了，要另外确认。

## 7.5 桌面调研已经查不出、只能上机回答的问题

1. 从 fastboot 启动到我们内核接手时，GPU 的时钟和电源处于什么状态。**已缩小** [读到 ios-fuxi 在 SM8550 上的实测，同样是 fastboot boot、不带安卓]：他们按"上一级确认有电，才读下一级"的顺序读（`gcc_gpu_cfg_ahb` 使能位为 1 才读 GPUCC；CX GDSC 和 SMMU 投票时钟开着才读 SMMU；CX 和 GX 都开才读 GPU 核心寄存器），全程没出事。时钟起来后，GMU 处于复位中，HFI 未就绪，GX 的电从不由 CPU 直接打开。读时钟状态要看使能位：有硬件门控时 CLK_OFF 位会显示 1，但时钟其实是开的（UniXNU 读到 `0x80000003`）。接管瞬间的原始寄存器值两边都没导出，840 上要我们自己读。
2. 没有安卓内核时两个安全调用能不能成功。**已缩小一半**：
   - zap（PAS 13）：ios-fuxi 在 SM8550 上从 XNU 里调用成功 [读到]。init_image、mem_setup、auth 三步都返回 0，之后 TRUST_CNTL 从 1 变成 0，GPU 的写入才生效；不装 zap 时，GPU 的 2D 写入会被静默丢弃。他们的做法：SMC64 + SIP 调用，不用 SHM bridge，元数据放在一块固定的、4K 对齐、物理连续、不走缓存的保留内存里，一直留到复位。另外 `is_supported(13)` 返回 0，不能当真，直接调用就行。我们的 ADSP PAS 调用也已经在这台手机上做通过。
   - `GPU_INIT_REGS`：两个项目都没用过，这符合 Linux 只在 750 和 a8xx 上调用它的规则。不过这台手机的安卓 KGSL 驱动里有 `gen8_scm_gpu_init_cx_regs`、`gen8_scm_gpu_tsense_default`、`gen8_scm_gpu_tsense_work`，以及报错字符串 "Scm call for requests:0x%x failed" [实测，`/vendor/lib/modules/msm_kgsl.ko` 的字符串]。说明这台手机的安全世界支持这个调用，安卓每次开机都在用。剩下的不确定只是"从我们的内核调用行不行"，这和我们已经做通的 ADSP 安全调用是同一类事。
   - 投票：ios-fuxi 在 CPU 侧不发任何 RPMh 或互连投票，沿用 ABL 留下的状态，电压和带宽表通过 HFI 交给 GMU 去投。而 Linux 在 GMU 启动前会先投一次带宽。840 上要不要先投，由我们自己实测决定。
3. 我们能不能直接配置 `0x3da0000` 那块 SMMU 的上下文。**已缩小** [实测，我们自己的补丁]：0114 从我们的内核读过 apps SMMU（`0x15000000`）的全局寄存器和上下文；0131 在 apps SMMU 里新增了流匹配项（SMR/S2CR，流 `0x536` 和 `0x523`），给触摸的 GPI DMA 用，触摸因此能工作。也就是说，在这台手机上，我们的内核写 arm,mmu-500 的寄存器是被允许、也有效的。GPU 的 SMMU 是同一种 IP 的另一个实例，区别是要先打开 GPUCC 的时钟才能访问（安卓设备树里它的供应者包括时钟控制器 `0x3d90000`）。安全上下文（secure）我们第一阶段用不上。
4. GMU 固件 `gen80200_gmu.bin` 在我们按 Linux 顺序准备好的环境里，会不会报告初始化成功、会不会回复第一条 HFI 消息 `MSG_TABLE`。
5. iOS 16 上 `_MTLDevice` 的实例大小（ios-fuxi 实测是 528），以及 NativeMetal 经 N1 移植后，真正的 backboardd 会不会再拒绝别的功能。
6. 墙纸的 HEIC 图层：问题是出在 HEVC 解码，还是出在 CPU 画 YUV 图片。有了 GPU 能解决后者，解决不了前者。补充 [读自我们的系统库和根盘]：iPhone X 的 HEVC 解码走硬件（系统库里是 `AppleD5500` / `AppleVXD393` 那一套，以及 `HEVCH8` 等解码器登记），我们的移植没有这个硬件驱动。按名字在系统库里没找到软件 HEVC 解码器；根盘 `/System/Library/VideoDecoders/` 下只有 `AV1SW.metallib`（AV1 软件解码要用的 GPU 着色器）。但主线日志里 mediaserverd 建了解码会话和 L008 缓冲区，CollectionsPoster 也建了 420f 缓冲区，所以解码有没有成功，要看系统日志里 VideoToolbox/ImageIO 的报错才能定。这一项交给主线会话确认。

## 7.6 别人实测踩过的坑（ios-fuxi，SM8550 / A740）[读到]

1. 一次性读完 SMMU 的全部 24 组 SMR 和 22 个上下文时，触发同步异常，整机复位。改用能接住异常的读法后没再出现，原因没查明。
2. GMU 的 ACK 头里回的是命令 ID（如 `0x00010304`），不是另一个编号。
3. 要先写 SQE 指令基址，再写 SQE_CNTL；顺序反了，SQE 会从地址 0 取指令。
4. 不装 zap 时，GPU 的 2D 写入会被静默丢弃。
5. Apple 的汇编器把 `;` 当注释，写在同一行的 `smc` 指令会整条丢掉，要换行分隔。我们 ADSP 的 SCM 代码已经能用，照它的写法即可。
6. 用 `ml_io_map` 一次映射 32 MiB，撑爆了 8 MiB 的 IO 子映射，内核 panic。GPU 寄存器要按块分开映射。
7. GMU 一旦放出复位就锁死，不能再交给下一级程序。所以测试前不能有别的程序先启动过 GMU。

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
