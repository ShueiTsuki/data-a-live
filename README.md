# Data A Live

在小米 17 Pro（代号 pandora，骁龙 8 Elite Gen 5 / SM8850）上原生运行 iPhone X 的 iOS 16.7，内核是自己编译的苹果开源内核 xnu-8796。

> Running iPhone X's iOS 16.7 natively on the Xiaomi 17 Pro (pandora, SM8850) with a self-built xnu-8796 kernel. Experimental research project; documentation is in Chinese. Formerly `ios-pandora`.

**这是实验性的研究项目，不是能日常使用的系统。** 仓库里只有补丁、驱动和工具，没有苹果的系统文件，也没有手机固件。

## 现在能做什么

| 部分 | 状态 |
|---|---|
| 启动 | `fastboot boot` 临时启动，不刷任何分区；iOS 的根文件系统放在安卓数据分区里的一个固定文件中 |
| 锁屏、主屏幕、App | 能进、能开；后台划掉 App、连续开 App 不卡 |
| 触摸、电源键、音量键、亮度 | 能用 |
| 画面（Adreno 840） | 测试启动时由 GPU 合成整个系统界面：自写的 GPU 内核驱动，Vulkan 用 Mesa Turnip，Metal 用 UniXNU 的 NativeMetal 插件加我们改进的 metal2vulkan 翻译层。屏幕直接读 GPU 画好的图，不再由 CPU 拷屏；120 Hz 屏上滑动动画约 110 帧 |
| CPU | 8 个核全开，按负载调频，空闲时核休眠，过热降频 |
| 音频 DSP | 能启动并通信（SMEM、SMP2P、GLINK、GPR），固件从手机自己的固件分区读；声音还放不出来，喇叭在做 |
| 电池 | 显示的还是固定值；真实电量在做（经 SOCCP 的电池管理器） |
| Wi-Fi、蜂窝网络、相机、背屏 | 不能用 |
| App Store 应用 | 基本不能用（解密依赖苹果芯片里的安全模块） |

详细进度见 [PLAN.md](PLAN.md)，GPU 部分见 [docs/GPU_PLAN.md](docs/GPU_PLAN.md)。

## 目录

| 目录 | 内容 |
|---|---|
| `xnu/patches/` | 对 xnu-8796 的补丁，按编号顺序应用；前面一部分来自 ios-sweet / ios-fuxi，`pandora` 开头的是本机专用的 |
| `wsl/` | 编进内核的驱动源码（GPU、显示、触摸、按键、存储、固件包安装、音频 DSP 等），以及生成补丁、本地编译用的脚本；`wsl/qcom/` 是从 UniXNU-sheng 原样拷来的 SMEM/SMP2P/GLINK/GPR/FAT 代码 |
| `shim/` | `fastboot boot` 启动的小程序：`loader` 代替 iBoot 把内核拉起来，`hello` 是最早的硬件探测程序 |
| `tools/` | 启动镜像、设备树、固件包、信任缓存的生成工具，真机测试脚本，GPU 测试程序（vktest、pgputest、mtltest） |
| `tools/turnip/`、`tools/metal/` | 在 GitHub Actions 上为 iOS 16 交叉编译 Turnip 和 NativeMetal 插件 |
| `docs/` | GPU 移植计划和对照笔记 |
| `device/` | 只放说明文档；设备导出的原始数据不进仓库 |
| `.github/workflows/` | 内核（`xnu.yml`）和 GPU 用户态（`turnip.yml`）的自动编译 |

## 和 ios-sweet、UniXNU 的关系

- **ios-sweet 一系**：本项目沿用 [ios-sweet](https://github.com/dipa-studio/ios-sweet) 的做法，在骁龙手机上原生跑 iPhone X 的 iOS 16，驱动直接编进 xnu 内核，以补丁的形式维护（`xnu/patches/`）。前面一部分补丁来自 ios-sweet 和 [ios-fuxi](https://github.com/bangprovn/ios-fuxi)，`pandora` 开头的补丁是本机专用的。
- **UniXNU**：本项目**不是** UniXNU 的机型仓库（`UniXNU-<机型>`）。UniXNU 跑 iPadOS 17，有自己的构建系统，驱动是独立模块。本项目借用了 UniXNU 的 GPU 组件：NativeAdreno 的用户接口、ArmSmmu4K，以及 Metal 插件 NativeMetal 和配套的 Turnip 补丁。按 UniXNU 的[贡献指南](https://unixnu.com/zh/contributing/repository)带了 `unixnu` 话题。
- **以后**：对别人有用的驱动（首先是 Adreno 840 GPU 驱动）会拆成独立仓库，带 `unixnu` 话题，本仓库改用子模块引用。

## 安全

- 不刷写手机分区，不清数据；所有启动都是 `fastboot boot`，只在内存里，重启就回安卓。
- iOS 只读写它自己的那个固定文件：内核的存储驱动只认这个文件占用的存储块，不会访问之外的地方；不加 `ufsrw=1` 时完全只读。
- 仓库永远不放：苹果的系统文件（IPSW、dyld 共享缓存、SDK）、高通和小米的固件、手机分区备份、设备导出和日志。`.gitignore` 已挡住这些目录。
- 自己动手前，先完整备份手机；如果出现意外情况，后果需要自己承担。

## 致谢

- [ios-sweet](https://github.com/dipa-studio/ios-sweet)：在骁龙手机上原生跑 iOS 16 的开创项目，前面的内核补丁和启动方式都基于它。
- [ios-fuxi](https://github.com/bangprovn/ios-fuxi)：ios-sweet 在骁龙 8 Gen 2 上的移植，提供了大量参考。
- [UniXNU](https://github.com/UniXNU/UniXNU)：4K 页内核、存储驱动，以及 NativeAdreno、NativeMetal、ArmSmmu4K 等 GPU 组件；[UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) 的 SMEM、SMP2P、GLINK、GPR、FAT 读取代码。
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa)（Turnip Vulkan 驱动）、[metal2vulkan](https://github.com/steelbrain/metal2vulkan)（Metal 着色器翻译）。
- Linux 内核的 msm/Adreno 驱动：GPU 寄存器和启动顺序的参考。

## 支持开发

开发很费钱和时间的呜呜呜，全部的打赏将会用于之后的维护以及本人购买 [In Falsus](https://store.steampowered.com/app/3971950/)。

<img src="docs/wechat-reward.jpg" alt="微信赞赏码" width="240">

打赏与苹果公司无关，不购买任何软件，也没有付费功能或付费版本；本项目永远不提供苹果的系统文件。

## 许可证

内核补丁和编进内核的源码使用 APSL 2.0（与 xnu 相同）；其余本项目的代码和文档使用 MIT；第三方文件保留各自的许可证。详见 [LICENSE](LICENSE)。
