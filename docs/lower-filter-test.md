# Lower Filter 实机实验：0.5.6

## 0.5.6：减少 PCI 扫描开销，定位渲染程序等待

用户反馈0.5.5可以正常解锁和接管；解锁过程中依赖显卡渲染的程序会卡住，完成后恢复。
这并非已确认CPU忙等：当前两轮禁用／启用及长START窗口使设备暂时不可供NVIDIA渲染。
0.5.6仅优化PCI桥查找：读取function0，按多功能标志查询其余function。
通常身份探测从65536次减至8192次加真实多功能设备；仍重新验证唯一桥，
SBR子树中其他设备的排除仍全函数扫描，DMA回收门槛和全部复位／固件时序保持原样。
新增FILTER_TOPOLOGY_SCAN及FILTER_STAGE_TIMING，量化扫描和各解锁阶段耗时。
不能宣称该优化消除渲染等待，长期持有START和设备不可用窗口仍存在。

测试命令与0.5.5相同，配套EXE未更改，其HANDOFF_BEGIN版本标记仍表示0.5.5命令流程；
新SYS日志FILTER_SOURCE_CORE中的filter=0.5.6用于确认加载了新驱动。
签新SYS、重建并签CAT、安装INF、重启后运行：

```powershell
.\CMP90HXUnlocker.exe filter-unlock-handoff --bdf 02:00.0 --observe-seconds 180 --log filter-handoff-056.log
```

当前Lower Filter在设备被禁用时随REMOVE删除设备对象并释放PnP资源；
不能继续用旧BAR／DMA映射解锁。当前利用重新启用后的下层START窗口完成解锁，
然后第二轮启动NVIDIA。渲染等待的无中断解决方案尚未实现。
参考：[禁用设备触发REMOVE及资源释放](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-an-irp-mn-remove-device-request)。

---

# Lower Filter 实机实验：0.5.5

## 0.5.5：解锁后重新启用设备，让 NVIDIA 初始化接管

本版接入完整解锁和两轮自动禁用／启用，保留 NVIDIA 驱动。可在正常启动后，
或正常睡眠唤醒后运行；不要求在启动／恢复回调中自动解锁。本版尚未实机验证。
输出仍固定为 `build/filter-driver`，SYS 与配套 EXE 必须一起更新。

签名 SYS 后重新生成并签名 CAT，安装本目录 INF，然后重启使 0.5.5 生效。
之前失败的复位或解锁可能保留 DMA／PDO，必须先重启，不直接串接旧实验。
在这个目录的管理员 PowerShell 执行：

```powershell
.\CMP90HXUnlocker.exe filter-unlock-handoff --bdf 02:00.0 --observe-seconds 180 --log filter-handoff-055.log
```

无需另一终端或睡眠操作。命令只操作该 BDF 对应的固定 90HX 设备实例，要求起始
NVIDIA 服务为 nvlddmkm、Code 0、已启动。禁用采用非持久化标志，重启仍可恢复。

流程：
1. 武装第一轮 START，禁用／启用目标设备，在 lower START 成功后的 PASSIVE worker 执行完整解锁。
2. 最后一次验证成功的 SBR 必须晚于所有 GSP／SEC2 寄存器写入；另外验证身份、bootstrap、零存活子分配和停止／空闲 GSP。满足条件才释放 common-buffer、DMA adapter、额外 PDO 引用。
3. 第一轮主动失败 START，预计短暂 Code 10，避免 NVIDIA 使用被 SBR 清掉的 MSI。日志必须包含 `FILTER_PNP_DMA_RELEASED`，完整结果 stage=12/status=0/flags=0x603（没有 retained/quarantine）。这只表示解锁及 DMA 释放完成。
4. 用户态等待结果和 Code 10，再禁用／启用同一设备。第二轮未武装，过滤驱动透明转发 START，由 Windows 配置中断、NVIDIA 正常初始化。
5. 要求 Code 0、DN_STARTED，记录 PCI/MSI 和180秒观察。`FILTER_PNP_HANDOFF_COMPLETE` 表示 devnode 已启动，仍需功耗读取和实际负载验证。

成功交接后再次正常睡眠唤醒，可重新执行相同命令测试后续解锁。当前是手动测试入口，
尚未接入 GUI 的启动自动解锁或恢复事件。

若核心失败、DMA 释放门槛未通过或 quarantine，不执行第二轮，也不循环重试；
保留可能仍被硬件引用的 DMA，重启结束本轮。若停在中间 Code 10 或第二轮失败，
保留完整日志并重启。不能把第一轮 Code 10、核心解锁成功或无蓝屏单独作为最终通过。

签 SYS 后生成 CAT 示例（再使用你的签名方式签 CAT）：

```powershell
& 'H:\Windows Kits\10\bin\10.0.28000.0\x86\Inf2Cat.exe' /driver:. /os:10_GE_X64 /uselocaltime
pnputil /add-driver .\CMP90HXLowerFilter.inf /install
```

禁用／启用在用户态执行，绝不在持有本设备 START/remove-lock 的内核 worker 中重启设备。
参考：[CM_Disable_DevNode](https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_disable_devnode)。

---

# Lower Filter 实机实验：0.5.4

## 0.5.4：一次 Windows 总线函数级复位

**实机更新：本时序未通过。** 复位调用和完成回调成功（219ms），但MSI被清空，
配置检查返回0xc0000182、flags=0x186。过滤驱动主动以硬件错误失败START，随后设备栈
REMOVE，表现为Code 10和软件无法看到显卡。没有蓝屏不代表恢复通过。
请先重启系统、保持未武装透明路径；不要重复下面的0.5.4复位命令。
后续需协调复位与总线START重新配置的先后顺序。详见研究记录。

0.5.3 已捕获目标设备 REMOVE/新实例 START，MSI 保持启用，用户反馈功耗恢复且没有蓝屏。
0.5.4 新增 `filter-start-reset-probe`：在相同 START 窗口，通过总线返回的 v1 接口
调用一次 `FunctionLevelDeviceReset`，flags=0。不会调用平台级复位，不写 SBR、BAR、MSI，
不映射 GPU MMIO、不申请 DMA、不提交解锁固件。本版尚不解锁。

调用前验证目标 PCI 身份/配置、接口类型/长度/支持位/回调和中断配置可验证性。
当前不读取 MSI-X 表项，因此遇到已启用 MSI-X 时拒绝复位。
调用返回后若接受请求，最多等30秒完成回调；STATUS_PENDING 不能当作完成。
完成成功后重新通过 BUS_INTERFACE_STANDARD 读取 PCI，要求身份、六个 BAR、Command
及 MSI control/address/data（如有则包括mask/pending）保持原值。
此处是保守一致性检查，不证明中断实际投递或 NVIDIA 上下文正常。
如果已提交复位后失败、超时或配置丢失，失败本次 START、禁止再武装，不让 NVIDIA
在未知设备状态上继续初始化。未提交时的诊断失败仍保留总线 START 状态。

回调参数放在非分页内存，不引用栈、IRP 或设备扩展。
一次提交后保留这个小上下文和总线接口引用到系统重启，允许超时后迟到回调仍安全访问；
每次 Windows 启动最多提交一次，防止保留对象累积。不是释放 DMA 内存的方案。
30秒只约束调用返回后的回调等待，不能中断总线实现内部的同步调用。
当前没有自动取消、重试、手工修复 MSI 或完整解锁。

签名安装本版并重启后，在固定目录管理员终端运行并保持打开：

```powershell
.\CMP90HXUnlocker.exe filter-start-reset-probe --bdf 02:00.0 --seconds 600 --observe-seconds 180 --log filter-start-reset-054.log
```

在另一管理员 PowerShell，按0.5.3步骤只执行一次目标设备 `pnputil /restart-device`，
不需要睡眠。本轮是实际复位实验，复位后 NVIDIA 兼容性还未实测；若设备启动失败，
重启系统结束本轮，不继续旧 SBR/恢复解锁测试。
成功必须有 `FILTER_START_FUNCTION_RESET_BEGIN`、RETURN、COMPLETE（status=0/callback=1）、
`FILTER_START_RESET_CONFIG_VERIFY status=0/preserved=1` 和 stage=12/flags=0x183。
复位接口返回成功本身不足以判定本轮通过，还需180秒观察与功耗读取恢复。
提供 `filter-start-reset-054.log`；如果卡在 BEGIN、COMPLETE超时或配置检查失败，
这些阶段的日志即可决定下一步，不重复复位。

flags bit8（0x100）表示总线函数级复位实验，bit1表示已调用，bit2表示已提交后失败。
本版没有修改 IOCTL 数据大小。旧只读 `filter-start-probe` 仍可用。
待验证其是否能保留 PCI 配置并允许 NVIDIA 正常初始化后，才考虑用于解锁交接。

本版已通过 WDK 编译、匹配 worker 摘要检查、INF 校验、CAT 可签名检查及内核导入检查。
尚未签名、加载或执行实机复位。签 SYS 后重新生成并签 CAT，再安装 INF 和重启。

参考：[DeviceReset](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-device_reset_handler)、
[函数级复位完成参数](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_function_level_device_reset_parameters)。

## 0.5.3：PnP START 只读窗口

实机更新：用户反馈设备重启后功耗恢复、没有蓝屏；真实 REMOVE/新实例 attach/START
已捕获，flags=0x81、总线配置读取成功。MSI 在 START 窗口及后续125秒均启用，
复位接口 supported=0x1（支持函数级，不支持平台级，未实际调用）。
当前文本/日志未包含180秒结束标记；本轮证明正常设备重启路径可用，尚未证明完整解锁兼容。

三个 dump 已确认完整解锁、保留 arena、单次 SBR 均进入相同 NVIDIA TDR 失败路径。
本版增加 `filter-start-probe`，武装下一次目标设备的 PnP START；普通睡眠 D0 不消耗此武装。
在下层 START 成功后，由 PASSIVE worker 查询 `BUS_INTERFACE_STANDARD`，只读 256 字节
PCI 配置，记录 BAR/Command/MSI/MSI-X、翻译内存/中断资源，并查询 v1 设备复位接口支持。
不会调用复位接口，不做 SBR/MMIO/DMA 分配/固件提交；诊断失败也保留原 START 返回状态。
接口引用在 worker 内释放，remove-lock 持有到工作结束；控制句柄跨设备栈重建保持打开。
本版尚未接入 START 完整解锁，也不能凭下层 START 完成宣称 NVIDIA 尚未进行任何硬件访问。

签名安装匹配的 0.5.3 EXE/SYS 后重启，避免上次破坏性测试保留的 arena/PDO。
在固定 `build/filter-driver` 目录的管理员终端运行并保持打开：

```powershell
.\CMP90HXUnlocker.exe filter-start-probe --bdf 02:00.0 --seconds 600 --observe-seconds 180 --log filter-start-053.log
```

在另一管理员 PowerShell 只重启唯一的目标 90HX 设备（由 Windows 管理设备栈启停）：

```powershell
$cmpTargetDevices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_10DE&DEV_220D*' })
if ($cmpTargetDevices.Count -ne 1) { throw '必须只有一个目标 90HX，先核对设备实例 ID。' }
$cmpTargetDevices | Format-List FriendlyName,Status,InstanceId
pnputil /restart-device $cmpTargetDevices[0].InstanceId
```

本轮不要睡眠，不运行旧 SBR/恢复解锁命令。观察重启后功耗读取能否恢复，并保留日志。
若 pnputil 要求整机重启，这轮不会取得在线 START 窗口，结束探针即可，不能算通过。
成功证据为 `FILTER_START_WINDOW`、`FILTER_START_BUS_INTERFACE status=0 bytes=256`、
`FILTER_START_RESET_INTERFACE`、`FILTER_START_PROBE_END`，以及 stage=12/flags=0x81。
复位接口不支持时会记录其 NTSTATUS，不影响只读 PCI 证据是否成功。
`supported` 位0=函数级、位1=平台级，仅为总线报告的支持情况，不代表已协调 NVIDIA 使用。
EXE 继续只读观察 180 秒，覆盖前三轮约 78～160 秒后的崩溃时间；
PCI/MSI 与功耗恢复仍需一起判断，退出码0不是解锁成功。
新 flags bit7（0x80）表示 START 只读结果，IOCTL 包大小保持不变。

此轮验证 PnP 重新初始化的时序和资源，后续才决定正式解锁入口及 DMA 生命周期。
参考：[IRP_MN_START_DEVICE](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-start-device)、
[Windows 设备复位接口](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/working-with-guid-device-reset-interface-standard)。

本版已通过 WDK 编译、匹配 worker SHA256 检查、INF 校验、CAT 可签名检查和内核导入检查。
未签名、未加载；本轮尚无实机结果。先签 SYS，重新生成并签 CAT，再安装 INF 和重启系统。

0.5.0 已将 `native/rebuilt-core` 的 v5.1 C 源码直接编译进 Windows 内核驱动，
使用原生 Win64 ABI 和固定摘要校验的 GPU 固件数据。没有在内核装载原 ELF `.o`，
没有注入或替换 NVIDIA 功能驱动。默认仍透明转发；只有显式命令启用下一次成功 D3→D0。


## 0.5.2：单次 SBR 与中断配置证据

**实机结果更新（2026-10-08）：单次 SBR 后用户报告蓝屏。**
`filter-sbr-052.log` 记录 MSI 从 control=0x0081/address=0xfee006f0 变为
control=0x0080/address=0，NVIDIA 接手后持续 60 秒未恢复。
该次没有 DMA 分配或解锁固件，仅约 1 秒的一次 SBR。
下面命令保留用于复现记录，**当前不要重复运行单次 SBR 或完整恢复解锁测试**。
透明观察/只读查询仍可用。下一步转向设备栈重新初始化和 Windows 管理的复位交接；
详细证据见 [研究记录](nvidia-filter-research.md)。三个 dump 副本现已核对，均为
0x116/nvlddmkm，Arg3=0xc000009a；见 [转储对比](filter-crash-comparison-20261008.md)。

0.5.1 实机日志已确认保留资源生效：flags=0x23，窗口核验通过，耗时 20090 ms；
用户报告仍然出现相同 0x116，唤醒至蓝屏期间功耗始终不可读。
保留 arena 的改动没有解决问题，不再把立即释放资源作为已确认根因。
当前调查重点是：已进入 NVIDIA 恢复路径后，重复 SBR/引擎复位与固件操作是否破坏
睡眠保存的上下文，或导致中断配置未恢复。

本版新增一次性 `filter-resume-sbr-probe`：只执行一次现有 SBR、恢复原 BAR/Command、
核验配置/链路/GFW bootstrap；不申请 DMA adapter/common buffer，不提交解锁固件，
不主动写 Falcon CPU 启动命令，不打开 PLM，不改 Gen2 策略。
它仍会复位 GPU，可能影响 NVIDIA 恢复；本版是原因隔离实验，不是蓝屏修复版。
单次 SBR 成功时只保留 PDO/session 标记，以禁止本启动周期内再次武装破坏性实验。
失败且已写硬件时，保持原有隔离/D0 失败策略。

### 下一轮实机次序

按后面的签名步骤更新到 0.5.2，安装匹配 EXE/SYS 并重启，固定目录不变。
先用透明观察确认未武装解锁时，正常睡眠唤醒的功耗是否能恢复：

```powershell
.\CMP90HXUnlocker.exe filter-watch --seconds 600 --log filter-baseline-052.log
```

若正常，再进行单次 SBR 对照，保持进程打开，睡眠并唤醒一次：

```powershell
.\CMP90HXUnlocker.exe filter-resume-sbr-probe --bdf 02:00.0 --seconds 600 --observe-seconds 60 --log filter-sbr-052.log
```

窗口结果 stage=12/status=0/flags=0x43 表示一次 SBR 的有限核验通过，**不表示解锁成功**。
必须出现 `FILTER_SINGLE_SBR_COMPLETED`；sbr 计数应只有 1。
EXE 唤醒后继续观察 60 秒，每 5 秒只读 PCI 配置，日志立即落盘。
请同时记录功耗能否读取、是否卡顿/蓝屏。该观察不运行 GPU 负载或访问 GPU MMIO。

- 若透明恢复正常、单次 SBR 就失去功耗并 TDR：问题可缩小到总线复位/恢复交接，
  不依赖完整解锁固件、DMA arena 或 20 秒延迟。
- 若单次 SBR 正常：继续区分重复复位与 GSP/SEC2/PMU/FWSEC 操作，不因此宣称整套恢复解锁安全。
- MSI 在 before-sbr 启用而 after-sbr 关闭，是配置丢失证据；还需看 NVIDIA 接手后是否重新启用。
  MSI 启用位、地址相同也不能证明中断实际投递、固件或 WDDM 上下文正常。

新增 `filter-pci-status` 可独立只读查询目标 PCI 配置；不会武装实验：

```powershell
.\CMP90HXUnlocker.exe filter-pci-status --bdf 02:00.0 --log filter-pci-052.log
```

完整 `filter-resume-unlock` 仍保留 0.5.1 的 DMA 生命周期，本版额外记录每次 SBR 前后
PCI Command、链路状态、MSI control/address/data/mask、MSI-X control/table/PBA。
日志容量仍为 32 KB；flags bit3 表示截断。没有写回任何 MSI/MSI-X 字段，也不读取 MSI-X 表项。
PCI 命令/BAR 和桥控制的实验写入范围仍为已有范围。
Windows 管理 PCI header/capabilities，中断能力不能靠过滤驱动盲目回写修复：
[Microsoft PCI configuration access](https://learn.microsoft.com/en-us/windows-hardware/drivers/pci/accessing-pci-device-configuration-space)。

flag 0x40 新增为单次 SBR 结果；原其他 flags 定义不变。普通 probe-status 仍为 v5/288 字节，
unlock-status 仍为 v1/32832 字节，PCI snapshot 为 v1/272 字节。旧 SYS 不支持新 IOCTL/模式。
退出码 0 只表示内核实验核验和 PCI 观察完成，不能当作 NVIDIA 恢复/负载通过。
下一启动周期才允许新的破坏性实验，观察查询不受此限制。

## 0.5.1：延迟 0x116 的 DMA 生命周期对照

2026-10-08，0.5.0 实机完成恢复窗口解锁：stage=12/status=0/core=0，耗时 19915 ms，
GPU/root port 均 Gen2 x16。用户随后在几十秒内遇到 nvlddmkm 的 VIDEO_TDR_FAILURE (0x116)，
TDR 复位最后错误为 STATUS_INSUFFICIENT_RESOURCES (0xc000009a)。这不能单独证明系统内存不足，
也不能证明 common-buffer 释放是根因；还需区分 NVIDIA 恢复后的状态、调度和映射问题。

本版仅改变完整解锁后的资源回收：成功时保留原 32 MB common buffer、DMA adapter、PDO
和缓冲区内容到重启，不调用 FreeCommonBuffer 或 PutDmaAdapter，不再重用这些页。
失败后的原隔离策略保留；核心固件/SBR/PLM/Gen2 流程不改，便于只对照资源生命周期。
这是一次启动周期内的固定资源保留实验，不是已经验证的生产版 PnP 生命周期设计。
完成一次完整解锁后，所有新探针/解锁武装均拒绝直到重启，避免叠加 adapter 或覆盖旧缓冲区；
filter-status、filter-unlock-status 等查询和后续默认电源转发仍可用。

按下面相同流程重新签名 SYS、重新生成并签名 CAT、安装 INF、重启。运行原命令，
本轮建议日志名 filter-unlock-051.log。保持 VT-d/DMA 防护及其他环境设置和 0.5.0 一致。
先唤醒后空闲观察两分钟，再测试原来使用的负载，记录是哪一步发生卡顿/蓝屏。
日志应出现 filter=0.5.1、arenaLifetime=boot、FILTER_DMA_RETAINED_SUCCESS，最终 flags 应为 0x23；
0x20 表示 arena/adapter/PDO 保留到重启，不等价于隔离标志 0x4。
若仍出现同样 TDR，说明仅保留资源不能解决问题，应继续研究 NVIDIA 的恢复状态与 SBR 相容性。

蓝屏前或下次重启后，可导出此时间段内系统事件，保留 Display、nvlddmkm 和 WHEA 证据：

```powershell
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddMinutes(-30)} |
    Where-Object { $_.ProviderName -in @('Display','nvlddmkm','Microsoft-Windows-WHEA-Logger','Microsoft-Windows-WER-SystemErrorReporting') } |
    Select-Object TimeCreated, ProviderName, Id, Message |
    Format-List | Out-File -Encoding utf8 filter-tdr-events.txt
```

0x116 资料：[Microsoft VIDEO_TDR_FAILURE](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x116---video-tdr-failure)。
WDDM 的 DMA domain 由 Dxgkrnl 管理，保持下层 D0 IRP 不等同于获得其正式排他访问接口：
[Microsoft IOMMU DMA remapping](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-dma-remapping)。

## 本轮签名与测试

固定目录仍为 `build/filter-driver`，更新前退出正在运行的旧探针。
新 SYS、Extension INF 和匹配 EXE 必须一起更新。旧签名/CAT 对新 SYS 无效。
先签名 `CMP90HXLowerFilter.sys`，再在包目录重新生成并签名 CAT：

```powershell
& 'H:\Windows Kits\10\bin\10.0.28000.0\x86\Inf2Cat.exe' /driver:. /os:10_GE_X64 /uselocaltime
# 使用自己的签名流程签名新 CAT 后：
pnputil /add-driver .\CMP90HXLowerFilter.inf /install
# 然后重启；不能只替换文件而继续使用内存中的旧驱动。
```

重启后在此目录打开管理员终端，保留 NVIDIA 驱动，运行：

```powershell
.\CMP90HXUnlocker.exe filter-resume-unlock --bdf 02:00.0 --seconds 600 --log filter-unlock.log
```

保持程序打开，让电脑睡眠并唤醒一次。无需 `--core`，固件和源码核心都在 SYS 中。
它在 PCI 下层成功完成 D0、尚未向 NVIDIA 完成此请求的窗口内运行：
真实 PDO common buffer → 初始双 SBR → 计算 → 图形 → 九个 PLM → Gen2 策略/双 SBR/重训练 → 最终核验。
必须是整个上游桥 SBR 子树只有这一个 GPU 功能的单卡拓扑；发现其他设备会在写入前拒绝。
先检查 GSP halted、RISC-V 不活跃和 DMA idle。地址来自 Windows DMA adapter 的设备地址，
不能根据它与物理地址相同推断 IOMMU 防护已经启用或绕过。

整个内核工作有 120 秒预算，SBR 始终尝试释放 reset，失败后不自动重试。
写入硬件后失败会向上返回 D0 硬件错误并隔离设备；可能被 DMA 引用的 common buffer 与 PDO
保留到重启。此时应保存日志后重启，不能继续在同一启动周期反复尝试。
没有写硬件的前置失败保持下层 D0 状态，让 NVIDIA 继续正常恢复。

`FILTER_UNLOCK_RESULT stage=12 status=0 core=0` 且 flags bit0 表示恢复窗口中的最终核验通过；
这**不等价于 NVIDIA 初始化后仍然保持解锁**。唤醒后还要确认设备无错误、NVIDIA 服务仍为
`nvlddmkm`，并用已有查询/实际负载确认计算、图形及链路状态。0.5.0 已实机通过窗口核验，但随后 TDR；0.5.1 的稳定性待验证。
日志不会在恢复时等待用户进程；保存于驱动的 32 KB 有界缓存，唤醒后由 EXE 读取。

```powershell
.\CMP90HXUnlocker.exe filter-unlock-status --log filter-unlock-status.log
Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -like 'PCI\VEN_10DE&DEV_220D*' } |
    Select-Object Name, Service, ConfigManagerErrorCode
```

stage：1 已武装；2 拓扑/资源/停止条件；3 DMA arena；4 初始双 SBR；5 计算；6 图形；
7 PLM；8 Gen2 复位前；9 复位后；10 重训练；11 最终核验；12 核验完成。
flags：1 核验通过；2 曾写硬件；4 已隔离；8 日志截断；16 超出预算；32 DMA arena/adapter/PDO 保留到重启。
`live` 是源码未释放的 DMA 子分配数，失败时用来解释保留；物理地址只作诊断。
命令未触发、超时或解锁失败退出码为 1，窗口核验通过为 0。
若只有系统电源事件而没有 GPU D3→D0，本轮不会执行解锁。

## 0.4.1 探针与历史记录

以下旧探针仍保留，行为没有改为自动解锁。



历史交付为 0.4.1 设备 Lower Filter，保留 NVIDIA 功能驱动，默认透明转发。
记录 PnP / power 请求进入和下层完成的时间、NTSTATUS、IRQL 与 D 状态。
默认查询/观察不分配 DMA 内存、不触发 GPU 操作。
显式 `filter-resume-probe` 会在一次成功 D3→D0 下层完成之后，暂存电源 IRP 并转到
PASSIVE_LEVEL 内核工作线程：只读 GPU 状态，申请并释放一页未提交给 GPU 的 common buffer，
随后继续向 NVIDIA 完成原电源请求。它不等待用户程序，不复位 GPU、不发 GPU DMA、不执行固件。
新增显式 `filter-resume-dma-probe`：先验证上述基础条件，再在停止的 GSP Falcon 上
进行 256 字节 IN/OUT 数据校验，使用真实 PDO 的 Windows common-buffer 设备地址，
不把 CPU 物理地址直接交给 GPU。保存并恢复 DMEM 和探针改动的寄存器，不重放 DMA 命令。
不复位、不切换 Falcon/RISC-V、不启动 CPU、不执行固件、不写桥或 PCI 配置。
**0.4.1 尚未实现完整解锁；不复位的实机探针停在 DMEM 访问检查。**
暂存 D0 IRP 不是 NVIDIA/Dxgkrnl 的正式排他访问接口，尚不能据此宣称完全没有并发访问。

0.3.1 增加 DMEM 诊断，不改变探针执行顺序或失败后的处理：
`FILTER_D0_DMEM` 分别报告 initial-protected-or-invalid-word、pio-write-readback-mismatch、
pio-original-restore-readback-mismatch，给出首个失败字索引、期望值、实际值和 PIO 匹配数。
`FILTER_D0_DMEM_ORIGINAL/OBSERVED` 保存现有校验读取的前八个字；失败索引超过 7 时，
首个失败字仍单独输出。`FILTER_D0_CLEANUP` 独立报告后续清理的失败原因，避免覆盖原始失败。
初始读取保护值时 expected 和 matches 未参与校验，不能解释为 DMA 比较结果。
协议升级为 v3 / 248 字节，必须同时更新 SYS 和匹配 EXE 并重启。

## 0.4.1：显式 GSP 初始化实验

0.4.0 实机结果：初始化 stage=4，flags=0x6，engineAfter=0，BCR=0x110，
HWCFG2=0x67f7，CPUCTL=0xbadf5620；reset 已释放，未提交 DMA。
0.4.0 的检查顺序有误：在切回 Falcon 前检查了可能处于 lockdown 的 DMACTL。
0.4.1 修正为：stage=4 只等待 HWCFG2 的 MEM_SCRUBBING 清零；
stage=5 确认 RISC-V 不活跃，选择 Falcon并等待硬件 VALID，之后再检查 DMACTL。
VALID 最多等待 100 ms，不要求切换写入立即精确读回，也不通过写 VALID=1 代替硬件确认。
新增 dmaCtlAfter、riscvAfter，保留 reset/探针失败后隔离和 D0 失败处理；未证明 NVIDIA 恢复兼容性。

0.3.1 的实机结果已经定位为初始 DMEM 读取：第一个字及前八个字均为 `0xdead5ec2`，
DMEMC 写前/读后均为 `0x01000724`；尚未提交 DMA。CPU 停止与 BCR 选择 Falcon
不构成 DMEM 主机访问可用的证明。

后续统一交付到固定目录 `build/filter-driver`，版本更新原地覆盖 SYS、INF 和匹配 EXE。
更新前退出正在运行的探针（Ctrl+C），然后构建：

```powershell
.\build.ps1
```

构建会提前检查包内文件是否可写；文件被占用时明确报错，不自动更换目录。
签名 SYS 后，在固定包目录重新生成 CAT，再签名 CAT：

```powershell
& 'H:\Windows Kits\10\bin\10.0.28000.0\x86\Inf2Cat.exe' /driver:. /os:10_GE_X64 /uselocaltime
pnputil /add-driver .\CMP90HXLowerFilter.inf /install
```

重启后在固定包目录的管理员终端执行，保持进程打开，睡眠并唤醒一次：

```powershell
.\CMP90HXUnlocker.exe filter-resume-gsp-init-probe --bdf 02:00.0 --seconds 600 --log filter-gsp-init-041.log
```

**这是会丢失原 GSP 固件/DMEM 状态的实机实验，不是完整解锁。**
默认查询及旧探针命令不触发复位。新命令只启用下一次 D3→D0，检查身份、BAR0、
Falcon HALTED、RISC-V 不活跃、DMA idle、scrubbing 完成和引擎未处于 reset，
复位前再确认身份及引擎状态稳定。只切换 GSP ENGINE 的 bit0，assert/release 各等待 10 ms，
scrubbing 最多等待 100 ms，按当前 BCR 切换到 Falcon并写 FALCON_RM 的 BOOT0。
不执行 SBR、不复位 SEC2、不启动 CPU、不运行固件、不写 PCI 或 privilege mask。
这是独立的内核实现，没有在内核中装载既有用户态 ELF 核心。

NVIDIA 的公开实现说明 Falcon 切换与 priv lockdown 有关联，并在 reset 前等待 RESET_READY、
reset 后等待 memory scrubbing；RESET_READY 可能不被硬件置位，因此其 pre-reset 等待允许超时。
本实验将该等待限制为 1 ms，但仍须实机验证 GSP-only reset 能否解除本机所遇到的访问限制。
源码参考：[NVIDIA GA102 Falcon 实现](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/main/src/nvidia/src/kernel/gpu/falcon/arch/ampere/kernel_falcon_ga102.c)、
[GA102 GSP ENGINE 定义](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/main/src/common/inc/swref/published/ampere/ga102/dev_gsp.h)。

协议为 v5 / 288 字节。`FILTER_D0_GSP_INIT` 的 stage：
1 前置检查；2 assert；3 release；4 scrub；5 Falcon 选择；6 RM 写入；7 初始化完成。
flags：1 观察到 RESET_READY；2 曾尝试 assert；4 已确认 release；8 初始化通过；
0x10 RISC-V 不活跃检查通过；0x20 已请求 Falcon；0x40 硬件 VALID 通过；0x80 DMACTL 检查通过。
主结果 stage=14 表示停在初始化；成功后进入原 GPU 探针 stage=7..13。
主 flags 的 0x1000 表示初始化模式，0x800 表示曾尝试破坏性的 reset。
需要同时观察初始化 stage=7/status=0、后续 stage=13/status=0、IN/OUT 各 64/64，
以及 NVIDIA 唤醒后的设备状态与实际负载。`restored=True` 只指复位后的探针状态，
**不代表复位前的 NVIDIA/GSP 状态已经恢复**。

暂存 D0 IRP 不是 NVIDIA/Dxgkrnl 的正式排他协调接口。即使 GPU DMA 成功，
NVIDIA 也可能因原固件状态丢失而恢复失败；当前不保证保留 NVIDIA 的恢复兼容性。
如果已经尝试 reset，而初始化或后续探针失败，本版本向上报告 D0 硬件错误并禁止再次启用，
需要重启；reset 未解除访问限制时不会自动追加复位或降低校验条件。

2026-10-07 23:14 的 0.4.1 实机日志已通过：初始化 stage=7/status=0/flags=0xfe，
后续探针 stage=13/status=0，IN=64/64、OUT=64/64、restored=True、quarantined=False，
8192 字节 common buffer 已释放，工作线程内部耗时 181 ms；BCR=0x101。
此后同机只读查询显示设备 Status=OK、ConfigManagerErrorCode=0、服务 nvlddmkm。
这证明该次复位后的恢复窗口 GSP 双向传输和当前设备 Code 0，不证明完整解锁、
持续计算负载、多次睡眠稳定性或 IOMMU 保护当前生效。日志中的逻辑地址与物理地址相同，
不能单独据此判断映射或保护配置。
## 已有实机证据

2026-10-06 的 0.1.0 日志确认：一个过滤设备附加到 `02:00.0`，PnP START 成功。
一次 S3 睡眠中，设备 SET_POWER D3 和恢复 SET_POWER D0 的下层完成均为 SUCCESS，
两次完成回调都在 IRQL 2；D0 下层请求约 293 ms 后完成。
恢复过程没有新的 START_DEVICE，因此自动解锁不能只覆盖启动路径。
日志尚未证明 NVIDIA 最终设备状态、完整解锁状态或 IOMMU 映射可用性。

0.3.0 实机恢复探针返回 stage=8 / status=0xc00000a3 / flags=0x28f：
已进入 DMEM PIO 检查，尚未提交 GPU DMA；restored=True / quarantined=False。
这一版日志不能区分初始保护值、写入校验和恢复校验，因此本次交付补充各分支证据，
尚不能把失败归因于 IOMMU。

随后 0.2.0 实机日志确认：D0 下层完成在 IRQL 2，内核探测工作线程在 IRQL 0 执行，
结果 stage=6 / status=0 / flags=0xf，4096 字节 common buffer 已分配并释放。
设备地址与 CPU 物理地址均为 `0x5e882000`；BAR0=`0x80000000`，BOOT0=`0xb72000a1`，
GFW_BOOT=`0x00008b8f`，GFW_PROGRESS=`0x000003ff`，GSP_CPUCTL=`0x10`，
SEC2_CPUCTL=`0xbadf5620`，PCI_COMMAND=`0x0406`。elapsedMs=0 表示低于本次毫秒计时分辨率，
不表示零耗时。这次没有提交 GPU DMA，尚不能认定该页能被 GPU 在当前 domain 中访问。

同机只读设备查询确认 NVIDIA 服务 `nvlddmkm`、Status=OK、ConfigManagerErrorCode=0，
驱动版本 `32.0.16.1656`。设备 DmaRemappingPolicy 属性为 1；这不是 GPU 映射生效的证据，
后续须结合 WDDM 的映射机制及实际 GPU DMA 验证，不能机械改写 NVIDIA 的 remapping 声明。
当前已验证恢复窗口调度、GPU 状态读取、common-buffer 分配释放以及 NVIDIA 设备正常恢复；
尚未验证 CUDA 负载、恢复窗口 GPU DMA 和恢复自动解锁。

先确认真实电源路径，再实现恢复窗口。现有原生核心由 C# 用户态装载并调用，
无法直接搬进内核回调；在 D0 完成前等用户进程运行可能阻挡系统唤醒。
后续必须解决内核执行方式或证明另一个不会阻塞恢复的协作窗口，
并核验 NVIDIA/Dxgkrnl 的映射和硬件访问时序。
本阶段日志只能证明请求到达过滤驱动及下层完成，不能证明 NVIDIA 已静默、
IOMMU 映射有效，或解锁操作可以安全执行。

## 构建与签名

仓库根目录执行：

```powershell
.\build.ps1
```

输出 `build/filter-driver/CMP90HXLowerFilter.sys`、Extension INF、匹配工作进程和本说明。
与独占 IOMMU 驱动使用独立文件夹、服务和控制设备。

先签名 SYS，再生成并签名 CAT：

```powershell
& 'H:\Windows Kits\10\bin\10.0.28000.0\x86\Inf2Cat.exe' /driver:'.\build\filter-driver' /os:10_GE_X64 /uselocaltime
```

签名和加载接受条件沿用测试机现有的内核签名流程。
SYS 签名后必须重新生成 CAT；不要修改包内工作进程，驱动校验其精确哈希。
若需签名工作进程，先 `sign-gen2.ps1` 再重建驱动，最后签名 SYS 与 CAT。

## 安装

1. 先恢复 90HX 原 NVIDIA 功能驱动并重启。设备“服务”应为 `nvlddmkm`，
   不能仍由 `CMP90HXDmaIommu` 功能驱动接管。先留存 NVIDIA 当前设备状态。
2. 退出 CMP90HX GUI，测试期间暂停原自动解锁任务，保持 VT-d / 内核 DMA 保护开启。
3. 在管理员 PowerShell、签名包目录执行：

   ```powershell
   pnputil /add-driver .\CMP90HXLowerFilter.inf /install
   ```

4. 重启一次，确认 NVIDIA 仍是功能驱动。这个包是 Extension INF，
   **不要通过“从磁盘安装”把它选为显卡功能驱动，也不要用 sc.exe 安装**。
   INF 仅匹配 `PCI\VEN_10DE&DEV_220D`，通过 AddFilter 增加 Lower Filter，
   不覆盖 NVIDIA 的 DMA remapping 注册表声明。

可以查询实际驱动栈（若本机 pnputil 支持这些选项）：

```powershell
pnputil /enum-devices /deviceid "PCI\VEN_10DE&DEV_220D" /stack /services
```

应同时看到 NVIDIA 服务及 `CMP90HXLowerFilter`。
如果控制设备无法打开，保留设备状态、问题状态和 `C:\Windows\INF\setupapi.dev.log`。
首次安装可能会请求设备重启，确保 GPU 没有正在执行计算任务。

## 一次有意义的睡眠恢复验证

更新到 0.3.1 后，签名 SYS、重新生成并签名 CAT，安装新 Extension INF 并重启。
需要使用新包内匹配 EXE。已有 0.2.0 分配释放证据，不必重复基础探针。
在管理员终端、新包目录执行：

```powershell
.\CMP90HXUnlocker.exe filter-resume-dma-probe --bdf 02:00.0 --seconds 600 --log filter-d0-dma.log
```

保持进程开着，睡眠并唤醒一次。它只启用下一次成功 D3→D0；退出进程取消尚未消费的启用状态，
已排队的内核工作会自行完成。没有目标电源转换时不触发，不自动重试。
若停止在 GSP 状态或 DMEM 访问检查，不会自动复位 GPU 去绕过检查。
可用下面命令查询保留的最后结果（重启清空）：

```powershell
.\CMP90HXUnlocker.exe filter-probe-status --log filter-d0-result.log
```

输出含 `FILTER_D0_PROBE`、`FILTER_D0_GPU` 和 `FILTER_D0_DMA`：

- stage=1 等待恢复；3 身份/资源/MMIO；4 DMA adapter；5 基础页分配；6 基础探针成功。
- GPU 模式：7 上游 BME / 停止 Falcon / idle 检查；8 DMEM PIO 校验；9 DMA 缓冲区及配置；
  10 IN；11 OUT；13 完成。stage 表示最后到达的阶段，status=0 才是成功。
- 完整 DMA 成功要求 stage=13、status=0、IN=64/64、OUT=64/64、restored=True、
  quarantined=False、bytes=8192。传输命令变为 idle 本身不代表数据成功。
- flags：0x1 有效 BAR；0x2 adapter；0x4 基础页分配；0x8 缓冲区已释放；
  0x10 PIO 校验通过；0x20 IN 数据通过；0x40 OUT 数据通过；0x80 状态恢复通过；
  0x100 曾提交 GPU DMA；0x200 GPU 模式；0x400 无法确认恢复，已隔离。
- deviceAddress 是 Windows 返回的设备地址，physical 是 CPU 物理地址。
  两者相同或不同都不能单独证明 IOMMU 保护当前生效；需结合保护状态及实际 DMA 数据。
- PCI 扫描最多 100 ms，每次 DMA 最多等待 100 ms；不重试。
  elapsedMs 不含工作线程排队，事件时间可观察额外恢复延迟。

普通检查失败仍保留原 PCI 电源完成状态；**无法确认 DMA idle 或无法恢复状态时例外**：
记录 quarantined=True，保留可能被 DMA 引用的缓冲区和 adapter 至重启，向上报告 D0 硬件错误，
拒绝再次启用探针。此时 NVIDIA 可能恢复失败，需要重启；不要反复提交测试。

请提供 `filter-d0-dma.log` 和 NVIDIA 唤醒后的设备状态。即使成功，也只证明这次 GSP 的小块
双向传输，不能替代 SEC2、完整固件链和负载验证。当前仍没有内核版完整解锁核心。
原 `filter-resume-probe` 保留为只读状态/未提交 common-buffer 模式。

下面的命令保留用于纯时序观察：

在包目录用管理员终端执行：

```powershell
.\CMP90HXUnlocker.exe filter-status --log filter-before.log
powercfg /a
.\CMP90HXUnlocker.exe filter-watch --seconds 600 --log filter-resume.log
```

保持最后一个进程开着，通过 Windows 开始菜单进入睡眠，再唤醒。
唤醒后等几秒，可以用 Ctrl+C 结束观察。若睡眠超过观察时间，仍可通过下一条命令读取
内核保留的最后 64 条事件（重启会清空，睡眠保留）：

```powershell
.\CMP90HXUnlocker.exe filter-status --log filter-after.log
```

随后检查 NVIDIA 设备状态以及解锁状态是否丢失。不要运行完整解锁或 DMA 探针去测试这个过滤后端，
本阶段控制接口提供日志查询及上述内核探测；不提供任意 GPU/PCI 写入、DMA 提交或解锁 IOCTL。
发送三份日志、`powercfg /a` 输出以及 NVIDIA 恢复后状态即可，不需要重复多轮模拟测试。

## 日志解释

- `FILTER_STATUS devices=1` 表示有一个匹配设备附加了本过滤驱动；
  `started=True` 仅表示观察到成功的 PnP START，不等价于 NVIDIA Code 0 或 D0。
- `phase=enter` 是请求往 PCI 底层传递前；此处 status 可能仍是默认 `STATUS_NOT_SUPPORTED`，
  不能据此认定请求失败。`phase=lower-complete` 的 status 才是下层最终返回值。
- `major=0x16 minor=0x02 device=D3/D0` 是设备 SET_POWER；时间为 UTC ISO 8601。
  向上的 D0 完成日志是后续研究恢复窗口的重点，尚不代表完整 NVIDIA 恢复结束。
- `major=0x1b` 是 PnP；`minor=0x00` 为 START，`0x02` 为 REMOVE，`0x04` 为 STOP。
- `phase=probe-worker` 为 PASSIVE_LEVEL 探测开始；`probe-finished` 为探测结束。
- 如果只有系统电源事件，没有 GPU 的 D3→D0，不能对这一轮触发完整恢复解锁。
  S3、休眠和 Modern Standby 的结果需要区分。
- 环形缓冲最多 64 条，`FILTER_TRACE_DROPPED` 表示观察间隔内有旧事件被覆盖。
  多个匹配 90HX 时头部 BDF/started 表示最后产生事件的设备；当前第一轮针对单卡机器。

## 移除与下一阶段

用 `pnputil /enum-drivers` 找到 **CMP90HXLowerFilter Extension** 对应的 `oemNN.inf`，
执行 `pnputil /delete-driver oemNN.inf /uninstall`，然后重启。
不要删除 NVIDIA 的基础驱动包。实验 SYS 不卸载，更新二进制也需重启。

已根据真实恢复时序实现一次显式 D0 内核探测，继续确认 DMA adapter 能否申请，
以及解锁核心能否在不依赖尚未恢复的用户进程的情况下执行。
当前未修改已经通过完整解锁的独占 IOMMU 包。

参考：[Extension INF](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/using-an-extension-inf-file)、
[AddFilter](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addfilter-directive)、
[设备电源恢复请求](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-device-power-up-irps)。
