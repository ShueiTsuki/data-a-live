# 喇叭：sheng 的做法和 pandora 的差别（2026-10-26 调查摘要）

## 安卓上实际怎么做（DeepSeek 在手机上查的，`D:\DeepSeekHarness\speakercheck\RESULT.md`，我已抽查，见同目录 VERIFY.md）

- 生效的配置在小米 odm 分区：`/odm/etc/audio/sku_canoe/resourcemanager_canoe_mtp.xml`、`mixer_paths_canoe_mtp.xml`、`/odm/etc/usecaseKvManager.xml`。
- 喇叭后端 `TDM-LPAIF-RX-PRIMARY`（LPASS 主 I2S0，GPIO122 SCK、123 data0、124 data1、125 WS）：48 kHz，每槽 32 位，一帧 2 槽，位时钟 3.072 MHz。功放的帧格式 `0x4004` = DSP_A、时钟不反相、功放是从机（DeepSeek 把反相解错了，见 VERIFY.md）。
- 上面那颗（T，0x42）收槽 0，下面那颗（B，0x40）收槽 1；两颗的 ASPTX1 也用槽 0/1，回送给 DSP 当参考。
- 功放跑自己的扬声器保护固件：`/odm/firmware/cs35l43-dsp1-spk-prot.wmfw`，加两颗各自的 `T-/B-cs35l43-dsp1-spk-prot.bin`，再加按场景切换的调音 `T-/B-cs35l43-delta-{music,movie,video,navigation,voice}-spk-prot.bin`，默认 music。
- 出厂校准：`/mnt/vendor/persist/audio/crus_calr.bin`（每台手机不同，两颗各一个值），由用户空间的 `libcrussp.so` 写进各自的 `DSP Set CAL_R`。
- 喇叭路径的功放设置：`PCM Source = DSP`、`ASPTX Ref = Ref`、`AMP Enable = 1`、`Noise Gate = 16245`、`Amp Gain = 18`。
- DSP 上的流程：共享内存端点 → PCM 解码 → PCM 转换 → SPR（扬声器保护）→ TDM 端点（模块 0x0700100E），48 kHz / 32 位 / 2 声道。设备 KV 是 `0xA2000000 = 0xA2000001`、`0xAC000000 = 0xAC000100`。另外还开了一条回声参考的发送图。
- 放音时安卓的内核会向 DSP 发 PRM 的硬件资源申请和释放（0x0100100F / 0x01001010），多半是 TDM 位时钟。内容还没查到。
- 主处理器这侧开着 Dolby DAP 音效，44.1 kHz 的内容在 AudioFlinger 里先转成 48 kHz。
- 听筒：同一颗 T 功放。GPIO174 高是喇叭、低是听筒；听筒路径功放直通，增益 13。

由一个只读的助手读 UniXNU-sheng（HEAD d76556b）的音频代码和我们的设备树后整理（英文原文，未改）。已抽查：pandora 两颗 CS35L43 在 `i2c@1980000`，地址 0x40（"B"，复位 GPIO10，中断 GPIO11）和 0x42（"T"，复位 GPIO54，中断 GPIO47）——与设备树一致。其余条目动手前逐条对照原文核实。

Paths: **sheng** = `D:\DeepSeekHarness\claude\unixnu_review\UniXNU-sheng\` (AP = `package/AudioProbe.cpp`, AR = `package/ShengAudioReach.c/.h`, CS = `package/ShengCs35l43.c/.h`, ABI = `package/ShengAudioAbi.h`, SVC = `package/ShengAudioService.cpp`, PI = `hal/ShengAudioPlugIn.c`, PT = `porting.toml`); **DTS** = `device/pandora-live.dts`.

## 1. Sheng's speaker path

One kernel thread, in order (AP:1071-1106): DSP buffer and shared rings, SMMU, pins, clocks and I2C, amplifiers, `adsp_apps`, SPF ready, graphs, service, serving the plug-in.

**Amplifiers.** Six CS35L43 on QUP1 SE5 (`0x00a94000`) and QUP2 SE0 (`0x00880000`), amp n at `0x40 + n%3` on bus `n/3` (AP:80, 266-268; CS.h:2). GENI I2C 400 kHz from 19.2 MHz (AP:89-90, 354-382). Shared reset GPIO90: low 2 ms, high, wait 3 ms (AP:322-328). 32-bit big-endian register address and value (CS.c:96-125). Per-amp program (CS.c:10-49): DEVID/REVID; errata `0x7404=0x11330000`, `0x3850=0x50000802`; mask interrupts; DC input watchdog; boost peak 4 A; SDOUT hi-Z; GPIO2 source 4; VPBR 8/3/5; serial port I2S, 16-bit slots and words, 48 kHz; PLL on the 1.536 MHz bit clock (code `0x1B`, FS monitor 71/115); DAC input ASP RX1 (`0x08`); RX1 slot 0 top three, slot 1 bottom three (CS.h:61); gain 18 dB, 4 ms soft ramp. Power-up `0x2084=0x000f1aa0` under the test key, then GLOBAL_ENABLES and BLOCK_ENABLES; power-down mirrors (CS.c:51-78). No amp firmware, amp DSP bypassed (CS.h:3-4; PT:184). Amps up before the graphs (AP:596-599); PLL lock (EINT2 bit 0) read after 10 periods (AP:399-414, 695).

**DSP graph** (GPR on `adsp_apps`, our port `0x2001`, APM instance 1; AP:69; AR.c:61-71; sequence AP:749-759):
1. `GRAPH_OPEN 0x01001000`, device subgraph `0x4005`: container GC `0x0B001001`, ADSP, stack 8192; module `I2S_SINK 0x0700100A` instance `0x6051` (AR.c:86-132).
2. `SET_CFG 0x01001006` on `0x6051` (AR.c:176-188): `I2S_INTF 0x08001019` = {lpaif 0, interface 1 (secondary), SD line 2, WS internal}; `HW_EP_MF 0x08001017` = {48000, 16, 2}; `FRAME_SIZE_FACTOR` 1.
3. `GRAPH_OPEN` stream subgraph `0x4001`: `WR_SHMEM 0x07001000` (`0x6001`) → `PCM_DEC 0x07001005` (`0x6002`) → `PCM_CNV 0x07001003` (`0x6003`) → `0x6051` (AR.c:134-142).
4. PCM output `0x08001008` on decoder (interleaved) and converter (deinterleaved, unpacked) (AR.c:207-227).
5. `MEDIA_FORMAT 0x0800100C` to `0x6001` (AR.c:240-255).
6. `MEM_MAP 0x0100100C`: pool 3, one 64 KiB region; answer `0x02001001` has the handle (AR.c:269-278; AP:229).
- Start: PREPARE then START, device subgraph first; stop in reverse (AP:581-587). No PRM clock votes, no ACDB for the speaker path.

**Samples.** 64 KiB at `0xd0000000` (sheng's free gap), write-combined; DSP address `pa | 1ull << 32` (SMMU stream index in the upper word) (AP:297-313). Streams `0x1001`, `0x1081`, `0x1061` pass-through on the shared bypass bank; a translating context reset the board (AP:88, 281-295; PT:184). Periods 480 frames × 2 ch × s16 = 1920 B, 8 slots; capture at +32 KiB (AP:70-77). Per period: copy from the shared ring, clear, `dc cvac`, `WR_SH_MEM_EP_DATA_BUFFER_V2 0x0400100A` to `0x6001` (AP:544-579; AR.c:289-297). Four in flight; each `WRITE_DONE 0x05001004` refills one (AP:684-694).

**Timing commits.** 56551c9 ring wrap, fixed timeline, output up 5 s after stop. 4583f03 128-bit ticks-per-frame (625/12 timebase overflowed 64 bits), buffer answers during commands handled, capture byte counts, resync after 3-period drift. 1b75289 ring 65536 frames, minimum lead 300 ms, plug-in mixes each IO context. dea464e CoreAudio assumes a 24 MHz host clock: the plug-in finds the constant in `AudioGetHostClockFrequency` (adrp/ldr) and user-client selector 4 rewrites it on the shared-cache page; zero timestamps period 65536, Raw algorithm; result 4822 of 4843 IO cycles 15–25 ms apart, 0 late (PT:213).

## 2. iOS side (sheng, iPadOS 17)

CFPlugIn AudioServerPlugIn `ShengAudio.driver` (hal/Info.plist:25-36; PI:681-690), no IOAudio kext; arm64 for ios17.0 with macOS CoreAudio headers, ad-hoc signed, CDHash in the trust cache (scripts/build-sheng-audio-hal.py). Installed by a kernel union mount of `/private/var/ngpu/audio` over `/System/Library/Audio/Plug-Ins/HAL` (patches/xnu/zzyd-sheng-audio-addon-mount.patch). Loaded by `audiomxd`/`mediaserverd`. Kernel IOService `ShengAudio` with `ShengAudioUserClient` (SVC:72-83, 128-143), registered via `OSMetaClass::preModLoad` (AP:1062-1069); sandbox: property `NativeSandboxGrantProcess` + UniXNU patch `zzw-native-adreno-sandbox-grant`. User client: memory 0 control page, 1 output ring (65536×2 s16), 2 input ring; selectors 0–3 start/stop out/in, 4 host clock constant address (ABI; SVC:107-123). Device: Float32 48 kHz, stereo out, mono in, volume −36..0 dB; transport Thunderbolt (VirtualAudio drops built-in/USB from a plug-in); safety offset lead + 960 frames.

## 3. Channels

Speakers need only `adsp_apps` (answering side, 20×512 intents). The DSP offers 24 intents of 128 bytes and grants a 4 KiB one on request (graph-open packets need it). PRM (GPR instance 2) only for microphones. IPCRTR service locator is general infrastructure, not used by AudioProbe. SoundWire only for the WCD938x microphones.

## 4/5. Board values and reusable pieces

Board: amp count/buses/addresses, GCC votes, I2C pins, reset GPIO, I2S pins, slot map, gain/boost/VPBR, I2S interface and SD line, buffer address, stream IDs, mic path. Reusable: ShengGpr.c; ShengAudioReach.c (interface number and SD line to become parameters); the CS35L43 program runner (slot, gain, VPBR to become parameters); GeniI2c.c; SmmuBypass.h; the ABI; mostly SVC and PI (depend on UniXNU sandbox-grant and addon-mount patches and iPadOS 17 behaviour); the serving logic in AP.

## Pandora

- **Amps** (DTS:21489-21538): two on `i2c@1980000`; 0x40 "B" reset GPIO10 irq GPIO11; 0x42 "T" reset GPIO54 irq GPIO47, extra pin states on GPIO174 (rcv high/low, DTS:6305-6331). Both: VPBR 4/6/5 enabled, `low-pwr-mode-standby` 1, GPIO2 source 4, SDOUT hi-Z 3, boost peak 4000 mA. No `#sound-dai-cells`; not linked from the sound card.
- **I2C**: `0x1980000` size `0x4000`, SPI `0x175`, QUPv3 wrapper 3 `0x19c0000`; SDA GPIO64, SCL GPIO65. `pandora_amp.c` already probes both amps (DEVID `0x35A43`, REVID `0xB1`, log t4-touch) with FIFO-mode I2C at 100 kHz; its GCC/TLMM maps use `ml_io_map` and must move to `ml_io_map_unmappable`.
- **Interface**: TDM0 / LPASS I2S0 confirmed: GPIO122 `i2s0_sck`, 125 `i2s0_ws`, 123 `i2s0_data0`, 124 `i2s0_data1` (8 mA, no bias); 121 is `audio_ext_mclk0`. Sound card `qcom,canoe-asoc-snd`, `tdm-max-slots` 2, `lpass_audio_hw_vote` clock.
- **GPR/PRM/SMMU**: `qcom,gpr` identical to SM8550 downstream; audio SMMU SIDs `0x1001` mask `0x80`, `0x1041` mask `0x20`; apps SMMU `0x15000000`.
- **ref/cs35l43**: Cirrus GPL driver (no TDM slot code); the phone's `cs35l43_dlkm.ko` is newer (TDM slots, spk-prot .wmfw, delta tuning .bin).

## Differences that matter

1. 2 amps, one bus, separate resets; GPIO174 on the top amp has no sheng counterpart.
2. Primary interface (0) on pins 122–125; which data line plays is unknown; Android probably runs TDM (2 slots); sheng has no TDM sink code.
3. VPBR 4/6 vs 8/3.
4. SMMU: also stream `0x1041`.
5. Our own free 64 KiB below 4 GiB for the DSP buffer.
6. GPR and intents identical.
7. Timebase 19.2 MHz here too: the 128-bit math is needed; the host-clock patch if iOS 16 CoreAudio also assumes 24 MHz.
8. Sheng's iOS findings are iPadOS 17 / arm64e; ours iOS 16 / A11 arm64: recheck process names, HAL directory, transport trick.
9. UniXNU pieces we may lack: addon mount, sandbox grant, trust-cache insertion, kernel class registration.
10. No speaker protection: start below sheng's 18 dB.

## Open questions (phone or Android side)

1. TDM or I2S, slot width/count, which amp takes which slot, bit clock (Android: `dmesg | grep slots=`, regmap dump while playing of `0x4808`, `0x4820`, `0x4840`, `0x2C04`, `0x4C00`, `0x6C04`).
2. Data line: GPIO123 or GPIO124.
3. Does the SM8850 I2S/TDM endpoint give the bit clock without PRM/hardware votes (check amp PLL lock after graph start)?
4. GPIO174 level for the top amp as a speaker.
5. SMMU on SM8850: shared bypass bank works? translating context resets the board?
6. A free low-DRAM 64 KiB region.
7. iOS 16: does `mediaserverd` load HAL plug-ins; same adrp/ldr for the 24 MHz constant; accepted transport type.
8. Android's amp firmware, tuning and gain under `/vendor/firmware`, for a safe level.
