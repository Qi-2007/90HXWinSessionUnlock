# 保留 NVIDIA 驱动的 IOMMU 解锁方案研究

研究日期：2026-10-06。用户报告 0.1.1 独占 PnP 后端完整解锁成功；
此前提供的日志已确认同机 `02:00.0` 的 GSP 双向 DMA 探针通过。
本次研究未加载新驱动，也未验证 NVIDIA 接管；完整解锁成功日志尚未留档。

## 结论

保留 NVIDIA 为设备的功能驱动、额外安装设备过滤驱动，Windows 安装机制支持。
但目前不能承诺 NVIDIA 正在运行时，外部过滤驱动可以安全执行现有完整解锁。
优先研究 Lower Filter 在一次受控设备重启中的 START_DEVICE 窗口完成解锁，
然后让 NVIDIA 继续启动。这是待验证设计，不是已完成实现。

保留驱动包和绑定，不等于整个解锁过程都让 NVIDIA 持续访问 GPU。
本项目会复位 GSP、执行固件、写策略寄存器并对上游桥执行 SBR；
普通驱动锁只能串行化本项目 IOCTL，不能暂停 NVIDIA 的线程、DPC、调度或 DMA。

## 官方接口给出的边界

- Extension INF 可以为现有设备增加过滤驱动，不能提供替代功能驱动。
  使用 `AddFilter` / `FilterPosition=Lower`，不使用功能驱动的关联服务标志。
  不需修改或重新签名 NVIDIA 原 INF/SYS。
  来源：[Extension INF](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/using-an-extension-inf-file)、
  [AddFilter](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addfilter-directive)。
- `IoGetDmaAdapter` 使用真实 PDO，适合在 AddDevice 或 START_DEVICE 阶段调用。
  文档没有保证：另一个驱动取得的 adapter/common buffer 必然与 WDDM 当前映射兼容。
  来源：[IoGetDmaAdapter](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iogetdmaadapter)。
- WDDM 的 GPU domain、映射和切换由 Dxgkrnl 与显示 miniport 协调；
  切换时必须静默硬件。`DxgkDdiBeginExclusiveAccess` / `EndExclusiveAccess`
  是 Dxgkrnl 调用显示驱动的回调，不能当成过滤驱动可随意申请独占的函数。
  `DXGKCB_MAPMDLTOIOMMU` 属于显示 miniport 回调接口；当前项目没有合法获得
  NVIDIA 回调表和 adapter 上下文的路径。
  来源：[GPU isolation](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-based-gpu-isolation)、
  [WDDM DMA remapping](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-dma-remapping)。
- `DomainAttachDeviceEx` 对已绑定 domain 的设备可返回 `STATUS_INVALID_PARAMETER`。
  自建第二个 domain 不能自动解决共享问题；也不能先拆走 NVIDIA 的 domain。
  来源：[DomainAttachDeviceEx](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-iommu_domain_attach_device_ex)。

## 候选架构

设备栈：NVIDIA/Dxgkrnl → CMP90HX Lower Filter → PCI PDO。

首次验证只记录 PnP、power、真实 PDO、资源与 adapter 申请结果；不发起 DMA、
不改 GPU 寄存器、不延迟 START_DEVICE。先验证 NVIDIA 仍是功能驱动且状态正常。
不把现有功能驱动 INF 的 `RemappingSupported=1` / 服务兼容值机械搬到过滤驱动包，
不覆盖 NVIDIA/WDDM 原有的 remapping 声明。

后续受控实验使用以下状态：

1. 默认透明转发；启动和重启都不自动解锁。
2. 管理员工作进程显式准备下一次启动窗口，再请求目标设备重启。
   工作进程保持运行，不能依赖重新登录后才启动 GUI。
3. Lower Filter 把 START_DEVICE 转交 PCI 栈；底层完成后，在向上完成之前
   暂存 IRP 并进入有限时长窗口。PnP 已建立的电源、资源和设备状态仍须核验。
4. 窗口中申请真实 PDO 的 DMA adapter/common buffer；仅窗口内允许解锁 IOCTL。
   必须验证该时刻 NVIDIA/Dxgkrnl 是否已分配或切换 domain，不能假定尚未初始化。
5. 依次验证映射、GPU 探针、完整解锁。DMA 地址仅对当次窗口有效。
6. 确认引擎 DMA 停止后解除本项目映射并完成 START_DEVICE，由 NVIDIA 继续初始化。
   必须区分 GPU 寄存器访问与主机页 DMA：完成解锁后不再使用本项目 DMA 地址。
7. 验证 NVIDIA Code 0、NVAPI/NVML、CUDA 和实际 Gen2 x16；
   同时确认 NVIDIA 初始化没有清除解锁状态。

START_DEVICE 顺序的参考：[IRP_MN_START_DEVICE](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-start-device)。
上述窗口及 NVIDIA 启动时序是基于该顺序的设计推断，尚未实测。

## 必须解决的实现差异

| 当前独占后端 | 保留 NVIDIA 的过滤后端 |
| --- | --- |
| 处理功能驱动请求，非支持请求可拒绝 | 默认转发所有不属于本项目的请求 |
| common buffer/adapter/PDO 保留到重启 | 支持多次 PnP 启停，每次重新确认映射生命周期 |
| 分配后 veto query-stop/remove | 平时允许 NVIDIA 正常停启，只在显式窗口协调访问 |
| 不支持电源恢复 | 正确转发电源请求，退出窗口后禁止旧地址 DMA |
| 设备启动后任意时间可显式解锁 | 只在已确认 NVIDIA 不并发访问的窗口授权硬件写入 |
| 用独占 INF 替换 NVIDIA | Extension INF 增加设备过滤驱动 |

现有“禁用为 Code 22 → 解锁 → 再启用”的 GUI 流程不能直接复用：
禁用会停止整个设备栈，不能依赖过滤驱动在停机后继续保有有效 IOMMU 映射。
现有 boot-lifetime/quarantine 模型也不能原样用于正常 NVIDIA 停启。

未提交 DMA 时，超时可以释放资源并恢复正常 START 转发。
已提交 DMA 后，失败或工作进程退出必须先确认硬件静默；
不能单凭超时就释放页、解除映射并让 NVIDIA 接管。
需要定义带 DMA 状态证据的故障收尾，并确保有限等待、单次完成 IRP 与 remove-lock 生命周期。
若无法建立安全启动窗口，应停止 GPU 写入实验；透明过滤驱动仍可用于采集时序。

## 最小实机验证次序

1. 编译并签名独立透明 Lower Filter，确认真实设备栈位置和 NVIDIA 正常启动。
2. 增加一次显式重启的启动窗口，只记录顺序并返回，不分配 DMA 内存。
3. 窗口中申请映射并做现有 256 字节双向探针，确认映射在该时刻真正有效。
4. 通过后再复用完整解锁核心，验证 NVIDIA 接管与 CUDA。

后续已生成第一阶段透明过滤驱动 SYS，仅记录真实 PnP/power 时序，不执行解锁或 DMA。
见 [实机测试说明](lower-filter-test.md)。D0 恢复窗口不能等待尚未恢复的用户进程；
现有用户态核心的内核执行方式仍待解决。当前已签名加载的 0.1.1 独占实验包不受影响。

## 2026-10-07 恢复窗口实机进展

0.2.0 已通过 D0 内核工作线程及 common-buffer 分配释放，0.3.1 证明初始 GSP DMEM
返回 dead5ec2，DMEMC 未体现地址选择变化；未提交 GPU DMA，不能归因为 IOMMU。
0.4.0 编译显式 GSP-only reset 后探针模式，用于区分固件残留访问限制与 DMA 可用性。
这次 reset 会丢失原固件状态，不能把探针状态恢复解释为 NVIDIA 上下文恢复；
其恢复兼容性仍需实机验证。透明/default 模式不 reset。见测试说明中的 0.4.0 部分。
0.4.0 复位实验在 stage=4 停止：reset 已释放，BCR=0x110、HWCFG2=0x67f7、
Falcon CPUCTL=badf5620，尚未提交 DMA。0.4.1 修正检查顺序，在切回 Falcon 之前
只等待 HWCFG2 scrub 完成，再等待 Falcon VALID、检查 DMACTL。改动尚待实机验证。
0.4.1 随后实机通过：初始化 stage=7/status=0、GSP DMA IN/OUT 各64/64、
探针 stage=13/status=0、restored=True、quarantined=False，181ms。
同机后续只读查询：NVIDIA 设备 Status=OK、Code 0、服务 nvlddmkm。
尚未验证完整解锁、负载及重复恢复稳定性。后续交付统一 build/filter-driver 原地更新。
## 2026-10-08：源码核心的 Lower Filter 恢复解锁入口

重构后的 v5.1 C 主机核心已经直接编译为 WDK 原生 Win64 代码并接入过滤驱动，
不再受原 ELF/SysV 回调及低地址重定位约束。固件仅作为摘要固定的数据链接。
新增一次性 `filter-resume-unlock`，在 D0 下层完成窗口执行计算、图形、PLM、Gen2；
常用查询和旧探针保持独立。固定交付目录仍为 build/filter-driver，驱动 0.5.0。

内核适配器使用真实 PDO 的 32 MB common buffer 设备地址、非分页 host pool、
限定 BAR0/PCI/桥 ECAM、子树隔离检查和有限等待。源码 PROM 静态缓存增加仅内核编译的
释放入口，避免一次恢复结束释放 host pool 后，下次恢复仍引用旧缓存。
写入后失败停止 D0 恢复并保留 DMA/PDO 到重启，不自动再尝试。

WDK 编译、INF 校验和内核导入检查通过；还没有加载本版驱动或证明完整实机恢复解锁。
`stage=12` 仅证明 NVIDIA D0 完成前的窗口核验，还需要观察 NVIDIA 初始化后是否重新锁定。
测试与签名步骤见 lower-filter-test.md。

## 2026-10-08：0.5.1 TDR 复现后的研究

0.5.1 实机 flags=0x23，arena/adapter/PDO 保留，解锁窗口仍通过，随后同样 0x116，
功耗始终不可读取。此结果不支持把成功后释放 arena 单独视为根因。
核心现有交接计数 11，每次双 SBR，共 22 次 SBR；SBR 只恢复 BAR/Command，
不能将 GFW bootstrap ready 等同于 NVIDIA 固件、队列、显存上下文和中断都恢复。
下层 D0 成功完成是电源请求的一个节点，不是已经证实的 NVIDIA 冷启动窗口。

0.5.2 新增只做一次 SBR 的恢复对照，排除完整固件链、DMA arena 和 20 秒工作时长。
增加每次 SBR 前后 MSI/MSI-X 配置只读证据、独立 PCI snapshot IOCTL，
以及成功后 60 秒用户态只读 PCI 观察。没有修改 MSI/MSI-X 配置或尝试注入 NVIDIA。
尚未加载 0.5.2；实机步骤见 lower-filter-test.md。

PnP START 路径仍为候选，未接入完整解锁：Microsoft 的 DxgkDdiStartDevice 要求
建立新的硬件上下文并启用中断，而 DxgkDdiSetPowerState(D0) 恢复先前保存的上下文。
这使 START 值得调查，但不能假定任意 Lower Filter START completion 就一定是该冷启动窗口。
需要先验证 PCI 资源与 BME 可用时间、PnP stop/remove/add/start 顺序、过滤实例寿命、
DMA domain 在 Dxgkrnl 初始化前后的归属，以及保留资源与正式 stop/remove 的交接。
不能在持有 D0 IRP 的工作线程里同步重启同一个设备栈；那会引入依赖环/死锁。

[DxgkDdiStartDevice](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkddi_start_device)
负责初始化上下文、映射资源、开启中断；
[DxgkDdiSetPowerState](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkddi_set_power_state)
描述保存/恢复设备上下文；
[PCI 配置访问](https://learn.microsoft.com/en-us/windows-hardware/drivers/pci/accessing-pci-device-configuration-space)
说明 header/capabilities 的 Windows 所有权。

## 2026-10-08：0.5.2 单次 SBR 实机复现蓝屏

证据文件：`evidence/filter-sbr-052.log`。用户报告该次实验随后蓝屏。
下层 D0 完成后只执行一次 SBR，结果 stage=12/status=0/flags=0x43、1078 ms，
handoffs=0/live=0/deviceAddress=0/physical=0；没有申请 DMA adapter/common buffer，
没有运行解锁固件。此结果不能当作 NVIDIA 恢复成功。

- before-sbr：MSI cap=0x68/control=0x0081/enable=1/address=0xfee006f0。
- after-sbr 和 before-upper-completion：control=0x0080/enable=0/address=0。
- NVIDIA 接手后的 5～60 秒观察：MSI 始终禁用、地址为零；Command 从 0x0406 变为 0x0006。
- 新转储为 `C:\Windows\Minidump\100826-13437-01.dmp`，时间 17:55:19；
  最初系统目录读取被权限拒绝；用户随后提供副本，已确认本次也是 0x116/nvlddmkm，
  Arg3=0xc000009a，与前两次具有相同恢复失败路径。详见 [三次转储对比](filter-crash-comparison-20261008.md)。

这证明现有 SBR 的 BAR/Command 恢复没有覆盖被复位清除的 MSI 配置。
单次 SBR 已足以复现用户报告的蓝屏，完整固件链、32 MB arena、22 次 SBR 和
20 秒窗口不再是复现的必要条件。MSI 丢失是明确缺陷及 TDR 候选原因，
尚未证明是唯一原因；Command 后续清除 INTxDisable，也不能说设备绝无任何中断路径。

不继续重复相同 SBR 实验，不把手工回写 MSI 当作支持的修复。
Windows 拥有 PCI header/capabilities，绕过总线管理会同时遗留 NVIDIA 保存上下文问题。
下一实现方向是协调完整设备重新初始化，而非继续在暖 D0 恢复中直接 SBR。

官方 `GUID_DEVICE_RESET_INTERFACE_STANDARD` 可查询总线支持的复位类型：
function-level 复位仍需功能驱动重建状态；platform-level 复位会触发受影响设备栈拆除重建。
该接口不是允许过滤驱动在持有 D0 IRP 时随意复位的保证；不能在当前工作线程内
直接调用 platform reset，否则设备移除与本线程持有的 remove-lock/IRP 可能互相等待。
现有 RetainedCore/PDO/common-buffer 生命周期亦须先设计与 REMOVE 的交接。
先调查 NVIDIA 发起的 reset/query-interface 时序及 PnP START 可用窗口，再实现一次性的
操作系统管理恢复；单独查询接口支持不应实际触发 reset。

参考：[Windows 设备复位接口](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/working-with-guid-device-reset-interface-standard)、
[DeviceReset 调用约束](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nc-wdm-device_reset_handler)。

## 0.5.3：先验证真实 PnP START

新增显式 mode=5/`filter-start-probe`。武装状态留在控制设备中，允许目标栈 REMOVE/ADD 后
的新实例消费下一次成功 START；普通 D0 不消费 mode=5。下层成功完成 START 后挂起向上完成，
在 PASSIVE worker 内只读资源/PCI 配置及查询 v1 reset interface，再保留原 START 状态完成。
不调用 reset，不接入解锁核心，不申请 DMA arena，不触碰 GPU MMIO。
这样可以确认 Windows 管理的目标设备重启是否恢复 NVIDIA/MSI，并取得下一窗口的资源证据。
总线接口只在工作线程内持有，用完 dereference；工作线程由实例 remove-lock 保护。
ARM 时仍拒绝 GpuQuarantined/RetainedCore；前轮破坏性实验后必须先重启系统。

还没有解决正式 START 解锁后的 DMA domain 交接、固件状态清理和 stop/remove 保留资源问题。
目前只能交付 START 证据探针，不能把完整 D0 解锁移到 START 就宣称问题已修复。
步骤见 [0.5.3 测试说明](lower-filter-test.md)。

## 0.5.3 实机结果：设备栈重建正常

用户反馈功耗读取恢复、没有蓝屏。`evidence/filter-start-053.log` 与提供的文本
记录 QUERY_REMOVE → REMOVE → 新实例 attach → START，不是同一实例的暖 D0 恢复。
只读 START worker 为 stage=12/status=0/flags=0x81、elapsedMs=0，无复位或 DMA 提交。
读取 BUS_INTERFACE_STANDARD 成功（256 字节）。翻译资源 BAR0=0x80000000/16 MB，
另一个内存窗口=0xa400000000/32 MB，中断描述符1个、flags=0x3。

下层 START 完成窗口中，Command 已为0x0406、MSI 已启用（control=0x0081，
address=0xfee006f0）。上层初始化后 Command=0x0006，MSI 同样保持启用、地址不变。
本轮提供的文本及当前落盘日志截至 after-resume-125s；未见180秒结束标记，
因此只记录已核对的125秒配置证据与用户无蓝屏反馈，不宣称已核验完整180秒观察。

v1 reset interface 查询成功，size=56/version=1/supported=0x1：
函数级复位支持、平台级复位不支持，回调存在，但本轮没有调用它。
可继续调查总线函数级复位是否能代替现有手工 SBR，同时保持 Windows 管理的 PCI 配置。
不能把函数级复位的支持位解释为平台级拆栈重建能力，也不能直接认为它提供了 WDDM 独占窗口。

此结果验证透明 START 探针与 Windows 管理的设备栈重建可正常工作，尚未验证 START 解锁。
关键限制：MSI 在 lower START 窗口已经编程。简单把旧解锁核心移到这里，手工 SBR 仍会
清除它；必须先解决复位/总线配置恢复和 DMA 资源交接，不能仅更换回调时机。

## 0.5.4：总线函数级复位实机入口

新 mode=6/`filter-start-reset-probe` 只在下一次成功 lower START 工作线程中运行。
通过已查询的 v1 DeviceReset 接口调用 FunctionLevelDeviceReset/flags=0，附带完成回调。
调用返回的成功或 pending 不当作完成；最多等待30秒回调的最终 STATUS_SUCCESS。
此等待不能抢占总线内部同步调用。回调参数和接口引用在首次提交后保留到系统重启，
不会引用已完成 IRP、旧 FILTER_EXTENSION 或工作线程栈；每次启动只允许一次提交。
上下文只包含非分页参数与原子状态，不持有本项目 DMA arena。

复位前后配置均由 BUS_INTERFACE_STANDARD 读取，核对 ID、六 BAR、Command 及 MSI，
不进行手工修复。因不访问 MSI-X 表项，拒绝在已启用 MSI-X 的目标上提交复位。
已调用后失败/超时/配置不一致会 quarantine，失败 START，禁止新实验；
未调用前拒绝或读取失败保留原 START 状态。正常只读 mode=5 的行为保留。

它用于检验 Windows 总线复位是否保留配置并允许 NVIDIA 冷初始化。
不是已证明可替换全部双 SBR 的解锁后端；尚无 GPU firmware/bootstrap/MMIO、DMA domain
交接和完整解锁实机证据。没有调用平台级复位，也没有在持有 D0 的线程中重启设备栈。
实机步骤见 [0.5.4 测试说明](lower-filter-test.md)。

## 0.5.4 实机结果：函数级复位仍清除 MSI，过滤驱动主动失败 START

用户报告没有蓝屏，但设备 Code 10、功耗/软件不再识别。提供的日志明确区分了原因：
函数级复位返回 STATUS_SUCCESS，完成回调=1/status=0，耗时219ms；没有完成超时。
复位后的总线配置仍可读取，ID、所示 BAR0 和 Command=0x0406 正常，
MSI 却从 control=0x0081/address=0xfee006f0 变为 control=0x0080/address=0。
保守配置检查因此返回0xc0000182（STATUS_DEVICE_CONFIGURATION_ERROR），
flags=0x186（START+函数复位+已调用+失败，无成功位）。

`ResumeWorker` 按已提交失败策略设置 quarantine，把 START IRP 状态改为
STATUS_DEVICE_HARDWARE_ERROR（当前WDK定义0xc0000483），并将 Started 置FALSE。
紧接着日志出现 REMOVE、devices=0。此轮 Code 10/“致命硬件错误”来自本项目主动失败 START，
不能解释为已证明显卡永久损坏，也不能说 NVIDIA 初始化或实际负载已通过。
设备栈被移除后 NVML/软件无法看到这张运行中的显卡符合当前状态。
无蓝屏亦不能证明该复位方式兼容 NVIDIA，因为我们并未让它在配置丢失后继续启动。

这次证据否定“v1总线函数级复位完成成功会自动保持MSI”的假设。
继续在 lower START 已完成后复位，仍破坏已经由 PCI 编程的中断配置。
不要放宽检查、不直接补写MSI，也不把失败START作为最终恢复方案。
先重启系统清除本轮quarantine/保留回调引用，保持默认未武装透明启动。

下一候选需把复位与 PCI 总线正常 START 配置顺序协调：调查总线 START 之前
接口是否可用、设备是否已上电、函数复位是否允许，以及之后总线是否会重新编程MSI。
这些均尚未验证，不能直接挪动现有复位回调并宣称可用。另一候选为解锁后的
操作系统管理设备栈重建，但必须验证再次START不会清除解锁状态，并解决DMA资源交接。
当前不再重复0.5.4相同时序的复位实验。

## 候选交接：解锁后禁用/启用，由 NVIDIA 新 START 接管

用户提出在解锁完成后禁用并重新启用设备。该方向有实机依据：0.5.3 的 Windows
设备重启确实触发 REMOVE/新实例 START，MSI 保持启用且功耗正常；显示功能驱动的
StartDevice 负责新硬件上下文及中断初始化。此依据仅覆盖未解锁的正常设备重启，
不证明完整解锁状态能够跨重新启动保留。

候选流程需要分两阶段：
1. 在已验证 NVIDIA 不并发操作的窗口完成解锁，记录解锁状态和 DMA 静默证据。
2. 退出所有本项目 IRP/工作线程并处理本项目 DMA/PDO 资源，再由用户态请求目标设备
   禁用/启用（或经验证等效的 PnP restart），下一实例不再武装解锁，透明放行给 NVIDIA。
3. 新 START 后核对 MSI、设备状态、NVML 功耗和解锁结果，最后验证实际负载。

不能在持有目标 D0/START IRP 或其 remove-lock 的工作线程内同步禁用同一设备，
否则 REMOVE 等待工作线程、工作线程等待禁用完成会形成依赖环。
也不能把完整解锁后正常放行 D0、再让用户手工快速禁用当作可靠交接：
NVIDIA 已可能开始恢复失效上下文，存在此前 TDR 的同类间隙。
若选择完成解锁后主动失败第一次 START、随后由用户态重启再透明 START 的暂存方案，
必须明确第一次 Code 10 是预期中间态，并先解决正常 REMOVE 的资源清理；目前没有实现。

现有 RetainedCore 保留32MB common-buffer、DMA adapter和旧PDO到重启，
REMOVE 分支只释放 FILTER_EXTENSION 的 PDO/remove-lock，没有回收 RetainedCore。
因此当前版本不能直接把旧解锁命令与 Disable/Enable 串接作为可用功能。
需要先证实 GPU 不再访问本项目页，并建立与 PnP REMOVE 相容的释放和故障隔离策略。
live=0 或解锁寄存器通过不能单独替代硬件 DMA 静默证据。
此外 NVIDIA 重新初始化可能复位GPU、重新锁定PLM/链路策略，必须在接管后重新核验。

参考：[DxgkDdiStartDevice](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkddi_start_device)、
[禁用与REMOVE的资源要求](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-an-irp-mn-remove-device-request)。

## 2026-10-08：0.5.5 两轮 PnP 完整解锁试验

用户实测重新初始化保留解锁状态，目标调整为正常启动后／睡眠恢复后按需解锁。
新增 filter-unlock-handoff：固定实例 NVIDIA Code0起步，第一轮下层 START 窗口运行重构核心，
最终 SBR 必须晚于最后 GSP/SEC2写入并验证bootstrap、live0、GSP停止／DMA空闲。
通过才释放common-buffer/adapter/额外PDO引用；故意失败第一次START，等待Code10，
然后用户态第二轮禁用／启用，未武装透明START交给Windows/NVIDIA。
不手工修MSI，不从持有START/remove-lock的worker重启自身设备，不在失败时重复GPU操作。
这仍是候选方案，未证明下层START窗口的WDDM独占性或长期负载稳定性。

WDK编译、原有worker/core检查、INF验证、CAT可签名检查通过；尚未签名或加载。
固定包build/filter-driver；SYS SHA256 53451AE90CC0DB44715518F8CE88F89C8D7BCDEB54DAEF4834D93C54030B4524，
EXE SHA256 20206F86DCC8B15E8941EB6E0FFD7C6F55A5BC31C4086DD02EBB348EDC9C5465。
实机重点：是否到stage12/flags603、是否短暂Code10、第二START的MSI和NVIDIA Code0，
以及功耗读取／实际负载／180秒观察。启动和睡眠恢复后分别执行同一命令；GUI自动触发尚未接入。

## 2026-10-08：0.5.6 缩短扫描和阶段计时

0.5.5用户报告正常解锁交接，但渲染应用在过程内等待；不是已确认的整机CPU冻结。
0.5.6按function0/多功能标志减少桥发现身份探测（通常8192+真实多功能函数），
保持SBR子树全函数隔离扫描、唯一桥验证及全部双SBR／DMA释放门槛。
加入FILTER_STAGE_TIMING和FILTER_TOPOLOGY_SCAN；不修改固件DMA轮询/复位等待时序。
禁用触发REMOVE，不能在旧资源已释放的禁用状态继续解锁；两轮设备周期和长START
仍使NVIDIA渲染暂时不可用，优化效果需实测，不能承诺消除应用等待。
WDK编译／INF／CAT检查通过，固定目录，EXE沿用已验证0.5.5命令流程。
SYS未签名未加载，SHA256 59EFA5E9D6E8D21DAB824DA1A6457DD0BE4D03D1ED1B75A3E5D6489792330CAB。
