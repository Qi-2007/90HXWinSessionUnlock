# Lower Filter 三次蓝屏转储对比

使用 WDK CDB 和 `H:\Windows Kits\10\Debuggers\x64\sym` 中匹配的 Microsoft 符号离线分析。
初次网络符号路径未解析内核符号，产生 WRONG_SYMBOLS；已用本地缓存重新分析并覆盖输出，
以下结论只取重新分析结果。NVIDIA 无私有符号，不能把责任标记偏移命名为内部失败函数。

| 转储 | 崩溃时间（UTC+8） | 对应实验日志 | Lower Filter 编译时间 | 从窗口返回到崩溃 |
| --- | --- | --- | --- | --- |
| 100826-13281-01.dmp | 16:11:25.023 | filter-unlock.log，完整解锁、flags=0x3 | 15:20:00 | 约 80 秒 |
| 100826-11125-01.dmp | 17:08:43.160 | filter-unlock-051.log，保留 DMA 资源、flags=0x23 | 16:32:20 | 约 160 秒 |
| 100826-13437-01.dmp | 17:54:42.214 | filter-sbr-052.log，单次 SBR、flags=0x43 | 17:42:46 | 约 78 秒 |

实验对应关系按同日时序、已有日志及编译时间关联；时间间隔是从日志窗口返回起算，
不是首次 GPU 超时起算。0.5.2 的 60 秒观察于 17:54:24.626 结束，约 18 秒后崩溃。
不能以观察进程正常退出认定 NVIDIA 恢复正常。

## 共同结果

- Bugcheck：`0x116 VIDEO_TDR_FAILURE`。
- Arg2 指向同一 NVIDIA 镜像的相同责任标记偏移：`nvlddmkm+0x1935300`。
- Arg3：`0xc000009a`，Arg4：`4`。
- Failure bucket：`0x116_IMAGE_nvlddmkm.sys`。
- NVIDIA 镜像时间戳 `0x6A871D5D`、checksum `0x06CAD4C8`、size `0x06D39000` 一致。
- 崩溃栈均为 `nt!KeBugCheckEx ← dxgkrnl!TdrBugcheckOnTimeout ←
  ADAPTER_RENDER::Reset ← DXGADAPTER::Reset ← TdrResetFromTimeout ←
  TdrResetFromTimeoutWorkItem ← nt!ExpWorkerThread`。
- 三个转储的过滤驱动时间戳和 checksum 不同，确实加载了不同构建。

这三次崩溃均处于显示驱动超时恢复失败路径，不是此前客户的 `0xE6/0x28`。
责任模块标记不表示 NVIDIA 自身是最初破坏硬件状态的一方；当前过滤驱动 SBR 是明确相关干预。
Arg3 是最后失败操作状态，即使名字为 STATUS_INSUFFICIENT_RESOURCES，
也不能仅凭它证明系统 RAM 或 common-buffer 分配耗尽。

## 和 PCI 日志结合后的结论

单次 SBR 的 MSI 从 control=0x0081/address=0xfee006f0 变为
control=0x0080/address=0，NVIDIA 接手后 60 秒仍未恢复。
该实验无 common-buffer/adapter 分配、无完整解锁固件，仍进入与完整解锁相同的 TDR 失败路径。
因此完整固件、arena 释放、22 次 SBR 或 20 秒窗口不是本次复现的必要条件。
当前优先处理复位后的 PCI 中断配置与 NVIDIA 保存上下文的同步恢复。
MSI 丢失是直接记录的缺陷，尚不能证明是唯一原因。

不要重复当前破坏性 D0 SBR 测试，也不要直接回写 MSI 当作完整修复。
下一方向为 Windows 管理的设备栈重新初始化，并设计过滤驱动 DMA/PDO/remove-lock 生命周期交接。

## 分析边界与输出

三个文件均为 mini kernel dump。公开符号未提供 `dxgkrnl!_TDR_RECOVERY_CONTEXT` 类型，
无法可靠解码 TimeoutReason；不以其他 Windows 版本的结构偏移猜测。
`!blackboxpnp` 中 problem=24 的记录对象为显示器或 Intel HDAUDIO，
不能把它当作目标 CMP GPU 的 Code 24 或本次 SBR 根因。

CDB 原始分析输出和补充上下文输出均放在固定目录 `build/filter-driver`：
`100826-*-analysis.txt` 和 `100826-*-tdr-context.txt`。

参考：[Microsoft 0x116 定义及参数](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x116---video-tdr-failure)。
