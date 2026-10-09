# Ventoy 移植版：单命令启动

公开入口只有一个 GRUB 命令，**不依赖 GRUBFM**，也不要求运行原版 Ventoy
的大型 `grub.cfg`：

```grub
insmod ventoy

# Linux ISO
ventoy (hd0,gpt2)/ISO/archlinux.iso

# Windows ISO / WinPE
ventoy (hd0,gpt2)/ISO/winpe.iso
```

命令会验证物理文件、识别 Linux/Windows ISO、自动寻找对应的 Ventoy 运行资源，
调用原有 `vt_*` 引擎构建 Chain，再使用原版 EFI Consumer 完成启动。
GRUB 的命令行、`grub.cfg` 或任何普通菜单都可以调用它；无需界面模块的
专用注册、生命周期依赖或 Windows 启动逻辑。

## 启动前检查与运行资源

```grub
ventoy --check (hd0,gpt2)/ISO/winpe.iso
ventoy --runtime=(hd0,gpt1)/ventoy (hd1,gpt2)/ISO/winpe.iso

# 也可以设置默认的运行资源目录
set ventoy_runtime=(hd0,gpt1)/ventoy
```

默认先检查 ISO 所在分区的 `/ventoy`，再遍历其他可见分区。资源必须是
彼此匹配的版本：

- Linux x64：`ventoy.cpio`、`ventoy_x86.cpio`、`ventoy_x64.efi`、
  `iso9660_x64.efi` 等。
- Windows x64：`ventoy_x64.efi`、`udf_x64.efi`、
  `iso9660_x64.efi`、`vtoyjump64.exe`。

Windows 链路会自动检测 UDF、读取 BCD 并定位 WIM，必要时回退到
`boot.wim`。不要强行传入 Linux 专用的 `isoefi=on` 或 `FirstTry`；
这些参数此前导致了错误的 Windows 启动复现。

## 实测结果

在隔离的 x86_64 UEFI QEMU 测试中：

- Arch Linux ISO 已完成 EFI、内核及安装环境自动登录验收。
- **公共 `ventoy PATH` 的 WinPE 启动已进入完整 Windows 图形桌面。**
- 客体内打开 `X:\Windows\System32\ventoy.log`，实际出现
  `OpenVirtualDisk success`、`VentoyAttachVirtualDisk success` 和
  `Mount ISO FILE: SUCCESS`。
- Windows 资源管理器出现相匹配的 UDF DVD 卷。WinPE 的 `vtoyjump64.exe`
  已将原始 `C:\ISO\winpe.iso` 挂载成功。
- Windows Chain 数据与三月份已验收的结果一致：4 个 Override Chunk、
  460,988,416 字节虚拟镜像和正确的 UDF 标志位。

验证运行资源和截图位于：
`../out/testenv/work/winpe-direct-verified-20261009/`。

## 测试命令

```sh
tests/ventoy_boot_qemu.sh --check
tests/ventoy_boot_qemu.sh --boot

tests/ventoy_consumer_qemu.sh --direct-check
VENTOY_TEST_KEEP=1 VENTOY_SCREENSHOT=1 VENTOY_CAPTURE_AFTER=155 \
  tests/ventoy_consumer_qemu.sh --direct-boot
```

Windows 客体必须使用合适的 IDE/SATA 设备及可写 QCOW2 覆盖层；
最初错误地使用 VirtIO 时，虽然 WinPE 桌面可见，但 Windows 内找不到原始
ISO，`vtoyjump` 挂载失败。上述脚本使用原始 RAW 作为未修改的 backing，
QEMU 对覆盖层的写入不会影响原始测试盘。

**验收界限**：`--check` 只验证镜像和资源；`--direct-boot` 自动验证
GRUB/Chain/EFI 阶段，Windows 桌面及 ISO 挂载仍需要查看实际客体画面和
`ventoy.log`，QEMU 超时不等于系统启动成功。其他 Windows 发行版 ISO、
BIOS Windows 单命令、VHD/WIM 独立文件等尚未逐项通过端到端验收。

普通磁盘上原版 Consumer 可能有专用分区检查造成的误警告和短暂等待，
尚未独立化；本轮没有修改 EFI Consumer，也没有修改 GRUBFM。详见
`docs/ventoy/DIRECT_BOOT.md` 和 `docs/ventoy/CONSUMER_REGRESSION.md`。
