# 小米 17 Pro (pandora) 跑 iOS 16：完整计划

最后更新：2026-10-22

## 目标和边界

- 目标：小米 17 Pro 上运行 iPhone X 的 iOS 16.7.16 原版系统 + 自己编译的苹果内核（xnu-8796），和安卓做成双系统。
- 做成后能有：桌面、设置、控制中心、触摸和按键、部分自带应用。
- 不做：GPU（用 CPU 画画面）、Wi-Fi、蜂窝网络、声音、相机、背屏。App Store 的应用基本跑不了（解密要靠苹果芯片里的安全模块）。
- 为什么是 iOS 16：iOS 17 起 iPhone 只支持 A12 及以后，那些系统程序都用了苹果特有的指针签名指令；iPhone X 是最后一代不用的。
- 主要参考：ref/ios-fuxi（小米 13 / 骁龙 8 Gen 2，iOS 16 已进桌面，ios-sweet 的复刻）；ref/ios-sweet（红米 Note 10 Pro，已进桌面）；社区全貌见 COMMUNITY.md；D:\DeepSeekHarness\claude\unixnu_review（UniXNU，骁龙 8 Gen 2，4K 页、存储驱动）。

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
- 现在用量（2026-10-21）：314 GB。内部存储 183 GB、应用数据 69 GB、应用本体 40 GB、Magisk 88 MB，第三方应用 152 个。
- 电脑空间：D 盘 219 GB + E 盘 227 GB，分两个盘放：内部存储（183 GB）放 E 盘，应用和应用数据（约 109 GB）放 D 盘。
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
- 云端（GitHub Actions，私有仓库 ShueiTsuki/ios-pandora）编出 PANDORA 4K 内核；补丁 0082–0101 来自 ios-fuxi，0102–0110 是我们的：
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
- 实体按键：电源、音量+、音量-默认开启。三颗键在 iOS 里能稳定读到状态（音量+ 修正了 TLMM 基址）。按下事件送进 iOS 还要人按一下确认。
- 主屏触摸：默认开启。供电（RPMh L4I_E0 + GPIO 117）、复位、GPI DMA SPI、TouchComm v1 握手、应用信息、报告格式都已在真机上跑通；打开了芯片自己的触摸报告（0x11），同时保留原始帧找手指的备用路径。手指坐标送进 iOS 还要人摸屏确认，摸了才知道方向是否要翻转。
- 屏幕亮度：注册了 AppleARMBacklight 背光服务，iOS 开机已经写入亮度（50%），按亮度把像素调暗；只能在 ABL 设定的面板亮度以下调节。拖滑块要人验证。
- 扬声器：两颗 CS35L43 功放 I2C 应答正常（只探测，`pandora_amp=1`）。出声需要音频 DSP 驱动 I2S0，加上 iOS 能识别的音频驱动接口，没做。
- 摄像头：iOS 相机依赖苹果 ISP 和私有驱动接口，高通 CAMSS 接不进去，没做。
- 无人值守测试：`tools/ios_test.sh 名字 内核号 秒数 "附加参数"`，到时自动重启回安卓并取回日志 device/logs/名字.txt；日志文件 /data/local/tmp/ioslog.img（8 MiB，可删）。
