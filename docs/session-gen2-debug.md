# Session stage 10 / Gen2 调试

客户机 2026-10-18 21:22 日志中，临时驱动绑定成功，功能和 PLM 事务返回成功；
stage 10 约 5.4 秒后失败，GPU_LINK=0x1101、BRIDGE_LINK=0x7101，两端仍报告 Gen1 x16。
最终 quarantine=1、retained=1；重启后 21:29 恢复 NVIDIA Code 0 并移除恢复记录。
这定位到 CoreGen2 阶段，但不能据此断定主板、显卡、转接设备或某个寄存器写入是根因。
PMU DMEM scrub timeout 在这些已完成的事务中也出现，不能单独认定它导致 stage 10 失败。

## 诊断改动

`driver/src/resume_unlock.h` 增加 FILTER_GEN2_* 日志。沿用原来的配置写入、
50 次轮询、100ms 间隔、连续 3 次 Gen2 且宽度不降低的成功条件。
新增读取和日志带来少量时间开销。

随后客户机 22:02 Native 成功日志证明：第一次重训练等待约 5.4 秒失败，
第二次请求后约 0.32 秒达到 Gen2 x16，并通过 NVIDIA 重接管后的验证。
据此 Session 现在与 Native 一样最多重训练 3 次，每次最多 50 次轮询，
每次重新统计连续稳定样本；只重复链路重训练请求，不重复固件、SBR 或完整解锁。
保留原 120 秒事务预算，预算/fault、写入失败或 Link Disable 均立即停止。
16 位桥写入方式、成功条件和失败后的 DMA 隔离保持不变，供单变量对照。
此修改仍需客户机验证，Native 的一次成功不能证明 Session 必然成功。

- BEGIN：基线宽度和轮询参数，diag=1 标记诊断代码，attempts=3 标记本次重试修改。
- REG：GPU 和直接上游桥的 BDF、能力偏移、PCIe flags、Link Capabilities/Capabilities 2、
  Link Control/Status、Link Control 2/Status 2；分别在写入前、重训练请求后、结束时记录。
- POLL：第一次、每十次以及成功采样时的链路状态和连续达标计数；100ms 采样可能漏掉短暂训练状态。
- ATTEMPT / ATTEMPT_TIMEOUT：本次重训练序号及单次等待超时，POLL 带 attempt 字段。
- END：成功或具体失败分支及耗时。受共用 32 KiB 日志缓冲区限制，不记录每次轮询。

| END reason | 下一步核对 |
| --- | --- |
| capability | 两端 PCIe 版本和 GPU 原有速度支持判断 |
| gpu-target-write / bridge-target-write | Link Control 2 的目标速度及读回值 |
| gpu-link-control-write | GPU Link Control 写入与读回 |
| bridge-link-disabled | 桥 Link Disable 位 |
| bridge-retrain-write | 桥重训练写入路径是否拒绝 |
| budget-or-fault | 事务预算、fault、deadline 日志 |
| link-not-stable-gen2 | 目标速度是否保持为 2，两端速度、训练位、链路宽度与基线对比 |
| ready | Gen2 条件通过；若后续失败，按新的失败阶段检查 |

桥写入 API 返回成功不代表链路已经完成训练；REG 的读回值和后续 POLL 才是后续证据。

## 客户机采集步骤

1. 起点必须已恢复 NVIDIA Code 0、没有待恢复记录。记录主板型号、BIOS 版本、
   显卡插槽，以及是否经过延长线、转接卡或 PCIe 交换芯片；记录 BIOS 当前 PCIe 速度设置。
2. 按项目构建和签名流程制作完整 Session 配套包，再重新打包 Console。
   客户日志标记 filter=0.6.5，本地源码已有后续变化；诊断不能当作相同二进制的严格对照。
   worker 摘要嵌入 SYS，Console 也校验包摘要，不能只替换单个 SYS 或混用旧包。
3. 按正式更新流程安装诊断包并重启，确认加载更新后的驱动。
4. 只执行一次 Session 解锁并保存完整日志，确认 stage 10 出现 FILTER_GEN2_BEGIN diag=1 attempts=3。
   没有该标记时，先确认运行版本或是否尚未进入 stage 10，不据此比较训练结果。
5. 若出现 quarantine=1 或 retained=1，保留恢复记录，重启后运行 session-restore。
   确认 SESSION_RESTORE_COMPLETE 后才考虑下一次实验。
6. 先分析诊断数据。确需比较 BIOS Auto/Gen2 或直插/转接时，每次只改一个条件，
   记录原设置并按平台维护流程操作；不要先放宽成功条件、强行释放 DMA 或循环解锁。

交回完整 session 日志和上述平台信息即可，不需要反复执行同一失败流程。

主机回归检查：`./tools/test-session-gen2.ps1`。脚本从实际驱动源码提取 Gen2 函数，
用模拟 PCI 配置和时钟验证首次/第二次/第三次成功、重试上限、稳定计数复位、
宽度和训练条件、两端速度、预算及写入失败。测试不接触硬件，不能代替实机验证。
