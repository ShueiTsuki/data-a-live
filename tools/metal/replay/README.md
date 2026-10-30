# m2v-replay

在电脑上重放手机里的一次 Metal 绘制。它把绘制交给 metal2vulkan，跑在 lavapipe（Mesa 的软件 Vulkan）上，输出画出来的图、翻译后的 SPIR-V 和 metal2vulkan 的提示。2026-10-25 查 App 资源库背景时用过。

## 用法

1. 手机上用启动参数 `pandora_astc_probe=1` 跑一轮，插件会把探测到的绘制连同提交时的缓冲内容写进日志（"replay" 段）。把这些段拼成一个文件，例如 `device/logs/bb41-replay.txt`。
2. `tools/capture2spec.py bb41-replay.txt DRAW METALLIB` 把其中的绘制转成一份 spec。METALLIB 是这次绘制用的着色器库，从你自己的 iOS 根文件系统里取（例如 QuartzCore 的 default.metallib）。
3. `replay.sh SPEC [OUT]` 跑这份 spec。

`Cargo.toml` 里 metal2vulkan 的路径写的是作者 WSL 里打过补丁的那份（`/opt/work/lplugin/metal2vulkan`），用之前改成你的。lavapipe 不支持 ASTC，重放时只解码了纯色块。
