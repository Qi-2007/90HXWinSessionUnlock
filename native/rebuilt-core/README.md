# GA102 源码核心 v5.1

2026-10-08。当前 Windows GA102 `10de:220d` 解锁流程中此前复用的主机底层函数已迁到 C 源码。**发布核心不再链接原 469dc0c 主机机器码；GPU 固件仍为固定二进制数据，VBIOS 固件仍来自显卡 PROM。v4 已由用户确认实机解锁成功。v5 实机在 FWSEC 完成后返回 -1001；v5.1 修正 SEC2 复位前检查，v5.1 已签名并打包，2026-10-08 用户确认实机解锁成功。**

## 源码范围

| 文件 | 实现 |
|---|---|
| core.c | 七个 Windows 导出、计算/图形策略、Gen2 扫描和恢复、导出 GSP 复位 |
| transactions.c | 固件阶段、64 KB payload、选择器、handoff 和 DMA 所有权 |
| primitives.c | 身份、显存、ELF 签名查找、radix3、输入 DMA、4096 字节 WPR metadata、释放 |
| vbios.c | PROM 镜像链、BIT、FWSEC V2/V3、FRTS interface、PMU VDE/DEVENG 和缓存 |
| falcon.c | 内部 managed GSP 复位、FWSEC 签名/命令修补、DMA、启动和完成核验 |
| booter.c | SEC2 复位、签名版本、IMEM/DMEM DMA、halt、两次稳定目标就绪核验、超时保留 |
| pmu.c | PRI ring 恢复、PMU VDE IMEM/DMEM 加载和 devinit |
| power.c | GR 上电和媒体寄存器操作 |

GA104、Turing、其他设备 ID 和 legacy booter 模式不在本次复刻范围。GA102 managed FWSEC 执行路径支持 V3，与原路径一致；V2 解析保留用于兼容性检查。

## 发布与参考代码隔离

发布文件沿用 `nvpermissive-rebuilt-hybrid.o` 名称以兼容打包脚本，但内容已为源码核心，181904 字节。只链接八个 C 对象和 `firmware-data.o`。未定义符号仅为 ioread32、iowrite32、memset；没有 backend_* 或 reference_* 导出。每个源码对象和最终发布对象均有依赖检查，禁止重新引入原执行后端。

firmware/ 保存六份数据及 SHA-256 清单。build-core-firmware.py 固定各资源摘要，生成没有可执行节或重定位的数据对象。首次提取固定原对象 SHA-256 b533b7b245ed606c151ca336b9a6bacebe935e3ec478246e879c668a4dd98a6a。后续用这些资源即可编译；已验证数据构建在原对象路径不存在时成功，以及 -SourceOnly 编译与正常发布对象摘要一致。

GPU 固件没有公开源码，不能称整个 GPU 解锁链完全开源。本次消除的是原 .o 中主机执行代码依赖。

nvpermissive-rebuilt-reference-test.o 单独链接原对象和 booter-test-shim.c，仅用于离线对照，不放入 v5 包。发布核心不调用它。

## 行为与验证

保留目标提前就绪路径：目标从未开放变为 FFFFFFFF、PCI/BOOT0 身份一致、预期 WPR、mailbox/BROM 状态成立、DMA 空闲，并连续两次状态一致。handoff 核验前保留已提交 DMA，失败时不猜测设备已经停止访问，不额外复位。未证明实机总耗时更短。

v5.1 修正：原 managed 路径允许 SEC2 在复位前不可访问。源码恢复该复位顺序，仍检查 PCI/BOOT0 和 WPR 身份；复位后关键 MMIO 必须有效才允许 DMA。新增 CPUCTL、HWCFG2、engine 在复位后恢复的两种签名对照，写入、延时、镜像及结果证据与原对象一致。持续不可访问的 CPUCTL 在复位后退出，释放未提交的 DMA，不启动固件。

已通过 1000 组策略、事务所有权测试、九个完整 64000 字节 payload 对照，以及真实 ELF 在模拟硬件下的 radix/metadata、VBIOS V2/V3、FWSEC 成功/错误/超时、GR/媒体、VDE/PMU、两种 booter 签名、halt、运行 PC 变化时目标就绪、DMA 错误/超时、分配失败和无可用签名路径。匹配路径检查返回值、写入/延时轨迹、镜像/签名字节和结果结构。另有既有 128 组策略及 worker self-test/core-test。

GPU 固件没有在模拟器执行，离线测试不能替代实机解锁。证据在 build/rebuilt-core/source-boundary.json、compatibility.json、manifest.json 和 worker-hardware/rebuilt-test.log。静态报告 current_loader_accepts=false 表示普通生产 loader 的旧摘要白名单不接纳新对象；v5 worker 单独固定新核心摘要，并已通过实际 loader 测试。

## 构建与签名

需要 Zig 0.13.0、Python、.NET MSBuild 和 WDK。构建不会加载驱动或执行硬件解锁。

```powershell
.\tools\build-rebuilt-core.ps1 -Zig 'C:\path\to\zig.exe'
.\tools\build-rebuilt-worker.ps1 -EnableHardwareCommands
.\tools\build-rebuilt-driver.ps1 -OutputDirectory "$PWD\build\rebuilt-core\dma-driver-v5.1"
```

仅重编译发布核心、不运行原对象对照时用 -SourceOnly，摘要写入 manifest-source-only.json。参考对照仍需要原对象和 Capstone。

独立 v5 包在 build/rebuilt-core/dma-driver-v5.1/：CMP90HXDma.sys、匹配的 CMP90HXUnlocker.exe、配置、核心和清单。新 worker 摘要已固定到新驱动，因此 SYS 需要重新签名。签名后构建 GUI：

```powershell
.\tools\build-rebuilt-gui.ps1 -InputDirectory "$PWD\build\rebuilt-core\dma-driver-v5.1" `
  -DriverPath "$PWD\build\rebuilt-core\dma-driver-v5.1\CMP90HXDma.sys" `
  -OutputDirectory "$PWD\build\rebuilt-core\gui-v5.1"
```

旧 v3/v4/v5 解锁包已清理，当前仅保留 v5.1。v5.1 已重新编译匹配的核心、worker 与驱动，签名核验 Valid；gui-v5.1 已打包，68 项工作流测试及包检查（退出码 0）通过。2026-10-08 用户报告实机解锁成功。此记录不代表已验证长时间负载或所有设备配置。旧历史 README 已删除，以本文件为准。
