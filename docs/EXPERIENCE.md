# 经验汇总

写于 2026-10-30。这份是做这个移植一路攒下的具体经验，按部分分开，写给接手的人。现状和下一步看 [HANDOFF.md](../HANDOFF.md)。

每条后面括号里是出处：PLAN.md 里的测试名（如 post23、spk13、aud19）、补丁编号（xnu/patches/NNNN-*），或别的文档。细节去那里查。

## 1. 启动和内核

- 引导程序把 `fastboot boot` 的程序放在 0xC7800000，CPU 在 EL1，设备树在 0xD5C80000。屏幕缓冲在 0xFC800000，1220×2656，每行 4880 字节，ARGB8888，自动刷新开着，直接写就会显示。（PLAN"第 1 步结果"）
- Oryon 核心不支持 16K 页（MMFR0 TGran16 = 0）。内核用 4K 页：xnu-8796 以 BCM2837 的 4K 配置为底，加 UniXNU 的 3 个 4K 补丁。（PLAN"第 1 步结果"，0103-0105）
- 苹果的共享缓存按 16K 页签名。4K 内核要先按 16K 核对原签名，再算 4K 的校验值（0124）。代价是每次开机要把约 3.3 GB 的共享缓存读一遍，换成 ARMv8 SHA 指令后约 9 秒（0211）。
- 两个超大核的 MPIDR 是 0x10000 和 0x10100。唤醒核、查核号、找 GIC 重分发器三处都要保留第三段 Aff2，否则只能起 6 个核（0113）。
- 内存给到 5 GB 时，macOS 的算法给内存压缩器留 40 GB 地址空间，内核地址空间不够，要改用 iOS 的算法（0112）。macOS 的地址布局在 4K 启动页表下够不着内核，要用 iOS 的布局（0107）。
- 第一次开机时 lsd 重建 LaunchServices 数据库，停在苹果标志 25 分钟以上，这是正常的（PLAN"第 6 步"）。
- IORTC 由平台驱动发布以后，开机省掉 IOKitInitializeTime 的 30 秒等待（0210）。
- AMFI、AppleImage4 等 11 个原版驱动从 iPhone X 的内核缓存里取出来，预链接进我们的内核（`tools/mkprelink.py`，0148-0155）。AMFI 会等 ApplePMGRNub 30 秒，等不到就 panic，登记一个空的 ApplePMGRNub 就够。我们用 macOS SDK 编的内核会给类的虚表加填充，原版驱动就对不上了，PANDORA 配置下要关掉填充（0150）。
- 内核配置是从 macOS 抄来的，原来没有 jetsam。没有它，iOS 的进程都不受内存管理，`os_proc_available_memory` 恒为 0，后台进程也没人挂起。0315 加上 CONFIG_JETSAM 后好了（post8、post9）。iOS 的其他配置项（vps_dynamic_prio、freeze、secluded 内存等）还没加。
- 8 个核原来按一组算，调度器眼里都一样，空闲核从小号往大号取，超大核排最后。0320 改成两组核并打开 `__ARM_AMP__`。macOS 目标的内核这时用 Edge 调度器，它默认让线程组偏好开机所在的那组核，也不许从小核组迁到大核组。所以还要在线程组创建时推荐大核组（P-spill），不然超大核没人用（post14、post15）。

## 2. 存储和 iOS 根文件系统

- 引导程序交出来时，UFS 的连接停在 HIBERN8，自动休眠关着。先发 DME_HIBERNATE_EXIT（0118），再对 WLUN 0xD0 发 START STOP UNIT 切到 ACTIVE（0119）。SMMU 已经给存储（流 0x60）留了直通上下文。（PLAN"第 4 步进展"）
- 不清数据的做法：/data 是 default-key 加密映射，一比一叠在 sda34 上。在 userdata 里建一个钉住的文件（f2fs pin），直接写这个文件在 sda34 上的块，iOS 读到的就是明文。安卓内核允许写正在挂载的分区（CONFIG_BLK_DEV_WRITE_MOUNTED=y）。工具是 `tools/pinfile`。（PLAN"免清数据测试"）
- 在安卓里按文件读这个镜像会经过加密，读出来是乱码，要按块读（`pinfile rawread`）。（PLAN"开机画面"一节的日志读法）
- 安卓有时会在两次开机之间调换 sda 和 sdb 的编号，访问分区时一律用 `/dev/block/by-name/`。
- 16 GiB 的钉住文件有 4460 段。内核的位置表上限是 16384 段，用二分查找（0122）。
- HFS+ 的日志区要在第一次可写挂载时初始化，只读挂载会失败。第一次开机要带 `ufsrw=1 rootrw=1`（PLAN"第 6 步"）。
- 根文件系统镜像用 ios-fuxi 的 mkiosroot.sh 做。它带的 sweet-root-tweaks.py 删了 CommCenter 的启动项、把 VirtualAudio 挪进了 HAL.disabled，这两处后来都成了问题（post13、aud6）。
- 内核里的文件安装器（pandora_files.c）在根卷挂上约 2 秒后装文件，装进去的东西会一直留在 iOS 分区。launchd.plist 这种开机最早读的文件，当次开机装进去不生效。files.txt 的行尾不能是 CRLF（0161）。
- 在安卓侧原地改镜像里的文件时，长度也要跟着改。WWDC 壁纸的 main.caml 就是改了内容没改长度，尾巴上留着旧字节，CAML 解析失败（post29）。那个目录里还有重复的目录记录（post30）。

## 3. 屏幕、触摸、按键、亮度

- TLMM 的真实基址是 0x0f100000，用设备树里写的 0x0f000000 会卡死（device/DT_FINDINGS.md）。
- 电源键和音量下在 PMK8850 的 PON 上，音量上是 gpio-keys（DT_FINDINGS）。
- 主屏触摸是 Synaptics S3910，TouchComm v1，走 GPI DMA（DT_FINDINGS）。ios-fuxi 的 FTS 触摸驱动在这台手机上对应的是背屏。
- 亮度：AppleARMBacklight 收 iOS 给的亮度，用 DSI 命令 0x51 设面板，读 0x52 确认（0139-0141）。
- iOS 要以为屏幕是 iPhone X 的 1125×2436，否则 backboardd 一直去找缩放硬件 AppleM2ScalerCSCDriver（0125、0126）。
- 显示直接读 GPU 画的图（0217）：显示流的 SMMU 上下文打开翻译、用我们自己的页表，每次交换只换取图地址。CPU 拷屏从每帧 2.4 ms 降到约 0.09 ms。
- SwapWait（IOMobileFramebuffer 的选择子 6）要照原版内核的 swap_wait_gated 回答。一律答"已经不在屏幕上"的话，CA 会往正在显示的缓冲里画，滑动时闪屏（0218）。
- A11 上 CA 不用硬件垂直同步：CADeviceUseVBL 按 ChipID 判断，只有 A8 到 A10 用，帧节奏由 CA 自己的定时器决定。要 120 帧，就在 AppleCLCD 上照真机格式发布 `IOMFBDisplayRefresh`（0220）。
- 开机后屏幕不自己亮：backboardd 启动时等蓝牙（RemoteHID）和环境光传感器，各等约 5 秒超时，SpringBoard 的空闲计时器先到期把屏熄了（PLAN"开机画面"）。
- backboardd 开机时会问 HID 的响铃/静音开关（电话页 0x0B，用法 0x2E）。按键驱动不回答，iOS 就当成静音（0289）。

## 4. GPU 内核驱动

- Adreno 840 的上电顺序照 Linux 的 a6xx/a8xx 走：CX GDSC，SMMU 时钟，GPLL0 投票，安全世界调用 GPU_INIT_REGS，GMU 的 SMMU 上下文，GMU 冷启动（a6xx_gmu_fw_start 里的 a8xx 分支），HFI，性能表，带宽表，GX 上电。GX 上电要先发 GX_BW_PERF_VOTE，再发 OOB GPU_SET，顺序反了没有回应。之后是 GPU 的 SMMU 上下文、SQE、CP 初始化，zap 固件按 PAS 13 加载（docs/GPU_PLAN.md，0162-0175；GX 的顺序在 0169、0170）。
- 并行分箱要关，CP_THREAD_CONTROL 用 0x08000000，和 Linux 一样，开着会让 BR 和 BV 互相等（0172）。插件默认给 Turnip `TU_DEBUG=nocb`。
- 1 GiB 的 GPU 内存池在 launchd 之前预留（0176），晚了分不出大块连续内存。
- 每个进程一个 4 GiB 的 GPU 地址窗口（SMMU 输入 36 位，0219）。以前除第一个进程外只有 178 MiB，backboardd 报了 3439 次 OutOfMemory，开机标志一闪一闪。现在最多 15 个进程（0332）。
- GPU 的 SMMU 上下文打开 HUPCF 以后（0316），GPU 读到非法地址时得到 0、继续往下走，不再整个挂死。
- 映射时不刷 TLB，取消映射时按页 TLBIVAL，照 Linux arm-smmu（0224）。以前每次映射都把整组寄存器读两遍、再清整个 TLB，开机时占 backboardd 内核时间的 29%。
- 安全世界调用要排队一个一个来（0227）。ADSP 和 GPU 同时调用时，固件让其中一个排队，没人让它继续，之后整次开机所有调用都返回 2（adsp1）。
- 等 GPU 完成用中断（SPI 300，0216）。Turnip 的 darwin 后端原来每次只向内核要 1 ms，改成每次给剩下的时间、最多 100 ms（tools/turnip/pandora_wait.py）。
- 一次提交最多 128 个 IB（0194，tools/turnip/abi_128.py）。原来 Turnip 按 16 个拆开，每段都带线程同步，BR 和 BV 会卡死。
- GPU 调频（0198）：16 ms 一个窗口，目标忙 50%，升得快、降得慢。安卓在这颗 GPU 上用的是 GMU 固件自己调频，我们没试过。
- 10-30 找到的卡死根因：Turnip 的 darwin_bo_finish 先把 BO 交给延迟释放（fence 已过就当场 BoFree），解锁以后才把 tu_bo 清零。tu_bo 按内核给的位置号存放，内核又总从最小的空位分配，刚空出的位置马上分给下一个新 BO，新 BO 的结构就被这边清零了，之后取到的地址全是 0 加偏移，GPU 执行到地址 0 或者别人的数据。修法是在 retire 锁里、交给延迟释放之前清零（tools/turnip/pandora_bo_finish_order.py），内核的位置分配改成从上次的位置往后找（0331）。（PLAN"根因"一条、post28）
- 在 A8xx 上 Linux 不用 RBBM 的软复位，GPU 卡死后要停 SQE、GMU 断电、CX 断电再整套上电。我们的驱动还不能这样重来一遍（post21 一条）。
- 内核的内联汇编里 `;` 是注释，一条 `__asm__` 只写一条指令。
- 内核里映射寄存器窗口用 `ml_io_map_unmappable(pa, len, VM_WIMG_IO)`，RAM 用 `VM_WIMG_WCOMB`。`ml_io_map` 的映射放在 8 MiB 的小区域里，不能取消，大一点的或要取消的映射都会 panic（r1b、r2c、usb3）。

## 5. Metal 插件和 metal2vulkan

- 插件来自 UniXNU 的 NativeMetal（现在叫 AdrenoMetal），`tools/metal/adapt_ios16.py` 把它改给 iOS 16 用。_MTLDevice 在 iOS 16.7 是 528 字节，iPadOS 17.7 是 536。`NativeAdrenoMetal.clients` 写给哪些进程设备，`*` 是全部。
- 一个图层整个不见时，先查有没有被拒的纹理和表面（插件的 refuseSurface 计数），再查着色器。App 资源库的背景就是这样：CA 用两平面扩展范围表面 'b3a8'，我们内核的 IOSurface 对它报每行 0 字节，插件拒绝建纹理（10-25，34e5fe8）。
- supportsExtendedXR10Formats 要分进程回答。backboardd 必须答不支持，否则它会选 64 位的帧缓冲，显示链显示不了。其他进程要答支持，否则 CAMetalLayer 拒绝 BGR10_XR_sRGB 的图层（post1、post2）。
- IOSurface 的使用计数只在 backboardd 里记。客户端给自己 drawable 的纹理计数，会让 CAMetalLayer 拿不到下一张（post2）。
- metal2vulkan 的内存池第一块就是 64 MiB。海报扩展的内存上限 200 MB，它在 GPU 上映射了 209-252 MiB，系统算出来的可用内存是 0，ClockKit 就每帧扔掉贴图重建（post6）。m2v 0011 让池块从 4 MiB 起倍增。
- metal2vulkan 的 OwnedSets 每条命令建一个描述符池、用完就销毁。在 Turnip 上每个池是一块 GPU 缓冲，内核释放缓冲又要等所有进程的提交，主线程就卡在锁上（post11）。m2v 0012 从备用池里拿，内核 0318 释放时只等本进程自己的提交。
- 行间有空隙的渲染目标（1125 像素宽、行距 4608 字节）原来每遍都整张拷进拷出。改成直接用图像以后要注意：Turnip 的 GMEM 回写碰到图像右边和下边时按 16×4 对齐写，会写进行间空隙（r2p 实测 1125 像素的行写到 1136）。上游现在的做法是 PaddedReadsPolicy::AliasWrites 加它的 m2v 0016。
- 每次绘制的内存分配：r2n 时 malloc 一类占 backboardd 的 16.7%。m2v 0002、0004、0005、0007 把每次绘制的分配从 24.6 次降到 13.5 次。连同上面渲染目标直接用图像那一项，滑动帧率从约 101 到约 113（r2n 到 r2q）。
- RenderBox 用 methodForSelector: 缓存编码器的方法地址。拿到空编码器时缓存的是 0，一调用就跳到 0（post29）。上游 ce5e19e 改成返回一个会拒绝调用的编码器。
- 手机温度不同，帧率没法比。比翻译层的优化要看 backboardd 的 CPU 样本数（r2k-r2m）。

## 6. ADSP 和声音

- ADSP 固件直接从手机自己的 modem_a/modem_b 分区读（FAT16，LUN 4，image/ 目录），只读（0229）。PAS 的元数据在这代芯片上由 hypervisor 自己桥接，不要自己建 SHM bridge（0146）。
- 启动 ADSP 前要给 DSP 到内存的总线（rproc_ddr）投带宽，只投 LP0。不投的话 DSP 放开复位后一直不报就绪（adsp3，0228）。
- SMEM、SMP2P、GLINK、GPR 用 UniXNU-sheng 的代码原样搬（0226）。GLINK 要用新版：接收缓冲表 48 项，旧版 24 项会丢 DSP 的缓冲（0229）。
- 第一次问 APM 往往没回音，1 秒后重试就好（fw1）。
- apps SMMU 给音频数据流加直通表项时，固件会把写进去的"直通"读回成"报错"。做法是把最后一个上下文（80）设成不翻译，表项指向它（spk1）。
- 安卓放喇叭用的是 DEEP_BUFFER 图，不是 MMAP（10-26 改正）。喇叭的 DSP 图从手机自己的 ACDB 生成（tools/spk_graph.py，0262）。
- 功放 CS35L43 的 DSP 是 Halo 核，出复位后 ROM 自己在跑（核控制读到 0x81），要先照 cs_dsp_stop 停掉再写固件（spk3）。起核按安卓的顺序：复位，配置，PLL，路由，使能，查锁，加载，起核，然后做邮箱握手（spk13）。0x0104、0x0124 不要写，写了固件心跳不走（spk20）。
- 顶部功放的 GPIO174 拉高是外放，拉低是听筒。它原来是低，推的是听筒通路（spk22）。
- 喇叭的出厂校准值从手机的 persist 分区读，不进仓库、不打进日志。
- iOS 那侧自己写 IOAudio2Device 的子类，没有预链接苹果的 IOAudio2Family（0281）。VirtualAudio 能建出路由要两样：设备 UID 是 "Speaker"；另外还要一个 UID 是 "Hawking" 的设备，提供它必需的输入（vhaw）。少了 Hawking，整条路由退回空设备，这才是根因，和静音开关无关（aud18、aud19，0292）。
- 查路由用 VirtualAudio 自己的日志：偏好域 com.apple.coreaudio 里的 trace* 键（tools/logging/coreaudio_trace.plist）。设备管理器那几项别开到 spew，会把初始化弄坏（aud16）。
- 音量曾经低了约 45 dB：spk_graph.py 抄的音量档是安卓在媒体音量 0 时的（k0298）。
- 功放的电源跟着喇叭的 IO 走，照安卓：AudioFlinger 待机 2 秒后功放立刻断电（k0298）。
- 启动参数有 `pandora_spk=3` 时必须有 `pandora_spk_live=1`，否则开机放 18 秒测试音（post6）。

## 7. 电池、温度、CPU

- 电池电压、电流、温度走小米的 MCA 协议，在 SOCCP 的 GLINK 通道 PMIC_RTR_SOCCP_APPS 上（spk1）。百分比在主处理器 I2C 上的 bq27z561 电量计里。那条总线的 SE5 只能走 GSI（GPI DMA），FIFO 用不了（spk3、spk6）。
- 百分比照安卓算：剩余容量除以满充容量，97% 拉到 100%（0246）。
- 热敏电阻：iOS 下 PMIC 的自动测温没开，要照安卓的 adc_tm5_gen3_configure 写测量配置（0252）。其中 7 个通道要经过 PMIC 的 GPIO，安卓把这些 GPIO 设成高阻，不设的话读数会高 20 到 26 度（usb7、usb8，0256）。
- 降频按小米 mi_thermald 的表，看机身温度，不要只等芯片到 105 度（usb9，0257）。
- CPU 空闲时进 PSCI CPU_SUSPEND 的 "ret"（0x4），按设备树里的最短停留时间决定进不进（0215）。簇级和整机级休眠要 PSCI 的 OS 协调模式，固件支持，还没做。
- 调频和大小核推荐挂在苹果的性能控制器接口（sched_perfcontrol）上，结构照 iOS 官方，数字用这颗芯片自己的（PLAN"CPU"一节）。

## 8. iOS 系统服务

- fairplayd.H2 需要 FairPlayIOKit 驱动，我们没有。它不在的时候，locationd 卡在 com.apple.unfreed 上，CoreLocation 的同步查询一直等，设置和天文壁纸的编辑器都跟着卡（post3、post4）。苹果的 fairplayd 被拒以后会自己退出，不会留下来回错误（0314）。替身 tools/fairplay/pandora_fairplayd.c 认领这几个服务，对请求回 MIG 错误（post6）。
- 内核配置表（gIOKernelConfigTables）里不带引号的词只能有字母、数字和 `-`，带下划线会让整张表解析失败，开机就崩（post5）。
- CommCenter：iOS 的规矩是设备树里有 baseband 就加载 CommCenter.plist，没有就加载 CommCenter-noncellular.plist。ios-sweet 把两个都删了，vmd 连不上就一直重试，每次开机写约 55 万条日志、占满一个核（post13）。加回 noncellular 以后，CommCenter 自己仍按有基带的路子走，一直崩（post14）。
- 沙盒凭证：tccd 发不出 sandbox extension 时，相册的 assetsd 会异常退出。0329 实现了宽松策略里的 issue、consume、container 三个操作以后就好了（post22、post23）。
- 崩 3 次就停用（sweetcrashstop，0274）只是防卡死的护栏。被停的服务还挂在 launchd 上，同步调用它的程序会一直等（post25）。替身 pandora_sink 接住这些服务。收到 XPC 消息不能马上丢，对方会立刻重连，变成空转、手机发烫（post27），所以先压 1 秒再丢。
- dasd 给小组件算刷新额度时用量全是 NaN，查询失败后马上重查，coreduetd 一直占一个核的 75%（post13）。还没修。
- 用 Python 的 plistlib 重写 jetsam 配置后，launchd 涨到 4 GB 被杀，开机失败（post7）。改系统文件前留好原文件，出事能装回去。
- MobileGestalt 按设备树 /product 下的 product-id 查表认机型。

## 9. 日志和调试手段

- USB 串口：引导程序交出来时 USB 控制器开着、在设备模式、已停，可以直接接手（usb1）。电脑上识别成 VID 1209、PID 316D。重启前把没发完的日志发完（0253）。
- 统一日志：按块拷出 iOS 根镜像，只读挂载 HFS+，再用解析器读（tools/ioslog_fetch.py）。解析出来的时间被压缩过，只能看先后。
- 内核里的手段：线程栈 `pandora_stacks`，采样 `pandora_prof` 加 tools/prof.py，截屏（同时按两个音量键）加 tools/shot_fetch.py，GPU 卡死时的现场转储。
- 不用人拿手机的测试：`pandora_selftest`（内核按电源键、上滑），`pandora_autoswipe`（开机 45 秒起自动解锁、翻页）。
- 符号化共享缓存里的地址：`ipsw dyld a2s`，地址先减去日志里打的 slide。
- 某个系统组件表现得像不存在时，先看手机上的实际文件和 sweet-root-tweaks.py，再去反汇编（aud6）。
- 测试中手机如果重启进了开机菜单，什么都别按，否则菜单里旧版本的 iOS 会覆盖这次的日志（r2b）。下结论前先确认日志来自这次被测的内核。
- 开新一轮测试前，先停掉上一轮的串口抓取，否则串口被占着，日志写进了上一轮的文件（post14）。

## 10. 做事的方法

- 先读代码再加探针。GPU 卡死那件事前后加了好几轮探针，下了两个过早的结论（DSB、换进程时清上下文），最后是把 Turnip 释放 BO 的代码从头读完才找到（post28）。
- 抓第一个出错的地方。进程崩溃前最后几行日志多半是退出时的连带错误。
- 放宽限制之前，先有证明就是它的计数。GPU 地址窗口是看到 3439 次 OutOfMemory 才放大的（0219）。
- 芯片在安卓下好使、在我们这里不行时，先在 root 过的安卓里用 ftrace 的 regmap 事件抓下真实的寄存器读写顺序，照着一条条对（功放的 amptrace，Iris 的 tools/iris/capture_android.sh）。debugfs 里看到的寄存器值是驱动的缓存，不一定是芯片里的。
- 结构照 iOS 官方的做，数字用这颗芯片、这台手机自己的。例如调频挂苹果的性能控制器接口，温度阈值用小米的表。
- 别人（包括 AI 助手）查回来的结论要自己核对一遍。PLAN 里有好几处是核对后改正的，例如安卓放喇叭的 DSP 路径（10-26）。
- 一次测试多验几样东西。连续测试手机会很烫，测试之间让它凉下来。
- Metal 这块按"和真 iPhone X 回答得一样"做，不追着某个 App 修（HANDOFF.md）。
