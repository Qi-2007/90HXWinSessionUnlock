# 90HXWinSessionUnlock

正式方案使用临时 UnlockSession 功能驱动：停用 NVIDIA，绑定临时驱动，在 START 完成后通过独立 IOCTL 解锁，确认 DMA 释放，再恢复原 NVIDIA INF。正常运行不安装 Lower Filter。

在本目录执行 `./build.ps1`，需要 VS MSBuild、.NET Framework 4.8.1 Developer Pack 和 SDK/WDK。默认只编译 Session；历史 Lower Filter 对照实验使用 `./build.ps1 -IncludeLegacyFilter`。

交付目录为 `build/session-driver`，正式文件为 `CMP90HXUnlocker.exe` 和 `CMP90HXUnlockSession.sys/.inf/.cat`。配套 worker 摘要嵌入驱动，更新 EXE 后需同步重新编译、签名对应驱动包。构建不会加载驱动或操作显卡。

EXE/SYS 的产品名称统一为 `CMP 90HX Windows Unlock`，组件描述区分 Session Worker 与 Driver，版本为四段数字。Session 复用部分旧 Lower Filter 源码，正式包使用临时 PCI 功能驱动；历史过滤驱动仅用于显式选择的对照实验。

按自己的流程签 SYS、重新生成并签 CAT 后，在管理员终端执行：

```powershell
cd .\build\session-driver
.\CMP90HXUnlocker.exe session-unlock --bdf 02:00.0 --restore-mode update --observe-seconds 180 --log session-unlock.log
```

异常留下恢复记录时使用 `session-restore --bdf 02:00.0`。签名、运行和恢复详见 [Session 说明](docs/session-unlock-test.md)。

NVIDIA 接管后的实时解锁寄存器读取尚未实现；临时驱动的 session-status 与历史解锁结果不能替代当前状态读取。

历史 Lower Filter 源码、对照命令、研究日志和蓝屏分析保留用于研究，正式 Console 不调用、安装或部署该包。历史说明见 [对照路径](docs/lower-filter-test.md)。
