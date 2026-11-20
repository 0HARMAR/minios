# Mini-OS UEFI 启动（`uefi-boot` 分支）

这个分支从首提交 `a505332` 拉出，给 Mini-OS 增加一条**原生 UEFI 启动链**，
让它在现代笔记本（纯 UEFI、无 CSM）上也能从 U 盘启动。

首提交里的 `long-mode/`、`protected-mode/` BIOS 软盘构建**原样保留**，
UEFI 相关代码全部放在 `uefi/` 下，互不影响。

当前里程碑只做「启动自检」：

- UEFI 固件加载 `BOOTX64.EFI`
- 加载器从 ESP 读取 `KERNEL.BIN`
- 建立恒等映射页表，退出 Boot Services
- 跳到 64 位内核
- 内核在 GOP 帧缓冲上打印 CPU / 长模式 / 内存 / 帧缓冲信息，然后停机

还没有中断、键盘、堆、文件系统和驱动。

---

## 1. 目录结构

```
uefi/
├── boot/
│   ├── efi.h          # 最小自包含 UEFI 定义（无需 gnu-efi）
│   ├── bootx64.c      # UEFI 加载器
│   ├── handoff.S      # 切 CR3/GDT/栈并跳入内核
│   └── boot.lds       # PE32+ EFI 应用链接脚本
├── common/
│   ├── format.c/h     # 迷你 printf
│   ├── string.c/h     # memcpy/memset/...
│   └── serial.c/h     # COM1 轮询输出
├── include/
│   └── bootinfo.h     # 加载器 ↔ 内核的 BootInfo
├── kernel/
│   ├── start.S        # 内核入口
│   ├── kernel.c       # 自检 + GOP 控制台
│   ├── font8x16.h     # 内置点阵字库（自动生成）
│   └── kernel.lds     # flat binary，链接到 0x100000
├── tools/
│   ├── mkdisk.py      # 纯 Python 生成 GPT + FAT16 镜像
│   └── genfont.py     # 从 PSF 控制台字体生成 font8x16.h
├── build.sh           # 编译 BOOTX64.EFI 和 KERNEL.BIN
├── mkdisk.sh          # 构建 + 生成可写 U 盘的镜像
├── run_qemu.sh        # OVMF 下启动（磁盘 / USB）
└── README.md
```

构建产物都在 `uefi/build/`：

```
uefi/build/BOOTX64.EFI        PE32+ EFI 应用
uefi/build/KERNEL.BIN         固定链接到 0x100000 的 flat binary
uefi/build/minios-uefi.img    64 MiB GPT 磁盘镜像
```

---

## 2. 依赖

构建只需要：

- `gcc`
- `binutils`（`ld`、`objcopy`、`nm`）
- `python3`

不需要 gnu-efi、EDK2、mtools，也不需要 root。

QEMU 验证额外需要：

- `qemu-system-x86_64`
- `ovmf`（提供 `/usr/share/OVMF/OVMF_CODE_4M.fd` 等）
- `ffmpeg`（可选，用于把截图 PPM 转成 PNG）

Ubuntu / WSL 安装：

```bash
sudo apt install build-essential python3 qemu-system-x86 ovmf ffmpeg
```

---

## 3. 构建、制盘、运行

```bash
git switch uefi-boot

# 1) 编译加载器和内核
./uefi/build.sh

# 2) 生成 64MiB GPT + FAT16 ESP 镜像（内部会先调用 build.sh）
./uefi/mkdisk.sh
```

产物：`uefi/build/minios-uefi.img`。

### 在 QEMU 里测试

```bash
# AHCI 磁盘启动（带窗口；没有 DISPLAY 时自动无窗口）
./uefi/run_qemu.sh --disk

# USB 存储启动（最接近真实 U 盘）
./uefi/run_qemu.sh --usb

# 无窗口 + 串口日志
./uefi/run_qemu.sh --disk --headless

# 自动等待内核停机、截图并退出
./uefi/run_qemu.sh --disk --headless --screenshot uefi/build/shot.png --timeout 40
```

`run_qemu.sh` 会自动检测 OVMF。串口日志默认写到
`uefi/build/serial-<disk|usb>.log`（使用 `--screenshot` 时），也可以
`--serial FILE` 指定。内核会停住不重启，普通模式按 `Ctrl-C` 退出。

`--usb` 使用 QEMU 的 `qemu-xhci` + `usb-storage`，会真实走一遍
USB 大容量存储枚举和 `\EFI\BOOT\BOOTX64.EFI` 查找路径。

### 镜像布局

| 位置 | 内容 |
| --- | --- |
| LBA 0 | 保护性 MBR |
| LBA 1 | 主 GPT 头 |
| LBA 2–33 | 主 GPT 分区表 |
| LBA 2048 | ESP 起始（1 MiB 对齐） |
| LBA 131038 | ESP 结束（64 MiB 镜像） |
| LBA 131039–131070 | 备份 GPT 分区表 |
| LBA 131071 | 备份 GPT 头 |

ESP 是 FAT16、4 KiB 簇：

```
/EFI/BOOT/BOOTX64.EFI
/EFI/MINIOS/KERNEL.BIN
```

`BOOTX64.EFI` 是 UEFI 可移动介质默认启动路径，固件会自动找它。

---

## 4. 写入 U 盘并在笔记本上启动

> **警告：写镜像会清空整块 U 盘，并且写的是“整盘”而不是某个分区。**
> 不要 `dd` 到 `/dev/sdX1` 这类分区设备。

### Linux

```bash
lsblk                                     # 确认 U 盘设备，例如 /dev/sdb
sudo umount /dev/sdb* 2>/dev/null || true
sudo dd if=uefi/build/minios-uefi.img of=/dev/sdb bs=4M status=progress conv=fsync
sync
```

### Windows

推荐 **Rufus**：

1. 选择 U 盘；
2. 选择 `minios-uefi.img`；
3. Rufus 询问写入方式时选 **DD 模式**（不要选 ISO 模式）；
4. 开始写入。

也可以用 **Win32DiskImager** 直接写 `.img`。

### 笔记本固件设置

1. 进入 BIOS/UEFI Setup（开机时 F2 / F10 / Del 等）。
2. **关闭 Secure Boot**（Security → Secure Boot → Disabled）。
   本镜像未签名，Secure Boot 开启时固件根本不会加载 `BOOTX64.EFI`，
   而且不会显示任何 Mini-OS 错误。
3. Boot Mode 设为 **UEFI**（有 CSM/Legacy 选项的话关闭它）。
4. 保存退出，插上 U 盘，开机按启动菜单键：
   - Dell / Lenovo：F12
   - HP：F9
   - 华硕 / 宏碁：Esc 或 F12
   - 其他：F8 / F11 / F12
5. 选择带 **UEFI:** 前缀的 U 盘条目。

如果启动菜单里没有 U 盘：

- 换一个 USB 口（优先 USB 2.0 口）；
- 关闭 Fast Boot；
- 确认写盘时用的是 DD 模式；
- 某些固件只认 FAT32 ESP；本镜像的 ESP 是 FAT16（UEFI 规范允许），
  如果遇到这种固件，可以手动把两个文件复制到已有的 FAT32 ESP，
  见下一节。

### 手动复制到已有 ESP（可选）

如果机器上已有 Linux/Windows 的 FAT32 ESP，也可以不写整盘：

```
ESP/EFI/BOOT/BOOTX64.EFI
ESP/EFI/MINIOS/KERNEL.BIN
```

`KERNEL.BIN` 的位置是加载器里写死的；两个路径都要保留。

---

## 5. 预期输出

加载器（白色文字，来自 UEFI ConOut）：

```
Mini-OS UEFI Loader
===================

Firmware : ...
GOP      : 1280x800, 32 bpp, pitch 1280 px, format 1
Framebuf : 0x80000000 (3 MiB)
Kernel   : \EFI\MINIOS\KERNEL.BIN
Kernel   : 7736 bytes loaded at 0x100000
Loader   : 0x10000000 (368640 bytes)
Usable   : 974 MiB
Paging   : identity map 0-4 GiB, 2 MiB pages
BootInfo : 0x10059000, magic 0x4d494e4955454649

Exiting boot services...
```

随后加载器切到内核，内核清屏并打印：

```
Mini-OS 64-bit Kernel
=====================

BootInfo    : 0x10059000, magic OK, version 1
CPU         : ...
CPU vendor  : ...
Long mode   : EFER.LME=1, EFER.LMA=1
Paging      : CR0.PG=1, CR4.PAE=1, 4-level
Usable RAM  : 974 MiB
Framebuffer : 1280x800, 32 bpp, pitch 1280, format 1
FB address  : 0x80000000 (3 MiB)
Text grid   : 78 x 21 cells, scale 2
Kernel      : 0x100000, 7736 bytes
Loader      : 0x10000000, 368640 bytes
Firmware    : ...
Boot path   : UEFI -> GOP -> ExitBootServices -> 64-bit kernel
Console     : GOP framebuffer + COM1

[ SYSTEM HALTED - no interrupts or keyboard in this build ]
```

看到 `SYSTEM HALTED` 就说明整条 UEFI → 64 位内核链路已经打通。

---

## 6. 实现说明

### 启动链

```
UEFI 固件
  └─ LoadImage / StartImage: \EFI\BOOT\BOOTX64.EFI
       ├─ LocateProtocol(GOP)          读取当前模式，不切换分辨率
       ├─ LoadedImage→DeviceHandle
       │    └─ SimpleFileSystem       打开 ESP
       │         └─ 读 \EFI\MINIOS\KERNEL.BIN 到物理 0x100000
       ├─ GetMemoryMap / AllocatePages
       ├─ 建恒等映射页表（2 MiB 大页）
       ├─ SetWatchdogTimer(0)          关闭固件看门狗
       ├─ ExitBootServices
       └─ minios_handoff(BootInfo, 0x100000, CR3, stack)
```

`KERNEL.BIN` 是固定链接到 `0x100000` 的 flat binary。加载器用
`AllocatePages(AllocateAddress)` 精确分配这个地址；如果被固件占用，
会打印状态码并停机，不会偷偷换地址（因为内核没有重定位信息）。

### 内存映射

- 恒等映射范围：`max(4 GiB, 加载器末尾, 帧缓冲末尾)`，向上取整到 GiB；
- 使用 2 MiB 大页；
- 如果加载器或 GOP 帧缓冲被固件放在 4 GiB 以上，会自动扩展；
- 映射上限 64 GiB（真机 GOP 帧缓冲远低于此）；
- 如果固件开启了 5 级页表（`CR4.LA57=1`），会额外建一层 PML5，
  避免把 4 级页表当成 5 级使用。

### BootInfo

`uefi/include/bootinfo.h` 定义加载器传给内核的只读结构：

| 字段 | 含义 |
| --- | --- |
| `magic` / `version` | `MINIUEFI` / 1 |
| `fb_*` | 宽高、pitch（像素）、格式、bpp、通道掩码、物理地址/大小 |
| `usable_ram` | 内存映射里 Conventional + LoaderCode/Data 的总和 |
| `kernel_base/size` | 内核加载地址和长度 |
| `loader_base/size` | `BOOTX64.EFI` 的加载地址和长度 |
| `firmware` | 固件厂商字符串（CHAR16 转 ASCII），版本号 |

内核只读这个结构，不依赖 UEFI 的任何服务。

### 内核控制台

- 不依赖 VGA 文本模式（`0xB8000` 在纯 UEFI 笔记本上通常不可见）；
- 直接写 GOP 线性帧缓冲；
- 内置 8x16 点阵字库，按屏幕高度自动缩放 1–4 倍；
- 白/灰文字 + 黑底，左右和上下留边距；
- 同时把相同文字写到 COM1（115200 8N1），方便 QEMU/串口抓日志。

字库来自 Debian `console-setup` 的 `Lat15-Terminus16.psf.gz`。Debian 的
版权文件说明“All console fonts are public domain by nature”；生成脚本
`uefi/tools/genfont.py` 可重新生成 `font8x16.h`，构建本身不依赖该字体。

### 为什么不用首提交的 `boot_32.S` / `boot_64.S`

首提交的引导代码从 16 位实模式开始：BIOS 把 boot sector 放到 `0x7C00`，
再由它读软盘、开 A20、进保护模式、开 PAE/长模式。UEFI 应用一上来就是
64 位，已经没有实模式和 BIOS 中断；在 UEFI 下强行回到实模式既不通用，
也会破坏固件的内存管理。因此这条分支是**原生 UEFI 加载器**，而不是把
BIOS 引导代码塞进 UEFI。

---

## 7. 排错

### 开机直接回到固件菜单 / 没有 Mini-OS 输出

- Secure Boot 是否关闭；
- U 盘是否用 DD 模式写入（不是 ISO 模式，也不是把文件拖进 FAT 分区）；
- 启动菜单里选的是不是带 `UEFI:` 的条目；
- 换 USB 口，关闭 Fast Boot；
- 在 QEMU 里先跑 `./uefi/run_qemu.sh --usb`，确认镜像本身没问题。

### 屏幕显示 `KERNEL.BIN was not found on the ESP`

- 用 `./uefi/mkdisk.sh` 重新制盘，不要手工只复制 `BOOTX64.EFI`；
- 确认 ESP 里同时有：
  `\EFI\BOOT\BOOTX64.EFI` 和 `\EFI\MINIOS\KERNEL.BIN`；
- 文件名大小写不敏感，但路径必须一致。

### `cannot allocate KERNEL.BIN at physical 0x100000`

固件把 1 MiB 附近的低内存占用了（少数机器/虚拟化环境）。当前设计没有
内核重定位，加载器会明确报错。可以先在 QEMU 验证，换一台机器，或后续
给内核加 PIE/双链接地址支持。

### `GOP is not available` / `GOP does not provide a linear framebuffer`

固件没有可用的 GOP 线性帧缓冲（例如纯串口固件、`BltOnly` 模式）。
这种固件不是这条分支的目标；可以在 QEMU+OVMF 下验证镜像。

### `ExitBootServices failed after several retries`

固件在内存映射查询和退出之间又改了内存映射，重试 8 次仍失败。请记录
屏幕/串口日志和机器型号；这通常是固件实现问题。

### 加载器有输出，内核黑屏

- 固件 GOP 的像素格式/掩码比较特殊，内核会跳过绘制但串口仍有日志；
- 有串口的话接 COM1 看 `[kernel]` 输出；
- 在 QEMU 下用 `--screenshot` 对比，确认是固件差异还是代码问题。

### QEMU 报 `OVMF firmware not found`

```bash
sudo apt install ovmf
```

### WSL 下 QEMU monitor socket 报 `Operation not supported`

脚本已把 monitor socket 放在 `/tmp`（DrvFs 不支持 Unix socket）；如果你
手动启动 QEMU，请同样把 `-monitor unix:...` 指到 `/tmp`。

---

## 8. 已知限制

- 未签名，必须关闭 Secure Boot；
- 没有中断/IDT、键盘、PS/2、USB HID、堆、文件系统、驱动；
- 内核固定加载到 1 MiB，被占用则停机；
- 页表映射上限 64 GiB；
- 只支持 GOP 线性帧缓冲，不支持 `BltOnly`；
- 只做 UEFI，不生成 BIOS/UEFI 混合启动盘；首提交的 BIOS 软盘构建保留不变；
- 镜像的 ESP 是 FAT16；极少数只认 FAT32 的固件需要手动复制到已有 ESP。

---

## 9. 保留的 BIOS 构建

`long-mode/` 和 `protected-mode/` 下的脚本与源码没有改动，仍然可以按首提交
的 README 生成软盘镜像并用 `qemu-system-i386` / `qemu-system-x86_64` 启动：

```bash
cd protected-mode
./compile_bootloader.sh boot_loader.S ../floppy.img
./build_kernel.sh ../floppy.img
```

UEFI 镜像和 BIOS 软盘镜像是两套独立的启动路径。
