# 日志和二进制分析脚本

这些脚本在 WSL 里用 `python3` 运行。用法写在每个文件开头的说明里。

## iOS 统一日志

先用 `tools/ioslog_fetch.py NAME --minutes N --parse` 从手机里取日志，它会生成 `/opt/work/ul_NAME.jsonl`。下面的脚本读这个文件。

| 脚本 | 做什么 |
|---|---|
| `ul_grep.py` | 只取最后一次开机里进程名或内容符合正则的消息，按时间写成文本 |
| `ul_lastboot.py` | 统计最后一次开机里每个正则命中多少次，按进程分 |
| `ul_proc.py` | 列出一个进程在最后一次开机里的消息，相同的文字合并计数 |
| `ul_seq.py` | 按日志顺序取一个进程连续的若干条消息，用来看一个循环的一整圈 |
| `ul_since.py` | 以一条内核消息定开机时刻，统计之后某进程的消息 |
| `ul_top.py` | 一个进程出现最多的消息 |

我们的解析器给出的时间间隔会被压缩，不能当真实秒数用，只能用来看先后顺序。

## 线程快照

内核启动参数 `pandora_stacks=NAME:SECONDS` 会把这个进程所有线程的栈打进串口日志。

| 脚本 | 做什么 |
|---|---|
| `stacks.py` | 按进程和线程汇总快照：每个线程停在哪、停了几次。加 `--addrs` 输出要符号化的地址 |
| `sym.py` | 给 `stacks.py --full` 的输出补上符号。共享缓存里的地址用 `ipsw dyld a2s` 查 |

## dyld 共享缓存和 Mach-O

| 脚本 | 做什么 |
|---|---|
| `dscdis.py` | 反汇编共享缓存里某个地址开始的一段 |
| `dscstr.py` | 读共享缓存里某个地址处的 C 字符串 |
| `dscsel.py` | 读一个选择子引用或类引用指向的名字 |
| `dscfind.py` | 找出哪些映像里有某个字符串 |
| `kcread.py` | 读 Mach-O 里某个地址处的若干个 32 位字 |
| `nmsym.py` | 用 `llvm-nm` 找离某个偏移最近的前一个符号 |
| `chain_counts.py` | 统计 A11 驱动每个类和它各级父类各有多少个方法 |

Metal 的对照表在 `tools/metal/conformance.py`。
