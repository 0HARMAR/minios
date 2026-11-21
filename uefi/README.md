# Mini-OS UEFI 启动 + M0 Wi-Fi 侦察（`wifi-bringup` 分支）

这个分支从首提交 `a505332` 拉出，给 Mini-OS 增加一条**原生 UEFI 启动链**，
让它在现代笔记本（纯 UEFI、无 CSM）上也能从 U 盘启动。

首提交里的 `long-mode/`、`protected-mode/` BIOS 软盘构建**原样保留**，
UEFI 相关代码全部放在 `uefi/` 下，互不影响。

当前里程碑 = 启动自检 + **M0 Wi-Fi 侦察**：

- UEFI 固件加载 `BOOTX64.EFI`
- 加载器从 ESP 读取 `KERNEL.BIN`
- 建立恒等映射页表，退出 Boot Services
- 跳到 64 位内核
- 内核在 GOP 帧缓冲上打印 CPU / 长模式 / 内存 / 帧缓冲信息
- M0：把 UEFI memory map、ACPI RSDP、可选固件 blob 传给内核；
  内核全 bus/function 扫描 PCI，打印 BAR 大小、MSI/MSI-X 能力，
  并解析 `iwlwifi-9000-pu-b0-jf-b0-46.ucode` 的 TLV 头

M0 只读 PCI 配置空间（BAR sizing 除外，立即恢复），不编程 MSI/APIC、
不分配 DMA、不下载固件、不碰 802.11。还没有中断、键盘、堆、文件系统
和 Wi-Fi 设备驱动。

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
│   ├── kernel.c       # 自检 + GOP 控制台 + M0 入口
│   ├── memmap.c/h     # UEFI memory map + ACPI RSDP 摘要
│   ├── pci.c/h        # 全 bus/function PCI 扫描、BAR、MSI/MSI-X
│   ├── fw.c/h         # iwlwifi TLV 头解析（只读）
│   ├── log.h          # 内核日志接口
│   ├── io.h           # in/out 端口访问
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
uefi/build/WIFI9.UC           可选固件 blob（构建时从 /lib/firmware 解压）
uefi/build/minios-uefi.img    64 MiB GPT 磁盘镜像（含 WIFI9.UC）
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
git switch wifi-bringup

# 1) 编译加载器和内核
./uefi/build.sh

# 2) 生成 64MiB GPT + FAT16 ESP 镜像（内部会先调用 build.sh）
./uefi/mkdisk.sh
```

产物：`uefi/build/minios-uefi.img`。

`mkdisk.sh` 会优先从 `/lib/firmware/intel/iwlwifi/` 或
`/lib/firmware/` 找 `iwlwifi-9000-pu-b0-jf-b0-46.ucode.zst`，解压成
`uefi/build/WIFI9.UC` 并放进 ESP 的 `EFI/MINIOS/WIFI9.UC`。仓库不提交
固件二进制；可以用 `MINIOS_FW=/path/to/file.ucode` 覆盖，或让它缺省，
此时 M0 仍会做 PCI 扫描，只是没有 TLV 输出。

### 在 QEMU 里测试

```bash
# AHCI 磁盘启动（带窗口；没有 DISPLAY 时自动无窗口）
./uefi/run_qemu.sh --disk

# USB 存储启动（最接近真实 U 盘）
./uefi/run_qemu.sh --usb

# 无窗口 + 串口日志
./uefi/run_qemu.sh --disk --headless

# 自动等待内核停机、截图并退出（M0 推荐）
./uefi/run_qemu.sh --disk --headless \
    --serial uefi/build/m0-serial.log \
    --screenshot uefi/build/m0-shot.png --timeout 40
```

QEMU 的默认 e1000e NIC 会作为 M0 的测试 target，打印 BAR/MSI/MSI-X；
真实 9560 在目标笔记本上才会出现。

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
/EFI/MINIOS/WIFI9.UC     # 可选，M0 固件侦察
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

### 目标笔记本（M0）

本仓库当前的目标机是 Lenovo 81YJ（XiaoXinAir-14IIL 2020，i5-1035G1，
16 GB）。它的 Wi-Fi 是 **Intel Wireless-AC 9560，PCI `8086:34F0`，
subsystem `8086:0234`，位于 `00:14.3`（ACPI `\_SB.PCI0.CNVW`）**。
该机的 Secure Boot 已确认关闭，UEFI 镜像可以直接启动。

M0 目标机实测（2026-11-21）：

- BAR0：64-bit non-prefetchable，16 KiB，地址 `0x6013124000`（**在 4 GiB 以上**）；
- MSI-X：16 vectors，table BIR0 offset `0x2000`，PBA BIR0 offset `0x3000`；
  MSI 也支持 64-bit；两者当前都 disabled，IRQ 线为 255；
- ACPI：RSDP rev 0（ACPI 1.0），RSDT `0x6bc28000`，没有 XSDT；
- 固件 TLV：API 46；日志已通过 UEFI 变量读出（见下）。

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
ESP/EFI/MINIOS/WIFI9.UC     # 可选
```

`KERNEL.BIN`/`WIFI9.UC` 的位置是加载器里写死的；路径要保留。

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
Kernel   : 14396 bytes loaded at 0x100000
Loader   : 0x10000000 (135168 bytes)
Firmware : \EFI\MINIOS\WIFI9.UC
Firmware : 0x3dde7000, 1519568 bytes
ACPI RSDP: 0x3f77e000 sig RSD PTR  rev 0
Usable   : 975 MiB
Paging   : identity 0-4 GiB, 2 MiB pages
Paging   : 4-level, CR3=0x1001e000
BootInfo : 0x10020000, magic 0x4d494e4955454649

Exiting boot services...
MemMap   : buffer 0x3dde5000, 8192 bytes (desc 48)
```

随后加载器切到内核，内核清屏并打印：

```
Mini-OS 64-bit Kernel
=====================

BootInfo    : 0x10020000, magic OK, version 2
CPU         : ...
CPU vendor  : ...
Long mode   : EFER.LME=1, EFER.LMA=1
Paging      : CR0.PG=1, CR4.PAE=1, 4-level
Usable RAM  : 975 MiB
...
Console     : GOP framebuffer + COM1

--- M0 Wi-Fi probe ---

UEFI memory map: 112 descriptors at 0x3dde5000 (desc 48 bytes, version 1)
  conv 01: 0x0000000000-0x00000a0000 640 KiB
  ...
  conventional: 974 MiB in 8 ranges, largest 699 MiB
ACPI RSDP: phys 0x3f77e000 sig 'RSD PTR ' rev 0 oem 'BOCHS ' rsdt 0x3f77d000 (ACPI 1.0, no xsdt)

PCI topology (CF8/CFC, all buses/functions):
  00:00.0 8086:29c0 class 0600.00 rev 00 hdr 00
  ...

=== M0 target 00:14.3 ===
  ID 8086:34F0 sub 8086:0234 rev 30 class 0280
  CMD 0006 STS 0010 IRQ 255 pin 1
  BAR0 64-bit non-pref addr 0x6013124000 size 16384
  CAP 01 PM   @c8 version 3 state 0
  CAP 05 MSI  @d0 en=0 ... 64=1 ...
  CAP 10 PCIe @40 ver=2 type=9 slot=0 link_speed=0 link_width=0
  CAP 11 MSI-X@80 en=0 mask=0 n=16 table(bir=0 off=0x2000) pba(bir=0 off=0x3000)

M0 target: 1 device(s) dumped.
FW: WIFI9.UC size 1519568 API 46 build 7e3e4b69

[ M0 DONE - SYSTEM HALTED ]
```

QEMU 没有 9560，target 会是默认 e1000e（`8086:10d3`，`00:02.0`），
并打印 `no 9560; dumped the first network controller (QEMU/test NIC)`；
真实 9560 的 `00:14.3` 只会在目标笔记本上出现。

看到 `SYSTEM HALTED` 就说明整条 UEFI → 64 位内核链路已经打通；
看到 `=== M0 target ... ===` 和 `FW: ... API 46` 说明 M0 侦察完成。

### M0 日志（UEFI NVRAM）

内核在停机前调用 UEFI Runtime Services 的 `SetVariable`，把完整 M0
日志写成非易失变量：

- 变量名：`MiniOSM0Log`
- GUID：`{4d304f53-0001-4d30-9a4b-4d304c4f4731}`
- 目标机实测 `SetVariable status=0x0`，日志 4138 字节。

这不是内核直接写 U 盘文件——内核还没有 USB/FAT 驱动。Windows 重启后
用 `GetFirmwareEnvironmentVariableW`（管理员 + `SeSystemEnvironmentPrivilege`）
读取，已经保存为 `C:\Users\hemin\Desktop\M0LOG.txt`，并复制到
U 盘 `EFI/MINIOS/M0LOG.TXT`。项目内副本：
[uefi/build/m0-target.log](build/m0-target.log)。

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

- 低 4 GiB 恒等映射（2 MiB 大页），覆盖内核 1 MiB、加载器、栈和 BootInfo；
- 帧缓冲在 4 GiB 以下时，已经包含在低 4 GiB 里；
- 帧缓冲在 4 GiB 以上时，只映射它所在的 **1 GiB 区域**，按 PML4/PDP
  索引建表，不会把 0 到高地址之间的空洞全部铺开；
- 加载器本身必须在 4 GiB 以下（当前 PE ImageBase 是 `0x10000000`）；
- 帧缓冲必须完整落在一个 1 GiB 区域内；
- 如果固件开启了 5 级页表（`CR4.LA57=1`），会额外建一层 PML5，
  帧缓冲需位于 PML5[0] 覆盖的范围内。

联想小新 Air 14 2020（Insyde 固件）把 GOP 帧缓冲放在
`0x4000000000`（256 GiB）；这条“低 4 GiB + 帧缓冲所在 1 GiB”规则就是
为这种 64 位 BAR 准备的，不会因为 256 GiB 超出映射上限而停机。

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

### 加载器报 `loader/framebuffer is above the ... mapping limit`

这是旧版本把 0 到帧缓冲末尾连续映射导致的。现在只映射低 4 GiB +
帧缓冲所在 1 GiB；如果仍然报错，检查：

- 帧缓冲是否跨了两个 1 GiB 区域（当前不支持）；
- 加载器是否被固件放到了 4 GiB 以上（当前不支持）；
- 是否确实是 5 级页表下 PML5[0] 之外的地址。

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
- M0 只做只读侦察：不编程 MSI/MSI-X/APIC、不做 DMA、不下载固件、
  不实现 802.11；PCI BAR sizing 会短暂写 all-ones 并立即恢复；
- 目标 9560 的 ID 是 `8086:34F0` / subsystem `8086:0234`；当前 Linux
  `drv.c` 把所有 `34F0` 归到 QuZ/AX201，不能直接照抄，必须按 subsystem
  匹配 9560/9000；
- 目标 BAR0 在 `0x6013124000`（>4 GiB），当前 loader 不映射；M1 必须
  增加显式 uncached 高地址 MMIO 映射；
- 目标 ACPI 是 ACPI 1.0（RSDP rev 0，只有 RSDT，没有 XSDT）；ACPI 解析
  必须支持 RSDT fallback；
- 没有中断/IDT、键盘、PS/2、USB HID、堆、文件系统、Wi-Fi 驱动；
- 内核固定加载到 1 MiB，被占用则停机；
- 加载器必须在 4 GiB 以下；帧缓冲必须完整落在一个 1 GiB 区域内；
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
