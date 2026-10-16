# 0.6.5：临时功能驱动，START 完成后解锁

本轮针对0.5.5用户反馈：解锁及NVIDIA接管正常，但渲染程序在解锁期间等待。
新路径先完成NVIDIA停用，让系统有机会处理渲染切换，然后绑定临时PCI功能驱动。
临时驱动的START不运行固件、不做SBR、不分配DMA；长操作只来自之后的独立IOCTL。
因此可以验证长START窗口是否是等待原因。尚未证明渲染不再卡顿或所有应用会切到核显。

输出固定为本项目 `build/session-driver`，新增 `CMP90HXUnlockSession.sys/.inf/.cat`。
原Lower Filter路径保留作为对照。两种驱动有各自控制设备，调用者均限制为配套EXE。
本轮新EXE摘要改变：测试session路径需要匹配新session SYS；继续测试filter路径也要更新其配套SYS。

0.6.1修正保存原驱动信息时的PnP属性编号：使用CM_DRP_HARDWAREID（0x02）读取硬件ID列表，避免误读设备描述（0x01）而报Unexpected hardware ID。该报错发生在禁用和驱动切换之前。

0.6.2修正重启后首次绑定成功但控制设备Win32=5：进程创建通知从驱动加载时开始，父进程此前已启动。START完成后自动启动配套子进程打开控制设备并运行解锁，父进程持续持有流程锁；子进程关闭句柄、确认DMA回收后才自动恢复NVIDIA。验证摘要和恢复记录保护保持启用。正常完成无需手工session-restore。

0.6.3速度实验：session重置后最小等待从250ms改为50ms，然后每20ms读取完整就绪条件；连续6次通过维持至少100ms稳定窗口，恢复预算仍约8秒。保留100ms复位断言、释放后100ms等待、全部22次SBR、固件事务和DMA回收门槛。Lower Filter保持250ms/50ms/3次策略。新增FILTER_SBR_TIMING用于比较实机耗时，未承诺加速幅度。SESSION_DEVICE_READY表示已恢复NVIDIA，其后的180秒仅为观察；确认稳定后可使用--observe-seconds 0立即退出。

0.6.4恢复速度实验：禁用前通过DI_FLAGSEX_INSTALLEDDRIVER只保存当前NVIDIA驱动节点，正常解锁后使用DiInstallDevice将该节点装回精确设备实例，并设置DI_NOFILECOPY避免重复复制同一包文件。保留安装器、设备初始化及Code0检查。该实验原默认--restore-mode direct；--restore-mode update使用原UpdateDriver路径作对照。保存节点失败或直接安装返回失败时回退旧路径；要求重启则保留恢复记录。手工session-restore继续用旧路径。新增SESSION_RESTORE_DIRECT/installMs与SESSION_RESTORE_READY/elapsedMs。实际速度和兼容性待实机验证。

参考：[DiInstallDevice](https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-diinstalldevice)、[安装参数](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/ns-setupapi-sp_devinstall_params_w)。

0.6.5恢复已验证策略：用户0.6.4实测直接安装27.863秒，比0.6.2原更新路径17.689秒慢，且没有回退。默认恢复方式改回update；direct仅作显式实验。核心实测20.578秒，原19.882秒，恢复250ms等待、50ms轮询、3次稳定样本，保留SBR计时。两个API内部都包括安装器与设备启动处理，单次日志不能断定是哪部分占主导。正常测试使用--restore-mode update。

## 签名和运行

1. 运行项目根目录 `./build.ps1` 编译正式 Session 包。构建不会加载、签名或操作显卡；旧 Lower Filter 仅在显式添加 `-IncludeLegacyFilter` 时编译。
2. 签名 `CMP90HXUnlockSession.sys`。如果要测试旧Filter路径，另签 `CMP90HXLowerFilter.sys`。
3. 在包目录重新生成CAT，然后签 `CMP90HXUnlockSession.cat`（旧路径另签对应CAT）。
4. 保持显卡当前为 NVIDIA Code 0。更新过已加载的session SYS，需要重启加载新版本。

```powershell
cd E:\code\90HX\90HXWindowsUnlock\90HXWinSessionUnlock\build\session-driver
& 'H:\Windows Kits\10\bin\10.0.28000.0\x86\Inf2Cat.exe' /driver:. /os:10_GE_X64 /uselocaltime
# 按自己的签名流程签对应CAT，然后在管理员终端执行：
.\CMP90HXUnlocker.exe session-unlock --bdf 02:00.0 --observe-seconds 180 --log session-unlock-065.log
```

不要提前 `/install` session INF，否则当前NVIDIA包信息无法在命令开始前正确保存。
新命令自动绑定并恢复；可以仅 `/add-driver` 预放入Driver Store，也可以直接由命令安装。
要求系统仅有一块在线90HX；UpdateDriver按hardware ID匹配，多90HX系统本版主动拒绝。
普通睡眠恢复后也可执行相同命令。本轮不在睡眠/START回调中自动解锁，不运行旧Filter武装命令。

## 完整流程与证据

- 保存当前设备实例、BDF、最具体hardware ID、NVIDIA Driver Store INF及SHA256到 `C:\ProgramData\CMP90HX\session-restore.json`。
- 完成禁用、确认Code22，用户态等待750ms供系统处理切换，然后绑定session INF并启用。
- 确认session服务和Code0；日志 `SESSION_START_COMPLETED` 之后才运行完整解锁IOCTL。
- 复用v5.1核心、双SBR和既有IOMMU common-buffer回收检查。成功stage12/flags=0xe03，应有 `SESSION_DMA_RELEASED`。
- 关闭session控制句柄后，强制恢复保存的原NVIDIA INF；不删除NVIDIA包、不重新选择其他版本。
- 最终 `SESSION_RESTORE_COMPLETE`、NVIDIA Code0，随后180秒观察。另确认功耗、实际负载、解锁状态和其他渲染程序是否仍等待。

控制IOCTL在PASSIVE worker运行；排队前释放FAST_MUTEX，避免在APC_LEVEL执行核心。
设备remove-lock保护FDO直到IOCTL结束；busy/quarantine时拒绝正常query-remove/stop和功耗请求。
长START核心从未在session构建中武装；旧ARM IOCTL被拒绝。
临时驱动在设备移除后仍可留在内核直到重启，以保留异常DMA现场；成功事务释放arena/adapter/额外PDO，不保留成功DMA。

## 失败恢复

如果安装API要求重启，就不提交解锁IOCTL。命令会在可确认安全时恢复原NVIDIA。
核心未写硬件的失败可恢复；已写硬件且回收未通过、busy或quarantine，不自动重绑，也不循环重试。
恢复记录仍保留。查看已绑定session状态：

```powershell
.\CMP90HXUnlocker.exe session-status --bdf 02:00.0 --log session-status.log
```

进程中断或恢复失败后，执行下面命令读取恢复记录和内核DMA状态：

```powershell
.\CMP90HXUnlocker.exe session-restore --bdf 02:00.0 --log session-restore.log
```

busy/quarantine需要先重启。重启后当前session驱动若没有提交事务，runs=0可恢复NVIDIA。
不要手工删除恢复记录绕过DMA检查；若session设备无法启动或恢复命令打不开控制设备，保留日志，并在重启后通过设备管理器选择原NVIDIA驱动恢复。
临时接管期间不进行新的睡眠/设备重启操作。

参考：[UpdateDriver的hardware ID范围及重启条件](https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-updatedriverforplugandplaydevicesw)、
[准确查找已安装INF的Driver Store路径](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupgetinfdriverstorelocationw)。
