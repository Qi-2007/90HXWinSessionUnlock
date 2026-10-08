# CMP90HX Windows Filter Driver Unlock

独立的 Lower Filter 解锁研究项目，当前开发版0.6.0新增临时功能驱动。研究完整解锁、IOMMU DMA 与 NVIDIA PnP 交接。
不依赖旁边的 90HXWindowsUnlock 项目、原 ELF .o、GUI 或 vendor 目录。

在本目录执行 `./build.ps1`，需要 VS MSBuild、.NET Framework 4.8.1 Developer Pack 和 SDK/WDK。
固定交付目录为 `build/filter-driver`，文件名沿用 CMP90HXLowerFilter.sys、CMP90HXUnlocker.exe。
新 worker 仅保留 Filter 命令。迁移后 EXE 摘要改变，必须与重新编译的 SYS 一起更新并签名。
构建只生成文件，不签名、加载驱动、不重启设备。

签 SYS 后重新生成并签 CAT、安装 INF、重启，再在管理员终端运行：

```powershell
cd .\build\filter-driver
.\CMP90HXUnlocker.exe filter-unlock-handoff --bdf 02:00.0 --observe-seconds 180 --log filter-handoff.log
```

`src/` 为独立命令行工具；`driver/` 为 Lower Filter；`native/rebuilt-core/` 为该项目自己的核心和固定固件副本。
`docs/` 保存实验说明、研究与三次蓝屏对比；`evidence/` 保存迁移前日志与原始 dmp。
`migration/` 保存复制哈希清单、迁移前测试包及清理原项目时涉及的文件备份。
迁移前0.5.5用户反馈解锁及NVIDIA接管正常，过程中渲染程序等待；0.5.6减少扫描并增加阶段计时，尚未证明消除等待。
新路径先完成NVIDIA停用和临时驱动START，再在IOCTL中执行完整解锁，最后恢复准确的原NVIDIA INF；不长时间挂起START。
实机命令为 `CMP90HXUnlocker.exe session-unlock --bdf 02:00.0 --log session-unlock.log`。
新session路径的签名、测试及失败恢复见 [0.6.0测试说明](docs/session-unlock-test.md)。
原Lower Filter签名、安装、测试及已知限制见 [对照路径](docs/lower-filter-test.md)。
