# 小米 17 Pro (pandora) 跑 iOS 16：完整计划

最后更新：2026-10-24（GPU 一节见 docs/GPU_PLAN.md；CPU 一节在文末）

## 目标和边界

- 目标：小米 17 Pro 上运行 iPhone X 的 iOS 16.7.16 原版系统 + 自己编译的苹果内核（xnu-8796），和安卓做成双系统。
- 做成后能有：桌面、设置、控制中心、触摸和按键、部分自带应用。
- GPU：原计划不做（用 CPU 画画面）；2026-10-23 起改为主线，进展见 docs/GPU_PLAN.md。2026-10-24 主屏幕第一次由 Adreno 840 画出（实验性，快速操作时会卡死，在查）。
- 不做：Wi-Fi、蜂窝网络、声音、相机、背屏。App Store 的应用基本跑不了（解密要靠苹果芯片里的安全模块）。
- 为什么是 iOS 16：iOS 17 起 iPhone 只支持 A12 及以后，那些系统程序都用了苹果特有的指针签名指令；iPhone X 是最后一代不用的。
- 主要参考：ref/ios-fuxi（小米 13 / 骁龙 8 Gen 2，iOS 16 已进桌面，ios-sweet 的复刻）；ref/ios-sweet（红米 Note 10 Pro，已进桌面）；社区全貌见 COMMUNITY.md；[UniXNU](https://github.com/UniXNU/UniXNU)（骁龙 8 Gen 2，4K 页、存储驱动）。

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

## 扬声器路线（调查结果，2026-10-22，未开始）
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
- **下次上手机（需用户同意）**：插件 /opt/work/lplugin/out（m2v 0001–0008，工作计数，两个启动参数开关），Turnip local216（等围栏单次 100 ms），内核 0225。
  - r2o：默认（Shadow；rebinds 拒绝默认开）。和 r2n 比 backboardd 的 CPU、分配占比、`drop CallLog` 是否消失；工作计数里每 10 秒的影子纹理、影子拷贝次数和像素数（分清两平面和单平面）、短轮询次数；part 4b 应通过。
  - r2p：加 `pandora_m2v_padded=alias`。part 4b 判断 Turnip 是否碰行间空隙；比较单平面影子拷贝、GPU 等待帧数、帧率和温度。part 4b 不过就不用 Alias。
  - 有问题时的对照开关：`pandora_m2v_rebinds=allow` 恢复调用日志。
  - 之后再看：关掉 `pandora_bigfirst` 时的温度和帧率；两平面表面每遍拷贝的方向（见上面“需要你定的”）。
- **Turnip 等围栏改成单次最多 100 ms（tools/turnip/pandora_wait.py，local216，待上手机）**：UniXNU 的 darwin 后端 `dw_wait_fence_raw` 每次只向内核要 1 ms，等一帧 GPU 工作要来回 IOKit 好几趟（r2n：backboardd 每秒约 420 次短轮询超时）。内核从 0216 起就睡到 GPU 围栏中断为止，照 Linux msm 后端 MSM_WAIT_FENCE 一次给整个超时的做法，改为每次给剩余时间、封顶 100 ms（内核等待不可中断，与翻译层自己的 100 ms 粒度一致）。CI 的 turnip.yml 同步加了这一步。

### CPU 深度休眠：分析（2026-10-25，未动代码）
- **核心级“ret”已在 0215 做了**（PSCI CPU_SUSPEND 0x4，按下一次定时器远近选 ret 或 WFI）。没做的是簇级和整机级，设备树层级：cpu0–5 → cluster-pd0（ret-pll-off 0x1000054，最短停留约 9.1 ms），cpu6–7 → cluster-pd1（同上），两簇 → cluster-pd2（apps-pc 0x200c354，约 10.2 ms）；要 PSCI OS 协调模式（已确认固件支持）。
- **结论：现在做簇级休眠对发热几乎没用。** 滑动时每 10 ms 一帧，簇里所有核同时空闲 9 ms 以上的机会很少；静止时 vmd 占约 93% 的一个核、服务反复拉起，簇里总有核在忙。它要等“服务反复查询”解决后，对待机和静止画面的功耗才有意义。
- **滑动时更直接的是调度**：测试一直开着 `pandora_bigfirst=1`，线程优先放 4.4 GHz 的大核。下一步可对比关掉大核优先（或换苹果自己的大小核调度）时的温度、帧率、CPU 占用（要上手机，需用户同意）。
