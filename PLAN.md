# 小米 17 Pro (pandora) 跑 iOS 16：完整计划

最后更新：2026-10-28（全局进度见下一节；GPU 一节见 docs/GPU_PLAN.md；CPU 一节在文末）

## 目标和边界

- 目标：小米 17 Pro 上运行 iPhone X 的 iOS 16.7.16 原版系统 + 自己编译的苹果内核（xnu-8796），和安卓做成双系统。
- 做成后能有：桌面、设置、控制中心、触摸和按键、部分自带应用。
- GPU：原计划不做（用 CPU 画画面）；2026-10-23 起改为主线，进展见 docs/GPU_PLAN.md。2026-10-24 主屏幕第一次由 Adreno 840 画出；第 1 轮（k0215）起不再卡死，现在由 GPU 合成整个界面，屏幕直接读 GPU 的图，滑动约 110 帧。
- 原来不做的声音，2026-10-25 起开始做（用户定，先 ADSP 通信、再喇叭）；真实电量 2026-10-26 起做。还不做：蜂窝网络、相机、背屏；Wi-Fi 只调查过。App Store 的应用基本跑不了（解密要靠苹果芯片里的安全模块）。
- 为什么是 iOS 16：iOS 17 起 iPhone 只支持 A12 及以后，那些系统程序都用了苹果特有的指针签名指令；iPhone X 是最后一代不用的。
- 主要参考：ref/ios-fuxi（小米 13 / 骁龙 8 Gen 2，iOS 16 已进桌面，ios-sweet 的复刻）；ref/ios-sweet（红米 Note 10 Pro，已进桌面）；社区全貌见 COMMUNITY.md；[UniXNU](https://github.com/UniXNU/UniXNU)（骁龙 8 Gen 2，4K 页、存储驱动）。

## 全局进度和顺序（2026-10-28 重新整理）

做好了的（真机验证过）：
- 开机进 iOS 桌面，触摸、按键、控制中心；GPU 合成整个界面（Turnip + Metal 插件，VKTEST/MTLTEST 过）。
- 亮度：滑条跟手、熄屏亮屏照 iOS 的渐变时长、重启后开机标志亮度正常（0263/0264，bl2）。
- CPU 调频、核心级休眠、perfcontrol。
- 电量：电量计读数进 SweetBattery，百分比照安卓（bat4）。
- 温度：7 个热敏电阻读数准（GPIO 高阻、PMIC 自动测温），按 mi_thermald 表限频，机身约 40 度不再到 105 度（0255-0257）。iOS 温控接入第 1 步做通过（usb11），现在默认关（pandora_therm_ios=1 才开）。
- USB 串口实时日志（0250-0258）。
- ADSP：启动、GLINK 通信；喇叭的 DSP 播放图能开能跑，TDM 时钟送到两颗功放，功放功率级照安卓运转（spk7）。

卡住的：
- 功放里的 Halo 处理器核一起动就被 MPU 拦下（spk8-spk12 六轮）。已排除：固件写入（整块读回全对）、写法、PLL、系数和场景数据、ROM 停的时机。

没验证或没做的（按原来定的顺序：电源 → 声音 → Metal）：
- 电源：插拔马上反映（0245）没测到；温控第 2 步（这台手机自己的 plist、接 iOS 的背光/充电/CLPC 输出）暂停。
- 声音：功放跑起来以后 → 校准值、PLAY、试音 → iOS 的音频设备（IOAudio2），那时一并拿上游的 work interval 补丁（UniXNU 8b6a0f5）。
- GPU：三个 Metal 问题；着色器预热。
- 以后：原生 USB（iTunes）、原版壁纸、开机画面、80 帧上限、十点触控验证、后台划掉 App 卡几秒（待查）。
- 开机菜单里的 iOS 格子还是 k0244，之后的修正（亮度、串口、限频等）没进去，装要用户说"装"。

思路调整：
1. 扬声器不再一轮猜一个。安卓（已 root）下有 "B/T DSP1 Preload Switch" 开关，关再开会让驱动重新加载、启动 DSP；开 regmap 的写入跟踪（ftrace regmap 事件）抓下这一次从停核到起核的每一笔寄存器读写，照着逐条对。要切到安卓开机一次，上手机前问。spk13（配 MPU 前先强制开 PLL）可以同时编好备用。
2. 上游 UniXNU 的 GPU 拆成 AdrenoMetal（原 NativeMetal，我们的插件基于 c6d29ca，之后 24 个提交：两平面 IOSurface 纹理、系统内存渲染、着色器预热整套）和 Adreno740（原 NativeAdreno，提交路径优化）。三个 Metal 问题和着色器预热合成一项：升级到 AdrenoMetal 最新，再照 Adreno740 补我们的内核驱动。
3. 小件随时插：UniXNU 477e530（开机读 dyld 缓存不塞满内存）。
4. **上游 GPU 比对结论（2026-10-28，6 个只读代理的报告，核查只跑完 Turnip 一份；报告 scratchpad/gpuup_reports.json，草稿在 scratchpad/gpuup/）。用户：「应该是不能直接照搬的」。** 不整体升级 AdrenoMetal：
   - 三个 Metal 问题上游都没修（AdrenoMetal 0008-0016、ios-fuxi 都不碰 metal2vulkan 720ef1c 里拒绝的那三处：render_pass.rs:921-928、render.rs check_pipeline、blend.rs:770 SrgbForm）；我们 439 份日志里没出现过（来自同 SoC 另一位移植者）。问题 3 有候选：上游未合并分支 feat/ipados27-gpu-parity 的补丁 0018（sRGB 扩展范围混合的舍入模型）。问题 1、2 要自己改 metal2vulkan（ycbcr_renders 判断条件；先把格式对打进日志）。
   - AdrenoMetal c6d29ca 之后 24 个提交：0009-0015 和"答案缓存"就是我们的 m2v 0001/0002/0004-0008 和 adapt 第 2 轮；0008+0016 和我们 0003+0009 同一件事不同写法，冲突，保留我们的；着色器预热、管线缓存导出只在上游 publish 模式下跑，我们的 adapt 绕开了。要拿的只有 ce5e19e（被拒时返回会拒绝调用的编码器，不返回 nil）：在 adapt_ios16.py 加一处替换。
   - 内核 GPU 驱动（Adreno740）：每次提交不清 TLB、128 IB 一帧等我们已有。要补：cedcaa2 导入加 kIOMemoryPersistent（只读重试也要接 withAddressRange 返回 NULL）、c285ae8 提交方换了就重置 BR/BV 上下文、按句柄查 BO 并把 4096 上限提到 16384（峰值实测 2576）。自己的：ring_kick 每次提交打一行（日志里 22 万行），改成计数。ABI 线格式和 Adreno740 v2 相同，只差版本号；不动。
   - Turnip：和上游逐字节一样，不动。
   - **同芯片作者（myron，iPadOS 17.7.11 on SM8850）发来的 AEGIRPOSTER-SOLUTION.md（2026-10-28，原件在 D:\QQtalkfiles\Tencent Files\screenshot\，用户：「可以看，按你的顺序进行」）**：三个 Metal 问题就是他们机器上的。AegirPoster 崩溃（SIGABRT，`pipelineState != nil` 断言）根因是 BGR10_XR_sRGB（555）混合被 metal2vulkan 以 ExtendedRangeBlend(SrgbForm) 拒绝 → 他们的 0018-noted-srgb-blend-arithmetic（安全数学的舍入模型，近似）修好，壁纸能应用显示。RGB8_444_2P 用 0021-rgb8-planar-render（加 RGBA8 颜色候选，固定核写回两平面；近似）→ 桌面小组件显示。还有：0017 小内存块（16 MiB，按堆 1/8）、0023 内存压力回收、0025 内存池按 GPU 地址窗口/16；ASTC 204 块布局（插件）；XR10 门只对 backboardd 关、其余进程开；着色器缓存目录用容器目录 + 64 MiB 上限；GPU SMMU 地址空间扩到 4 GiB、每个客户端 384 MiB 窗口；连接数按进度放行。AttachmentFormat(Color(0)) 他们也没解。**我们每次开机 AegirPoster 崩 10 次（SIGABRT），多半同因**；GPU 那一步先做它：拿 0018（他们的或上游未合并分支 feat/ipados27-gpu-parity 的 0018），再按需要拿 0017/0023/0025、XR10 作用域、ASTC。他们的补丁源码在他们的工作区（E:\xnu2，我们没有），要问用户要。
   - xnu：477e530 我们已有 1 MiB 分块读，新的只有 >32 MiB 文件 IO_NOCACHE + 计数，草稿 fix_cs4k_nocache.py（树副本上能打），要真机开两次对比（关预读可能变慢）。8b6a0f5 草稿 fix_wi_instance.py 照 20H392 原版内核做法（6 个文件），接 iOS 音频时做。

## 一、双系统怎么做

- 安卓的所有分区都不动，只把安卓数据分区（userdata）尾部切出 64 GB，新建一个 `ios` 分区放 iOS。
  已确认 userdata 是 sda 这块盘的最后一个分区（起点 28282880、长度 1000869848 个 512 字节扇区，后面只剩 40 个扇区放备份分区表）。
- 平时开机：照常进安卓，什么都不变。
- 进 iOS：手机进 fastboot，电脑执行 `fastboot boot` 我们的启动镜像（只在内存里，不写手机）。
- 回安卓：重启即可。
- 安全：iOS 用的存储驱动只允许写 `ios` 分区那一段地址，其他地址一律拒绝；在第 6 步之前完全只读。
- 另一种做法（待查证）：ios-fuxi 把 iOS 镜像作为一个固定位置的文件放在安卓数据分区里，不改分区表、不清数据；前提是这个文件在存储上是明文，加密怎么处理它的公开文件里没写。查清之前两种都保留。
- 以后可选：不接电脑也能进 iOS（借用 A/B 两套启动分区里闲着的那一套）。系统更新会覆盖它，等主体完成后再评估。

## 二、安卓数据怎么办

- 原地保留做不到：安卓数据分区整体加密，要缩小只能重新格式化。所以是"备份 → 改分区 → 恢复"，只做这一次。
- 备份前先核对手机用量和电脑剩余空间；内部存储和应用数据可以分放在两个盘。
- 备份内容：
  - 内部存储（照片、视频、文档、下载）：adb 拉到电脑，核对文件数和总大小。
  - 应用 + 应用数据：root 下用开源的 Neo Backup 逐个备份；小米自带备份再做一份。
  - 微信、QQ 聊天记录：用它们自己的"迁移到电脑"。
  - 恢复不了、只能重新登录或绑定的：银行、支付、指纹支付、用硬件密钥的登录。两步验证器提前导出。
  - Magisk 模块和隐藏设置：记下清单，恢复后重装。
  - 清空前：关闭"查找设备"，确认记得小米账号密码，否则清空后可能卡在激活锁。
- 这一步排在第 5 步，只有前 4 步都成功才做，做之前单独确认。

## 三、分步计划

每一步：做什么 / 碰不碰手机 / 怎么算成功 / 卡住怎么办。不估时间。

### 第 0 步 准备（进行中）
- 已完成：设备信息、设备树（device/pandora-live.dtb）、分区表（device/partitions.txt）、37 个分区备份 + sda 主/备分区表（backup/，全部核对通过；super 和 userdata 未备份）。个人数据：E:/pandora_backup/selected（照片、微信、QQ、桌面、主题、设置、输入法；其余用户不需要）。
- 待做：
  1. 只读备份 sda 的分区表（开头和结尾两份）。
  2. 从备份里取出原版 boot_b、vendor_boot_b、init_boot_b、dtbo_b，分析 17 Pro 引导程序要求的启动镜像格式。
  3. 在 Docker 的 Linux 里搭本地编译环境：ARM 交叉编译器、mkbootimg、苹果版 clang、ld64 的 Linux 移植、MIG、SDK 头文件，以及 QEMU。
  4. 把 ios-sweet 的 84 个补丁分成"通用"和"红米专用"两类。
  5. 通读 ios-sweet 的 NOTES.md（每次启动尝试和踩过的坑都记在里面），整理出和我们相关的问题。
  6. 只读导出 /proc/iomem；找小米公开的 pandora 内核源码（触摸芯片协议等设备树里没有的细节要从这里查）。
  7. 按 ios-sweet 移植指南（ref/ios-sweet/site/porting.html）列出要换成 17 Pro 的值：可用内存段、中断控制器、屏幕缓冲区地址和尺寸、UFS 地址、计时器、CPU 列表、复位寄存器、按键和触摸的总线地址。
- 碰手机：第 1、6 项，只读。

### 第 1 步 让手机临时启动我们的程序
1. 先用原版 boot_b 做一次 `fastboot boot`，应该正常进安卓，证明这台机允许临时启动；顺便读 fastboot 一次能传多大。
2. 小引导程序在屏幕上打印：运行级别、CPU 型号、是否支持 16K 内存页、内存表。
- 碰手机：临时启动，不写任何分区。最坏黑屏，长按电源键重启。
- 决策点：不让临时启动 → 停下重新评估。不支持 16K 页 → 讨论是否改走 4K 页（工作量大很多）。

### 第 2 步 电脑上先跑通苹果内核
1. 本地编译 ios-sweet 原版内核，在 QEMU 里复现它的启动，证明工具链没问题。
2. 做 17 Pro 版：苹果格式的硬件说明（21 段内存、中断控制器、计时器、8 个核心）和内核里的机器配置，在 QEMU 里用同样的内存布局跑通。
- 碰手机：不碰。

### 第 3 步 苹果内核在真机上跑起来
- 顺序：内核日志显示在屏幕上 → 单核运行 → 内存盘里的测试程序成为第一个进程 → 8 个核心全开。
- 碰手机：每个节点一次临时启动 + 你拍照。
- 卡住可能的原因：高通监管层不让碰的内存或寄存器，表现为手机直接重启（不伤数据）。

### 第 4 步 存储驱动，只读
- 给 SM8850 的 UFS 存储写驱动（参考 ios-sweet 和 UniXNU），在屏幕上列出分区名，和我们导出的分区表对上。
- 同时写好"写保护"：此阶段一律禁止写入。
- 碰手机：临时启动；不写存储。

### 决策点：前 4 步都成功，才讨论要不要做第 5 步

### 第 5 步 双系统分区（唯一清安卓数据的一步）
1. 按"二"完整备份安卓并核对。
2. 临时启动一个只装了分区工具的迷你 Linux（用手机自己的内核，不写启动分区），在安卓数据分区没挂载时改分区表：userdata 缩小 64 GB，尾部新建 `ios`。
3. 重启进安卓，系统会格式化新的数据分区；完成初始设置后恢复数据。
4. 确认安卓一切正常再继续。

### 第 6 步 iOS 进桌面
- 用 iPhone X 的 iOS 16.7.16 固件做出系统镜像（ios-sweet 有脚本），写进 `ios` 分区。
- 驱动只对 `ios` 分区开放写入。
- launchd → SpringBoard，逐个解决崩溃。

### 第 7 步 能操作
- 触摸（芯片型号从设备树查）、电源键和音量键、亮度和熄屏、电量显示。

### 第 8 步 可选
- 不接电脑启动 iOS；用 USB 网络看日志；性能优化。

## 第 1 步结果（2026-10-22，shim/hello，fastboot boot 成功）
- EL1；ABL 把程序放在 0xC7800000；设备树在 0xD5C80000。
- 屏幕：0xFC800000，1220×2656，每行 4880 字节，ARGB8888；直接写缓冲区就会显示（自动刷新开着），不用手动触发。
- 10 个 SSPP 按设备树偏移读出的地址全是 0：寄存器排布和推算的不同，待查（不影响显示）。
- **CPU 不支持 16K 页**（MMFR0 TGran16 = 0；UniXNU 作者也确认）。决定改走 4K 页：iOS 16 那一代内核（xnu-8796）用 BCM2837 的 4K 配置为底，加 ios-sweet/ios-fuxi 驱动补丁，移植 UniXNU 的 3 个 4K 补丁（native-4k-bootstrap-capacity / native-4k-user-abi / native-4k-signature-hash-cache，约 400 行）。

## 第 2 步进展（2026-10-22，模拟器）
- 云端（GitHub Actions，私有仓库 ShueiTsuki/data-a-live，当时名为 ios-pandora）编出 PANDORA 4K 内核；补丁 0082–0101 来自 ios-fuxi，0102–0110 是我们的：
  0102 机型配置（4K）、0103–0105 UniXNU 4K 补丁（0103 手动合并）、0106 4K 编译修正、0107 iOS 地址布局（不用 macOS 大内存布局，否则 4K 启动页表够不着内核）、
  0108 不碰小米 13 的显示寄存器、0109 iOS 的内存分配器区域大小、0110 PANDORA 自己的机型/CPU 名。
- tools/mkadt_pandora.py：17 Pro 的硬件说明生成器（手机模式填真实值，--qemu 为模拟器版）。
- QEMU（wsl/ci_qemu.sh、qemu_run.sh）：内核初始化完成 → 内存盘挂成根目录 → **测试程序作为 PID 1 在用户态运行**（"init running as PID 1"，每 5 秒心跳）。构建 #7（提交 8ed225c，内核 sha f7d03f27…）。

## 第 3 步准备（2026-10-22，未上真机）
- shim/loader：真机引导程序。启动镜像里嵌着内核 + 17 Pro 硬件说明 + 内存盘（测试程序），开机后在屏幕上打几行状态，把内核放到 0x8c0000000，填好启动参数后跳进内核。不写任何存储。
  - 做法：`wsl/make_phone_payload.sh /opt/work/runN` → `shim/loader/build.sh` → `shim/loader/out/loader-boot.img`。
- tools/qemu_loader.py：把这个真机镜像原样放进模拟器跑（模拟器内存按手机地址摆）。硬件说明加 `--qemu-gic`（只把中断控制器换成模拟器的）时，能跑到测试程序成为 PID 1。
- 模拟器抓到并修掉的问题：
  - 0112：内存给到 5 GB 后，macOS 算法给内存压缩器预留 40 GB 地址空间，内核地址空间不够，启动时崩溃。改用 iOS 算法（预留量等于内存大小）。
- 上真机前的安全检查：
  - 存储驱动只在 `rd=ufsroot0` 或 `fuxi_ufs_probe=1` 时启动，写入还要 `ufsrw=1`。命令行里都没有，所以不碰存储。
  - 0111：小米 13 的触摸、按键驱动默认会按小米 13 的地址写时钟、GPIO 和电源管理芯片总线。PANDORA 下改成默认关。
  - GPU、Wi-Fi、电池、USB 驱动在这个配置里默认不启动。
- 当前镜像：构建 #9（run 39674275521，提交 4902f72，内核 sha b9c2e85d…）。
- 真机测试（等你在场）：`fastboot boot shim/loader/out/loader-boot.img`。
  - 预期：先闪过几行黄白字（引导程序），然后内核日志刷屏。
  - 可能停在中断控制器、计时器或某个高通部件上（拍照即可）。
  - 也可能直接重启回安卓。这不伤数据。

## 第 3 步结果（2026-10-22，真机，shim/loader，构建 #9）
- `fastboot boot shim/loader/out/loader-boot.img` 一次成功，不写任何分区。
- 苹果内核在骁龙 8 Elite Gen 5（Oryon 核心）上单核运行：
  - 内存 5 GB 识别正常；中断控制器正常（SGI1 OK）；计时器中断正常（等时钟芯片的 30 秒超时后继续走）。
  - 屏幕驱动认出 1220×2656、地址 0xfc800000。
- 内存盘挂成根目录，**测试程序作为 PID 1 在用户态运行**，每 5 秒心跳，持续稳定（照片里约 2 分钟的心跳）。
- 第 3 步的前三个节点（内核日志上屏 → 单核运行 → PID 1）完成；剩下"8 个核心全开"。
- 8 核（构建 #10，补丁 0113：核心上限 8；超大核编号 0x10000/0x10100 的第三段 Aff2 在唤醒、查核心号、找重分发器三处都保留）：
  - 真机 `cpus=8`：cpu1–5（0x100–0x500）和 cpu6/7（0x10000/0x10100）全部 PSCI 唤醒成功，"8/8 CPUs initialized, SGI1 verified"，"mcache: 8 CPU(s)"，测试程序 PID 1 心跳正常。
  - **第 3 步全部完成。**

## 第 4 步进展（2026-10-22，真机，只读）
- 探测（0114）：SMMU 已给存储（流 0x60）留了直通上下文；控制器开着、传统单门铃模式、时钟开着。
- 诊断（0116/0117）：交接时连接在 HIBERN8、自动休眠关，命令发不出去；见 device/DT_FINDINGS.md。
- 修法：0118 DME_HIBERNATE_EXIT；0119 对设备 WLUN 发 START STOP UNIT（ACTIVE）。
- 结果（构建 #17，`pandora_ufs_probe=2`）：TEST UNIT READY 成功，**读出 LUN 0 的分区表，34 个分区的名字、起点、大小与安卓里导出的 device/partitions.txt 全部一致**（如 userdata 起点 0x35f200、0x77501fb 块；super 0x320000 块）。全程只发了"准备好了吗"、"切到工作状态"、读两次分区表，没有写。
- 正式驱动（0120，构建 #18，`pandora_ufs_probe=3 ufsroot=super`）：sweet_ufs_init 在 PANDORA 下不再写 SM8550 时钟、先退出休眠再 ACTIVE；按名字找到 super（起点 0x3f200、12800 MiB、只读），读出第 1 块开头 `67 44 6c 61 34 00 00 00`（安卓动态分区几何结构的标记 gDla、长度 52），**第 4 步完成**。
- 下一步讨论：iOS 根文件系统放哪。免清数据方案 = userdata 里的钉住文件 + 绕过加密层直接写 sda34 上该文件的块；先只读确认内核是否允许写正在使用的分区（CONFIG_BLK_DEV_WRITE_MOUNTED），再做 64 MB 测试文件。

## 免清数据测试（2026-10-22，真机，tools/pinfile）
- 依据：/data 是 default-key 加密映射（dm-62，1:1 叠在 sda34 上）；小米自己的 cust 就是 sda34 内一段被 linear 直接映射的区域（libfiemap 机制）。内核 CONFIG_BLK_DEV_WRITE_MOUNTED=y。
- 阶段 A：/data/local/tmp/iostest.img，64 MiB，钉住，23 段（2 MiB 对齐），避开 cust（0x61400000–0x10a200000）。
- 阶段 B：直接写 sda34 上第 0 块 → 通过文件系统读只有第 0 块变成乱码，证明位置就是这个文件自己的块。
- 阶段 C：写满 16384 块，从存储读回全部一致；通过文件系统读 16384 块全部变化；文件仍钉住，GC 未动；dmesg 无 f2fs/IO 错误。块位置存档 device/iostest-extents-20261022.json。
- 阶段 D（进行中）：第 1 次重启后复查通过（仍钉住、GC 未动、23 段位置与存档完全一致、16384 块测试内容全在、dmesg 无 f2fs/IO 错误）。待：正常使用几天、多次重启后再查。
- 苹果内核读这个文件（0121，构建 #19，`pandora_ufs_probe=4`，ADT 由 `mkadt_pandora.py --root-extents device/iostest-extents-20261022.json` 生成）：正式驱动以只读打开 userdata，载入 23 段 16384 块；第 0 块开头读出 `IOSPANDORA RAWTEST v1 L=0000000000000000 P=000000005fe00000`；**16384 块全部与 pinfile 写入的内容一致，0 块不一致**。免清数据方案的整条读路径打通。

## iOS 系统镜像（2026-10-22，电脑上，未放进手机）
- 固件：D:/ClaudeAre/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw（d22ap，OS 087-86698-044 / SystemOS 087-86593-047 / AppOS 087-86715-047）。
- WSL 内核没有 HFS+：wsl/prep_root_tools.sh 编了同版本 WSL 内核取 hfsplus.ko（vermagic 一致）并加载；apfs-fuse 现编。
- ios-fuxi mkiosroot.sh → 16 GiB 区分大小写带日志 HFS+（用 12 GB），fsck 通过；fuxi-stage-root.py；sweet-root-tweaks.py 基础修改（launchd 7 处 _PanicOnCrash、删 2 个 CommCenter 任务、containers、迁移、音频、MediaToolbox、SpringBoard 看门狗、跳过设置向导）。mobileactivationd 系统容器不存在，激活数据包跳过（激活靠 ADT 的安全状态属性）。
- 结果：WSL /opt/work/iosroot/ios-root.img（HFSX 签名 HX），SHA-256 62c712f35248d68570977bfb7bf7cbdab1f7835121d6aa1af7412e2f61c42994；未修改的副本 ios-root-base.img。
- d22ap 的 Apple 硬件说明已取出：/opt/work/ipsw/d22ap.adt（mkadt_pandora.py 的 /product 节点从这里来）。

## iOS 系统放进手机（2026-10-22，真机）
- /data/local/tmp/iosroot.img：16 GiB，钉住，4460 段（2 MiB f2fs 段，空闲空间零散；小米整理后重建仍 4460 段）；整理对钉住文件无影响（GC move failures 0）。
- pinfile 段数上限改为 65536；rawwrite 按段内 ≤1 MiB 成批写；内核 0122：位置表上限 16384、二分查找、排序后查重叠（电脑上 800 万次随机查询与逐段对照一致）。
- 先写第 0 块：文件系统读只有第 0 块变化，原始块前 4 KiB SHA-256 与镜像一致；再整个写入（adb exec-in 传输，4 分 15 秒），写入内容与从原始块重读的 SHA-256 都是 62c712f3…2994，与电脑上的镜像一致。安卓 dmesg 无 f2fs/IO 错误。

## 第 6 步：iOS 启动（2026-10-22，真机，tools/boot_ios.sh，rd=ufsroot0）
- 只读启动：HFS+ 日志区要在第一次挂载时初始化（mkfs 只留了"待初始化"标记，Linux 不碰日志），只读模式写不了 → 挂载失败。加 ufsrw=1 后日志初始化成功，根卷挂上，launchd 作为 PID 1 运行。
- launchd 加载 libSystem 失败：共享缓存签名是 16K 页（"disallowing unsupported code signature page shift: 14"），4K 内核不认 → 0124 打开 UniXNU 的 XNU_NATIVE_CS_HASH_CACHE（先按 16K 核对苹果原签名，再算 4K 校验值）。代价：每次开机要读一遍约 3.3 GB 共享缓存，有一段明显停顿。
- 之后 launchd 拉起大量服务（xpcproxy、backboardd、medialibraryd、bluetoothd、fairplayd.H2…）；backboardd 一直找 AppleM2ScalerCSCDriver。原因：SweetFramebuffer 只在屏幕比 iPhone X 小时才报告 1125x2436，17 Pro 比它大 → 报告了 1220x2656。0125 改为报告 1125x2436；0126 新增 sweetfb_fill=1，用 CPU 最近邻放大铺满 1220x2656（不加则居中留黑边）。
- 0123：pandora_initwait=N，launchd 退出时先停 N 秒再 panic，方便拍照。ci_fetch.sh 改为每 10 秒查一次、只下载内核。
- **可写启动（ufsrw=1 rootrw=1 sweetfb_noconsole=1 sweetfb_fill=1，构建 #24）：进入苹果标志。** 日志区重放正常（上次写入的内容恢复）。
- 苹果标志停留：sweetsample 采样 + ipsw dyld a2s（共享缓存 slide = 采样打印的 slide）符号化：SpringBoard 主线程 CCSModuleRepository → LSApplicationRecord 等 LaunchServices 数据库锁，t1 在 __LAUNCH_SERVICES_IS_RESEEDING_ITS_DATABASE_AND_MAY_BLOCK__ 同步等 lsd。即第一次开机 lsd 重建应用数据库，约 25 分钟以上（存储驱动慢）。sweetnoexec=bluetoothd,fairplayd.H2 压掉刷屏。
- **🎉 2026-10-22：iOS 16.7.16 锁屏出现在小米 17 Pro 上**（构建 #24，根 = userdata 里的钉住文件，免清数据）。还不能操作：主屏触摸（Synaptics TCM）、电源/音量键（PMK8850）驱动未做。
- 0127（未上机）：cs_native4k_prepare 每次读 1 MiB，tools/tests/cs_native4k_equiv.c 在 43 个共享缓存文件上与 0105 结果逐字节一致、损坏文件同样被拒，读取次数 166777 → 2630。构建 #25。

## 第 6 步方案草稿（放正式 iOS 系统，未执行，等用户确认）
1. 电脑上做系统镜像：ios-fuxi mkiosroot.sh（三个 APFS 镜像 → 16 GiB 区分大小写、带日志的 HFS+）+ sweet-root-tweaks.py（与手机无关的用户态修改）+ fuxi-stage-root.py。WSL 内核无 HFS+，现编 hfsplus.ko（wsl/prep_root_tools.sh）。镜像算好 SHA-256。
2. 手机上建钉住文件：`pinfile create /data/local/tmp/iosroot.img 16384`（16 GiB，剩余空间 164 GB），status / extents / rawcheck 彩排同 64 MB 测试。
3. 写入：给 pinfile 加 `rawwrite`（从 adb 标准输入读镜像，按块位置写 sda34，每块写前做 inside_one_extent 检查），写完 `rawhash` 在手机上按块位置算 SHA-256，与电脑上一致才算成功。先写第 0 块并 fsread 验证位置，再写全部（同 64 MB 测试的顺序）。
4. 第一次启动只读：`rd=ufsroot0`，不加 ufsrw / rootrw，手机存储零写入，看 launchd 能走到哪。
5. 之后可写启动：`rd=ufsroot0 ufsrw=1 rootrw=1`；写入只能落在这个文件的块里（fx_write_allowed + ufs_scsi 过滤层）。每次都用 tools/boot_ios.sh（复查钉住和位置、HFSX 签名）。
6. 撤销：安卓里删掉 iosroot.img，f2fs 收回空间；其余不受影响。

## 主要风险
- CPU 是高通自研 Oryon 核心（8 Elite Gen 5，6 个性能核 + 2 个超大核），ios-sweet（Cortex-A55/A76）和 UniXNU（骁龙 8 Gen 2，ARM 公版核心）都没在它上面跑过，目前没有苹果内核在 Oryon 上运行的先例。
- 存储、地址转换单元、总线、显示控制器、虚拟化层这些高通自家部件，骁龙 8 Gen 2（UniXNU）比 732G（ios-sweet）更接近 17 Pro，但仍要对照设备树逐项核实。

## 全程规则
- 不刷写安卓的任何启动或固件分区；唯一改动存储的是第 5 步，要单独征得同意。
- 每次真机测试前先问；结果靠你拍照。
- 每次只做一小步，做完汇报，你点头再走下一步。
- 结论分清"看到的"和"推测的"。


## 输入与外设驱动（2026-10-22，内核 #29–#33，补丁 0130–0135）
- 实体按键：电源、音量+、音量-默认开启，状态读取正常；内核自检按键证明 iOS 会响应（电源键熄屏/亮屏）。物理按下那一下还要人按一次确认。
- 主屏触摸：默认开启，芯片在线；内核自检的触摸事件让 iOS 进主屏幕、开控制中心、拖亮度滑块。真手指的坐标和方向还要人摸一次确认。
- 屏幕亮度：AppleARMBacklight 背光服务接收 iOS 的亮度，换算后用 DSI 命令 0x51 直接设面板亮度（读回 0x52 确认面板已生效）；DSI 不通时退回像素调暗。拖滑块要人验证。
- 扬声器：两颗 CS35L43 功放 I2C 应答正常（只探测，`pandora_amp=1`）。出声需要音频 DSP 驱动 I2S0，加上 iOS 能识别的音频驱动接口，没做。
- 摄像头：iOS 相机依赖苹果 ISP 和私有驱动接口，高通 CAMSS 接不进去，没做。
- 无人值守测试：`tools/ios_test.sh 名字 内核号 秒数 "附加参数"`，到时自动重启回安卓并取回日志 device/logs/名字.txt；日志文件 /data/local/tmp/ioslog.img（8 MiB，可删）。

## 硬件适配：和 UniXNU sheng（SM8550）的寄存器核对（2026-10-25）
- DeepSeek 按 `D:\DeepSeekHarness\regcheck\TASK.md` 做了核对表（`RESULT.md`，WiFi 40 项、ADSP/音频 36 项），我逐条对照原文复核（`VERIFY.md`）：大多数正确；改正 W08（pandora ATU 是 `0x410000000`）、A16/A28（GLINK 的 SMEM 项和 APM 端口是协议常数，两边相同）、W16 的时钟投票（寄存器和比特相同，多 phy_aux `0x52018` bit31、cfg_noc_pcie_anoc_ahb `0x52000` bit20）；W09 配置空间和 W31 MSI 只能上机确认。
- 结论：ADSP 通信（SMEM、SMP2P、GLINK、GPR）两边编号一样，最好搬，接在我们已有的 `pandora_adsp.c` 后面；出声还要自己做播放图和两颗 CS35L43（sheng 也没做到）；WiFi 芯片不同（`cnss-peach`，PCIe ID `0x110e`，不是 WCN7850，主线 ath12k 也不认），固件和 MHI 配置要从手机自己的分区查。

## ADSP 通信（2026-10-25/26，内核 0226–0228）
- 用户定：先做 ADSP 通信，WiFi 只调查；固件先用单独的测试包，再做正式的（从手机自己的固件分区读，和安卓、UniXNU 一样）。
- 0226：sheng 的 SMEM、SMP2P、GLINK、GPR 四个库原样放进 `bsd/dev/qcom/`（许可证 BSD-2-Clause-Patent，`wsl/qcom/SOURCE.txt` 记了来源提交号；上游自带的 PC 测试在我们的副本上通过，4183 + 37 项）。`pandora_adsp.c` 在原来的启动步骤上加了：
  1. 启动前：建我们的 SMP2P 项 429（加 TCSR 互斥锁 3），加 "master-kernel" 条目，打开 IPCC 门铃；
  2. 启动后 5 秒内等 DSP 的 "slave-kernel" 就绪位；交接位到了就把 LCX/LMX 投票降回 0；出错时读 SMEM 423 的崩溃原因，并关掉两个镜像；
  3. 就绪后建 GLINK（SMEM 478/479/480），开 "adsp_apps" 通道，问 APM 音频框架是否就绪（1 秒等回答，100 毫秒重试，最多 100 次）。
- 还没有中断：内核线程每 1 毫秒读一次 IPCC（30 秒后改 20 毫秒）。85 MiB 的固件区和各寄存器窗口改用 `ml_io_map_unmappable`，固件拷完就取消映射。
- 测试：`tools/adsp_test.sh 名字 --tc r2q --write-pack --args "pandora_gpu_metal=1"`。测试包是 GPU 固件加 ADSP 镜像，57 个文件 34.4 MB；手机上的 GPU 文件保持 r2q 的样子。之后再跑 gpu_test.sh 要带 `--write-pack`，因为两个包加起来放不进 40 MiB 的固定文件。
- adsp1（0226）失败，GPU 也没起来：开机 0.18 秒，ADSP 线程的启动查询和 GPU 的 GPU_INIT_REGS 同时进了安全世界。GPU 那个调用返回 2（WAITQ_SLEEP，固件让它排队），没人让它继续，之后整次开机的所有安全世界调用都返回 2：GPU 停在第 3 级，ADSP 停在 init_image。以前从没撞上，是因为 GPU 测试不开 ADSP，10 月 22 日测 ADSP 时 GPU 没开到这一级。
- 0227：所有安全世界调用（ADSP 和 GPU 共用 `scm_smc`）加互斥锁，一次一个，和 Linux 以前的 qcom_scm_lock 一样；如果还是收到"排队"，照 Linux 的 waitq 流程处理：轮询 GET_WQ_CTX，代替中断（SPI 962），再发 WAITQ_RESUME。
- adsp2（0227）：加锁后 GPU 正常（vktest PASS 16，没有失败），安全世界没再排队。ADSP 两个镜像第一次都校验通过、放开复位（主镜像 33.8 MB，auth_and_reset 用了 96 ms），但 5 秒内 DSP 没建它的 SMP2P 项 443，门铃一次没响，SMEM 里也没有崩溃原因。
- 0228：照 Linux qcom_q6v5_prepare，在启动前给 DSP 到内存的总线（DT "rproc_ddr"）投最大带宽。按主线 kaanapali 总线驱动，这条路上只有 LP0 是 LPASS 专用的，所以只投 LP0（SH0、MC0、ACV 是主处理器自己的内存通路，不动），交接或失败时撤掉。启动顺序改成 Linux 的：总线票、load_state、LCX/LMX；失败时 load_state 关。`pandora_adsp_wait=毫秒` 可以把等就绪的时间拉长，用来分清是起得慢还是没起来。
- **adsp3（0228）：ADSP 通了。** LP0 总线票一投，DSP 放开复位后 434 ms 建出 SMP2P 项 443，626 ms 报就绪，653 ms 交接（电压和总线票按 Linux 撤掉）。GLINK 版本协商成功，DSP 打开 "adsp_apps"，我们这边 667 ms 通道就绪；问 APM，第一次没回音（1 秒超时），第二次回"框架就绪"，离通道就绪 1001 ms。和 sheng 走到的位置一样。GPU 同时正常（vktest PASS 16，没有失败）。
- DSP 还打开了 8 个我们没接的通道：glink_ssr、pcie_drv、mhi_sat、IPCRTR（QRTR 路由，安卓上音频服务、传感器都走它）、fastrpcglink-apps-dsp、adsp_apps2、sleepmonglink-apps-adsp，统计里丢掉的 92 条消息是发到这些通道的。
- 0229（2026-10-26）：ADSP 固件改成正式做法，和安卓、sheng 一样从手机自己的固件分区读：当前槽的 modem_a/modem_b（安卓的 /vendor/firmware_mnt，在 LUN 4，FAT16）里的 image/ 目录，就地只读。
  - UFS 驱动加了只读接口：在任意 LUN 上按名字找分区（先问 LUN 4），按字节读分区内容，读的范围限在找到的分区里；命令过滤照旧，只放行"测试就绪"和"读"。
  - FAT 读取器用 sheng 的 FatVolume.c，原样拷贝。上游测试（加上手机自己的 adsp、adsp_dtb 固件）在 FAT16 和 FAT32 上都通过；Linux 上要关掉泄漏检查，泄漏的是测试程序自己，不是读取器。
  - 槽由 `tools/boot_ios.sh` 从安卓的 ro.boot.slot_suffix 读出，作为 `pandora_slot=a|b` 传给内核。拿不到分区时退回测试包；`pandora_adsp_fw=pack` 强制用测试包。
  - GPU 固件在 vendor 分区，不是 FAT，所以还留在测试包里。
  - 同时换上 sheng 新版 GLINK：远端接收缓冲表从 24 项扩到 48 项，表满时用大的替换最小的空闲项。DSP 先在 adsp_apps 上发 24 个小缓冲，大的那个以前会被丢掉，adsp3 第一次问 APM 没回音可能就是这个原因。
- **fw1（0229，完整 GPU 包 + ADSP 从分区读）：都正常。** 内核在 LUN 4 找到 modem_b（FAT16，16 KiB 簇），33.8 MB 主镜像从分区读进固件区约 70 ms；DSP 409 ms 建 SMP2P 项，602 ms 就绪，628 ms 交接；APM 就绪。GPU 同一次开机里正常：vktest PASS 17，没有失败，没有重启。GLINK 丢弃的消息从 92 条降到 16 条（新版 GLINK 不再丢 DSP 的接收缓冲）。第一次问 APM 仍然没回音，第二次（1 秒后）回"就绪"，猜测是 DSP 那边的服务还没注册好；重试能处理，暂不追。
- 测试包从此只装 GPU 的东西，`tools/adsp_test.sh` 只在要强制用测试包里的 ADSP 固件时才用。

## 扬声器路线（调查结果，2026-10-22；2026-10-26 开始）
- 用户定的目标（2026-10-26）：音质做到和安卓原生一样、不额外损失，同时按 iOS 的方式接好。上游 sheng 的做法、两边的差别、安卓上实测的做法见 `docs/SPEAKER_RESEARCH.md`（安卓那部分由 DeepSeek 在手机上查，我已核实）。
- 用户转告上游开发者的话（2026-10-26）：「上游是模拟speaker，开发者希望我们有更好的方案做doc」，接着「希望我们不要用这个speaker的方案」。上游音频指南（UniXNU `docs/porting/power.md` §3）照的是 sheng 的做法：功放直通、不跑保护固件和校准、增益固定 18，16 位 I2S，手写的最小 DSP 图；iOS 那侧是音频服务器插件造出来的设备，不是 iOS 原生的接口。**所以不用 sheng 的喇叭方案**：它的功放程序（`ShengCs35l43`）、搭图（`ShengAudioReach`）、插件和音频服务都不拿；通信库（SMEM、SMP2P、GLINK、GPR）不属于喇叭方案，照旧用。做通以后写成上游文档（指南加 pandora 设备页），只写真机核实过的。
- 分步（2026-10-26 按上面改过）：
  1. 硬件准备，和做法无关：apps SMMU 给两个音频数据流加透传表项（最后一个上下文 80 设成不翻译），4 GiB 以下固定留一块 DSP 缓冲（写进 ADT），DSP 登记这块内存；TDM 引脚（GPIO 122–125）；PRM 时钟申请和释放（内容照安卓实测，`D:\DeepSeekHarness\prmcheck\VERIFY.md`）；I2C 驱动从 `pandora_amp.c` 扩出来。
  2. 第一次出声就按安卓：两颗功放加载扬声器保护固件、各自的系数、出厂校准（运行时从手机 persist 分区读，不进仓库）和 music 调音，`PCM Source = DSP`，寄存器照安卓放音时的实际值（`D:\DeepSeekHarness\speakercheck\raw\during\`，注意文件名 T/B 标反了）；DSP 图照安卓：共享内存端点 → PCM 解码 → PCM 转换 → SPR → TDM 端点，48 kHz / 32 位 / 2 声道（这只是 AGM 日志里带标签的模块，完整列表要从 ACDB 读；SPR 是会话时间模块，不是保护——2026-10-26 改正）。不做"先直通出声"这一步。功放的文件在 odm 分区（不是 FAT），放进固定固件包。
  3. 接 iOS，照真 iPhone 的方式：先查 iOS 16（iPhone X）自己的音频驱动栈怎么认设备，让 iOS 自己的音频软件直接驱动我们的设备，不另做音频服务器插件。喇叭 / 听筒切换（GPIO174）、音量都按 iOS 的做法。
  4. 杜比：效果名 `DAP_offload`、驱动 `DlbQdspGefDriver`，可能是跑在高通 DSP 图里的模块（AP 只下参数），不一定是搬不过来的用户空间音效。解析 ACDB 后再定。
- 更好的做法（2026-10-26 用户同意）：功放跑安卓的保护固件和各自的系数、校准、调音；iOS 那侧按 iPhone X 内置音频用的 `IOAudio2Family` 写原生设备，iOS 音频服务器混好的声音直接写进 DSP 读的缓冲；端点、槽、时钟、功放、数据流、缓冲都写进 ADT，换机器换描述。要先查的三件交给 DeepSeek：`D:\DeepSeekHarness\mmapcheck`、`iosaudiocheck`、`ampfwcheck`（电池那份是 `battcheck`）。
- DSP 这一段改正（2026-10-26，mmapcheck 复核）：原先说"DSP 用安卓 MMAP 放音那条路"是错的。资源管理器的喇叭设备没有声明 ULL，安卓从不用 MMAP 放喇叭；安卓放喇叭用的是 DEEP_BUFFER 图，阻塞式共享内存（15360 字节 × 2 块），时间由 SPR 给。ACDB 里有一张没人用的 ULL 放音子图 `0xA1000011`。两种选法都要先把 ACDB 解析出来（喇叭设备那几段子图的完整模块、连接和校准值）再定：照安卓实际用的 DEEP_BUFFER 图（安卓验证过，声音一致最有把握），或换成 ULL 那段流子图、设备那段保持和安卓一样（更贴 iOS 的环形缓冲，但安卓没在喇叭上用过）。
- 功放启动顺序（2026-10-26，ampfwcheck 复核，`D:\DeepSeekHarness\ampfwcheck\VERIFY.md` 末尾）：Halo 核，不是 ADSP2；0x2900 ← 2，MPU 23 条，核控制 0x2BC1000 置 0x201 再清 0x200（安卓读回 0x81、MPU 锁读回 0xBEEF）；放音：0x2084 ← 0x000F1AA0（解锁/上锁之间）、0x2014 ← 1、0x2018 bit0；第一次放音清 0x10110 bit31、写 `CAL_R_SEL`（校准文件是两个 u32，T 第 0 个、B 第 1 个，原值照写）、`ALLOW_ACT_BOOT` ← 1；每次放音邮箱 0x13020 ← 0x0B000001，停止 ← 0x0B000002。
- 0236（2026-10-27 写好，k0236 已编译，未测）：`pandora_amp=3` = 0235 的加载 + 开机补丁（0x7404、0x3850）+ 0x4C48–0x4C54 + DSP 启动，读回核控制、MPU 锁、`HALO_STATE`/`HALO_HEARTBEAT`（固件自己 XM +0/+1，安卓转储里在 0x2800fa8/0x2800fac，空闲时心跳也在涨）、`AUDIO_STATE`。控件位置由 `csfw_find_ctl` 按 `.wmfw` 的控件描述算，host 测试对上安卓转储的 5 个地址。`pandora_gauge=1`：电量计总线只读状态（用户 2026-10-27 同意"先只看状态"）：GCC 投票和分支、GPIO52/53、SE5 固件/模式/状态，200 ms 采样看有没有别人在用，自己的投票还原，不发任何传输。和 0235 合成一次真机测试（spk2），要等 DeepSeek 的 acdbcheck 用完手机。
- **spk2（2026-10-27，k0236，`pandora_amp=3 pandora_gauge=1`）：**
  - 功放：两颗探测时核控制已经是 0x81，0236 的加载按设计没动它。（当时以为是安卓的固件还在跑，spk3 证明不对，见下。）
- **spk3（2026-10-27，k0237，复位后再加载）：** 复位后核控制仍是 0x81，串口寄存器是复位默认值（0x4808 = 0x18180200，安卓配好的是 0x20200000；spk2 也是这个值，说明重启时功放本来就被复位过）。所以 0x81 是 CS35L43 出复位后自己跑的 ROM 程序，安卓在 `cs35l43_dsp_init` 里先 `cs_dsp_stop`（看门狗 0x2BC7000 bit0 清、核控制 bit0 清、0x2B80010 bit0 置）再写固件。0238 照做。
  - 电量计总线（只读，没发任何东西）：时钟本来就开着（投票寄存器 0x52008 已有 18–21、27 位），GPIO52/53 功能 1、上拉；SE5 固件 0x301（I2C），但 `if_disable` = 1、`dma_mode` = 1、GSI 事件 0xf：**这块 SE 只能走 GSI（GPI DMA），FIFO 用不了**，和安卓用 GPI 命令字 0x00300001 一致。200 ms 里没有别人在用。要读电量百分比，得先写 GPI DMA 的 I2C（参照 Linux gpi.c 和 i2c-qcom-geni 的 GSI 模式）；MCA 协议里没有电量百分比。
  - 其余和 spk1 一样：SOCCP/MCA、ADSP、DSP 缓冲登记、TDM 引脚、PRM 时钟申请/释放都正常，vktest PASS 17。
  - 用户说开机后划了几下卡了几秒。0236 的新代码 8.4 秒就结束了（屏幕 19.7 秒才亮），不是它；时间上紧挨着的是 20.2–22.0 秒的 ADSP 固件加载和启动，待查。
- **spk4（2026-10-27，k0238）：固件真正写进去了。** ROM 停下（看门狗 0x111000，核控制回 0）；两颗各写固件 23484 字节 2.2 秒、系数 5660 字节 1.1 秒、music 场景 0.2 秒，id 头 173 字、PM 开头、602+114 个 XM 系数全部回读一致。但启动后核控制 0x1（安卓 0x81）、MPU 锁读 0x800000（安卓 0xBEEF）、心跳 0 不动：像没时钟。0239：心跳不动就照 preload 强开 PLL（0x2C04 bit16）再看；再把 232 个普通寄存器打进日志，和安卓空闲转储逐个比（会话 scratchpad ampcmp.py）。
- **spk5（2026-10-27，k0239）：** 强开 PLL（0x2C04 0x10 → 0x10010）后仍是核控制 0x1、MPU 锁 0x800000、心跳 0。232 个普通寄存器和安卓空闲转储比，两颗各 33 个不同（`D:\DeepSeekHarness\ampregcheck\diff_spk5.txt`）：串口/采样/PLL（0x2C04 安卓 0x430、0x2D10 0x3c024、0x4808–0x4840）、源选择（0x4C00/0x4C20/0x4C24/0x4C44）、0x2018/0x201C、增益与 VPBR（0x6404、0x6C04）、0x0104/0x0124 等。推测 DSP 的时钟要靠 PLL 锁到 TDM 位时钟（安卓放音时先 GLOBAL_EN 再 PLAY，之后才有 "PLL Lock INT"），空闲时可能本来就不跑。交给 DeepSeek 查：`ampregcheck`。
- 0240（2026-10-27，k0240 已编译，未测）：GPI DMA 的 I2C（`wsl/pandora_gpi.c/.h`），照 Linux 6.6 `gpi.c` 和 `i2c-qcom-geni` 的 GPI 模式写，轮询不用中断；TRE/事件编码有 host 测试。`pandora_gauge=2`：在 `qcom,gpii-mask` 里找一个空闲 GPII，读电量计 10 个寄存器（电压、电流、温度、百分比、剩余/满充/设计容量、循环、健康度、标志），读完关掉。SCL 时序取 SE 当前寄存器里的值（安卓重启前最后配的）。共享总线的特别处理（命令字 0x00300001 一类）等 DeepSeek 的 gpicheck。
- iOS 那侧（2026-10-27，iosaudiocheck2 复核）：iPhone X 的内置音频设备是 IOAudio2Device ← AppleARMDMAAudioDevice ← AppleARMIISAudioDevice ← AppleEmbeddedAudioDevice；CoreAudio 的 `HALS_IOA2Manager` 按类名 `IOAudio2Device` 找设备。用户客户端 13 个方法（旧式 IOExternalMethod 表 `0xfffffff004e9dcb8`，每个方法号 = 设备的一个虚表槽）+ 1 个陷阱（`0x5954850`）；`clientMemoryForType` type 高 4 位：0 = 40 字节状态页 `{QQQQQ}`，1/2/3 = 流缓冲（设备槽 190–192）。我们的做法倾向：预链接苹果的 IOAudio2Family，自己写 AppleARMDMAAudioDevice 那一层的硬件子类接高通 DSP。下一轮 iosaudiocheck3 查 13 个方法、陷阱、状态页字段和用户态调用顺序。
- **spk1（2026-10-26，k0234，`pandora_spk=2`）：第 1 步的硬件准备都通了。**
  - apps SMMU：引导程序配的 7 个表项用的是上下文 0–5，最后一个上下文 80 没人用；固件把写进去的"直通"读回成"报错"（`000200ff`），和 Linux 的 qcom 处理、sheng 遇到的一样，所以上下文 80 设成不翻译（SCTLR 0、CBAR `00010000`），新表项 7（`0x1001`/`0x80`）、8（`0x1041`/`0x20`）指向它，读回都对。
  - DSP 缓冲：ADT `/chosen pandora-dsp-shm` = 0xb0000000 + 1 MiB，写合并映射；APM 登记（地址高位放 1）给了句柄，注销状态 0。
  - TDM 引脚 GPIO 122–125 从 `0x1`（下拉、GPIO）改成 `0xc4`（功能 1、8 mA、不上下拉）；PRM 位时钟申请（3.072 MHz、属性 4）答复状态 0，2 秒后释放状态 0。PRM 答复的负载是 {参数号 `0x0800102C`, 状态}。
  - 电池：小米 MCA 协议通了，每 20 秒一轮：`pack_vbat` 4213–4388（应是 mV），`pack_ibat` 343–4463（mA，方向待定），`pack_tbat` 27→39（℃，GPU 测试时在升温），`bus_volt` 约 5 000 000（µV），`chg_status` 3、`chg_type` 3（含义待定）。开道后第一个请求的答复晚到 1 秒以上，之后都按时。百分比要读电量计。
  - 功放 I2C：连写 3 个、连读 3 个寄存器都对（中断屏蔽读回 `ffffffff 3fffffff ffffffff`）；连读 17 个超了单次 64 字节的上限（已改成 128，下次测）。TX/RX FIFO 都是 32 个字。
  - GPU 同时正常：vktest PASS 17，没有失败。
- TDM 端点参数的线索：libagm.so 常量表 0xd448 `0x08001063`/12 字节（接口配置），0xd478 `0x0800113D`/28 字节，尺寸正好是 DeepSeek 说的 48 字节"槽配置"块（16 字节头 + 28 + 4）。搭图时对照反汇编确认。
- 电量计总线（`i2c@a94000`）：QUP 包装层 1 的 SE5，SDA GPIO 52、SCL GPIO 53，功能 1（qup1_se5），上拉 2 mA；主线 gcc-kaanapali.c：SE5 分支 0x18508（投票 0x52008 位 27），时钟源 0x18518，包装层核心 0x23148/0x2315c（位 19/18），从 AHB 0x23144（位 21）。设备树的时钟编号是安卓自己的，和主线头文件对不上，按引脚功能名对。共用的问题查清前不碰这条总线。
- 功放固件大小：`.wmfw` 37.5 KB（Halo 核，要写进 DSP 的 PM 19.3 KB、XM 约 4.2 KB、YM 40 字节），系数 `.bin` 14 KB，场景 delta 约 4.7 KB；按 100 kHz 的 I2C 每颗约 4 秒，以后提到 400 kHz 或 1 MHz。
功放 CS35L43 的音频输入（TDM0，GPIO 122–125）只能接 LPASS I2S0，由音频 DSP（ADSP）驱动。要让喇叭出声，至少要做完下面这些，每一步都是独立的大块工作：
1. 固件：/vendor/firmware_mnt/image/adsp.mdt + adsp.b00–b40（约 30 MB）和 adsp_dtb.mdt，装进 pinned 文件，内核按扩展区读入保留内存（adspslpi_region@0xa1400000、q6_adsp_dtb_region@0xa1380000）。
2. 安全世界启动：SCM PAS 调用（init_image / mem_setup / auth_and_reset，两个镜像），这代芯片的元数据缓冲要先通过 SHM bridge 登记；通过 AOP 的 QMP 消息给 ADSP 上电，RPMh 投 LCX/LMX 电压。
3. 通信：SMP2P 等待 ADSP 就绪，SMEM 上的 GLINK、rpmsg 通道、GPR 协议。
4. 音频图：AudioReach APM 命令建播放图（共享内存端点、PCM 解码、TDM0 硬件端点），参数来自 /vendor/etc/acdbdata 的校准数据；同时把 CS35L43 配成接收 TDM、不跑或跑功放自己的扬声器保护固件。
5. iOS 侧：实现 iOS 音频守护进程能认的内核音频驱动接口（苹果私有，要逆向 iOS 16 的音频框架）。
摄像头同理不可行：iOS 相机依赖 A11 的 ISP 和 AppleH10CamIn 系列私有接口。

## CPU：调频、睡眠、大小核、温控（2026-10-24 加入，GPU 这一轮之后做）
现状：8 个核（6 个性能核 + 2 个超大核，Oryon）都由 PSCI 启动（补丁 0065、0078、0113），只做到"能跑"。
1. 先量：回安卓时读 CPU 调频驱动名、各核的频率档位、温度传感器；iOS 里加一行日志，用周期计数器对计时器算出 CPU 实际频率。
2. 调频：这代芯片的 CPU 频率由 CPUCP 固件管（分区 cpucp_a/b、cpucp_dtb_a/b），系统通过它的接口请求档位；具体接口按第 1 项读到的安卓驱动确定，再在内核里做按负载升降的调频（和 GPU 的 DCVS 同一套思路）。
3. 睡眠：空闲核进 PSCI CPU_SUSPEND 的省电状态，不只是原地等待；状态编号取自设备树的 idle-states。
4. 大小核：核实交给苹果调度器的核心类型（性能核 / 能效核）和簇编号，让它按大小核分配任务。
5. 温控：读温度传感器，过热时降 CPU 和 GPU 的档位。

方向（2026-10-25 用户定）：按官方的设计思路改，不照搬官方的数字。
- iOS 这边的官方设计是苹果的性能控制器接口（内核里的 sched_perfcontrol 回调）。真 iPhone 上，性能控制器从这里拿到每个线程有多急、每帧的截止时间。
- 我们的调频和大小核分配挂在这个接口上，再按这块芯片的实际情况调整。
- 芯片本身的事实取自厂商数据：频率档位、睡眠状态、温度传感器、降频温度。
- 安卓（walt 和小米的性能服务）的做法只作参考，不照搬：
  - 亮屏时小核在 2.2–2.7 GHz 干日常的活；
  - 大核多数时间停在 0.88 GHz，上限压在 1.5 GHz，只在打开 App 时冲到 3–4 GHz；
  - 性能服务随触摸和开 App 改频率的上下限。

## 温控和发热（2026-10-25）
- 0213 温控已做，门槛全部取自设备树：
  - 任一核 105°C：这组核的频率上限每 100 ms 降一档，回到 100°C 以下升回；
  - 114°C：用苹果性能控制器停核的接口（REASON_CLPC_SYSTEM）停用该核，108°C 以下恢复；
  - 118°C：这组核降到最低档；
  - GPU 95°C：GPU 上限一档一档降。
  - 测试开关 pandora_thermal_test=N：所有门槛降低 N 度。
- 测过：
  - therm1（测试开关 30）：各条路径都触发了。但测试期间 App 点了打不开：SpringBoard 打开 App 后，马上识别到一次底部横条上滑（DeckGrabberTongue），又把 App 收了回去。
  - app1（不开测试开关）：App 正常打开。原因还没查清，怀疑是频繁停核或降频打乱了触摸事件的时序。以后再查。
- 正常使用时真的会热（app1：连点 App 时，小核 105.9°C、GPU 95°C，温控按门槛降了频）。发热来源，按影响大小：
  1. vmd（语音信箱）没有基带，死循环重试，一直占约 93% 的一个核；
  2. bluetoothd、fairplayd 一启动就被 sweetnoexec 杀掉，launchd 反复拉起（launchd 约 30%，ReportCrash 也在忙）；
  3. 每帧由 CPU 把整屏（约 11 MB）复制进面板显存，每帧约 2.5 ms，见 GPU 计划里的 direct scanout；
  4. GPU 用 sysmem 模式渲染（TU_DEBUG=sysmem，没用片上 GMEM），内存读写多，划屏时冲到 1200 MHz；
  5. CPU 核没有深度休眠（只有 WFI）；"大核优先"让大核干了更多活。
- 用户决定（2026-10-25）：不靠关掉服务来降温，以后慢慢把对应的硬件适配上（蓝牙、基带等），让这些服务正常工作。

## UniXNU 迁移和项目改名（2026-10-25 加入，以后做）
1. UniXNU 设备仓库格式迁移（https://unixnu.com/zh/porting/migration）：设备仓库要从格式 1 迁到格式 2。
   - 格式 2 的内容：
     - `board.toml` 写 `schema_version = 2`；
     - 配置分层：ARM64 / SoC / OS / 设备；
     - 组件构建命令；
     - 有序的补丁列表 `patches/xnu/series`；
     - 每个目标单独的输出目录。
   - 另外要用 `porting.toml` 写结构化的移植记录，设备页由它生成。
   - 这份指南主要针对官方设备仓库。社区移植保留自己的来源，要先登记进组件目录，才能在格式 2 里用。页面没写截止时间。
   - 我们是独立的社区移植。先读全文和仓库规范，再决定 data-a-live 要不要也改成格式 2、要不要交移植记录。
   - 结论（2026-10-25 读了 UniXNU 的 records.md 和 devices 索引）：格式 2 和由 `porting.toml` 生成的设备页只针对官方设备仓库（UniXNU-<设备名>）；社区移植的页面仍是手写的，迁移不要求我们改动。以后如果想交结构化的移植记录，再按 records.md 的分区（配置、启动、CPU、存储、显示、输入、GPU 等）写。
2. 改名：UniXNU 的 pandora 设备页还把旧名 ios-pandora 列成一个仓库，和 data-a-live 并列，状态也还是旧进度（"GPU 内核驱动可执行命令缓冲"）。
   - 要改成只列 Data A Live，并更新状态。和用户那边待提的 UniXNU 文档 PR 一起改。
   - 2026-10-25 已做：
     - 文档分支 `docs/pandora-gpu`（ShueiTsuki/UniXNU）接到 UniXNU 最新 main 上，两行合成一行 Data A Live，状态更新到资源库正常、CPU 调频。等用户开 PR；
     - PLAN.md 的旧名改了；
     - `tools/metal/adapt_ios16.py` 和 `tools/turnip/abi_128.py` 的注释标记改成 "data-a-live"。在全新的 NativeMetal 源码上跑新旧两版，打出的代码除标记名外完全一样；
     - README 和 LICENSE 里的 "formerly ios-pandora" 是有意保留的说明。
   - 还剩本地文件夹名 ios_pandora：WSL 脚本里的路径都用它，改名要连路径一起改，而且不能在别的会话正用这个文件夹时改。

## 开机画面（2026-10-25 加入，以后做）
现在开机时内核把日志文字画满屏幕，直到锁屏出现（0156/0157）。改成像 iPhone 一样：屏幕不画文字（日志照样写进日志文件），只显示一个开机 Logo 保持到锁屏；加开机参数在排查开机卡住时打开文字。Logo 两种做法：自己的 Logo 编进内核，一开机就显示，可公开；苹果 Logo 只能放在本机的固件包里，内核读到存储后才能画（开机先黑一两秒）。不影响开机速度（不画文字反而省一点时间）。

2026-10-25 进展：
- 开机 Logo：
  - 用苹果 Logo。tools/bootlogo.py 从用户自己的 IPSW 解出 applelogo，打包时编进启动镜像（不进仓库）。加载器 v2 在没有 -v 时只画 Logo。
  - 内核文字按用户决定先保留：0206 只挡住了 printf 这一路，kprintf（debug=0x8）还会画字，要去掉就在 fbcon_transmit_data 里也查 sweet_console_muted。
  - 量过，画字总共约 0.5 秒，影响不大。
- 开机提速：第一帧从 74 秒提前到约 20 秒（boot1 → boot3）。
  - 0208/0209 给日志加了时间戳；
  - 0210 由平台驱动发布 IORTC，省掉 IOKitInitializeTime 的 30 秒等待；
  - 0211 用 ARMv8 SHA 指令算代码签名哈希，共享缓存校验从 33 秒降到约 9 秒。
- 开机后屏幕不自己亮（要按电源键才出锁屏）。原因查清了，以后适配硬件时一起改：
  - **现象**：backboardd 撤掉启动遮罩的同一刻，SpringBoard 的空闲计时器已经到期，直接熄屏。屏幕上露出的是垫在最底层的那层苹果 Logo（boot4，0212 只截第 N 帧的截图，以及系统日志）。
  - **根本原因**：backboardd 启动花了 10.5 秒，主要在等两样我们没有的硬件。SpringBoard 要等 backboardd 准备好，所以锁屏出来得太晚。
    1. "HID system start" 4.93 秒：IOHIDEventSystemOpen 里 RemoteHID 的 BTSessionAttach 在等 bluetoothd（被 sweetnoexec 拦着），等到超时；
    2. "window server" 5.47 秒：CoreBrightness 的 CBU_ShouldWaitForALS 等环境光传感器，等到超时。
  - **另外**：
    - vmd（语音信箱）连不上 CommCenter（没有基带），死循环重试。开机这段时间写了约 37 万条日志，还占着一个核；
    - 很多进程的 MobileGestalt 在等某个驱动出现，等到超时（"giving up waiting for match"）。
  - **改法（按官方结构）**：
    - 给手机的环境光传感器做一个 ALS 驱动，以后自动亮度也能用；
    - 不想要的服务用 launchd 的 disabled.plist 关掉（vmd、bluetoothd），而不是在 exec 时杀掉，让客户端立刻知道服务不存在；
    - 查清 MobileGestalt 在等哪个驱动。
- 系统日志的读法：
  - 用 `pinfile rawread` 按磁盘块拷 iosroot.img。直接按文件读会经过安卓的文件加密，读出来是乱码；
  - 然后用 hfsplus 模块只读挂载，再用 unifiedlog_iterator 解析；
  - 解析出的时间戳会被压缩，计时以内核日志和 backboardd 自己记的耗时为准。

## 着色器预热（2026-10-26 用户加入，以后做）
目的：第一次打开某个界面或 App 时，不再因为现编着色器而卡一下。照 UniXNU 主仓库 16e592a（docs/drivers/adreno-metal.md）的做法，按这块芯片和 iOS 16 调整：
1. 管线缓存跨重启保留：编好的管线导出到磁盘，下次开机直接用。
2. 空闲时预热：渲染服务（backboardd）在桌面空闲时，把系统镜像里所有 Metal 着色器库提前编一遍；要编的库列在一个清单文件里（上游叫 NativeAdrenoMetal.primelist），随固件包装进去。渲染服务的沙盒读不到的库（上游是 cryptex 里的 QuartzCore 库）另拷一份。
3. 不抢当前画面：只在显卡提交少的时候做（上游门槛：每 250 ms 超过 8 个命令缓冲就暂停；我们的门槛要在这台手机上量桌面空闲时的提交数再定）。
- 我们先要查：我们的插件版本（NativeMetal c6d29ca + adapt_ios16.py）是否已经有跨重启的管线缓存；iOS 16 的系统镜像里有哪些 Metal 库、哪些在 backboardd 沙盒外；上游清单和预热代码在 NativeMetal 哪个提交。
- 验证：预热前后各开一遍同一组界面和 App，用分析器看着色器编译的时间是否从首次打开时消失；桌面空闲时不掉帧。

## 原版壁纸（2026-10-26 用户加入，以后做）
- 现状：iOS 16 默认壁纸的图层是 HEIC，也就是用 HEVC 视频编码存的图片。iPhone X 靠苹果自己的视频解码硬件来解（系统里是 AppleD5500 / AppleVXD393 那一套驱动），这台手机没有这块硬件。所以现在的文件安装器（补丁 0147）用 `tools/mkwallpaper.py` 把这些图层换成 PNG 再装进去。
- 目标：用原版的 HEIC 图层，不再换成 PNG。
- 先查清楚：
  - iOS 的图片库（ImageIO）解 HEIC 时走哪条路：是不是只走 VideoToolbox 的硬件解码，有没有软件解码可以退回去；
  - 去掉 PNG 以后，系统日志里 VideoToolbox / ImageIO 报的是什么错。`docs/GPU_PLAN.md` 第 6 项还没有结论。
- 可能的做法，查清以后再选，选之前先问用户：
  1. 给 iOS 提供一个它认得的 HEVC 软件解码器，只解静态图片，用 CPU 解；
  2. 驱动这块芯片自己的视频硬件（高通 Iris），接到 iOS 的解码接口上。工作量大，但视频播放也能一起受益。
- 验证：去掉替换用的 PNG 以后，原版壁纸各层都显示正确，包括亮色、暗色两种外观和景深效果。

## GPU、显示、CPU 休眠：第 1 轮（2026-10-25，k0215，日志 r1c）
用户要求：GPU 换掉 sysmem 模式；翻译层要做得好，不偷工减料；每帧 CPU 拷屏要解决；CPU 核要能深度休眠；"大核优先"不是官方做法（官方是苹果的 AMP 调度加性能控制器的核推荐），以后换掉。
- **日期时间（0214）**：PMK8850 的 RTC 加安卓的偏移，iOS 时间正确（状态栏显示太平洋时间，时区由用户在设置里改）。
- **GPU 渲染模式**：插件默认 `TU_DEBUG=nocb`，由 Turnip 自己的 autotune 按渲染过程选片上分块（GMEM）还是直接写内存。r1c：GMEM 2049 次（每次 1–2 块，没用硬件分箱），sysmem 7227 次（"Autotune selected sysmem"），没有卡死，画面正常。并行分箱仍关着（UniXNU 在 A740 上碰到 BV/BR 互等，原因以后查）。
- **核休眠（0215）**：空闲时下一个定时器足够远（设备树 min-residency：小核 560 µs、大核 686 µs）就用 PSCI CPU_SUSPEND 进设备树的 "ret"（0x4），否则 WFI；固件一次没拒绝。小核一次睡 10–20 ms，两个大核平均 0.1 ms 就醒（大核优先把活都放在大核上）。簇级（ret-pll-off）和整机级（apps-pc）要 PSCI 的 OS 协调模式，下一步。
- **翻译层开销（插件按函数计时，每 10 秒一份 "pandora-metal B"）**：动画时桥接层最多占 23% 个核，是 backboardd CPU 时间的大头。
  1. 设备属性查询 nmb_device_answer：每 10 秒约 3100 次，每次 140 µs，答案不变却每次重算；
  2. 每次绘制 nmb_render_draw_indexed 约 22 µs，偏高；
  3. 新建渲染管线约 10 ms 一次（现场编译着色器）；
  4. 结束渲染 0.09 ms，提交 0.35 ms，导入外部内存 0.2–0.6 ms。
  另外：提交到完成平均 5.6 ms；每帧 CPU 拷屏平均 2.4 ms。
- **显示控制器摸底（0215，只读，按主线 Linux 的 DPU 13 排布）**：
  - 主屏：ctl0 → intf1（DSI0），vig0 和 vig1 各送 610 像素宽的半屏，都读 0xFC800000（行宽 4880），两个混合器、两个 DSC；
  - 背屏也亮着：ctl1 → intf2（DSI1），vig2 送 572×904，地址 0xFD45C600；
  - 命令模式 + 自动刷新（autorefresh 0x80000001），撕裂检查开着；主屏 100 ms 内 12 帧 = 120 Hz；
  - SMMU：显示流 0x800 进第 2 号上下文，翻译关着，直接读物理地址；主线 Linux 只写 32 位地址，所以直接显示要打开这个上下文的翻译、用自己的页表（切换前先把现在读的内存原样映射）。
  - 教训：ml_io_map 的映射不能 ml_io_unmap（r1b panic，记忆里早有这条）。

### 第 2 轮（顺序）
1. 设备属性的答案按（属性，参数）记下来，只算一次；
2. 查清一次绘制为什么要 22 µs，按原因修；
3. 等 GPU 完成改用中断，不再轮询；
4. 显示直接读 GPU 的图：SMMU 页表、每次交换换取图地址、帧完成中断当垂直同步、亮度交给屏幕的亮度命令。

### 第 2 轮结果（2026-10-25，k0216–k0219，日志 r2a–r2h）
- **等 GPU 完成改中断（0216）**：GIC 中断控制器（SweetGIC）接上 GPU 的中断线（SPI 300），等待者睡眠、被围栏中断唤醒；空中断 0 次。
- **设备属性答案缓存**：插件里按（属性，参数）记住答案。
- **显示直接读 GPU 的图（0217）**：显示流的 SMMU 上下文打开翻译、用自己的页表，每次交换只换两条管道的取图地址加缩放；CPU 拷屏没了（每帧呈现 2.4 ms → 约 0.09 ms）。教训：ml_io_map 不能映射大窗口（r2c panic），要用 ml_io_map_unmappable；FSR 的 FORMAT 位不是故障（r2e）。
- **滑动闪屏（r2f）→ 交换队列和 SwapWait（0218）**：QuartzCore 的 IOMFBDisplay::can_update 用 SwapWait（sel 6）问"上次用这块缓冲的那次交换还在屏幕上吗"，我们以前一律答"不在了"，CA 就往屏幕正读的缓冲里画。按 iPhone X 原版内核的 swap_wait_gated 实现：交换分排队 / 上屏中 / 显示中，选项第 31 位=不等待（答 kIOReturnBusy），0=已离开屏幕，1=已上屏，2=全部完成；CA 每帧最多等 1 ms。上屏改由专门线程做：GPU 围栏到了就换地址，CTL0 刷新位清零且两条管道读到新地址才算"显示中"（平均 4.5 ms，最长 8.5 ms，即一次 120 Hz 刷新），不再等 60 Hz 软件定时器。熄屏也由这个线程做：先把屏幕内存涂黑，再让管道回去读它。
- **闪开机标志（r2g）→ GPU 地址窗口（0219）**：backboardd 的 Metal 设备报 OutOfMemory 3439 次（纹理、导入、连画面缓冲本身也被拒），没画上的帧照样上屏，露出缓冲里开机时的内容。原因是 32 位布局里只有第一个打开 GPU 的进程有 1 GiB 窗口，其余 178 MiB，vktest 先打开，backboardd 只有 178 MiB。GPU 上下文改 36 位输入（SMMU 上游总线 49 位，ID2 0x5511），每个进程从 4 GiB×(序号+1) 起拿 4 GiB，同 msm 的私有地址空间；GMU 仍 32 位。r2h：OutOfMemory 0 次，backboardd 实际映射约 620 MiB，GPU 内存池 536/1024 MiB 后稳定，用户确认不闪。
- **内存**：iOS 用 5 GiB 那块内存里的约 4 GiB（扣 1 MiB 崩溃日志、1 GiB GPU 内存池）；高地址那块约 3.9 GiB 还没给 iOS。
- **帧率**：屏幕 120 Hz 刷新，iOS 按 iPhone X 的 60 Hz 画；要 120 帧得让 iOS 把屏幕当高刷屏，另做。
- **未解决**：mtltest 第 9 部分（9a–9c，填充三角形 992/4096 像素没画到）在 r2f、r2g、r2h 一样失败，不是这轮改出来的，待查；r1c 照片小组件"无可用内容"一段时间乱码：用户说之后可能已不再出现，没再看到就不追（GMEM 测试已排除 GMEM）。
- **待查（2026-10-26 用户报）**：在后台（多任务界面）划掉 App 时，整个界面几秒点不动。adsp2 测试进行中报的，还没确认是不是只在这次出现。普通 App 不是 GPU 客户端（只有 NativeAdrenoMetal.clients 里的 bluetoothd、vktest、mtltest、backboardd 拿 GPU），所以不是 App 的 GPU 地址空间拆除；adsp2 日志里没有对应的超时记录，要开分析器专门测。fw1（0229）里用户反复划掉后台、开 App 压力测试：不再卡，开 App 也不掉帧（2026-10-26），不再追。
- **待查（2026-10-26 用户报）**：电池一直显示"充电中 100%"。现在的电池是 ios-sweet 带来的替身服务 `SweetBattery.cpp`，数字是写死的，不读硬件（它的真实读数路径是 sweet 的 PM6150，不是这台手机）。pandora 的电量由 SOCCP 管，不是 ADSP：设备树 `pmic-glink` 走 GLINK 通道 "PMIC_RTR_SOCCP_APPS"，SOCCP 的 remote-pid 19，IPCC 客户端 13 信号 0，还有小米的 `mca,adsp_glink`。和 ADSP 用的是同一套 SMEM/GLINK 代码，ADSP 通了以后可以接着做；sheng 的 ShengBattmgr.c（高通 battmgr 协议）可参考。要先查 SOCCP 开机时是不是已经由引导程序启动了。（2026-10-26 进展：SOCCP 由引导程序启动，我们接上它的 GLINK 会话（0231）；电池走小米 MCA 协议，电压、电流、温度能读到（spk1）；百分比在主处理器 I2C 上的 bq27z561 电量计，等 `D:\DeepSeekHarness\battcheck` 查清那条共用总线。电池服务还用替身数字。）

### iOS 显示接口调查（2026-10-25，QuartzCore + IOMobileFramebuffer 反汇编，原版 iPhone X 内核）
- **帧节奏**：CA 的 `CADeviceUseVBL` 按 MobileGestalt `ChipID` 决定用不用硬件垂直同步，只有 0x7000、0x8000–0x8004、0x8010（A8–A10）用；A11（iPhone X，0x8015）不用，CA 自己开定时器（EAGLServer 里 0x30d 标志第 1 位为 0 → add_vsync_source 走定时器）。所以 CA 不需要我们的垂直同步通知；显示中断只给我们自己的上屏线程用。
- **刷新率**：CA 的 AppleDisplay 从显示服务属性 `IOMFBDisplayRefresh` 读刷新能力；做 120 帧要照真机格式发布它。CA 读的其他属性：IOMFBScalingLimits、color-accuracy-index、FFR_raw_index、PanelLayout、DisplayPipeStrideRequirements、DisplayPipePlaneBaseAlignment、IOMFBMaxSrcPixels、maxAverageBpp、maxPeakBpp、PCCCabalEnable、W40a_Blending_OK、IOMFBSupportsICC、Brightness_Scale、SupportsRotation、IOMFBUUID、DisplayContainerID、DisplayAttributes、Transport。
- **被忽略但有用的接口**：55 SetMatrix 和 69 SwapSetColorMatrix（夜览、色彩滤镜、反转、降低白点），78/79 数据块 0x4d/0x4e（色域映射），84 SetContrast，50 SetBrightnessCorrection；作用待查：68 SetParameter（参数 3、6、7、8，值 1），79 数据块 0x48/0x4a，29 GetDisplayArea（回 0），74 SupportedFrameInfo（回 0 = 不支持帧信息）。
- **SwapWait 选项**（0218 已实现）：第 31 位不等待；0 离开屏幕、1 已上屏、2 全部完成、3/4 显示是否就绪；无超时 5 s 后清交换。

### 用户插入的任务（2026-10-25）
- **120 帧并解开 iOS 的 80 帧限制**：先照真机格式发布 `IOMFBDisplayRefresh` 等属性，让 iOS 把屏幕当 120 Hz 高刷屏；再查 80 帧上限是 iOS 哪一层的策略（CA 的帧率范围、UIKit 动画、MobileGestalt 设备能力），找到后解开。
- **十点触控**：触摸芯片固件报 10 个点（app info objects 10），内核 pandora_touch.c（PT_MAX 10）和 SweetHIDTouch（SWEET_TOUCH_FINGERS 10）都按 10 点写；r2g 里最多出现 5 指同时按下（编号 0–4），6–10 指还没试。待办：内核记下同时按下的最多手指数；上手机试 6–10 指；查 iOS 在 iPhone 上是否只收 5 点（常见说法 iPhone 5 指、iPad 11 指），是的话找到限制处解开。

### 120 帧和探测结果（2026-10-25，k0220，日志 r2i）
- **120 帧（0220）**：AppleCLCD 上发布 `IOMFBDisplayRefresh`（displayMinRefreshInterval = 2^32/120，不给步长 = 不声明 APT）。动画时平均 102–104 帧（以前 60），最短间隔 2.5–5.7 ms；没满 120 是 GPU 慢（600 帧里 480–570 帧要等 GPU）。交换到上屏平均约 11 ms，最长约 20 ms。
- **十点触控**：内核记到 10 指同时按下，iOS 端没再卡。
- **PSCI**：1.1，CPU_SUSPEND 特性 0x3（支持 OS 协调模式、扩展状态号），SET_SUSPEND_MODE 支持，STAT_RESIDENCY 不支持 → 簇级/整机级休眠按 Linux cpuidle-psci-domain 做。
- **显示中断（只读）**：100 ms 内各 14 次：INTR bit 8、10（pingpong 0/2 完成），INTR2 bit 9（CTL0 开始一帧）、bit 10（CTL1，背屏），INTF1 bit 11，INTF1 tear bit 0–3、6。CTL0 开始一帧可代替上屏线程的轮询。
- **被忽略接口的参数**：17 伽马表全 0、55 颜色矩阵为单位阵、68 参数 6=0 和 7=0x200000、79 数据块 0x4d（1580 字节）和 0x40（40 字节）、84 对比度 0、thermalmonitord 78 数据块 2（1788 字节）。

### 帧开始中断和手机上的采样分析（2026-10-25，k0221–k0223，日志 r2j）
- **帧开始中断（0221）**：MDSS 中断线（SPI 83）接进 GIC，CTL0 开始一帧时唤醒上屏线程，代替轮询；r2j 里 1189 次帧开始，漏的很少。
- **采样分析器（0222）**：启动参数 `pandora_prof=backboardd`，每秒约 1000 次记下 backboardd 各线程停在哪；`tools/prof.py` 按函数名汇总。r2j 用户态：插件 34%、QuartzCore 18%、malloc 17%、Turnip 11%、memmove/memset 8%、mach_absolute_time 3%（插件给每个调用计时引起的）；另有约 19% 在内核，其中 GPU 调用里等 SMMU TLB 刷新完成的读寄存器循环和关中断的区段最多。
- **Turnip 带符号重看 r2j**：时间很分散，最大的单个函数 0.5%；排前面的不少是每个命令缓冲都要重来的开销（tu_cs_reset、ResetCommandBuffer、tu6_init_static_regs、tu_init_hw）→ 减少命令缓冲数量（几个渲染遍合成一次提交）值得做。
- **调用栈（0223，r2k 已验证可用）**：内核样本记下 5 层调用者；每个样本让线程回用户态前走一遍用户栈（最多 16 层，照 kperf 的做法用 AST），按样本数加权；日志只剩 4 MiB 循环缓冲，所以不再单独打逐地址统计（由栈推算）。插件计时改为随机抽 1/16 的调用计时，等待类调用全计。

### 自动滑屏、GPU 映射的 TLB 处理、帧率卡在温度上（2026-10-25，k0224–k0225，日志 r2k–r2m）
- **r2k 调用栈**：开机建界面时，backboardd 内核时间的 29% 是 SMMU 读寄存器：每次 GPU 映射/取消映射都把整组上下文寄存器读两遍核对，再清空整个 TLB。
- **0224 TLB 处理照 Linux arm-smmu**：映射不刷 TLB（只填空项，SMMU 不缓存出错项）；取消映射按页 TLBIVAL 再等一次 TLBSYNC，超过 512 页整个 ASID 作废；页表项按连续段清缓存。`pandora_smmu_legacy=1` 切回旧做法。r2l（旧）对 r2m（新），同样自动滑 116 次：backboardd 内核样本每 10 秒约 900 → 815，无出错，画面正常。滑动时这段只占内核时间 3.8%，主要省在开机和建界面。
- **自动滑屏（0224）**：`pandora_autoswipe=秒数`，开机 45 秒起（`pandora_autoswipe_at`）点亮、上滑解锁、主屏和 App 资源库来回翻页，测试不用人拿手机。
- **帧率由温度决定**：滑屏一分钟后小核 105–106 °C、大核 103–104 °C、GPU 95 °C；GPU 到 95 °C 上限后频率被压到 500–726 MHz，600 帧里 530–599 帧等 GPU。CPU 合计约 3.6 个核忙（大核 4.4 GHz），backboardd 只占约半个核；其余来自硬件没适配导致服务反复查询（见“温控和发热”）。所以比较翻译层优化看 backboardd 的 CPU 样本数，不看帧率（手机温度不同，帧率不可比）。
- **Turnip 渲染遍**：自动选择器把约 77% 的渲染遍放在 sysmem，GMEM 约 23%，没有硬件分箱（hw binning 0）。
- **采样日志**：滑屏时采样器约 10 MB，4 MiB 日志只留最后约 40 秒；0225 起每次报告都重打库地址表（r2l/r2m 的用户态地址因此无法命名）。

### 翻译层每次绘制的分配（2026-10-25，日志 r2n，m2v 补丁 0002）
- **r2n 调用栈**（0225，自动滑屏，backboardd 16812 个样本）：翻译层绘制调用连同子函数占 33%（交给 Turnip 记录 6%，其中换管线时 Turnip 整块拷贝动态状态；检查绑定形状 3.7%；生成绑定 3.4%；记录资源使用 1.9%），结束渲染遍 9.3%，提交 5.9%。翻译层已经去掉了重复绑定同一管线。
- **内存分配占 16.7%**（malloc/free 及其引起的清零、归还内存），其中翻译层 Rust 代码 12.8%、QuartzCore 2.3%、Turnip 0.8%。用户选 A：改热点代码，不换分配器。
- **0002**（tools/metal/m2v/0002-fewer-allocations-a-draw.patch）：推送字段字节放在栈上（FieldBytes）、视口和裁剪框用最多 16 个的定长列表（PerViewport，设置时已限制在 16 个以内）、动态状态记录改为整块拷贝、静默提示记录不再复制绑定计划自带的特化常量、draw_uses 不建临时数组。电脑基准（frame_costs_plain）每次绘制 24.6 → 17.8 次分配（含 lavapipe 自己约 7–8 次，手机上没有）；运行时 + 验证库约 2600 个测试结果与基线逐个一致（唯一不同的 worker 峰值测试单独跑两边 5/5 通过，是并行负载下的偶发失败）。插件已编好，待上手机测。
- **0004**（0004-draw-commands-kept.patch）：渲染编码器保留上一次绘制的 DrawCommands，下一次绘制清空后原地重填（`instantiate_draw_into`、`bind_buffers_in`），不再每次新建约 9 个列表；中途提前返回的绘制丢掉它，下一次从空的开始，记录的内容不变。电脑基准每次绘制 17.8 → 15.8 次分配；测试结果与上一版逐个一致。插件已重编（0001–0004），待上手机测。
- **按手机数据挑下一批**（r2n 调用栈里 Vec 扩容 `grow_one` 的调用者）：`command_uses` 16%、资源跟踪表 `EncoderUses::record` 14%（哈希表扩容也有一半在它）、`bind_buffers` 13%（0004 已解决）。和电脑上的排序一致。
- **0005**（0005-draw-lists-kept.patch）：绘制的资源使用列表由编码器保留、下次清空重用，并按上限一次留足；存活采样器列表同样保留（绘制结束时清空，不延长采样器寿命）；资源跟踪表里每个资源的第一段范围直接放在表里，第二段起才开列表。电脑基准每次绘制 15.8 → 13.5 次分配，结束渲染遍 37.6 → 35.4；测试结果和 0004 一致（只差 worker::alloc 两条已知偶发用例）。
- **0006**（重新绑定日志）：设备上只要有一块无拷贝缓冲（CA 有），每个编码器就把每个调用记进日志，只为“把无拷贝缓冲换到别的内存页”这个运行时扩展重编码用；插件桥文档写明从不这么做（NativeMetalObjects.h），日志记了就扔。r2n 里日志相关约占 backboardd 0.8–1%。新增 `DeviceOptions::refuse_rebinds`（默认关，与原来一样）：开了就拒绝重新绑定（`RebindProblem::Refused`），编码器不记日志；分离（detach）和页面持有登记都不动。插件默认打开，启动参数 `pandora_m2v_rebinds=allow` 关掉。
- **0007**（0007-tracker-key-map.patch）：编码器自己的资源跟踪表原来用标准库带随机种子的 SipHash，换成运行时 `key_hash.rs` 里给跟踪表准备的 `KeyMap`（理由见那个文件：键是运行时自己编号的，不需要抗碰撞；原来的随机种子让迭代顺序本来就每次不同，没有调用方依赖它）。电脑上计时波动比这项大，量不出；测试结果一致。
- **看过、不做的**：每遍新建 VkFramebuffer 在手机上建+销毁合计约 0.6%，缓存它要处理视图寿命，不值得；推送常量字节每次绘制约 1.75 次分配，要改 m2v 公开类型才能复用，手机上约 0.2%，不做。
- **CA 的两平面扩展范围表面（'b3a8'，`RGB10A8_2P_XR10`）**：内核日志 `SWEET SURF: b3a8` 显示 CA 建了全屏 1125×2436 的这种表面（平面 0 每行 4544 字节放颜色字，平面 1 每行 1152 字节放 alpha），还有 1065×282、24×132 等小的。Vulkan 和 Adreno 都没有这种格式，m2v 用一张 8 字节/像素的私有图像做影子（`texture/surface_two_planes.rs`）：每个用到它的编码器先把两平面拷进暂存缓冲、用计算核排成紧凑纹素、再拷进图像，写过就反过来拷回。全屏一次拷入加拷回约 70 MB 的 GPU 内存读写。r2n 里 `new_surface_planes_texture` 合计 2.7%，影子拷贝 2.8%，其中有两平面拷贝核（`HeapPlaneCopies::record`）。0003 的 Alias 只管单平面的行间空隙，管不到这种。
- **0008**（两平面影子建图不写零）：运行时对普通影子早就按“拷入会在任何读之前写满每个纹素，所以图像按内存原样开始”处理（`texture.rs` 注释：写零占了建图的大部分时间），两平面表面影子却还走写零的 `new_image`；r2n 里它的时间 2/3 在 `fdl6_memcpy_linear_to_tiled`（把零拷进分块图像）。两平面的拷入写满整张（或整张清除时由清除写满），初始的零读不到，改成按内存原样开始。验证：验证库里两平面、扩展范围、新内容、表面相关的 110 条测试（WSL 没有验证层，临时去掉层要求在 lavapipe 上跑）除 `new_private_textures_read_metal_s_pattern_by_format` 外全过，那条不改也一样失败（设备自有内存的 Private 纹理，和两平面无关）；常规等价测试和 0007 一致。
- **需要你定的（两平面表面每遍的拷入拷回）**：这是格式模拟本身的代价。可选：(a) 保持（iPhone X 官方就用这种表面，只是它的 GPU 原生支持）；(b) 让 CA 不建扩展范围表面（显示属性报不支持扩展范围），画面就和 iPhone X 不一样了；(c) 让 m2v 对这种纹理的着色器直接读写两平面（工作量大）。先用 r2o 的工作计数看每 10 秒有多少次两平面拷贝、拷了多少像素，再定。
- **还剩的（手机）**：结束渲染遍里每遍一次的小分配约 25 处，各自很小。

### 影子纹理：画面缓冲每遍都整张拷进拷出（2026-10-25，m2v 补丁 0003，待上手机）
- **发现**：metal2vulkan 对“行间有空隙、又要渲染或写入”的纹理一律走影子（另建一张图像，每个编码器用前把像素从缓冲拷进来，写完拷回去），因为 Vulkan 的渲染遍加载/存储可以改写整个子资源，包括行间空隙，而 Metal 从不写那些字节（docs/FAST-PATHS.md 写明的取舍）。CA 的画面缓冲 1125 像素宽、行跨度 4608（Turnip 线性图像要求行跨度是 64 字节的倍数），所以每个渲染进画面缓冲的渲染遍都要整张拷进拷出；宽度不是 16 倍数的图层表面同样如此。r2n 里结束渲染遍的四分之一（全部样本 2.4%）在影子拷贝，还要占 GPU 时间和内存带宽。
- **0003**：DevicePolicy 新增 `padded_targets`（默认 Shadow，与原来完全一样；Alias 时直接用图像，和只读纹理的 `padded_reads = Alias` 同一种做法），FAST-PATHS.md 记了这一条，不影响编译配置指纹。lavapipe 上：行跨度是 64 倍数的布局在 Alias 下直接用图像，渲染结果与私有纹理逐像素一致、零影子拷贝；全部测试结果与基线一致（新测试因 WSL 缺验证层与同组一样无法建设备）。
- **插件**：启动参数 `pandora_m2v_padded=alias` 才启用 Alias；每 10 秒的统计多打 `pandora-metal W` 行（运行时工作计数的变化：影子纹理、影子拷贝次数、拷回像素数、提交次数等）。mtltest 新增 part 4b：1125×64、行跨度 4608 的 IOSurface，两遍渲染后检查像素和行间空隙字节。
- **r2o / r2p 结果（2026-10-25，插件 0001–0008，Turnip local216，内核 0225，自动滑屏）**：
  - 帧率（120 Hz 屏，GPU 发热封顶）：r2n 平均约 101 → r2o 约 106 → r2p 约 111 fps。GPU 忙碌率 r2o 75% → r2p 65%。温度三次都在 95 °C 上下。
  - backboardd 调用栈 r2n → r2o：分配函数自身 16.6% → 12.9%，`instantiate` 3.4 → 2.2，`EncoderUses::record` 1.8 → 1.2，`command_uses` 1.1 → 0.6，Vec 扩容 3.5 → 2.2，调用日志释放 0.6 → 0，两平面建图 2.7 → 0.9，等 GPU（`wait_for_queue`）3.7 → 2.1；内核样本 2728 → 2243。短轮询超时每秒约 690 → 133 次。
  - 工作计数：r2o 每 10 秒影子拷贝约 4.3 万次、拷回约 74 亿纹素（每帧约 2.5 张全屏）；r2p 影子拷贝约 3.9 万次、**拷回 0**。所以拷回全部来自单平面行间有空隙的渲染目标；两平面 'b3a8' 表面滑屏时只被读，剩下的都是拷入（每个用到它的编码器都要重新拷入一次，因为 CPU 可能改过表面）。
  - part 4b：r2o（Shadow）通过；**r2p（Alias）不过**：每行之后有 44 字节被改（6912 中 2816），即 1125 像素的行被写到 1136 像素（看起来是按 16 像素对齐写满）。Metal 从不碰这些字节。mtltest part 9 的 3 处失败三次都有，是原来就有的。
- **Alias 的决定（用户 2026-10-25：有条件地用）和 0009**：Turnip 源码确认了多写的范围：GMEM 回写在渲染区域碰到图像右边或下边时按 `gmem_align_w × gmem_align_h` 对齐写（Adreno 6xx–8xx 都是 16×4，`A6xxGPUInfo`；`tu_attachment_store_unaligned` 注释"x2/y2 can be unaligned if equal to the size of the image, since it will write into padding space"；单级线性图像下边也不要求对齐，`fd6_view.cc` 的 `need_y2_align`），所以可能向下多写最多 3 行、越出图像。0009 新增 `DeviceOptions::render_store_alignment`：给了对齐时，Alias 只在多写部分落在本行空隙里才生效（单层；高度是 4 的倍数；行距 ≥ 宽度按 16 对齐后的字节数；最后一行多写的部分在缓冲内），否则照旧影子。插件默认开 Alias 并给 [16, 4]，`pandora_m2v_padded=shadow` 退回全影子。mtltest：4b（1125×64）允许每行第 1125–1135 像素被写、其余空隙不许动；4c（1125×62）应退回影子，空隙一字节不许变。
- **r2q（2026-10-25，插件 0001–0009 默认，有条件的 Alias）**：帧率平均约 113 fps（109.5–114.8），GPU 忙碌率 62%，拷回 0（CA 的表面全部满足条件、直接画），backboardd 每 10 秒样本约 4060–4370 用户 / 640–690 内核。mtltest 4b：2816/6912 字节变了，**超出 16 像素对齐范围的 0 字节**；4c（1125×62）：**0/6696 字节变了**（退回影子）。只剩 part 9 原有的 3 处失败；vktest 全过。温度仍封顶（小核 104 °C、GPU 94 °C）。
- **上次的测试安排（已做完）**：插件 /opt/work/lplugin/out（m2v 0001–0008，工作计数，两个启动参数开关），Turnip local216（等围栏单次 100 ms），内核 0225。
  - r2o：默认（Shadow；rebinds 拒绝默认开）。和 r2n 比 backboardd 的 CPU、分配占比、`drop CallLog` 是否消失；工作计数里每 10 秒的影子纹理、影子拷贝次数和像素数（分清两平面和单平面）、短轮询次数；part 4b 应通过。
  - r2p：加 `pandora_m2v_padded=alias`。part 4b 判断 Turnip 是否碰行间空隙；比较单平面影子拷贝、GPU 等待帧数、帧率和温度。part 4b 不过就不用 Alias。
  - 有问题时的对照开关：`pandora_m2v_rebinds=allow` 恢复调用日志。
  - 之后再看：关掉 `pandora_bigfirst` 时的温度和帧率；两平面表面每遍拷贝的方向（见上面“需要你定的”）。
- **Turnip 等围栏改成单次最多 100 ms（tools/turnip/pandora_wait.py，local216，待上手机）**：UniXNU 的 darwin 后端 `dw_wait_fence_raw` 每次只向内核要 1 ms，等一帧 GPU 工作要来回 IOKit 好几趟（r2n：backboardd 每秒约 420 次短轮询超时）。内核从 0216 起就睡到 GPU 围栏中断为止，照 Linux msm 后端 MSM_WAIT_FENCE 一次给整个超时的做法，改为每次给剩余时间、封顶 100 ms（内核等待不可中断，与翻译层自己的 100 ms 粒度一致）。CI 的 turnip.yml 同步加了这一步。

### CPU 深度休眠：分析（2026-10-25，未动代码）
- **核心级“ret”已在 0215 做了**（PSCI CPU_SUSPEND 0x4，按下一次定时器远近选 ret 或 WFI）。没做的是簇级和整机级，设备树层级：cpu0–5 → cluster-pd0（ret-pll-off 0x1000054，最短停留约 9.1 ms），cpu6–7 → cluster-pd1（同上），两簇 → cluster-pd2（apps-pc 0x200c354，约 10.2 ms）；要 PSCI OS 协调模式（已确认固件支持）。
- **结论：现在做簇级休眠对发热几乎没用。** 滑动时每 10 ms 一帧，簇里所有核同时空闲 9 ms 以上的机会很少；静止时 vmd 占约 93% 的一个核、服务反复拉起，簇里总有核在忙。它要等“服务反复查询”解决后，对待机和静止画面的功耗才有意义。
- **滑动时更直接的是调度**：测试一直开着 `pandora_bigfirst=1`，线程优先放 4.4 GHz 的大核。下一步可对比关掉大核优先（或换苹果自己的大小核调度）时的温度、帧率、CPU 占用（要上手机，需用户同意）。

- 2026-10-28：交给 DeepSeek 的 spkstart 任务（D:\DeepSeekHarness\spkstart\TASK.md）：安卓放音时 AP→ADSP 的 GPR 命令序列 + 功放寄存器写序列，目标是让 TDM 口出位时钟、功放 PLL 锁上。gpicheck 修正（立即数 TRE、GO 写 rx_len 0、时钟 {2,5,11,22}、先用 GPII1）已改代码，待主机测试和编译。
- 2026-10-28 spk6（k0241，pandora_gauge=2）：iOS 下经 GPI 读电量计成功，十个寄存器全部 err 0，用 GPII1，事件在 TX/RX 各一条。下一步把读数接进 SweetBattery（现在 ad_batt 还没人填）。交给 DeepSeek 的 battui 任务（D:\DeepSeekHarness\battui\TASK.md）：安卓的显示百分比、充电状态、电池温度怎么从原始读数算出来。
- 2026-10-28 交给 DeepSeek 的 iosaudiocheck4（D:\DeepSeekHarness\iosaudiocheck4\TASK.md）：状态页写入方、ARMDMA/ARMIIS 全部重写槽、CoreAudio 读的注册表键、基类对子类的要求、开流到每周期的时序。thermalcheck、spkstart 的 RESULT 已交回，待复核。正在做：电量计读数接进 SweetBattery（显示一直是充电中 100%）。
- 2026-10-28 daily-k0244 已装进开机菜单的 iOS 格子（0244，参数加 pandora_gauge=3 pandora_battery=1；bat3 验证：80%、4234 mV、-1109 mA、接电源、充电中）。
- **用户插入（2026-10-28）：电池"每 20 秒刷新"不够好，要改。** 计划：
  1. 插拔立即反映：SOCCP 的 MCA 通知（type 2）一到，马上重读 bus_volt 和电量计，并立刻推给 SweetBattery（给它加"马上刷新"的入口，不等 20 秒定时器）。先确认插拔时 SOCCP 会发通知：要一次测试中途拔插 USB（需要用户动手）。
  2. 平时的刷新节奏照安卓：查 bq27z561 驱动和 MCA 业务层的轮询周期（battui 没查），电量计照那个周期读；值变了才通知 iOS。
  3. 充电时安卓多显示 1-2 个点的处理（battui 第 2.2 节）查到后补上。
- 2026-10-28 用户：之后自己查，不再给 DeepSeek 派任务。
- battui2 复核（VERIFY.md）→ 0246：显示百分比照安卓（剩余/满充，97% 拉伸到 100%，国行不分充放电；DeepSeek 说放电用 rsoc 是欧版分支，已改正），电量计 5 秒一读、显示变了才通知，插拔以 SOCCP 通知 0 为准。平滑（每次走 1 点）未做。
- thermadc 复核+自查（VERIFY.md）→ 0247：板上 14 个热敏电阻只读可得（SDAM0 0x090 第 1-7 格、SDAM1 0x091 第 0-6 格，设备树顺序；安卓单位 1/1024 度当 m 度报）；pandora_therm=1 每秒读、每 10 秒记机身温度（五个加权和取最大，2428 行 dump 验证）。未接降频。
- 待上机（合并成一次 bat4）：0245-0247 = 插拔马上反映 + 显示百分比 + 读热敏电阻；要用户中途拔插 USB。看点：插拔日志、百分比、PANDORA THERM 的"格子变了多少次"（iOS 下 PMIC 是否还在测）。
- 之后：按 mi_thermald 的 SS-CPU0/CPU6/MONITOR-GPU 表接降频（等 bat4 证明温度可读）；iosaudiocheck4 未完成部分自己看。
- **用户插入（2026-10-28，bat4 之后查）：** Metal 翻译层的三个问题：
  1. renderCommandEncoder 不支持 MTLPixelFormatRGB8_444_2P（两平面 YCbCr 4:4:4 8 位）；
  2. AttachmentFormat(Color(0))：渲染管线要求的颜色附件和当前渲染目标不兼容，部分绘制被拒；
  3. BGR10_XR_sRGB：翻译器只支持它的一部分混合组合。
- **用户同意（2026-10-28）：** USB 排在三个 Metal 问题之后。现状：iOS 下只有充电（SOCCP 管），没有数据（无控制器驱动）。方向：查 iPhone X 内核缓存里 USB 控制器驱动是否是 Synopsys DWC3 一系；若是，高通外壳（ssusb@a600000 glue、PHY、eUSB2 repeater）由我们上电配置，控制器交给 iOS 原生驱动，用 ADT 描述接上，iTunes/usbmuxd 原生可用。
- bat4（k0247，2026-10-28）：无死机，VKTEST 17 过。百分比照安卓（显示 78%，电量计 76%）；电量计 5 秒读通。插拔没测到（180 s 内 bus_volt 一直 5 V、无通知）。热敏电阻：iOS 下 SDAM 14 格全 0（安卓驱动配的 TM 自动测量重启后没了；SDAM0 0x4F-0x55 = 27 00 01 14 80 44 01，即引导程序单次测过 pmih010x 0x44 batt_therm，第 0 格 21401）→ 要读温度得照安卓 adc_tm5_gen3_configure 写 ADC 配置（PMIC 写，问用户）。芯片又到 105 C。
- **用户插入（2026-10-28）：** USB 串口（调试日志走 USB），参考 https://github.com/Lux12138/nabu-usb-cdc 和 https://github.com/xiaoleGun/QcomXnuUsbDebug（只读参考）。与 USB 计划合并考虑。bat4 时用户没有拔插 USB，插拔测试仍待做。
- **用户（2026-10-28）：先做 USB 串口（实时日志），Metal 三条暂停。** Metal 进度：三条来自同 SoC 的另一位移植者，不是我们日志里的。①RGB8_444_2P 当颜色附件：metal2vulkan 只让"读出来要换算"的双平面格式经 RGBA32F 替身渲染（ycbcr_renders 要求 reads==Converted），RGB*_444_2P 这类"存原码"的直接拒绝（render_pass.rs ~915）；shader 写入规则已有（ycbcr_write.rs：Y=G、Cb=B、Cr=R，round(clamp(v)*(2^b-1))），但渲染存回的规则作者只量过 converted 格式，code 格式未量。②AttachmentFormat(Color(i))：render.rs route/check_pipeline，管线颜色格式无法按 Metal 的放法落到通道颜色上时拒绝。③BGR10_XR_sRGB 混合：degradation.rs ExtendedRangeBlendSource 等。②③要具体格式/混合组合才能补。
- **用户同意（2026-10-28）：** 可以往 PMIC 的 ADC 写测量配置（只写 vadc@9000 的测量设置，不碰电压/电源），让 TM 重新自动测温；读到后照 mi_thermald 的表降频。顺序：USB 串口 → 电源（电池+温控）→ 音频 → 三个 GPU 问题。
- USB 串口测试（2026-10-28）：usb1（k0248）只读：引导程序交出来时时钟开、电源域开、设备模式、已停，正好接手。usb2（k0249）0.19 s 重启：模块在关中断里调 ml_io_map 申请内存 → 0250 先在开中断时映射。usb3（k0250）串口起来了（phase 3，DMA 走直通上下文 bank 3），电脑 16:50:08 识别到 VID 1209 PID 316D、首次装驱动分到 COM8；5.18 s 重启："kmem_alloc(..., 131072, 0x100021): failed with 3"——三个窗口 2.25 MiB 用 ml_io_map 放进了只有 8 MiB 的 io_submap，触摸驱动随后要 128 KiB 没地方 → 0251 改用 ml_io_map_unmappable。
- 温控 TM 配置（下一步，已同意）：照上游 qcom-spmi-adc-tm5-gen3.c adc_tm5_gen3_configure，每个 TM 通道：等 HS(0x45)==READY 且 CONV_REQ(0xE5)==0；读 0x4F..0x5A 12 字节；写 SID=reg>>8（gen4 含总线位，如 0x27）、PERPH_CH=0x80|格号、TIMER_SEL=4（1 s）、DIG_PARAM 改 bit5:4=1（比例校准）bit3:2=2（默认抽取）、FAST_AVG=0x80、CH_SEL=reg&0xff、DELAY_CTL=2（200 us）、CH_EN=0（不开阈值中断）；再写 CONV_REQ=1。通道顺序 = DT 里 qcom,adc-tm-type 的顺序（14 个），SDAM0 第 1-7 格、SDAM1 第 0-6 格。写走 SPMI 通道写（chnls 0x0c900000）。
- usb4（k0251，2026-10-28）：USB 串口可用。16:56:01 电脑打开 COM8，从内核 0.000 s 到 180 s 的日志实时传到电脑（1.5 MB），和手机写盘的那份逐字节一致（1,525,912 字节），只少重启前 1 ms 写的最后两行；模块状态 phase 3，dropped 0，io failed 0；VKTEST 16 过，无死机。抓取：tools/usbserial_capture.ps1。0253：每次重启前（pl_psci_reset）pandora_usb_final 把剩下的发完，最多等 2 s。0252：pandora_therm=2 配 TM（待测）。
- usb5（k0253，2026-10-28）：串口日志和写盘日志 1,544,638 字节完全一致，重启前最后一行也到了（0253 生效）；VKTEST 17 过，无死机。TM：14 格配置无错，每秒更新（TM_EN_STS SDAM0 fe、SDAM1 7f）。但 SDAM0 第 1-6 格（xoagg qtm pa2 display charger wireless）比约 80 s 后安卓读数高 20-26 度（如 wireless 80 度、pa2 83 度，同时 cpu_therm 56 度），其余 8 格比安卓低 1-5 度（切回安卓途中升温，合理）。唯一已知差别：抽取率，安卓写的是 DIG_PARAM 0x14（档位 1，thermadc regmap_0-00_9000_t1），我按上游默认写了 0x18（档位 2）→ 0254 改档位 1，pandora_therm=3 同次开机先档位 2 后档位 1 对比。
- usb6（k0254，2026-10-28）：同次开机抽取率 2 和 1 读数差不到 1 度 → 不是抽取率。偏热的 6 个通道（xoagg qtm pa2 wireless display charger）加 cpu_therm 都经过 PMIC GPIO，安卓 pinctrl 把它们设成高阻（bias-high-impedance = EN_CTL 0x46 清零）：vadc 自己的 pinctrl-0（pmk8850 gpio7 @0xbe00、pmh0101 gpio2/3）和 pmh0101（gpio9、gpio12）、pmih010x（gpio12、gpio18，SPMI 第二条总线）GPIO 控制器的 default。安卓下实读（临时挂 debugfs，regmap 0-00/0-01/1-07）：7 个都是 MODE 00、PULL 04、OUT 01、EN_CTL 00。iOS 下没人设。0255：pandora_therm=2 先读这 7 个 GPIO；pandora_therm_gpio=1 时同次开机先记读数、清 EN_CTL、3 s 后再记读数。SPMI 读写支持第二条总线（APID 接在总线 0 之后，owner 表 0x0c43b000）。写 GPIO 超出"只写 vadc 测量设置"的范围，测前问用户。串口：usb6 收到 1,863,119 字节。
- usb7（k0255，2026-10-28，用户同意写 7 个热敏电阻 GPIO 的 EN_CTL）：iOS 下 6 个 EN_CTL=0x80（开着），cpu_therm 的 0x00。同次开机清 xoagg 的 EN_CTL 后读数 71.9 → 48.2 度，和 xo 47.7 一致，直到测试结束都贴着 xo（54.8 对 54.3）。另外 5 个写失败（-2）：pandora_spmi_write 只缓存 2 页读写通道 → 0256 改 16 页；pandora_therm=2 默认先清 7 个 EN_CTL 再配 TM（安卓顺序），pandora_therm_gpio=1 改为前后对比模式。无死机，VKTEST 16 过，串口 1,528,926 字节完整。
- usb8（k0256，60 s，手机凉下来后，2026-10-28）：7 个 GPIO 写全部成功，EN_CTL 80 → 00。同次开机前后：xoagg 67.4→43.7、qtm 68.9→35.5、pa2 74.6→42.8、display 60.9→35.8、charger 70.5→45.9、wireless 73.5→48.1；其余 8 个不变（xo 43、pa1 43、quiet 42、cpu 47）。机身温度（mi_thermald 公式）20 s 36.9、29 s 39.0、49 s 40.6。无死机，串口完整。热敏电阻读数问题解决。下一步：按 mi_thermald 表降频（SS-CPU0/SS-CPU6/MONITOR-GPU，thermalcheck §2）。用户提醒手机很烫（连续 7 次测试）→ 以后测试间隔散热、短测试优先，降频先做。
- **用户（2026-10-28）：** 同意照 mi_thermald 表降频（SS-CPU0/SS-CPU6/MONITOR-GPU，含 25 度那档）。随后插入：「能有iOS的温度就更好了」→ 把板上温度交给 iOS 自己的温控（thermalmonitord / CLTM 读 HID 温度传感器、算热压力，CLPC 和背光、充电按它限），也让 iOS 里能看到发热状态。顺序：先做安卓表（手机会过热，先有保护），同时查 iPhone10,3 的 thermalmonitord 要哪些温度服务、属性和名字，查清后提方案；iOS 那套工作后安卓表降为保险或去掉。
- usb9（k0257，180 s，2026-10-28）：机身温度降频起作用。9.2 s 起限（机身 36.3：小核 2.75 GHz、大核 2.44 GHz、GPU kgsl 5 档），37 度、39 度、40 度各收紧一档；整整 180 s 核心没到过 105 度（无 PANDORA THERMAL 记录）；机身 173 s 时 40.3 度（usb7 同样测试 63 度）。VKTEST 16 过，无死机。回到安卓后 cpu_therm 53、xo 52、充电 50（安卓开机自己也在发热）。串口日志有 13 个字节是 0（写日志先占位置后写字，串口线程读早了）→ 0258：sweet_log_done 记写完的字节，sweet_log_copy 只读到"占的 = 写完的"那一刻；重启前最后一抄用 sweet_log_copy_any。
- iOS 温控调研（进行中）：thermalmonitord + /System/Library/ThermalMonitor/D22AP-Info.plist（iPhone X）：12 个热点（DieTempHS 80、MLBHighHS 48、BandMLB、Radio、Camera、Arc、CGLED、Charger…，各自 PI 控制 kp/ki、target、强制等级），DecisionTable 把控制力度换成 dtCPU/dtGPU/dtBacklight/dtCharger 等上限，背光表、充电限流表。传感器经 HID（IOHIDEventSystemClient，PrimaryUsagePage/PrimaryUsage/Product/LocationID）：PMU_Tdev1-8、PMU2_Tdev1-3、TG0B（电量计电池）及 4 字母虚拟传感器（TP0Z、TSFD、TSBE…）。文件留在 WSL /opt/work/exf/thermal。
- iOS 温控调研结果（2026-10-28，文件在 WSL /opt/work/exf/thermal，dis/ 为反汇编）：
  - 我们的 iOS 上 thermalmonitord 已在跑（ul_therm1 日志）：no HID services、找不到 CLPC/PMGR/PPM/PMU/AppleVTempDispatcher 服务、batteryData NULL、"Could not set SGX Max power to 65000"。
  - 输入：HidSensors 用 IOHIDEventSystemClient 匹配 PrimaryUsagePage 0xff00 / PrimaryUsage 5 的服务，按 "Product" 字符串对 D22AP-Info.plist 的 Sensors 表（57 项，4CharCode + matchName）：PMU tdev1-5、PMU tcal、PMU tjunc、TG0B（电池，经 IOPMPowerSource）、PMGR SOC Die Temp Sensor0-3（含 Avg:/Max: 版，0/1 大核、2/3 GPU）、ACC Temp Sensor0-2、mic1、mic2、NAND CH0 temp 等；另有 synthSensor 由 D22 类（tm + md5("D22")）updateAllThermalLoad 计算：TSBE=2.22+0.45电池+0.48tdev2(+0.025后摄)，TSRM=-0.80+0.5电池+0.5tdev4，TSRR=-1.09+0.63电池+0.37tdev3，TSRQ=1.16+0.45电池+0.47tdev2，TSFD=1.10+0.7电池+0.26mic1，TSFC=-0.55+0.64电池+0.35mic2，TSFL=1.09+0.17电池+0.83mic1，TSBa=-1.06+0.16电池+0.89mic1，环境=-0.58+0.65mic1滤+1.29mic2滤-1.08tdev4(600s)。
  - 热点 → 温度：DieTempHS(80)←芯片；MLBHigh/Low(48/45)←TSBE；BandMLB(46/43)←TSRM；Radio(46)←TSRR；Camera(49)←TSFC；Arc←TSBa；CGLED(49)←TSFL；Charger(43)←TSRQ。THERMAL_TRAP_LOAD 53-58 → 降温界面。
  - 输出：AppleCLPC（setDieTempControllerProperty: CPUMaxPower/GPUMaxPower/PackageMaxPower，mW）、AGXAccelerator（SGX/GPU 功率）、AppleARMBacklight（亮度上限）、充电 AppleChargeRateLimitIndex、ApplePMGR、AppleDieTempController。
  - 小米 → iPhone X 对应建议：电池←电量计；tdev2←charger；tdev3←pa1/pa2 取大；tdev4←quiet；mic1←display；mic2←flash；tdev1←cpu；tdev5←wifi；芯片传感器←tsens。usb9 173 s 试算：TSBE 42.0、TSRM 41.8、TSRR 40.5、TSRQ 40.5、TSFD 37.4（mi_thermald 机身 40.3）。
- **用户（2026-10-28）：** 指出应该用小米机器的数据而不是 iPhone X 的（公式、阈值描述的是 iPhone X 机身）；问清区别后决定「接入」：温度接进 iOS 的温控机制（应用的发热等级、降温画面、亮度/充电走 iOS 的路），但机身温度公式和阈值用小米 mi_thermald 的，频率功耗用高通芯片的。下一步：查 thermalmonitord 能否不经 D22 类写死的公式、直接拿内核给的温度当热点温度；plist（目标温度、决策表）可换成按小米表写的版本放进固件包。
- usb10（k0259，2026-10-28）：30 个 HID 温度传感器全部发布，thermalmonitord 一直在读（798 次），mic1/mic2 被设成 4 s 推送；但只送回 TK0C/TK0F/TK1C（摄像头，0），没算表面温度。原因（反汇编 timerCallBackHID）：每轮要等看门狗名单里的传感器都有新值才运行 updateSystemLoad；requiresWatchDog 默认 true，TG0B、TG0V（电量计电池温度，iPhone X 上也是 HID 服务）、NAND CH0 temp 没有服务 → 永远不算。0260 补上：TG0B/TG0V = 电量计，NAND = quiet_therm（仅参考）。usb10 其余：无死机，VKTEST 16 过，无 105 度，串口 1,486,669 字节与写盘一致（0258 生效）。
- usb11（k0260，2026-10-28）：第 1 步验证通过。33 个传感器发布，thermalmonitord 开始运行控制，并把算出的表面温度送回 AppleVTempDispatcher（单位是 16.16 定点的度，0261 已改换算）。和小米虚拟传感器的差：163 s 时 TSBE 36.04/背面 36.19、TSRM 38.90/侧 39.16、TSRR 38.93/顶 39.16、TSFL 37.16/正面 37.19（iOS 对 tdev 有 30 s 平滑，稍滞后）。无死机，VKTEST 16 过，无 105 度，串口 1,469,953 字节一致。下一步第 2 步：plist 换成小米阈值 + 接 iOS 的输出（亮度、充电先问用户），先查 plist 各项（热点 target/kp/ki、Forced* 等级、DecisionTreeTable）怎么起作用、输出走哪些服务。
- **用户（2026-10-28）：** 温控第 2 步（plist 换小米阈值、接 iOS 输出）太慢，先放着，继续扬声器。现状保留：内核按 mi_thermald 表限频（0257），iOS 拿到温度并照小米公式算表面温度（0260/0261）；之后的测试仍带 pandora_therm=2。
- 0262（2026-10-28，k0262 已编译，未测）：喇叭播放图。tools/spk_graph.py 照 AudioReach 源码（acdb_command.c BuildSubgraphDataRsp/BuildSpfPropertyBlob、AcdbCmdGetSubgraphConnections、AcdbGetSubgraphCalibration、AcdbCmdGetModuleTagData；gsl_graph.c 开图）从手机 ACDB 生成 spkgraph.bin（不进仓库，放 ref/ampfw，gpu_test.sh --amp-fw 时打进固件包）：开图 2776 B（4 子图 + 3 条子图连接）、校准 7128 B/168 参数（默认表 + Volume 15）、TDM 标签数据（Channels 2：LPAIF 0、主口 0、内部同步、槽掩码 3、2 槽 32 位、同步模式 1、延迟 1，28 字节旧版结构）。pandora_spk=3：映射不撤、时钟持有 → GRAPH_OPEN、校准、MFC(0x40b6)、共享内存入口格式(0x4001)、TDM 格式与接口(0x4023)、PCM 解码/转换输出(0x4003/0x4004)、SPR(0x4163) → PREPARE → START（都照 AGM/PAL 源码，命令载荷放 DSP 缓冲带外发）；跑 12 s 静音后 STOP/CLOSE、释放时钟、撤映射。pandora_amp_tdm：写安卓空闲时的功放配置（diff_spk5 的配置类寄存器），开串口和放音路由，0.5 s 后看状态；再照 cs35l43_main_amp_event 上电（解锁写 0x2084、0x2014=1、0x2018 bit0），0.5 s、3.5 s 看状态，再下电。不发 PLAY、不写校准。看点：0x2C14（安卓放音时 3）、IRQ 状态、核控制/MPU 锁/心跳。
- spk7（k0262，2026-10-28）：DSP 侧全部成功——GRAPH_OPEN、校准、8 条运行时配置、PREPARE、START 状态全 0（29 ms 开起），12 s 后 STOP/CLOSE/释放时钟/撤映射也 0。功放上电后 0x2040=0x586dfa、0x2044=0x250cfd8a、0x4300=0xc0000000、IRQ 原始 0x10090=0x8000、0x10094=0x345027、0x100a0=0x4000000，和安卓放音中转储完全一样 → TDM 时钟到了，功放时钟/PLL/功率级照安卓运转。但 Halo 核仍不跑：核控制 0x1（安卓 0x81）、MPU 锁 0x800000（0xBEEF）、心跳 0；0x2C14=0（安卓放音中 3；CS35L41 头文件 0x2C10 是 DATA_FS_SEL，0x2C14 未定义）。下一步方向：时钟在时重新启动 Halo 核（停核、软复位、照 pa_start 再起）；放音中补 pa_dump 和安卓转储全量对比。日志行太长被别的线程截断，要拆短。
- **用户插入（2026-10-28）：** 「亮度调不了了，而且以前调的亮度会影响进入logo的亮度，以前调的亮度实际显示也不是无极的而是有明显升降的」→ 三件事：①现在调不了亮度（先查是不是 0259 起 iOS 温控接上后它的背光控制在起作用）；②iOS 里调的亮度会带到下一次开机 logo；③亮度不是连续的，有明显台阶。扬声器（spk8：时钟在时重启 Halo 核）暂停，先查亮度。
  - 查到的：
    - ①：usb10、usb11、spk7（k0259 起）开机约 24 s 都有一次 FadePeriod 0、brightness = 1（面板 7，最低），usb8/usb9 没有。这是 thermalmonitord 有了传感器后第一轮背光控制：BackLightCC（-[BackLightCC defaultAction]）往 CoreBrightness 发 MaxBrightness（BacklightBrightness 表的百分比）和 BrightnessWeakCap（_maxLICeilingSoft，初值 100，第一轮必发）。CoreBrightness 收到后把亮度压到最低，滑条也被卡住。CoreBrightness 这边怎么换算没拆完；它给我们写的 DisplayBrightness 里 Nits 一直是 0。
    - ②：每次开机我们第一次写亮度前，面板读回的都是 0x394（916）。iOS 调暗后热重启，面板保持 iOS 最后的亮度，直到有人重新写。
    - ③：CoreBrightness 拖滑条时发的是 FadePeriod 0、每次跳 1000-6000（满 65536）；熄屏时 FadePeriod 12124 = 0.185 s（16.16 秒，和 backboardd 日志的 fade duration 0.185 对上）。我们以前忽略 FadePeriod，每次直接跳到新值。面板 DT 有小米的渐变命令：mi,mdss-dsi-dimmingon-command = 53 28，off = 53 20。
  - 0263（k0263）：
    - iOS 温度传感器默认关，要 pandora_therm_ios=1 才发布（温控第 2 步给出这台手机自己的 plist 时再开）。内核自己按 mi_thermald 表限频不受影响。
    - 第一次写亮度前读回面板亮度（记作开机值），每次重启前（pl_psci_reset → sweet_bl_final）先关面板渐变、写回开机值。
    - 打开面板自己的渐变（53 28）；FadePeriod 由一个内核线程照做：每 16 ms 走一步，在 FadePeriod 内线性走到新值；FadePeriod 0 直接写，由面板渐变平滑。
    - 换算（平方律，0x1554 为顶）暂不改。
  - 测试 bl1：开机后看 24 s 处不再有 brightness = 1；用户拖滑条看是否跟手、是否平滑；重启看 logo 亮度。
  - bl1（k0263，2026-10-28）：24 s 处不再压到最低（全程 brightness 只有开机 8996、熄屏 0、亮屏 8996 和拖动）；开机读到面板 916。拖滑条时 CoreBrightness 每 8 ms 发一次、FadePeriod 0，内核立刻写；熄屏、亮屏照 0.184 s 走 10 步，面板读回对得上。**用户：「亮度变的太慢了」** → 慢在面板自己的渐变（53 28），每一步都被面板拖慢。0264：面板渐变默认关（pandora_bl_dim=1 才开），FadePeriod 仍由内核照做。
  - bl2（k0264，2026-10-28）：**用户：「跟手了，logo也正常」**。开机读到面板 916、面板渐变关；bl1 重启前写回 916。亮度三件事完成。iOS 温度传感器要等温控第 2 步再开（pandora_therm_ios=1）。下一步回到扬声器 spk8。
- 0265（k0265，2026-10-28）：功放的 PLL 照安卓设。cs35l43_component_set_sysclk（ampfwcheck raw/cs35l43_dis2.txt 0x529c）每次开流时改 0x2C04 分六步：开环（bit 11）→ 参考频率（bits 5-10，3.072 MHz = 0x21）→ 关 PLL（bit 4）→ 选源（bits 0-2，0 = 位时钟）→ 闭环 → 开 PLL；再写 FS_MON 0x2D10。cs35l43_pcm_hw_params 先写 0x2C0C = 3（48 kHz）。spk7 是一次把 0x2C04 写成 0x430，PLL 可能没重新锁。另外安卓空闲转储 0x2C14 也是 0（放音中才 3），所以 0x2C14 跟着放音走，不是 Halo 跑不跑的原因。
  - TDM 那一步：配置 → 照安卓设 PLL（最后清掉 pa_start 加的 bit 16，安卓转储是 0x430）→ 看心跳；不走就在时钟在时停核（清 EN、软复位置 1）、再照 pa_start 起核，前后核对 XM[0]（固件还在）→ 上电 0.5 s 时读安卓转储里有的那批 Halo 寄存器、全量 pa_dump 两颗，和安卓放音中转储对比 → 3 s 后下电。不发 PLAY（还没有校准值），DSP 送的是静音。日志行拆短。
  - 测试 spk8：参数同 spk7，--secs 90（喇叭这段在开机 30 s 内）。
- spk8（k0265，2026-10-28）：PLL 照安卓设好（0x2C04 = 0x430）、时钟在时停核再起，心跳都不走，0x2C14 仍 0。找到直接原因：MPU 一条都没写进去。访问寄存器读回 3 1 ff0000 fffff 0（安卓 20 个全 0xffffffff），锁存器 0x800000（安卓 0xbeef），越界状态 0x2BC3104 = 0x400000、310C = 0x400000、3114 = 0x10001（安卓都 0；cs_dsp 里这三个是 XM/YM/PM_VIO_STATUS）。也就是核跑过、碰内存被 MPU 拦下停住。XM[0] 读出 0（加载时 173 个字都对）。安卓 cs_dsp_halo_configure_mpu 的 23 次写和我们逐条一样。差别在时机：安卓 cs35l43_probe 复位后读 ID、打补丁、设中断屏蔽，就进 cs35l43_dsp_init → cs_dsp_stop 停 ROM，前后几毫秒；我们先读固件文件才停，第二颗等到第一颗加载完（几秒）。推测 ROM 跑久了自己配了 MPU 并锁死。
- 0266（k0266）：复位后照安卓顺序（读 ID、补丁）马上停两颗的 ROM；停前、停后、起核后各记一次 MPU。测试 spk9 参数同 spk8。
- spk9（k0266，2026-10-28）：早停 ROM 没改变结果。ROM 在跑时锁存器读 0xbeef（和安卓转储一样，所以 0xbeef 是核在跑时的读数），停后访问寄存器 3 1 0 0 0、越界状态全 0；我们配完读 3 1 ff0000 fffff 0（写是生效的，可能每个寄存器只有这些位；安卓转储的 0xffffffff 可能是 regmap 缓存值）；核一起动越界状态就置上、核停住。安卓 cs35l43_dsp_preload_ev：事件 1 若有外部回调（偏移 1000，本模块里没人写它）先强制开 PLL 再加载；事件 2 写 0x2900=2、cs_dsp_run，再撤强制。
- spk10（k0267，2026-10-28）：越界只在起核之后出现（ROM 停后、加载完都是 0），两颗一样：XM 0xffffe、YM 0x820d6、PM 0x7a90，全在固件加载范围外（wmfw：PM 0..0xf0c 一块 19260 字节，XM 到 0x1e7e，YM 到 0x7aa）；YM 0x820d6 = 固件号 0x1820d6 的低 20 位。核被 MPU 停住后核控制写不掉 EN，0x40 的重新加载在 pa_load 里直接放弃了（没测到）。我们只核对过 PM 前 128 字节，PM 是一次 I2C 发 19260 字节。
- 0268（k0268，用户：「要不做多点一次」→ 一次多做几组）：①写完固件整块读回（每次读 30 个寄存器，整组，不从压缩组中间读）；②TDM 那一步先硬复位两颗（pa_reset，照 cs35l43_probe），有位时钟时重新加载：0x40 每次发 120 字节，0x42 一块一次发完，都整块读回再起核；③TDM 那一步等开机加载做完（最多 20 s）；喇叭图跑 30 s。测试 spk11 参数同 spk10。
- spk11（k0268，2026-10-28）：写入没问题——开机和复位后、一块一次和 120 字节分包，四次整块读回 5871 个寄存器全对（含 PM）。核照样跑飞到同样三个地址。但复位后的那组是先加载起核、后设 PLL（复位把 0x2C04 清回 0x10），"有时钟"并不成立。和安卓转储对比：安卓转储里驱动只写不读的寄存器是 regmap 缓存值（0x0104/0x0124 驱动从不写，转储的 0 是默认值），不能当芯片读数；cap_during.sh 把文件名标反了（T_regs = 13-0040 = DT 里的 "B"）。安卓 cs35l43_irq：0x10014 bit 0 = PLL Lock INT，bit 3 = Unlock。
- 0269（k0269）：照安卓开流顺序：复位 → 配置、PLL（set_sysclk）、路由、使能 → 查锁（0x10014 bit 0）→ 再加载起核。0x40 全量；0x42 不写场景（安卓在 DSP 跑起来后才叠场景）。开机那组 0x42 只写固件（看是不是数据害的）。不再整块读回。喇叭图 20 s。测试 spk12 参数同 spk11。
- **用户插入（2026-10-28）：** 「开个fork给 xnu-ios-build-linux 的 GitHub 页面 .MD 重写」→ fork 会话 xnu-build-readme 在做，推送前先给用户看。
- 同步上游（2026-10-28 做了）：三个本地克隆都快进到最新。UniXNU-sheng d76556b → 08cde20（11 个提交：A510 当 E 簇其余当 P 簇的 AMP 调度补丁 zzyf-sheng-amp.patch、八核调频、触摸时拉高 CPU/GPU、启动日志加行、ShengUfs 显示和 WiFi 窗口），我们拷过的 SMEM/SMP2P/GPR/GLINK/FAT 五个库都没改，不用更新；ios-sweet 只加了 MIT 许可和网站；ios-fuxi ba239fc → d49884e（263 个提交，主要是 Metal、扫描输出、GPU ABI、WiFi），提交说明里没有我们那三个 Metal 问题的关键词。
- spk12（k0269，2026-10-28）：开机 0x42 只写固件也一样跑飞 → 不是系数/场景数据。设好 PLL 后 0x10014 锁定位为 0（位时钟在也没锁；安卓 PLL Lock 也在 GLOBAL_EN 之后才出现，安卓首次起核时同样没锁）。照安卓顺序复位、PLL、加载起核仍跑飞。下一步（spk13，待写）：照 cs35l43_dsp_preload_ev 事件 1，先置 0x2C04 bit 16（强制开 PLL）再加载、配 MPU、起核，起核后再清（spk4/spk7 只在出故障后才置过）；配 MPU 后立刻读回访问寄存器看 bit 是否写满。推测：MPU 在 DSP 时钟域，没时钟时解锁/写入只生效一部分，核碰 XM 顶端系统寄存器区（0xffffe = 0x2BFFFF8）被拦。
- **用户（2026-10-28）：「推进！」** → 安卓下抓功放 DSP 重载的寄存器跟踪（Magisk root；单独的 ftrace 实例 amptrace，regmap 事件只记 13-0040；tinymix 把 "B DSP1 Preload Switch" 关 2 s 再开；之后删实例）。文件在 D:\DeepSeekHarness\ampfwcheck\amptrace（手机日志，不进仓库）。结果：
  - 停核：读 WDT、SCRATCH1-4；核控制从缓存读 0x81，写 0x80；软复位 0x2B80010 写 1。
  - 加载：0x2900=2，固件、系数整块写；读一批 XM 控件；写 XM 0x280125c（ALLOW_ACT_BOOT 的缓存值）。全程不碰 0x2C04（这台手机上没有强制开 PLL 的回调）。
  - 起核：MPU 23 次写和我们完全一样（20 个访问寄存器都写 0xffffffff）；核控制写 **0x281 再 0x81**。cs_dsp_halo_start_core 是在 regmap 缓存上 update_bits，缓存里是停核时写的 0x80，所以 bit 7 又写回去了。我们从芯片读（软复位后读出 0），写的是 0x201、0x001，bit 7 一直是 0。
  - 起核后 3 ms 内固件发邮箱中断（0x10010 = 0xc0008000），驱动清 bit 31、写 0x13020 = 0x02000003、读 0x13000 起 8 个邮箱寄存器，内核日志 MESSAGE 0x2000002 "AWAKE"。
  - 重载后不放音时 HALO_HEARTBEAT 读出 0 且不动，所以空闲时心跳不动不代表核没跑；看邮箱、越界状态和锁存器 0xbeef。
- 0270（k0270）：起核写 0x281 / 0x81；起核后照安卓做邮箱握手；开机两颗都只写固件和系数（场景等 DSP 跑起来后再叠，这次先不写）；TDM 那一步不再复位重载：配置、PLL、路由、使能、上电，固件醒着就发 PLAY（看 AUDIO_STATE 1/2/6/7），3 s 后 PAUSE（0/2/6），再下电。还没写校准值（CAL_R_SEL、ALLOW_ACT_BOOT），DSP 送静音。测试 spk13 参数同 spk12。
- **spk13（k0270，2026-10-28）：功放 DSP 跑起来了。** 两颗都是：起核后邮箱 0x10010 bit 31 置位，mailbox[1] = 0x2000002（AWAKE，和安卓一样）；核控制 0x81、锁存器 0xbeef、越界全 0。PLAY 后 AUDIO_STATE 2；3 s 时 0x2C14 = 3、0x2038 = 1（和安卓放音转储一样），心跳 100 ms 走 0x9a；PAUSE 后 AUDIO_STATE 0；下电正常。无死机，VKTEST 过。
  - **用户（2026-10-28）：「都可以，你自己来」**（同意从安卓读校准值、测试音音量我定；GPU 比对流程 6 个阅读代理读完就停，结论我自己看）。校准文件 /mnt/vendor/persist/audio/crus_calr.bin（8 字节）拷到 ref/ampfw（git 忽略），gpu_test.sh 有就放进测试包。
  - 0271（k0271）：功放第一次 PLAY 前清 0x10110 bit 31、CAL_R_SEL = 本颗校准值（文件第 0 个字给 T/0x42，第 1 个给 B/0x40，原值写入，不打印）、ALLOW_ACT_BOOT = 1；上电时先叠 music 场景（cs35l43_apply_delta_tuning = cs_dsp_load_coeff）。pandora_adsp.c：图跑起来后往共享内存端点（0x4001）送 1 kHz、-30 dBFS 的正弦，20 ms 一块、4 块在路上（DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER 0x04001000，DONE 0x05001000），功放 PLAY 到 PAUSE 之间约 2 s 能听到。测试 spk14。
  - spk14（k0271）：校准、ALLOW_ACT_BOOT、场景都写进去了，两颗 PLAY 2 / PAUSE 0；但 0x4001 对 v1 写数据命令回 AR_EUNSUPPORTED (3)。spk15（k0272，改 v2）：打包缓冲 64 字节放不下 24 字节 GPR 头 + 44，一块没发出去。spk16（k0273）：送出 1005 块、退回 1002 块、无错，两颗在播放，但用户没听见（2 s、-30 dBFS、开机途中）。
  - **用户（2026-10-28）：「能不能弄失败不重新拉起」；同芯片作者把发热那些（停服务等）都做好了，会发给我们**（到了先对照，重复的用他的或合并）。spk16 一次开机：AegirPoster 崩 10 次，SiriTTSSynthesizer/MacinTalkAUSP/KonaSynthesizer 各 6 次，fairplayd.H2 8 次，assetsd 3 次，ReportCrash 最多占 45% 一个核；vmd 68%、coreduetd 58%；开机前十几秒调频从最高档起步。
  - 0274：sweetcrashstop（同一程序本次开机崩满 3 次就在 exec 处拦下，核心进程除外）。0275：-20 dBFS 断续"嘀"8 s，播放第 1/4/7 s 读固件的 AUDIO_EN_STATUS、CLOCK_OK、SAMPLING_RATE、ERROR、ABNORMAL_MUTE、AUDIO_IN_MAX、VOLTAGE_MAX、CURRENT_MAX（安卓放音时 1/1/48000/0/0/0x109/0x1a8/0x172）。
  - **spk17（k0275）panic**：「kfree: addr …, size 8 found in heap data.* instead of *」，开机 10.18 s（prng_seedctl 前后）。原因：为 0274 把 kern_exec.c、kern_exit.c 放进 local_build 本地编译；local_build.sh 头注释写明本地编的目标文件没有 Apple clang 的 kalloc_type 内建（xnu-cc.py 用替身），这两个文件大量用 kalloc_type，和 CI 部分对不上。**改 xnu 核心文件（用 kalloc_type 的）只能走 CI。** 0274 等 CI。spk17b（k0275b）= 0275 但 kern_exec/kern_exit 用 CI 目标文件（不含 0274）。
  - spk17b：嘀声被喇叭图收下，两颗时钟/采样率/使能和安卓一样，但 AUDIO_IN_MAX 0 → 0276：流的音量模块（0x401f，ACDB 里没有校准）设单位增益 0x2000、取消静音，设备音量模块（0x437e）取消静音。spk18（k0276）：两颗都收到嘀声（输入、电压、电流都不为 0），用户没听见。0277：-12 dBFS 连续 1 kHz 15 s，每秒读一次。spk19（k0277）：15 s 里两颗输入、电压、电流都稳定且比安卓放音乐时大，用户没听见；同一次开机 48 s 一次 GPU 提交被拒（0xe00002c2）→ Turnip DeviceLost → 屏幕划不动、一直闪（放到 GPU 那一步）。
  - 安卓对照（2026-10-28，手机在安卓）：喇叭音乐音量原是 0，临时调 60 用系统音乐播放器放 3 s 提示音、录两颗功放的 regmap 跟踪（agmplay 只开 DSP 侧，不会打开功放，录到 0 条），放完音量改回 0。安卓开声音：0x4C00=0x36、0x6808=0x3F75（噪声门）、0x4800=0x10001、0x2018=0x3721 → 场景 → 解锁写 0x2084 → 0x2014=1 → 清 0x10110 bit 31 → PLAY；我们除 0x6808 和 0x2018 bit 0 的先后都一样。再用 regmap debugfs 的 cache_bypass 读安卓空闲时两颗芯片的真实值（跟踪/转储在 ampfwcheck/amptrace，不进仓库）：我们照旧转储写的配置表只有 **0x0104、0x0124 不对**——芯片上是 1（和我们复位后一样），旧转储的 0 是驱动缓存的默认值，驱动从不写，我们却写成了 0。
  - 0278（k0278）：不再写 0x0104/0x0124；照安卓顺序写 0x4C00、0x6808、0x4800、0x2018=0x3721；放音第 2 s 两颗普通寄存器整块打印，和安卓芯片真实值对比。测试 spk20。
  - **spk20（k0278，2026-10-28）：喇叭出声了**（用户：「有声音了」，没分清是一个还是两个喇叭）。功放固件心跳开始走（0x5b33 → 0x5bcc；spk19 一直 0），所以 0x0104/0x0124 = 0 时固件只是报读数、没正常运转；读数本身和 spk19 差不多，不能用来判断出没出声。放音中 232 个普通寄存器和安卓芯片真实值比，只剩固件运行值（0x4304/0x4308）、两颗各自的 TDM 槽（0x4810/0x4820）、中断锁存（0x10014/0x10020）和中断屏蔽（0x10110–0x10118：安卓开了错误中断，我们全屏蔽、轮询）。这次没出现 GPU 提交被拒。
  - 下一步（等用户点头）：接 iOS 的音频设备，让系统和应用的声音走这条路；之前可选一次上下喇叭轮流响，用耳朵确认两颗都出声。
  - 0279（用户：「可以上下用不同频率」）：从测试音开始算 3–8 s 只有底部 500 Hz（右声道，TDM 槽 1），9–14 s 只有顶部 1 kHz（左声道，槽 0），15–18 s 一起；功放放 18 s。spk21（k0279）：用户忘了听；读数对上了——底部那段只有 0x40 有输入（0x6d1，电流 0x4e7），顶部那段只有 0x42 有输入（0x7c5，电流约 0x20a），段外两颗都是 0。右→底部、左→顶部确认。一起那段的读数行丢了（日志通道丢行）。顶部功放推的是顶部喇叭还是听筒，要靠耳朵确认；同一个内核可以再跑。
  - 用户：「我听不出听筒和喇叭呀」→ 查安卓：resourcemanager 里听筒（PAL_DEVICE_OUT_HANDSET）和喇叭都走 TDM-LPAIF-RX-PRIMARY；顶部功放 0x42 有 pinctrl rcv_gpio_high/low（GPIO174），混音控件 "T Enable RCV Pin Control" 默认 1（拉高，外放），只有 handset 路径设 0（拉低）；安卓下读到 "gpio174 : out high func0 2mA pull up"。我们从没设过。0280（k0280 已编译）：TLMM 映射扩到 GPIO175，TDM 线程开头把 GPIO174 设高，记下原状态。测试 spk22 等用户同意。
  - spk22（k0280）：GPIO174 原来 0x801/0（输入、下拉、低 = 听筒模式），设成 0x1a03/3（输出高、上拉）。同样输入 0x7c5 下顶部功放电压峰值 0x400 → 0x8c0–0xa15、电流 0x210 → 0x502–0x56a，翻了一倍多：之前推的是听筒通路，现在是外放。底部不变；各段仍只在自己那颗。
  - **iOS 音频设备调查（2026-10-28，用户：「可以，开始查吧」）**：方向沿用 10-26 用户同意的原生 IOAudio2Device（不是 sheng 的用户态 HAL 插件）。本轮新查到（我自己读的，地址在 iPhone10,3 20H392 的内核缓存和 dyld 缓存里的 CoreAudio）：
    - 设备描述走注册表字典，键是小写带空格的：CoreAudio `HALS_IOA2UCDevice::CopyStreamDictionaryByID`（0x1af656a34）读设备的 `output streams` / `input streams` 数组、按 `stream ID` 找；之前 iosaudiocheck4 只搜了 `IOAudio*` 开头的键，漏了这套。
    - 键在内核里的出处：IOAudio2Family 有控件/流字典的键（`control ID`、`class`、`scope`、`value`、`minimum value`、`start db value`、`db per step`、`stream ID`、`starting channel`、`current format`、`available formats`、`terminal type`、`format ID`、`bytes per frame`、`channels per frame`、`bits per channel` 等）；AppleARMIISAudio 设设备级的 `transport type`、`device manufacturer`、`input/output streams`、`io buffer frame size`、`output latency`、`output safety offset`、`clock domain`、`sample rate`；AppleEmbeddedAudio 有 `device name`、`controls`、`supports prewarming`、`spkr`、`speakerID`、`headset info`。
    - iOS 16 的 `AudioGetHostClockFrequency` = caulk timebase_info（mach_timebase_info 的 denom/numer × 1e9），不像 sheng 的 iPadOS 17 写死 24 MHz；内核 timebase 对就不用改 CoreAudio。
    - 我们的 iOS 里跑的是 mediaserverd（没见 audiomxd），占一个核约 23%。
    - 方案（等用户点头）：①最小设备：苹果的 IOAudio2Device 实例 + 我们改写的虚函数（A11 没有 PAC，复制虚表换槽），一条立体声 48 kHz 输出流，注册表描述照上面的键；看 mediaserverd 找到、打开、映射状态页和流缓冲，每次调用都记日志，不出声。②开停 IO：开流时起喇叭图和功放，环形缓冲放在 DSP 能读的内存里，DSP 交回缓冲的时刻写状态页的零点时间；目标：系统提示音出声。③音量键/音量控件、静音、作为机内喇叭的路由，没声音时功放下电。④以后：麦克风、听筒模式（通话时 GPIO174 拉低）、耳机。
  - **第 1 步（用户：「可以，开始第1步」）**：0281（k0281）自己写的 IOAudio2Device / IOAudio2DeviceUserClient（wsl/PandoraAudio.cpp → iokit/Kernel/arm/PandoraAudio.cpp，KernelConfigTables 挂 IOResources，开关 pandora_audio=1；苹果的 IOAudio2Family 没预链接，类名空着）：注册表描述照上面的键，13 个方法、陷阱、状态页（类型 0）、IO 缓冲（类型 1）都记日志，开流后用自己的时钟按环形缓冲绕圈写零点时间，每秒记缓冲峰值。
    - aud1（k0281）：mediaserverd 的 HAL 把它激活成 33 号设备（48 kHz 2 声道），但没人打开它。VirtualAudio（/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin，不在 /System 下）没建路由：「vadInitializationCompleteSemaphore: Timeout」「No Audio Device Available」，默认虚拟设备输出格式 0 个，插电提示音 -50。iOS 的统一日志读法：拷 iosroot.img → WSL insmod /opt/work/wslk/fs/hfsplus/hfsplus.ko、只读挂载 → unifiedlog_iterator（会话 scratchpad ulparse.sh，只取最近 90 分钟的 trace 文件；全取有 40 GB）。
    - VirtualAudio 按 MGGetProductType 选内置的换能器数据库，再按 UID 找物理设备：`Codec`（CS42L75：听筒、耳机、麦克风）、`Speaker`（CS35L26）、`Voice`、`Reference`、`Baseband Voice`、`DSP Debug1/2`（对应设备树的 audio-data 节点）。D22 的调音文件在 /Library/Audio/Tunings/D22（VAD/d22_speaker_*.austrip 等）。0282（k0282）：设备 UID 改成 `Speaker`。
    - aud2（k0282）：HAL 以 `Speaker` 激活成 33 号设备，VirtualAudio 照样不建路由。aud3（k0282 + tools/logging/com.apple.coreaudio.plist 装到 /Library/Preferences/Logging/Subsystems，VirtualAudio 的 34 个 va_* 分类 Debug 存盘；gpu_test.sh 新参数 --log-cfg）：整次开机 VirtualAudio 一条日志都没有。0283（k0283）：启动参数 pandora_audio_tt=1 把设备标成雷电（sheng 的做法）；aud4：照样没人打开设备、虚拟设备 ID 0。
    - CoreAudio 的 HALS_PlugInManager::LoadPlugIns 只收 `.driver`（AudioServerPlugIn），三个目录 /Library/Audio/Plug-Ins/HAL、/System/Library/Audio/Plug-Ins/HAL、/Library/Apple/Audio/Plug-Ins/HAL；VirtualAudio 是老式 AudioHardwarePlugIn（类型 F8BB1C28…，工厂 New_VirtualAudio_PlugIn）的 `.plugin`，aud 日志里只有 9 次加载 = /System 那 9 个。怀疑 VirtualAudio 没被加载。0284（k0284）：启动参数 pandora_cslog=NAME 在 ubc_cs_blob_add 记该进程挂签名的每个文件（共享缓存外的库/插件）。测试 aud5。
    - **aud5（k0284，pandora_cslog=mediaserverd）：VirtualAudio 没被加载。** mediaserverd 只挂了 8 个 `.driver` 的签名（AirPlayHalogen、AppleTimeSyncAudioClock、AppleUSBAudio、BTAudioHALPlugin、BasebandVoice、CarPlayHalogen、NetworkUplinkClock、OctaviaHalogen；AppleAOPAudioPlugin 推迟）。共享缓存里没有老式插件类型 UUID（字符串和字节都没有）、没有 New_VirtualAudio_PlugIn、没有 VirtualAudio 二进制路径（MediaExperience 只用插件路径读本地化文字）；系统里没有 audiomxd。**用户：「先暂停吧」（2026-10-29）**——停在“VirtualAudio 在 iOS 16 上由谁、怎么加载”这个问题上。下次从这里接：查 /Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin 在真机 iOS 16 上的加载者（mediaserverd 的启动参数/环境、AudioToolboxServers、HALS_System::Activate 里的 IOUserAudio/注册器路径），或者 iOS 16 的虚拟设备是否改由别的组件提供。
    - **继续（「继续」）**：0285 pandora_pathlog 记 open/access/stat；aud6：HALS_System::StartServer 打开 /Library/Audio/Plug-Ins/HAL（那里只收 `.plugin`，老式插件类型 UUID 是指令里的立即数，所以按字节搜不到）却是空的——**手机上 VirtualAudio 被 ios-sweet 的镜像 v12 挪进了 HAL.disabled**（sweet-root-tweaks.py audio()，当时没有音频设备 mediaserverd 崩）。用户：「我去我真受不了了，竟然是sweet的问题？？？？」。手机 iOS 分区和原始镜像全量比对（tools/iosroot_diff.py）：sweet 只改了 launchd（删 CommCenter 两个任务、7 个 _PanicOnCrash 改名）、MediaToolbox 三份 SubServerConfiguration（StartRecorderServer 关，以后做相机/录音要恢复）、HAL.disabled 和 /private/var 的几个偏好；其余是我们装的。
    - 0286 安装器支持 "> FROM TO" 改名（gpu_test.sh --rename）；aud7：VirtualAudio 挪回后加载、打开我们的设备，但取独占失败（setProperties 未实现 → 0xE00002C7 "Unable to take hog mode"）。0287 setProperties 照收（"exclusive access owner" = mediaserverd pid）。aud8：**VirtualAudio 接受设备，建出扬声器端口（pspk "Speaker"，界面名「扬声器」）**、遥测和回采端口；默认虚拟设备 vdef 的路由却是 pap → pnul（Null_Device），系统提示音进空设备。aud9（设备树加 /product/audio，日志补 `va_rtmgr ` 等补空格的分类）：「Culling route connection because ringer switch is off」；用户：音量条拖不动。aud10/aud11（0288 pandora_audio_delay 推迟登记，日志时间比开机计时晚约 13.7 s）：无论早晚，SpringBoard 开机都 `setRingerMuted: true`（偏好 SBRingerMuted 是 false），打字时打开控制中心拖音量会卡死。
    - 原因：backboardd BKButtonHIDEventProcessor 初始化时向 HID 系统要电话页 0x0B 用法 0x2E 的键盘事件（响铃/静音开关）当前状态，down 0 = "silent"、1 = "tone1"；我们（和 sweet/fuxi）的按键驱动从不回答，iOS 就当静音。0289：SweetHIDButtons::copyEvent 回答“响铃”（启动参数 sweetringer=0 为静音）。测试 aud12（不再需要 pandora_audio_delay）。卡死的原因待查。
    - aud12（k0289）：开关读成 "tone1"，不卡了，但系统提示音照样进 Null_Device、音量条照样拖不动（用户：「和上一轮一样」「只是不卡」）。VirtualAudio 只在端口增减时做完整的路由选择；开关变化时只检查当前路由（「Ringer switch update.」→「Route did not change」）。扬声器在开关设好之前就出现了，第一次选路由时被当成静音剔掉，之后不再重选。用户：「再想想呢」→ 不拍固定延时，按事件来：0290（k0290）：backboardd 读开关时（SweetHIDButtons::copyEvent）通知音频设备，3 秒后再登记设备；没人读开关就在开机 120 s 登记；pandora_audio_delay=秒 仍是固定时间，pandora_audio_now=1 立即登记。测试 aud13。
    - aud13（k0290）：设备按设计在 backboardd 读开关（内核日志 21.03 s）后 3.1 s 登记（24.12 s），mediaserverd 立刻打开、取独占；但 VirtualAudio 照样剔掉扬声器（ringer off），系统提示音 1106 在 Null_Device 上开始播放，我们的设备没收到开流。用户：拖音量条时整个控制中心被拖动，音量条本身没反应。**原因：iOS 统一日志的时间间隔被压缩了约 52 倍**（解析时把 19.2 MHz 计数当成纳秒；用内核里 3.10 s 对应日志里 0.0595 s 核出来的），之前算的“backboardd 读开关后约 0.2 s SpringBoard 就设好开关”实际约 11 s（SpringBoard setRingerMuted / mediaserverd「Ringer switch state: on」≈ 内核日志 31.9 s），扬声器还是早了约 8 s。另外：开关变“响铃”后，铃声类别 crng 查路由仍一直失败（「failed with result 1」，属性错误 'nort'），说明路由结果只在端口增减时算、之后沿用。以后看统一日志的时间差要乘 52。
    - aud14（k0290 + pandora_audio_delay=60，确认实验）：开关先变“响铃”（日志 0.41），扬声器后出现（0.689），没有任何因静音剔除的记录，默认路由照样留在空设备；用户：「不行，重新深度think」。**开关不是根因。** 用户要求深想后查到（Ghidra 反编译 VirtualAudio，工程在仓库外 D:\DeepSeekHarness\vaghidra，run.sh + scripts/DecompAt.java）：
      - 端口增减时 VirtualAudio 只检查当前路由还能不能用（RoutingManager「Attempting to find a valid replacement route」→ 空设备仍有效 →「Redundant concrete route(s)」），不会因为多了扬声器就换。aud12（设备开机就在）也一样：初始化时只先看到空设备并定下默认路由，扬声器随后才作为新端口加入。
      - 按类别查路由（FUN_00024bf0 → FUN_0008132c → 候选遍历 FUN_00056a70/FUN_000b51ec）：每个类别在按机型编进程序的路由数据库里有一串候选，每条候选是若干“来源端口类型 → 目的端口类型”连接，候选里的端口全在才可用。扬声器在场、开关响铃时铃声类别 crng 仍「failed with result 1」且没有剔除记录 → iPhone X 的扬声器候选还要别的端口。推测缺：听筒（iPhone X 的扬声器是底部喇叭 + 听筒的立体声，听筒属于 "Codec" 设备）或扬声器保护的遥测输入。未核实。
      - 选库入口 FUN_00229778（按内部机型编号 switch），机型编号来自 MGGetProductType（FUN_0005026c 映射）；MobileGestalt 的机型表以 20 字节 SHA-1 为键（libMobileGestalt 0x19c5a9428，0x174 项），还没对出 iPhone X 是哪项。
      - 扬声器出现后，所有类别查路由一次都没成功过（只要输出的铃声 crng 也失败），说明是“把抽象路由落到具体设备”这一步（FUN_001dd878 → FUN_001dddf8 → FUN_001d8bc0）不认我们的设备；VirtualAudio 建 Speaker 时缺数据源/遥测都不报错（FUN_0037093c 有就设、没有就跳过）。真 iPhone X（设备树 + 公开的 iPhone10,1/10,4 IORegistry 树）：Codec、Speaker、Mikeybus Secondary、Loopback、Baseband Voice 等多个 IOAudio2Device；AppleEmbeddedAudio 的 Speaker 带遥测输入（每个喇叭的电压/电流/供电/升压等声道）、speakerID、扬声器保护、Top/Bottom Speaker 数据源。照真机补齐要先读 AppleEmbeddedAudio。
      - 用户：「实在不行你暴力一些（但不要无脑」→ 先测已被 sheng 证明能出声的路：0291（k0291）pandora_audio_tt=1 时设备 UID 改成 "PandoraSpeaker"、传输类型雷电，VirtualAudio 当外接设备建端口（Device_Thunderbolt_Aspen）。0283 测雷电时 VirtualAudio 还没加载，等于没测过。测试 aud15。同时读 AppleEmbeddedAudio，准备照真机补齐的正式做法。
    - aud15（k0291 + pandora_audio_tt=1）：VirtualAudio 建了雷电端口 ptbo，但「Port update completed without a route change (there is no supported route in the current category for the updated port(s).)」——iPhone X 的路由表没有雷电端口，sheng 的办法在 iPhone 上不成立。用户：音量条拖不动、又卡死。日志：MediaRemote 报当前输出「volume capabilities = MRVolumeControlCapabilitiesNone」（音量条不可用的直接原因）；SpringBoard 主线程两次停约 10 s 和 15 s（HangTracer「Hang detected: 10.27s / 15.29s」，aud12 也有一次 10.25 s），停在控制中心听力/音频模块查当前路由的地方（MRNowPlayingAudioFormatController → HUComfortSoundsSettings；CCUIAppLauncherViewController → HUUtilities liveListenRouteSelected），mediaserverd 那段时间没被堵住；推测是等路由描述超时，未坐实。三个现象同根：没有可用路由。
    - 用户：「是不是又是sweet给音频改了其他地方」→ 核对 sweet-root-tweaks.py：音频只有 VirtualAudio 挪走（已挪回）和 StartRecorderServer 关；其余是 launchd/向导/激活/迁移/SpringBoard 看门狗，不碰路由。也排除了机型识别：MobileGestalt 用 IODeviceTree:/product 的 product-id（20 字节）查表，iPhone X = 第 36 项、MGGetProductType 0x669eebc5，VirtualAudio 映射成内部编号 16；我们的设备树原样拷了真 D22 的 product-id，系统也报 iPhone10,3。路由数据库在代码里建（不是 plist），编号 16 的分支 FUN_000eced8 只是设备设置。
    - 用户：「再机智的暴力一些吧，我受不了了」→ 改打法：①用 VirtualAudio 自己的详细日志代替逐层反编译：每个 va_* 日志器的级别读偏好域 com.apple.coreaudio 里的 trace* 键（traceroutingmanager、tracerouting、tracedevice、traceport 等，值 error…spew 或数字；FUN_002ecfdc/FUN_002ec664），“Attempting to build concrete route …”这类行要高于默认级别才打。tools/logging/coreaudio_trace.plist 把路由/设备/端口相关的设成 spew，装到 /Library/Preferences/com.apple.coreaudio.plist（gpu_test.sh 新参数 --put "本地 权限 目标"）；日志配置补上漏掉的 va_anc/va_io/va_smprt/va_telsr。②判断成败只看日志（开机插电提示音 1106 会自动走一遍路由），请用户一次性允许连续自动测、不用他操作，成了再叫他。测试 aud16：k0291 不带 tt（内置 "Speaker"）。
    - 用户：「可以，你自己连续测吧」（音频路由这一串测试不用每轮问，记忆 audio-autotest-autonomy）。
    - aud16：tracedevicemanager 等设成 spew 后，设备管理器打印设备细节时查询连空设备都不支持的属性（ExceptionBarrier 抓到 'who?'），设备列表建不起来，VirtualAudio 初始化失败（「VirtualAudio PlugIn is not initialized yet」）。但路由管理器写出了失败原因的格式：「Attempt to build concrete route(s) failed: Connection [ pap / pspk ] is required and cannot be used for a concrete route: source is connected: 1; destination is connected: 0」。
    - aud17：设备相关日志降到 details，只留路由/端口/音量在 spew。扬声器出现时（读开关后 3 s）开关仍是静音：「Culling route connection because ringer switch is off … pap → pspk」；开关变响铃时只打「Ringer switch update.」→「Route did not change」，不重算（反编译 FUN_00293518：只在当前类别的路由表条目带“受开关影响”标志时重算，csav 没有；而扬声器端口本身带受开关影响的标记）。
    - aud18（扬声器 60 s 才出现，开关已响铃）：**「Successfully Built concrete route { pap → pspk 扬声器 }」**，随后默认设备要建 8 个附属虚拟设备的路由，其中 Hawking（vhaw，类别 chaw）是 Mandatory：「Connection [ phki / pap ] is required … source is connected: 0」→「Recursion failed after skipping optional alternate VAD」→ 退回空设备。VirtualAudio 的 Device_Hawking_Aspen 从 UID "Hawking" 的设备建 'phki'（「Hawking Input」）和输入音量控件（FUN_0042f3f8）；真 iPhone X 有 AppleSecondaryAudio "Hawking"（/arm-io/mca4/mca4b/audio-codec-hawking）。
    - 0292（k0292）：IOResources 上第二个 IOAudio2Device 实例（personality PandoraAudioRole = Hawking），UID "Hawking"，一条输入流（静音），开机就登记；日志带设备名（PANDORA AUDIO Speaker/Hawking）。测试 aud19：扬声器仍 60 s 出现，先验证整条路由；通了再把“扬声器在响铃之后出现”换成事件触发。
    - **aud19（k0292，扬声器 60 s）：路由通了。**「Attempting to activate with concrete route(s): { vdef: pap → pspk 扬声器 Speaker }, { vhaw: phki → pap }」「Created aggregate device ID 84 with master PhysicalDevice ID 76 (uid "Speaker")」「Successfully activated new concrete route」「Route change succeeded」；扬声器端口有了音量范围「VolumeProperties for Port: pspk is [ Min: -58; Max: 0 ]」（之前空设备是 [ ]）。内核侧可见：路由切过来那一刻两个设备都收到 method 6 set stream active (1, 1)。用电脑提示音告诉了用户。
    - 0293（k0293）：不再固定等待。读开关后 3 s 登记扬声器，5 s 内没收到 set stream active(…, 1) 就“拔掉再插上”（新实例登记、旧实例 terminate），最多 12 次；用户端持有设备引用（旧实例被换掉后不悬空）。测试 aud20。
    - **用户：「先暂停」（2026-10-29，aud20 正在手机上跑）。** 下次从这里接：①看 aud20：内核日志 "PANDORA AUDIO Speaker: registered … attempt N" / "routed at"，iOS 日志 ioslog_fetch.py aud20（之后删 /opt/work/la_aud20、ul_aud20.jsonl，留 _media）；②路由稳定后做第 2 步：pandora_adsp.c 的 spk_tone_pump 从正弦波改成从 IOAudio2Device 环形缓冲按 IO 位置取样，零点时间戳改由 DSP 写完回执推进；③请用户实测音量条和打字声（StartIO、环形缓冲峰值）。
    - 用户：「继续」。aud20（k0293）：扬声器第 1 次登记（读开关后 3 s）就被路由（0.1 s 后 set stream active 1），iOS 日志：扬声器出现时开关**仍是静音**，默认路由照样切到 pap → pspk（「Route change succeeded」）；那些「Culling … ringer switch is off」是铃声类别查路由。**之前一直卡住的真正原因只有缺 Hawking**，开关时序的推测不对（0293 的重插用不上，留着无害）。插电提示音 1106 走到了我们的扬声器（「Server_Internal_StartIOProcID: 88 Speaker (VAD [vdef] AggDev 2)」），但 HAL 只发 method 0（engine start）、等 5 s 没有零点时间戳就放弃（「the IO thread failed to start, Error: 'nope'」）再发 method 1。0294（k0294）：method 0/10 启动时钟，1/11 停止。测试 aud21。
    - **aud21（k0294）：系统声音写进了我们的环形缓冲。** 32.659 s method 0 → IO started，33.662 s「ring peak 0x7fdcac, 32768 of 32768 samples nonzero」（插电提示音 1106），35.765 s method 1 停；之后 65.9 s、83.9 s 又各开一次，84–93 s 多次非零（峰值到 0x5247150）。第 1 步（iOS 把系统声音送进我们的设备）完成，电脑提示音通知了用户。下一步第 2 步：环形缓冲 → DSP 喇叭图 → 功放出声。
    - 用户：「可以，开始第2步」。0295（k0295）：启动参数 pandora_spk_live=1（与 pandora_spk=3 pandora_amp=3 一起；之前的 aud 测试没带 pandora_spk，喇叭图没起）：喇叭图不再 26 s 就停、不放测试音，每个 20 ms 缓冲由 PandoraAudio.cpp 的 pandora_audio_out_fill 填（扬声器设备环形缓冲，读位置跟在“现在”的采样时间后 960 帧，连续往后读，偏出就重新对齐并计数；IO 停时静音；每次开 IO 清空缓冲）；功放 PLAY 后不暂停不下电，头一分钟每 10 s 读一次。闲时下电留到第 3 步。测试 aud22（自动，先看机制），通了再叫用户来听。
    - aud22（k0295）：不崩；喇叭图常开，两个功放 PLAY 后一直播；插电提示音 IO 32.3–35.4 s。DSP 每还回一个缓冲打一行「GPR in」（每秒 50 行）冲掉了内核日志，环形缓冲统计和功放读数没留下。0296（k0296）：「GPR in」只打前 64 行，常开时每 10 s 汇总一次缓冲数。测试 aud23。
    - **用户：「关机有声音了」「不，就是正常手机息屏关闭屏幕的咔咔声」——iOS 的锁屏音从喇叭放出来了（aud22 结束、手机重启前锁屏那一下）。第 2 步通了。** 请用户在 aud23 里试打字声、音量条、锁屏音。
    - aud23（k0296）：DSP 连续取数据（92 s 时送出 3508、收回 3504、无错误，每秒 50 个）；插电提示音 32.7–35.8 s 时下功放 V max 0xad、I max 0x73（其余读数为 0）；读位置每秒前进约 48000 帧，开播时重新对齐共 3 次。**用户：「充电提示音也有，可能是没开打字的声音，音量条正常，没有杂音」——第 2 步完成。**
    - 第 3 步待用户点头：①没声音时功放下电（现在常开），关机/重启前先下电；②音量键、静音；③收尾：0293 的重插和 pandora_audio_delay/now 可去掉或留作开关，coreaudio_trace.plist 只在调试时装；④「GLINK version agreed」行太密。第 4 步：麦克风、听筒（通话时 GPIO174 拉低）、耳机；做录音/相机时恢复 StartRecorderServer。
    - 用户：「闲时给功放下电：安卓和iOS是怎么做的，如果也是这样就也这么做，实体按键我用过了是正常的」「能不能导入这个视频或者这个视频的音频我听一下」「能不能编一个简单的app进去播放这个」。做法：视频音轨原样抽成 out-root/media/audplay.m4a（22.7 s，531 KB，不进仓库），装到 /private/var/mobile/Media/audplay.m4a；tools/player/PandoraPlayer.m（一个按钮的 UIKit App，AVAudioPlayer，播放类别）装进 /Applications/Camera.app/PandoraPlayer，启动参数 sweetexecswap=Camera:/Applications/Camera.app/PandoraPlayer 让相机图标打开它（相机在本机本来不可用；替换只在带这个参数的那次开机生效，相机程序本身不改）。**用户：「别忘了之后换回去」→ 测完删掉 Camera.app/PandoraPlayer 和 Media/audplay.m4a（安装器要加删除指令），之后的测试不再带这个参数。**
    - aud24：播放器启动、打开文件、播放，音乐写进了扬声器设备缓冲区，但**整轮没有声音**：开机参数超过 607 个字符（loader BOOT_LINE_LENGTH 608），末尾的 pandora_rtc_offset 和 pandora_slot=b 被截掉，内核没读固件分区、ADSP 没起，DSP 一次没取数据。tools/boot_ios.sh 现在检查参数行末尾是否完整，不完整就不开机。aud25：播放器装成 Camera.app/PP、去掉 pandora_bigfirst，约 593 个字符。要删的文件：Camera.app/PandoraPlayer、Camera.app/PP、Media/audplay.m4a。
    - 新工具 tools/ioslog_fetch.py NAME [--minutes N] [--parse]：用 pinfile rawreadat 只读 iOS 镜像的 HFS+ 卷头、目录表和日志文件，uuidtext 缓存在 /opt/work/ul_cache（第一次 520 MiB / 214 s，之后只读新文件），不再拷 17 GB。用户：「日志太大了能截取吗」。
  - 用户（2026-10-28）：「能不能给垃圾删了」→ 删了 out-root 的三个 17 GB 镜像（ios-root.img 和 WSL 里那份 SHA-256 相同才删）、127 个旧测试包、WSL 里的日志解析结果（ul_*.jsonl / la_*，version.plist 存到 /opt/work/tools/ul_version.plist）；WSL 虚拟磁盘没压缩（用户：「算了，继续任务」）。
  - 用户（2026-10-28）：「如果有什么大事的时候你可以用电脑播放一次急促的音频给我」→ D:\DeepSeekHarness\claude\alert\alert.ps1（记忆 pc-alert-sound）。
  - 原计划（已在做）：①第一次放音前照安卓写校准：清 0x10110 bit 31、CAL_R_SEL（这台手机的校准值，从安卓读，不进仓库、不打印）、ALLOW_ACT_BOOT=1；②DSP 跑起来后叠 music 场景（cs35l43_main_amp_event 的 delta）；③往喇叭播放图的共享内存送一段小音量的正弦波（pandora_adsp.c 现在只送静音），真正出声；④之后接 iOS 的音频设备。
- 用户（2026-10-28）：「本地的几个东西你先看看，感觉unixnu的可能比较大」→ UniXNU 主仓库（unixnu_review/src，origin/main 40d21cc）自 dec4c7b 起 277 个提交，116 个是文档/设备目录。GPU 拆成单独仓库：NativeMetal → AdrenoMetal（我们插件基于的 c6d29ca 之后 24 个提交）、NativeAdreno → Adreno740（29 个）。已克隆到 unixnu_review/AdrenoMetal、Adreno740。和我们相关的：477e530（签名文件 1 MiB 一块读、dyld 缓存不进页缓存，我们开了同一个 hash cache）、8b6a0f5（work interval 操作 6/7/8，音频服务用）、1640f6f/AdrenoMetal（两平面 IOSurface 纹理，对应暂停的 RGB8_444_2P）、系统内存渲染和关并发 binning、着色器预热整套、Adreno740 的提交路径优化。列给用户选。
- **用户插入（2026-10-28）：** 「记得今天同步一下上游」→ 今天内做：拉 UniXNU-sheng（本地 ../unixnu_review/UniXNU-sheng，上次 d76556b 2026-10-25；wsl/qcom/SOURCE.txt 记了我们拷的提交）、ios-sweet（425b251）、ios-fuxi（ba239fc）的新提交，列出和我们相关的改动，拷过来的库照新提交更新，再问用户要不要合。不打断扬声器 spk11。
- 0267（k0267）：pa_halo 加读越界地址（HALO_MPU_XM/YM/PM_VIO_ADDR 0x2BC3100/08/10）和 AHB 主口错误（0x2B82040/44），加载完未起核时也记一次；去掉不起作用的强制开 PLL；TDM 那一步改成 0x40 在有位时钟时从头重新加载（停核、写固件/系数/场景并核对、MPU、起核），0x42 不动作对照；喇叭图跑 16 s。测试 spk10 参数同 spk9。
