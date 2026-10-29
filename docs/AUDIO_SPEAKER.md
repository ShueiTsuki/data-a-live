# SM8850 的扬声器：让 iOS 16 的声音从喇叭出来

截至 2026-10-29，小米 17 Pro（pandora，骁龙 8 Elite Gen 5，SM8850）上的 iOS 16.7 能从两个喇叭放出系统声音和应用的声音。控制中心的音量条和实体音量键都能用，满音量和安卓一样响。没有声音时功放会下电，关机前也会先下电。

这份文档写整条路是怎么做通的，给在同一颗 SoC 上做移植的人看。ADSP 启动、SMMU、共享内存和 AudioReach 播放图是 SoC 层面的，同 SoC 的机器可以照着做。功放型号、I2C 地址、GPIO 和 ACDB 里的取值是 pandora 的，换了机器要从自己手机的安卓里读。每一段写了代码在哪里、结论来自哪次测试。测试名（adsp3、spk20、aud19 这类）在 [PLAN.md](../PLAN.md) 里能查到当时的日志和判断。

## 1. 整条路

1. iOS 的 mediaserverd 里，VirtualAudio 插件把混好的声音写进内核里一个 UID 为 "Speaker" 的 IOAudio2Device 的环形缓冲。格式是 48 kHz、2 声道、32 位有符号整数。
2. 内核每次取 20 ms（960 帧）交给 ADSP。ADSP 经过 apps SMMU 读 4 GiB 以下的一块共享内存。
3. ADSP 跑安卓放喇叭时用的那张 AudioReach 播放图，图和校准都从手机的 ACDB 里取。图的出口是 TDM0，也就是 LPASS 的主 I2S 口（GPIO 122 到 125）：48 kHz，一帧 2 槽，每槽 32 位，位时钟 3.072 MHz。
4. 两颗 Cirrus CS35L43 功放从 TDM0 收声音。顶部那颗（I2C 地址 0x42）收槽 0，也就是左声道。底部那颗（0x40）收槽 1，右声道。这个对应关系是在 spk21 里分别给两个声道放不同频率、看两颗功放的输入读数确认的。
5. 功放上跑安卓的扬声器保护固件，加上这台手机的出厂校准。

代码：

- `wsl/pandora_adsp.c`：ADSP 的启动和通信、SMMU、共享内存、播放图、往图里送声音。
- `wsl/pandora_amp.c`：功放的 I2C、固件加载、上电和下电。固件文件的解析在 `wsl/pandora_csfw.c`。
- `wsl/PandoraAudio.cpp`：iOS 看到的两个音频设备。
- `tools/spk_graph.py`：从 ACDB 生成播放图文件 `spkgraph.bin`。
- `wsl/qcom/`：SMEM、SMP2P、GLINK、GPR 和 FAT 读取器，从 UniXNU-sheng 原样拷来，来源提交写在 `wsl/qcom/SOURCE.txt`。

这些文件由 `wsl/fix_*.py` 拷进 xnu 源码树。每一步的改动按编号放在 `xnu/patches/` 里，文中的 0289、0293 这类编号就是补丁号。

## 2. 启动 ADSP

固件用手机自己的。它在当前槽的 modem_a 或 modem_b 分区里（安卓的 `/vendor/firmware_mnt`，UFS 的 LUN 4，FAT16），文件是 `image/adsp.mdt` 加分段和 `image/adsp_dtb.mdt`。系统更新后两个槽里的版本可能不同，所以要读安卓当前的槽：`tools/boot_ios.sh` 读安卓的 `ro.boot.slot_suffix`，作为开机参数 `pandora_slot` 传给内核。内核只读这个分区，用 sheng 的 FatVolume.c 在原处读文件。

启动顺序照 Linux 的 `qcom_q6v5_pas.c`：

1. 给 DSP 到内存的总线（DT 里的 `rproc_ddr`）投带宽。照主线 kaanapali 的总线驱动，这条路上只有 LP0 是 LPASS 专用的，我们只投 LP0。不投的时候，两个镜像都能通过校验、放开复位，但 DSP 5 秒内不会报就绪（adsp2）。投了以后 434 ms 就起来了（adsp3）。
2. 经 QMP 让 AOP 打开 `adsp` 的 load_state。
3. RPMh 的 LCX 和 LMX 电压投到 384（DT 的 cx-supply、mx-supply）。
4. 先启动 DTB 镜像（PAS 36），再启动主镜像（PAS 1）。每个镜像：元数据（ELF 头、程序头、hash 段）放进一块连续内存，PAS_INIT_IMAGE，需要重定位时 PAS_MEM_SETUP，各段拷进 DT 的预留区，最后 PAS_AUTH_AND_RESET。主镜像的预留区是 0xa1400000 起 0x5580000，DTB 的是 0xa1380000 起 0x80000。

安全世界（SCM）调用要一次只发一个。adsp1 里 ADSP 线程和 GPU 驱动同时发了调用，固件让 GPU 那个排队（返回 2，WAITQ_SLEEP）。没人去唤醒，之后整次开机所有调用都返回 2。现在 `scm_smc` 加了互斥锁。万一还收到排队，就照 Linux 的 waitq 流程轮询 GET_WQ_CTX，再发 WAITQ_RESUME。

DSP 起来以后的通信照 sheng 的 AdspProbe.cpp 和 AudioProbe.cpp：

- 启动前先建我们的 SMP2P 项 429，放在 SMEM 的 apps-ADSP 分区里，分配时拿 TCSR 互斥锁 3，条目名 `master-kernel`。
- 启动后等 DSP 建 SMP2P 项 443，条目 `slave-kernel`。bit 1 是就绪，bit 2 是交接。交接以后 DSP 自己维持电压，我们把 LCX、LMX 的投票撤回 0。fw1 里放开复位后 409 ms 建项，602 ms 就绪，628 ms 交接。
- GLINK 用 SMEM 项 478（描述）、479（我们的 FIFO，16 KiB）和 480（DSP 的 FIFO）。门铃是 IPCC（DT `qcom,ipcc@1106000`），LPASS 是客户端 3，信号 2 给 SMP2P，信号 0 给 GLINK。我们还没接中断，内核线程每 1 ms 读一次 IPCC 的 RECV_ID，开机 30 秒后改成每 20 ms 读一次。
- DSP 会打开 GPR 通道 `adsp_apps`。按 DT 的 `qcom,gpr`，我们给它 20 个 512 字节的接收缓冲。然后问 APM 音频框架是否就绪。第一次通常没有回答，1 秒后再问就回就绪。我们最多重试 100 次。
- DSP 先给 24 个小的接收缓冲，再给一个大的。GLINK 记对方接收缓冲的表太小时，大的那个会被丢掉。sheng 新版把表扩到 48 项，换上以后 GLINK 丢的消息从 92 条降到 16 条（fw1）。

DSP 还会打开 glink_ssr、pcie_drv、mhi_sat、IPCRTR、fastrpcglink-apps-dsp、adsp_apps2、sleepmonglink-apps-adsp 这些通道。放喇叭用不到它们，我们没接。

## 3. DSP 能读的内存

DSP 经 apps SMMU（DT `apps-smmu@15000000`）读内存。它的音频数据流在 DT 的 `qcom,msm-audio-ion` 里：`iommus = <0x1001 0x80>, <0x1041 0x20>`。开机后这些流在 SMMU 里没有表项，没匹配上的流会出错（CR0 的 USFCFG）。我们照 Linux `arm-smmu-qcom.c` 处理直通流的办法：把最后一个上下文 bank 设成不翻译（SCTLR 为 0），给这两组流各加一个表项指过去。只用空着的表项，和已有表项重叠就不写。

共享内存要放在 4 GiB 以下，因为给 DSP 的地址是 32 位的，高位那个字放 SMMU 流的序号（我们用 1）。xnu 管的内存都在 0x8c0000000 以上，所以我们在手机 DT 的 `/memory` 里找了一段安卓也没固定占用的地方：0xb0000000 起 1 MiB。加载器把它写进 ADT 的 `/chosen` 下的 `pandora-dsp-shm`（`tools/mkadt_pandora.py`），内核按写合并映射。

这块内存用 APM_CMD_SHARED_MEM_MAP_REGIONS（0x0100100C）登记给 DSP：内存池 SHMEM8_4K（3），一个区域，和 Linux 的 `audioreach_map_memory_regions` 一样。DSP 回 0x02001001，里面是之后命令要带的句柄。前 256 KiB 放命令的载荷，后面放声音。

## 4. 播放图

做法是开安卓放喇叭时开的那张图，所有取值都来自手机的 ACDB。

### 4.1 从 ACDB 取图

安卓放音乐走 DEEP_BUFFER。这张图的图键（GKV）是 StreamRX = PCM_Deep_Buffer、Instance = 1、DeviceRX = Speaker、DevicePP_Rx = Audio_MIPLAY，校准键（CKV）是 Volume。键和值的名字对照在手机的 `/vendor/etc/kvh2xml.xml`。DevicePP_Rx 的取值是小米的，别的厂商要看自己的 ACDB。

`tools/spk_graph.py ACDB OUT` 读手机的 ACDB，生成 `spkgraph.bin`。pandora 的 ACDB 是 `/odm/etc/acdbdata/Mi/Mi_acdb_cal.acdb`。解析照 AudioReach graphservices 的源码写，主要是 `acdb/src/acdb_command.c` 和 `gsl/src/gsl_graph.c`。生成的文件有四段：

- 开图数据：每个子图的 SPF 属性和子图之间的连接。gsl 打开图时就是把这两段原样发给 APM_CMD_GRAPH_OPEN。
- 校准：每个子图的默认表，加上 CKV 匹配上的表。一个模块在匹配的表里有数据时，就不用默认表里它的数据。
- TDM 硬件端点的标签数据，按 Channels = 2 取。
- 子图列表，开始、停止和关闭时要用。

这张图有四个子图：流（0xb0000001）、0xb000001c、设备后处理（0xb000019f）、设备（0xb0000002）。流子图里依次是共享内存端点 0x4001、PCM 解码 0x4003、PCM 转换 0x4004、音量模块 0x401f、模块 0x4019、SPR 0x4163，中间还有几个日志模块。SPR 管会话时间，不是扬声器保护。下一个子图 0xb000001c 里是 MFC 0x40b6。设备子图的出口是 TDM 端点 0x4023（模块 0x0700100E）。这些实例号是小米 ACDB 里的。

ACDB 是手机上的文件，`spkgraph.bin` 由它生成，两个都不放进仓库。测试时 `tools/gpu_test.sh --amp-fw` 把 `spkgraph.bin` 打进固件包。

### 4.2 音量档用 Level_0

流子图的音量模块 0x401f 有 16 张按 Volume 分的校准表。Level_0 的分声道增益（参数 0x08001038，Q28）是 0x10000000，即 0 dB。之后每档少约 3 dB，到 Level_15 是 0x00170894，约 -45 dB。Level_15 还把两个静音参数 0x08001036 和 0x08001039 设成 1。

安卓在媒体音量为 0 时用 Level_15。我们最早照抄的那份安卓日志恰好是在媒体音量 0 时抓的，所以 aud25 以前喇叭比安卓小很多。iOS 在把声音写进环形缓冲之前已经按自己的音量缩放过，DSP 这边应该不再衰减，所以用 Level_0，相当于安卓开满音量。换成 Level_0 以后，用户听下来和安卓一样（aud26）。

### 4.3 命令顺序

照安卓 AGM 和 PAL 开一路放音的顺序。命令经 GPR 发给 APM（实例 1）。命令的载荷放在共享内存里，带外发送，和 gsl 的做法一样。

1. 向 PRM（GPR 实例 2）申请 TDM 位时钟。命令是 PRM_CMD_REQUEST（0x0100100F），参数 PARAM_ID_RSC_AUDIO_HW_CLK（0x0800102C）。时钟号 0x200（主 TDM 内部位时钟），3.072 MHz，属性 4（DT 的 tdm-clk-attribute），root 0。这些值是在安卓上照 machine_dlkm 的 `msm_common_snd_hw_params` 核对的。
2. APM_CMD_GRAPH_OPEN（0x01001000），载荷是 `spkgraph.bin` 的开图数据。
3. APM_CMD_SET_CFG（0x01001006），载荷是 ACDB 的校准。
4. AGM 和 PAL 在运行时设的那些参数，每样一条 SET_CFG：
   - 流的 MFC（0x40b6）：48 kHz，32 位，2 声道 L R。
   - 共享内存端点（0x4001）的媒体格式。
   - TDM 端点（0x4023）的媒体格式：48 kHz，32 位，2 声道，定点。
   - TDM 端点的接口：ACDB 的标签数据，填上 LPAIF 类型和接口号。主 TDM，2 槽，每槽 32 位。
   - PCM 解码（0x4003）和 PCM 转换（0x4004）的输出：32 位 q31，分声道，不打包。
   - SPR（0x4163）的路径终点设成 TDM 端点，并重置会话时间。
   - 音量模块 0x401f 的总增益设 0x2000（Q13 的 1 倍），取消总静音。设备的音量模块 0x437e 取消静音。用 Level_0 时这几项本来就是这个值。
5. APM_CMD_GRAPH_PREPARE（0x01001001）和 APM_CMD_GRAPH_START（0x01001002），带上四个子图。

每条命令 DSP 都回 GPR_IBASIC_RSP_RESULT（0x02001005），状态为 0 才算成功。

### 4.4 送声音

声音用 DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2（0x0400100A）发给共享内存端点 0x4001。要用第 2 版。第 1 版的 0x04001000 每块都回 AR_EUNSUPPORTED（3）（spk14）。

一块是 20 ms，960 帧，交错存放的 32 位 q31 立体声。同时有 4 块在 DSP 手里。DSP 用完一块回 DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2（0x05001004），这时内核把下一个 20 ms 填进这块再发出去。

现在这张图开机后一直开着。iOS 没在放音的时候，内核往里送静音。

## 5. 功放 CS35L43

### 5.1 总线和引脚

- I2C 在 QUPv3 wrapper 3 的 SE0（0x01980000，安卓的 i2c-13）。SDA 是 GPIO 64，SCL 是 GPIO 65，功能 1，上拉。时钟：wrapper 3 的投票在 GCC 0x52018 的 bit 8 到 12，SE0 的时钟源在 0xa8014。我们用 FIFO 模式，100 kHz，做法照 Linux 的 `i2c-qcom-geni.c`。
- 底部功放 0x40（DT 里叫 "B"）的复位是 GPIO 10。顶部 0x42（"T"）的复位是 GPIO 54。
- 寄存器地址和值都是 32 位，大端。
- 顶部那颗也接听筒。GPIO 174 高时推喇叭，低时推听筒。安卓的混音控件 "T Enable RCV Pin Control" 默认是 1（拉高），只有听筒路径设 0。我们开流时把 GPIO 174 设成高。

### 5.2 要用的文件

在安卓的 `/odm/firmware` 里：

- `cs35l43-dsp1-spk-prot.wmfw`：两颗共用的扬声器保护固件。
- `T-cs35l43-dsp1-spk-prot.bin`、`B-cs35l43-dsp1-spk-prot.bin`：两颗各自的系数。
- `T-cs35l43-delta-music-spk-prot.bin`、`B-cs35l43-delta-music-spk-prot.bin`：music 场景的调音。安卓还有 movie、video、navigation、voice 几个场景，默认用 music。

出厂校准在 `/mnt/vendor/persist/audio/crus_calr.bin`，是两个 32 位小端数，第一个给 T，第二个给 B，原值写进功放。这两个数每台手机都不一样，要从自己的手机上读，不要照抄别人的。我们的内核不打印它们，文件也不放进仓库。

### 5.3 开机：加载功放上的 DSP

照安卓 `cs35l43_dlkm.ko` 里 `cs35l43_probe` 和 `cs35l43_dsp_init` 的顺序。安卓上用 ftrace 抓过一次功放 DSP 重载，逐条对过。

1. 复位，读 DEVID（0x35A43）和 REVID（0xB1）。
2. 写补丁：0x7404 = 0x11330000，0x3850 = 0x50000802。
3. 马上停掉 ROM 程序。CS35L43 出复位后会自己跑 ROM，核控制读出来是 0x81。停法：看门狗 0x2BC7000 的 bit 0 清掉，核控制的 bit 0 清掉，0x2B80010 的 bit 0 置上。
4. 写 0x2900 = 2，再写固件和系数。
5. 起核：MPU 写 23 次，然后核控制先写 0x281，再写 0x81。安卓驱动是在 regmap 缓存上改位的，缓存里还是停核时写的 0x80，所以 bit 7 也被写回去了。我们照芯片读数写成 0x201 和 0x001 时，核一跑就被 MPU 拦下，XM、YM、PM 的越界状态都置位（spk8 到 spk12）。
6. 起核后 3 ms 内，固件会拉起邮箱中断（0x10010 的 bit 31）。把它清掉，写 0x13020 = 0x02000003，再从 0x13000 起读 8 个邮箱寄存器，消息是 0x2000002，表示固件醒了（spk13）。

### 5.4 第一次放音

照安卓的音乐播放器开声音时两颗功放的寄存器跟踪：

1. 写配置。值取自安卓空闲时芯片上的真实值，表在 `pandora_amp.c` 的 `pa_cfg`。读安卓的值时要先打开 regmap debugfs 的 cache_bypass。普通的 regmap 转储里，驱动只写不读的寄存器显示的是缓存值，不是芯片上的。0x0104 和 0x0124 就是这样：芯片上是 1，转储里是 0。我们照转储写成 0 时，固件能报读数，但心跳不走，喇叭不出声（spk19）。不写它们以后就出声了（spk20）。
2. 设 PLL，照 `cs35l43_component_set_sysclk`。先写 0x2C0C 的低 5 位为 3（48 kHz）。再分六步改 0x2C04：开环（bit 11），参考频率 3.072 MHz（bit 5 到 10 写 0x21），关 PLL（bit 4），时钟源选位时钟（bit 0 到 2 写 0），闭环，开 PLL。然后写 0x2D10 = 0x3C024。最后 0x2C04 是 0x430。
3. 0x4C00 = 0x36，0x6808 = 0x3F75（噪声门），0x4800 = 0x10001，0x2018 = 0x3721。
4. 叠 music 场景：把 delta 文件当系数写进去，和 `cs35l43_apply_delta_tuning` 一样。
5. 解锁测试寄存器（0x40 写 0x55、0xAA），写 0x2084 = 0x000F1AA0，再上锁（0x40 写 0xCC、0x33），然后 0x2014 = 1。
6. 开机后第一次放音时才做：0x10110 的 bit 31 清掉；CAL_R_SEL 写这颗的校准值；ALLOW_ACT_BOOT 写 1。CAL_R_SEL 和 ALLOW_ACT_BOOT 在固件的 XM 里（算法 0x5F210 和 0x5F224），地址按 `.wmfw` 里的控件描述算。
7. PLAY：0x13020 = 0x0B000001。等约 3 ms 读 AUDIO_STATE（算法 0x5F212），1、2、6、7 都表示在放。

### 5.5 没声音时下电

在安卓上量的（第 7 节有方法）：AudioFlinger 的输出线程在声音停 2 秒后待机（dumpsys 里是 `Standby delay ns=2000000000`）。0.3 秒的提示音放完后 1.99 秒，两颗功放就下电了，没有再等 TDM 链路的 pmdown_time（5000 ms）。iOS 这边也差不多：aud26 里播放器 148.8 s 放完，HAL 在 150.9 s 停 IO。

所以我们的功放跟着 "Speaker" 设备的 IO 走：停 IO 就下电，开 IO 时先上电。下电的写法照安卓：

1. 两颗都写 0x2018 = 0x21。
2. 每颗依次做：
   - PAUSE（0x13020 = 0x0B000002），约 3 ms 后读 AUDIO_STATE，0、2、6 表示已停。
   - 0x2018 = 0x20。
   - 解锁，0x2084 = 0x000F1AA3，上锁。
   - 1 ms 后 0x2014 = 0，0x4800 = 0。
   - 等 0x10010 的 bit 16（下电完成，最多 100 ms），再写 0x10010 = 0x10000 清掉。
3. 两颗都写 0x6808 = 0x33，再写 0x4C00 = 0。

再上电时不用重写场景和校准，安卓第二次放音也不写：

1. 两颗都写 0x4C00 = 0x36、0x6808 = 0x3F75，再写 0x4800 = 0x10001、0x2018 = 0x3720。
2. 每颗：解锁，0x2084 = 0x000F1AA0，上锁；0x2014 = 1；0x2018 = 0x3721；PLAY，查 AUDIO_STATE。

安卓上每颗功放上电约 5 ms。我们的 I2C 慢一些，两颗一共约 30 ms，下电约 55 ms（aud27，10 次上电 10 次下电都没出错）。用户听下来锁屏音和音乐开头都完整，上下电时没有杂音。

关机和重启前也要下电。"Speaker" 设备开机时用 `registerPrioritySleepWakeInterest` 登记一个回调。iOS 关机或重启时，`IOPMrootDomain::handlePlatformHaltRestart` 会逐个同步调用这些回调，消息是 kIOMessageSystemWillPowerOff 或 kIOMessageSystemWillRestart。回调里让功放下电。aud28 里滑动关机时，日志里有这一步。

## 6. iOS 这一侧

### 6.1 音频设备

iPhone X 的内置音频用 IOAudio2Family。我们的内核没有预链接苹果的 IOAudio2Family，这两个类名没人用。所以我们自己写了 IOAudio2Device 和 IOAudio2DeviceUserClient（`PandoraAudio.cpp`），挂在 IOResources 上，开机参数 `pandora_audio=1` 打开。CoreAudio 用 `IOServiceMatching("IOAudio2Device")` 找设备。下面这些接口是从 iPhone X（20H392）的内核缓存和 CoreAudio 里读出来的：

- 设备描述放在注册表属性里，键是小写带空格的。设备级的有 "device name"、"device UID"、"transport type"（内置是 'bltn'）、"output streams"、"io buffer frame size"、"output latency"、"output safety offset"、"clock domain"、"sample rate"。数字用 32 位。采样率用 64 位，32.32 定点。
- 每条流是一个字典："stream ID"、"starting channel"、"terminal type"（喇叭 'spkr'，麦克风 'micr'）、"current format"、"available formats"。
- 用户端有 13 个方法和一个陷阱。陷阱是 DoIO，内置设备在这里什么也不做。`clientMemoryForType` 的 type 右移 28 位后，0 是引擎状态页（采样时间、主机时间、seed，CoreAudio 每次读两遍），1 是流的 IO 缓冲。
- 打开设备后，HAL 映射状态页和 IO 缓冲，然后只发方法 0（engine start）开始 IO，停的时候发方法 1。方法 10 和 11 是带标志的开始和停止，也要接上。只接 10、11 的时候，HAL 发了方法 0 后等 5 秒没有零点时间戳，就放弃了（aud20）。
- 方法 6 是设流活动。路由切到我们的设备时，两个设备都会收到 (1, 1)。
- `setProperties` 要接受独占请求（"exclusive access owner" 是 mediaserverd 的 pid）。不接受的话 VirtualAudio 拿不到独占，报 0xE00002C7 "Unable to take hog mode"（aud7）。
- 时钟用我们自己的：开 IO 时记下起点，每绕环形缓冲一圈（16384 帧）写一次零点时间戳。HAL 写在"现在"之前 960 帧（output safety offset）。内核给 DSP 取声音时，读位置跟在"现在"后面 960 帧。

设备 UID 要叫 "Speaker"。VirtualAudio 按机型选一个内置的换能器数据库，再按 UID 去找物理设备，iPhone X 的有 Codec、Speaker、Voice、Reference、Baseband Voice 等。机型来自 MobileGestalt：它用 `IODeviceTree:/product` 的 product-id 查表，我们的设备树照抄了 iPhone X（D22）的值，iOS 认成 iPhone10,3。

### 6.2 VirtualAudio 要的东西

VirtualAudio 是老式的 AudioHardwarePlugIn，在 `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin`，不在 `/System` 下。ios-sweet 的根文件系统镜像 v12 把它挪到了 `HAL.disabled`，因为那时没有音频设备，mediaserverd 会崩。用 sweet 系镜像的要先挪回来（aud6、aud7）。

光有 "Speaker" 还不够，还要一个 UID 为 "Hawking" 的输入设备。默认虚拟设备 vdef 要给 8 个附属虚拟设备建路由，其中 Hawking（vhaw）是必需的。它的输入端口 'phki' 从 UID 为 "Hawking" 的设备建出来。缺它的时候，默认路由退回 Null_Device（aud18 的日志），系统声音进了空设备，音量条也拖不动。我们在 IOResources 上放了第二个 IOAudio2Device 实例（personality 里 `PandoraAudioRole = Hawking`），带一条静音的输入流。加上以后路由就通了：日志里是 `{ vdef: pap → pspk 扬声器 Speaker }, { vhaw: phki → pap }`（aud19）。真 iPhone X 上它对应 AppleSecondaryAudio 的 "Hawking"。

响铃/静音开关也要有回答。backboardd 开机时向 HID 系统要电话页 0x0B、用法 0x2E 的当前状态，0 是静音，1 是响铃。按键驱动不回答的话，iOS 一直当成静音。我们在按键驱动里回答响铃（0289）。

音量条和音量键：iOS 自己做软件音量。路由通了以后，扬声器端口的音量范围是 -58 到 0 dB，音量条和实体音量键都能用。

看 VirtualAudio 的详细日志：它每个日志器的级别读偏好域 com.apple.coreaudio 里的 trace* 键（traceroutingmanager、tracerouting、traceport 等），值是 error 到 spew。"Attempting to build concrete route" 这类行要调高级别才打。设备管理器的几个日志器设成 spew 时，它查询设备不支持的属性会出异常，VirtualAudio 初始化失败（aud16），所以只把路由、端口、音量相关的设成 spew。我们用的配置是 `tools/logging/coreaudio_trace.plist`，装到 `/Library/Preferences/com.apple.coreaudio.plist`。路由失败时，日志会写明哪条连接用不了，比如 `Connection [ phki / pap ] is required and cannot be used for a concrete route: source is connected: 0`。

### 6.3 IO 和功放

- `startIO` 先让功放上电并等它（最多 300 ms），然后才记下时钟起点。这样第一块声音出来时功放已经在放了。
- `stopIO` 让功放下电。
- "Hawking" 设备的开停不碰功放。

## 7. 在安卓上量

上面安卓的做法和时间都是在 root 过的安卓上量的：

- 功放寄存器的写入：打开 ftrace 的 regmap 事件 `regmap_reg_write`，要看整块写入再加 `regmap_hw_write_start`，按设备名 13-0040、13-0042 过滤。
- 放声音要走 AudioFlinger。我们用 `app_process` 跑一个只有一个类的小程序，用 AudioTrack（USAGE_MEDIA）放 0.3 秒小声的正弦波。`/vendor/bin/agmplay` 只开 DSP 那一路，不打开功放的混音路由，功放根本不上电，量不出东西。
- 待机时间：`dumpsys media.audio_flinger` 里每个输出线程的 `Standby delay ns`。
- 功放芯片上的真实值：先打开 regmap debugfs 的 cache_bypass，再读 registers。
- 只想测 DSP 那一路时可以用 agmplay。放喇叭的参数在厂商测试程序 `/odm/bin/playback.sh`（是个 ELF 程序）的字符串里：`agmplay 文件 -D 100 -d 100 -i TDM-LPAIF-RX-PRIMARY -num_intf 1 -skv 0xA100000E -dkv 0xA2000001 -ikv 1 -dppkv 0xAC000001`。

## 8. 其他要注意的

- 开机参数最多 607 个字符（loader 的 BOOT_LINE_LENGTH 是 608）。超了的部分会被悄悄截掉。aud24 就是 `pandora_slot` 被截掉，内核没读固件分区，ADSP 没起来，整轮没有声音。现在 `tools/boot_ios.sh` 会检查参数行末尾是否完整，不完整就不开机。
- 内核里映射大窗口或用完要取消的映射，用 `ml_io_map_unmappable(pa, len, VM_WIMG_IO)`，不要用 `ml_io_map`。ADSP 85 MiB 的固件区就是这样映射，拷完就取消。
- 这台手机的 mach 时基是 19.2 MHz。iOS 统一日志里存的是这个计数，解析工具要是当成纳秒，算出来的时间间隔会小 52 倍。

## 9. 还没做

- 麦克风、听筒（通话时 GPIO 174 拉低）和耳机。
- 没有声音时关掉播放图、停 TDM 时钟。安卓会这样做，我们现在只给功放下电，图一直开着。
- 杜比音效。安卓开着 Dolby DAP（效果名 DAP_offload）。它是不是设备后处理子图里的某个模块、在我们开的图里有没有起作用，还没查清。
- ios-sweet 的镜像把 MediaToolbox 的 StartRecorderServer 关了，做录音和相机时要恢复。
- `PandoraAudio.cpp` 里 0293 那套"没被路由就重新登记扬声器"的逻辑是按一个后来证明不对的推测加的，真正的原因是缺 Hawking。现在用不上，以后删掉。
