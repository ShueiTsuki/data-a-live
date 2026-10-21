# 社区现状（2026-10-21 晚核对）

来源：GitHub 搜索（topic:unixnu、"unixnu" 关键词、"xnu sm8850/canoe/8 elite"）和各仓库 README。只记 README 里写明的状态；群里没公开的进展不在这里。
"原生"= 手机上直接跑，不经过安卓；"虚拟机"= 在安卓里开虚拟机跑。

## 跑 iOS（iPhone 系统）的

| 项目 | 设备 / 芯片 | 状态（README 原话概括） |
|---|---|---|
| [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)（822 星） | 红米 Note 10 Pro / SM7150 | 原生，iOS 16.7（iPhone X），进桌面 |
| [bangprovn/ios-fuxi](https://github.com/bangprovn/ios-fuxi)（2026-10-20 建） | **小米 13 / SM8550（骁龙 8 Gen 2）** | 原生，iOS 16.7（iPhone X），**进桌面、触摸和按键能用、能开设置等应用**；ios-sweet 的复刻 |
| ios-topaz（只有报道，没找到仓库地址） | 红米 Note 12 4G / 骁龙 685 | 原生，iOS 16，进桌面 |
| [licg89/UniXNU-dandelion](https://github.com/licg89/UniXNU-dandelion) | 红米 9A / 联发科 | iOS 16，早期 |
| [lvyufeng/ios-le-pro3](https://github.com/lvyufeng/ios-le-pro3) | 乐视 Le Pro 3 / 骁龙 821 | iOS 16，只做了硬件探测 |
| [NoNameBuilder/UniXNU-gta4l](https://github.com/NoNameBuilder/UniXNU-gta4l) | 三星 Tab A7 | 用 ios-topaz 的版本启动到内核控制台 |

## 跑 iPadOS 17（UniXNU 路线）的

| 项目 | 设备 / 芯片 | 状态 |
|---|---|---|
| UniXNU 本体 | 小米平板 6S Pro / SM8550 | 原生，进桌面，GPU 能用 |
| [Droid-VM/UniXNU-droidvm](https://github.com/Droid-VM/UniXNU-droidvm) | 骁龙手机上的 DroidVM **虚拟机** | 8 Gen 3 进桌面；**8 Elite Gen 5（SM8850）上内核能启动**（4K 页） |
| [2426120557/UniXNU-hummer](https://github.com/2426120557/UniXNU-hummer) | 一加 Ace 5 Pro / SM8750 | 原生，UEFI 菜单在真机上能显示、按键能用 |
| [IssacL04/UniXNU-oneplus13t](https://github.com/IssacL04/UniXNU-oneplus13t) | 一加 13T / SM8750 | 原生，UEFI Shell 能启动 |
| [7Night-kqx/UniXNU-dada](https://github.com/7Night-kqx/UniXNU-dada) | 小米 15 / SM8750 | UEFI 在 Mu-Silicium 里能用；UniXNU 部分还没上过真机 |
| [Allenwdk/UniXNU-uke](https://github.com/Allenwdk/UniXNU-uke) | 小米平板 7 / SM7675 | 原生，到苹果内核入口 |
| [Lux12138/unixnu-nabu](https://github.com/Lux12138/unixnu-nabu) | 小米平板 5 / SM8150 | 原生，内核开始执行后早期崩溃 |
| UniXNU-fuxi、vermeer、pipa、piano、amethyst 等 | 各种 | 准备中或还没上真机 |
| [NekoWannn/uefi-hongkong](https://github.com/NekoWannn/uefi-hongkong) | 小米 18 Pro / SM8950 | 只有 UEFI |

## 工具类
- [2010475745xie/ios16-xnu-bringup](https://github.com/2010475745xie/ios16-xnu-bringup)：在 Windows 11 + WSL2 上做 iOS 16 系统镜像、用 QEMU 测内核的步骤和脚本；编译内核仍借 GitHub 的 Mac 云端机器。

## 对我们最有用的：ios-fuxi（小米 13 跑 iOS 16）
- 和我们同一条路线（iOS 16 + iPhone X + xnu-8796，从 ios-sweet 改来），硬件是骁龙 8 Gen 2 旗舰，比红米接近 17 Pro 得多。补丁 0082 以后是它为小米 13 加的。
- **命令模式屏幕已经解决**（补丁 0087）：小米 13 也是 DSC 命令模式屏。ABL 交接时屏幕的"按 TE 信号自动刷新"是开着的，往缓冲区写就会显示；反而手动触发刷新会把画面搞花。17 Pro 很可能一样，要验证。
- **不清数据的双系统做法**（补丁 0088）：iOS 系统镜像作为**一个固定位置的文件放在安卓数据分区里**，内核只按这个文件的块列表读写，越界一律拒绝；不改分区表。
  - **未解决的疑问**：安卓数据分区是加密的，这个文件在存储上的原始内容是不是明文，公开文件里没写。他们安装要用 OrangeFox 恢复模式和 f2fs_io 工具。要弄清楚这点，才能知道 17 Pro 能不能这么做。
- 日志：引导程序在另一个 CPU 核心上跑 USB 控制接口，电脑实时看内核日志、读内存，不用拍照。
- CPU：8 个核心用了 7 个，限制了频率，持续使用约 51 °C。

## 17 Pro（SM8850）原生运行
公开仓库里没找到有人在 8 Elite Gen 5 上原生跑起苹果内核。离得最近的是：上一代 8 Elite（SM8750）的一加 Ace 5 Pro、一加 13T，到了 UEFI 菜单那一步；以及在 SM8850 的虚拟机里启动了内核。
