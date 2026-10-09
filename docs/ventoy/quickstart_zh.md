# Ventoy 移植版：直接启动与 GRUBFM 使用指南

本项目的公共启动入口是 `ventoy PATH`。不需要使用原版 Ventoy 的整个
`grub.cfg`，也不要求镜像文件位于 Ventoy 专用分区。

## 1. 一条命令启动 Linux ISO

在 x86_64 UEFI GRUB 中：

```grub
insmod ventoy
ventoy (hd0,gpt2)/ISO/archlinux.iso
```

命令会打开和检查镜像、定位运行资源、生成 Ventoy 启动数据、调用 EFI
Consumer，并实际执行 `boot`；失败时返回 GRUB 错误。

可以先检查镜像存在及资源文件可读性（不保证该发行版一定能启动）：

```grub
ventoy --check (hd0,gpt2)/ISO/archlinux.iso
```

## 2. 资源不在镜像所在分区时

本项目目前仍复用原版 Ventoy 的 EFI Consumer 和 CPIO 文件。默认会先查找
`(<镜像所在设备>)/ventoy/`，再搜索其他 GRUB 可访问分区的 `/ventoy/`。

如需指定确定的版本和位置：

```grub
set ventoy_runtime=(hd0,gpt1)/ventoy
ventoy (hd1,gpt2)/ISO/archlinux.iso

# 不修改全局配置的单次覆盖
ventoy --runtime=(hd0,gpt1)/ventoy (hd1,gpt2)/ISO/archlinux.iso
```

运行资源需包含匹配架构的 `ventoy.cpio`、`ventoy_x86.cpio`、
`ventoy_x64.efi` 和 `iso9660_x64.efi` 等文件。请使用彼此匹配的一套运行
资源，勿混用不同版本的 EFI Consumer。

## 3. GRUBFM 文件操作菜单

GRUBFM 是用户界面，Ventoy 是启动后端。

将本仓库的配置模板合并到 GRUBFM 的实际数据目录：

- `extras/grubfm/boot/grubfm/types/iso`
- `extras/grubfm/boot/grubfm/rules/ventoy.cfg`

若已有 `types/iso`，请合并 `[0]` 菜单规则，不要覆盖原有动作。GRUBFM
启动后选择 ISO 文件，点击 **Boot with Ventoy** 即可。规则调用的仍然是
`ventoy "$grubfm_file"`，不包含 `vt_*` 内部实现细节。

## 4. 尚未覆盖的能力

目前新命令只在 **x86_64 UEFI 的 Linux ISO** 上完成实际启动验收。
Windows ISO / WIM / WinPE、VHD(x)、VTOY、IMG 和 BIOS 的单命令启动尚未
在这个新入口上验收通过。原版 Ventoy 旧脚本中的 `vt_*` 命令是内部操作，
不能替代统一入口。

旧版文档出现过 `vtlinuxboot` / `vtwimboot` 示例，这些不是当前
`ventoy.mod` 的注册命令，不应作为本移植版的使用方式。

## 5. QEMU 验收

```sh
tests/ventoy_boot_qemu.sh --check
tests/ventoy_boot_qemu.sh --boot
tests/ventoy_boot_qemu.sh --grubfm
tests/ventoy_boot_qemu.sh --plain

# 带屏幕截图（保留至临时目录）
VENTOY_TEST_KEEP=1 VENTOY_BOOT_CAPTURE=1 tests/ventoy_boot_qemu.sh --boot
```

`--boot` 默认校验 EFI 引导菜单已经接管且未返回 GRUB；
`VENTOY_BOOT_CAPTURE=1` 用于人工检查随后进入 Linux 内核和登录界面的结果。
QEMU 默认使用 3 GiB 内存，以免现代 Arch Linux initramfs 在不足 1 GiB
的虚拟机内发生 OOM 或 EFI Stub 加载失败。所有测试镜像以只读方式挂载。

详细行为及限制：`docs/ventoy/DIRECT_BOOT.md`。

### 关于普通磁盘的兼容提示

`--plain` 测试使用 QCOW2 覆盖层删除原测试盘的 Ventoy 专用第二分区，
并将运行资源放在另外的 EFI 分区中。Linux ISO 仍能进入启动菜单，
验证了不强制要求原版分区布局。

**现存限制**：没有 Ventoy 专用分区时，原版 `ventoy_x64.efi` 可能会
错误提示设备不受支持、等待 10 秒后继续启动。这是外部 EFI Consumer
的硬编码警告，不能通过假冒 VLNK 参数安全解决；后续需要配套移植
Consumer 的明确 direct-boot 模式。
