# 交接说明

写于 2026-10-30。原作者停止开发，这份说明写给用小米 17 Pro（pandora）接着做的人。

项目的详细记录在 [PLAN.md](PLAN.md)，每一轮真机测试的结果都按日期记在里面。这份说明讲现在做到哪、怎么搭环境、怎么上机，以及踩过的坑。

## 现在的状态

截至 2026-10-30，下面这些在作者的手机上验证过（来源是 PLAN.md 对应的条目和测试日志）。

| 部分 | 状态 |
|---|---|
| 启动 | 电脑上 `fastboot boot` 临时启动。iOS 的根文件系统是安卓数据分区里一个位置固定的 16 GiB 文件，安卓的分区和数据保持原样 |
| 界面 | 锁屏、主屏幕、控制中心、设置能进能用，相册能打开。整个界面由 Adreno 840 合成，滑动约 110 帧 |
| 输入 | 触摸、电源键、音量键、亮度 |
| CPU | 8 个核按负载调频、空闲休眠。内核按两组核（6 个性能核、2 个超大核）调度，补丁 0320 |
| 电池和温度 | 电量计读数、按安卓的表限频 |
| 声音 | 两个喇叭能放系统声音，音量条能用（2026-10-29，k0296-k0298）。麦克风、听筒、耳机还没做 |
| 日志 | USB 串口实时看内核日志；iOS 的统一日志可以在回到安卓后从手机里取出来 |
| GPU 稳定性 | 10-30 找到并修好了 GPU 卡死的根因（Turnip 的 tu_bo 在释放后被清零，见 PLAN.md "根因"一条），post28 来回划壁纸选择没有出错 |

还在做或没做的：

1. 所有进程用 Metal。插件可以给每个进程一个 Metal 设备（`--clients "*"`），但小组件、部分壁纸、相册小组件还画不出来。原因和计划见下一节。
2. 卡顿。GPU 调频跟不上（多数时间停在 222 MHz），温控把 GPU 限在 726 MHz；后台 coreduetd 占一个核的 75%，biomed 24%。计划是按帧截止时间调频。
3. 锁屏没有时间和日期，原因没查到。
4. CommCenter 照有基带的路子走，3 次崩溃后被停掉，靠替身程序 `/usr/libexec/pandora_sink` 接住它的服务。正式的做法是让 iOS 知道没有基带，做成启动参数开关，以后有基带驱动再打开。
5. 替身有一个漏洞：App 扩展被换成替身后，系统等它报到等不到就杀掉，杀满 3 次替身自己也被停用。修法是 App 扩展不换替身，替身被杀不计崩溃次数（PLAN.md post31）。
6. HEIC/HEVC 解码（原版壁纸用 HEIC），每进程的 GPU 页表，GPU 卡死后的复位，Wi-Fi、蜂窝、相机、背屏。

## Metal：下一步做什么

10-30 定下的方向：不再追着某个 App 修，而是让 GPU 对所有程序的回答都和一台真 iPhone X（A11，Apple4 家族）一样。第三方应用按苹果公开的接口写，所以 SDK 里的公开方法都要有。A11 支持的功能要真能用；A11 不支持的，要像真机一样回答不支持。

对照表用 `tools/metal/conformance.py` 生成（用法在文件开头）。它拿 iOS 16.1 SDK 的 Metal 头文件和 A11 驱动的整条类链做参照，和插件实际有的方法比，再扫 dyld 缓存和系统卷，列出哪些库引用了缺的方法。10-30 的结果：渲染编码器 131 个公开方法插件有 46 个；参数编码器、间接命令缓冲、并行渲染编码器这几类对象插件里还没有。

定好的顺序：

1. 插件换到上游 AdrenoMetal 最新（ce5e19e）；每类对象声明它的 Metal 协议（现在只有设备声明了，Swift 里 `as? MTLTexture` 会得到空）；记录别人问"会不会某方法"而插件答不会的情况。这一步做了一半，在分支 `wip/metal-step1`，改动脚本演练通过，还没编译和上机。
2. 在电脑上用 metal2vulkan 把系统盘里的全部 135 个 metallib 翻译一遍，记下每处失败的原因。
3. 插件的对象改成继承 Metal 自带的基类。真机驱动的方法大头来自这些基类：_MTLCommandBuffer 81 个，_MTLCommandEncoder 64 个。插件的设备对象已经这么做了（继承 _MTLDevice）。
4. 按 iPhone X 补齐公开接口。
5. 所有 iPhone GPU 都有、应用一般不先问就用的功能：可编程混合（着色器里读当前像素）、memoryless 渲染目标、PVRTC/ASTC/ETC2 压缩纹理。还有像素格式，小组件要往 RGB8_444_2P（双平面）里画。
6. 对外声明从 Apple3 改成 Apple4。改了以后 Core Image、RenderBox 会开始用 tile 着色器、imageblock、光栅顺序组，这几样要先能翻译。

已知的崩溃和原因：

- chronod（小组件）：metal2vulkan 拒绝 RGB8_444_2P 当渲染目标，插件返回空编码器，RenderBox 缓存了空对象的方法地址，调用时跳到 0。上游 ce5e19e 改成返回一个会拒绝调用的编码器，第 1 步会带上它。真正画出来要做第 5 步。
- PridePosterExtension：崩在 MPS 的 MPSAutoCache 构造里，`initWithCommandBuffer:` 返回了空，原因没查。
- 同一颗芯片上另一位移植者（myron，跑 iPadOS 17）解决过天文壁纸 AegirPoster 的崩溃。他们的做法有四条：
  - BGR10_XR_sRGB 的混合用舍入模型近似，我们的 m2v 0010 就是这个。
  - XR10 格式只对 backboardd 回答不支持，其他进程回答支持。
  - 着色器缓存每个目录限 64 MiB。metal2vulkan 默认是 25 GiB，会写满数据分区。
  - RGB8_444_2P 先画到一张 RGBA8 的图上，再用固定的计算核拆成两个平面。

## 需要准备的

- 小米 17 Pro，bootloader 已解锁，安卓里有 Magisk root。
- 自己从苹果下载的 iPhone X（iPhone10,3 或 iPhone10,6）iOS 16.7.16 IPSW。要用里面的系统盘镜像、信任缓存和内核缓存（AMFI 等 11 个原版驱动从内核缓存里取出来链进我们的内核，见 `tools/mkprelink.py`）。
- 手机自己的固件：ADSP、GPU（gen80200 的 gmu、sqe、zap）、功放固件，从手机分区里取，用 `tools/mkfwpack.py` 打包。
- Windows 上的 Git Bash（adb、fastboot、串口抓取 `tools/usbserial_capture.ps1`），WSL2 的 Ubuntu 24.04（编译、做镜像、分析）。WSL 的虚拟磁盘放在空间大的盘上，作者那边曾经写满过 D 盘。
- WSL 里要有：clang 18 和 lld 18（含 ld64.lld）、qemu、hfsprogs、dtc、Rust 1.98（加 aarch64-apple-ios 目标）、glslc、spirv-tools、meson、[ipsw](https://github.com/blacktop/ipsw)（查 dyld 缓存）、apfs-fuse（挂 IPSW 的系统盘）、rcodesign（签名）。
- 本地编 Metal 插件和 Turnip 要一个 iOS 16.1 SDK（只用头文件和 .tbd）。SDK 要自己弄，作者用的那份的 .tbd 里 `platform: (null)` 要改成 `platform: ios`。

脚本里写死了作者电脑上的路径：仓库在 `/mnt/d/DeepSeekHarness/claude/ios_pandora`，工作目录在 WSL 的 `/opt/work`。在别的电脑上用要改这些路径。

## 编译

- 内核：GitHub Actions 的 `xnu.yml`（手动触发，在 macOS 机器上编）。fork 之后要在自己的仓库里打开 Actions。只改了驱动文件时，可以用 `wsl/local_build.sh` 拿一次 CI 的目标文件做底子，在 WSL 里只重编改过的文件。xnu 核心文件里用了 kalloc_type 的（例如 kern_exec.c）不能本地编，本地编出来会在手机上 panic，这类改动走 CI。
- 编好的内核要再用 `tools/mkprelink.py` 链上原版驱动，才能在手机上进 iOS。
- Turnip：`turnip.yml` 或 WSL 里的 `tools/turnip/local_turnip.sh`。
- Metal 插件：用 WSL 里的 `tools/metal/local_plugin.sh`（调用 `build_plugin_linux.sh`）。CI 的 `turnip.yml` 里也编插件，但不打 `tools/metal/m2v/` 下我们自己的 metal2vulkan 补丁，手机上一直用的是本地编的。
- 内核补丁的做法：`wsl/fix_NNNN.py` 或 `.sh` 在 `/opt/work/tree`（`wsl/make_tree.sh` 做的源码树）上改，导出成 `xnu/patches/NNNN-*.patch`。

## 上机测试

- 测试只用 `fastboot boot`，内核只在内存里，重启回安卓。
- 常用的是 `tools/gpu_test.sh`。它做固件包、写进手机里固定位置的包文件、启动 iOS、到时间后重启回安卓、取回内核日志。选项写在文件开头。作者最后一轮（post31）的用法：

```
AMP=1 NOEXEC=bluetoothd bash tools/gpu_test.sh post31 out-root/turnip/bochk4 --write-pack --secs 300 --hz 60 --no-vktest \
	--kernel /opt/work/plk/k0332-amfi.macho --metal-from /opt/work/lplugin/out --clients "*" \
	--amp-fw /d/DeepSeekHarness/claude/ios_pandora/ref/ampfw \
	--exec "out-root/pandora_fairplayd 755 /usr/libexec/pfd" --exec "out-root/pandora_sink 755 /usr/libexec/pandora_sink" \
	--args "pandora_gpu_metal=1 pandora_cpu=2 pandora_adsp=2 pandora_battery=1 pandora_gauge=3 pandora_therm=2 pandora_usb=2 qcom-usb-debug=1 pandora_audio=1 sweetexecswap=fairplayd.H2:/usr/libexec/pfd"
```

- `--put`、`--rm`、`--exec` 让内核里的安装器在开机时往 iOS 根文件系统里装文件或删文件。装进去的文件会一直留在 iOS 分区里，下次开机还在。
- 看结果的几种办法：
  - 串口日志用 `tools/usbserial_capture.ps1` 实时抓。开新一轮前先停掉上一轮的抓取，否则串口被占着。
  - 回到安卓后用 `tools/ioslog_fetch.py NAME --minutes N --parse` 取统一日志，再用 `tools/analysis/` 里的脚本筛。
  - 截图用 `tools/shot_fetch.py`。
  - 线程栈用启动参数 `pandora_stacks=进程名:秒数`，采样用 `pandora_prof=进程名` 加 `tools/prof.py`。

## 踩过的坑

- 启动参数最多 607 个字符，基础部分已经 575 个。有 `pandora_spk=3` 时不能删 `pandora_spk_live=1`，否则开机会放 18 秒测试音；要腾地方就把扬声器那组参数一起去掉。
- 那三个固定位置的文件（根文件系统、日志、固件包）在存储上的位置是编启动镜像时记下的。文件被删、重建或挪动以后再用旧镜像进 iOS，会写坏安卓的数据。
- 内核里映射寄存器窗口用 `ml_io_map_unmappable(pa, len, VM_WIMG_IO)`。`ml_io_map` 的映射放在 8 MiB 的小区域里，不能取消，大一点或要取消的映射会 panic。
- 内核的内联汇编里 `;` 是注释，一条 `__asm__` 只写一条指令。
- 手机上的 iOS 根文件系统来自 ios-sweet 的镜像，它挪走过一些系统组件（例如 VirtualAudio 插件被挪到 HAL.disabled）。某个组件表现得像不存在时，先看手机上的实际文件和 ios-sweet 的 `sweet-root-tweaks.py`。
- 用 Python 的 plistlib 重写 launchd 的 jetsam 配置后，launchd 涨到 4 GB 被杀，开机失败。改系统文件后要能装回 IPSW 原文件。
- `tools/ioslog_fetch.py` 解析出的时间间隔被压缩过，只能看先后顺序。
- WSL 只有 7.8 GB 内存时，两个 metal2vulkan 测试同时跑会被杀掉。
- 找问题时先把相关代码读完再加探针。GPU 卡死那次，前面一轮轮加探针、下了两个过早的结论，最后是读 Turnip 的 BO 释放代码才找到根因。
- 抓第一个出错的地方，不要看最后几行。崩溃的进程最后的日志多半是退出时的连带错误。

## 仓库里没有的

苹果的文件（IPSW、dyld 缓存、内核缓存和从里面取出的程序）、手机固件、分区备份、设备导出、日志和喇叭校准值，都只放在各自的电脑上，`.gitignore` 挡住了这些目录。用到它们的工具在文件开头写了从哪里取。

## 参考

- [PLAN.md](PLAN.md)：全部进度和每轮测试。
- [docs/GPU_PLAN.md](docs/GPU_PLAN.md)、[docs/GPU_PORT_MAP.md](docs/GPU_PORT_MAP.md)：GPU 移植。
- [docs/SPEAKER_RESEARCH.md](docs/SPEAKER_RESEARCH.md)、[docs/AUDIO_SPEAKER.md](docs/AUDIO_SPEAKER.md)：喇叭。
- [device/DT_FINDINGS.md](device/DT_FINDINGS.md)、[device/BOOTIMG.md](device/BOOTIMG.md)：设备树和启动镜像。
- [bootmgr/README.md](bootmgr/README.md)：开机菜单（写一次 boot_b，开机按音量上进 iOS）。
- 上游：[UniXNU](https://github.com/UniXNU/UniXNU)、AdrenoMetal、Adreno740、[ios-sweet](https://github.com/dipa-studio/ios-sweet)、[ios-fuxi](https://github.com/bangprovn/ios-fuxi)、[metal2vulkan](https://github.com/steelbrain/metal2vulkan)。
