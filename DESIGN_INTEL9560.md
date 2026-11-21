# Intel Wireless-AC 9560（CNVio）驱动设计与落地路线

> 状态：架构方案 + **M0 侦察内核已实现**（QEMU OVMF 与目标机 U 盘均已完成；目标机日志已读出）；Wi-Fi 驱动尚未实现。
> 目标：在 Mini-OS 上为真实的 Intel Wireless-AC 9560 提供 Wi-Fi 客户端能力，并以项目内的 **CNVio 框架**承载设备发现、传输、固件和 Wi-Fi 子系统。
> 实现基线（2026-11-21 项目决策）：`uefi-boot` 渐进 + 选择性移植 `main` 中与硬件无关的协议/lib；不整体 merge `main`。功能分支为 `wifi-bringup`。

## 0. 目标机实测（M0，2026-11-21）

首台目标机已确认，下列数据来自目标机上的 Windows PnP/INF 和只读查询：

- 机器：Lenovo 81YJ / XiaoXinAir-14IIL 2020；CPU i5-1035G1（Ice Lake）；16 GB；
  BIOS `DSCN22WW(V1.08)`；UEFI，**Secure Boot 已关闭**。
- Wi-Fi：`00:14.3`，PCI `8086:34F0`，subsystem `8086:0234`，rev `0x30`；
  ACPI 路径 `\_SB.PCI0.CNVW`（CNVi 集成）。
- Windows 驱动：Intel 21.90.3.2，INF section
  `Install_MPCIEX_GENM2CRF_9560_AC_2x2_HMC_WINT_64_AC_No_160_10`；
  无线能力为 802.11a/b/g/n/ac（无 802.11ax），2x2、no 160 MHz。
- Linux 侧映射需要小心：Windows INF 与 Linux 5.7 都把 `34F0/0234`
  映射到 **9560 Qu B0 JF B0**（`iwl9560_qu_b0_jf_b0_cfg`）；当前 Linux
  `drv.c` 却把整个 `34F0` 归到 QuZ/22000。驱动必须按 subsystem
  `0234` 匹配 9560，并用固件 TLV 的 MAC/RF/step 复核，不能照抄
  `PCI_ANY_ID` 表。
- 固件：`iwlwifi-9000-pu-b0-jf-b0-46.ucode`（1,519,568 字节；
  TLV `FW_VERSION` API 46，build `0x7e3e4b69`）在开发机
  `/lib/firmware/intel/iwlwifi/` 可用；该文件不入库。
- M0 已在 QEMU OVMF 和目标机 U 盘上完成；目标机日志写入 UEFI NVRAM
  变量 `MiniOSM0Log`（GUID `{4d304f53-0001-4d30-9a4b-4d304c4f4731}`），
  已读出并保存为 `uefi/build/m0-target.log` 与 U 盘
  `EFI/MINIOS/M0LOG.TXT`。内核还没有 USB/FAT 驱动，所以这是 NVRAM
  日志，不是内核直接写 U 盘文件。
- **目标机 M0 实测（PCI bus 0，18 个设备）**：
  - 9560：`00:14.3`，`8086:34f0` sub `8086:0234` rev `0x30`，
    class `0280`；`CMD=0x0006`（memory + bus master 已使能）、
    `STS=0x0010`、`IRQ=255`、`pin=1`。
  - **BAR0：64-bit non-prefetchable，16,384 字节，地址
    `0x6013124000`（在 4 GiB 以上）**。当前 loader 只 identity-map
    低 4 GiB + framebuffer，M1 必须新增显式 uncached 高地址映射。
  - **MSI**：64-bit capable，当前 disabled；**MSI-X**：16 vectors，
    table BIR0 offset `0x2000`，PBA BIR0 offset `0x3000`，当前 disabled。
  - PCIe capability @`0x40`：version 2，type 9（root complex integrated
    endpoint），link speed/width 0；符合 CNVi integrated。
  - **ACPI RSDP：rev 0（ACPI 1.0），OEM LENOVO，RSDT `0x6bc28000`，
    没有 XSDT**；M1 的 ACPI 解析必须支持 RSDT fallback。
  - Memory map：44 descriptors，conventional 15,982 MiB；
    framebuffer `0x4000000000`（7 MiB）。
  - 固件 TLV：API 46，build `0x7e3e4b69`，1,519,568 字节。

**结论：设计文档原先只列 `9DF0/A370` 不完整；本项目的 9560 是
`34F0/0234`，位于 `00:14.3`，且必须按子系统 ID 匹配。M0 还确认了
BAR0 在 4 GiB 以上、MSI-X 16 vectors、ACPI 1.0 RSDT——这三个事实
直接决定 M1 的 MMIO/中断/ACPI 设计。**

**M1 优先项**：先实现高地址 `mmio_map`，把 BAR0 所在的 1 GiB 区域
（`0x6000000000`–`0x6040000000`）按 PCD 映射，读取
`CSR_HW_REV`/`CSR_GP_CNTRL` 证明 BAR 访问；再做 IDT/Local APIC/MSI-X
和 DMA。MSI-X table/PBA 都在 BAR0 内（offset `0x2000`/`0x3000`），
没有 BAR 映射就无法配置中断。

---

## 1. 结论先行

Intel 9560 不能按 e1000 的方式实现成“读取 BAR、配置以太网 TX/RX ring、收发 Ethernet 帧”的简单网卡驱动。它是 **CNVio（CNVio 一代）平台设备**：Intel 产品规格标明接口为 M.2 CNVio，Wi-Fi 数字 MAC/控制功能依赖兼容的平台，M.2 模块不是可脱离主机平台工作的普通 PCIe Wi-Fi 卡。具体机器还必须有兼容的 CNVio 主机实现、固件/BIOS 配置和天线/射频模块。

建议把“CNVio 框架”作为 Mini-OS 内新增的分层框架来设计，而不是假定仓库中已经存在此框架：

```text
PCI/平台发现
    ↓
cnvio_core（设备生命周期、资源、日志、状态）
    ↓
cnvio_transport（PCI/MMIO、中断、DMA、设备通信）
    ↓
iwl9000（9560 对应的 Intel 9000 系列协议、固件启动、命令/通知、队列）
    ↓
wifi_core（扫描、认证、关联、密钥/加密、802.11 数据面）
    ↓
netdev/802.3 适配层
    ↓
现有 ARP/IP/ICMP/TCP 栈
```

完整 Wi-Fi 客户端不是一个单独的 PCI 驱动任务，而是从设备发现、DMA/中断、固件启动一直到 802.11 管理帧和数据帧的纵向工程。应先把“真实硬件可发现、固件能启动、关联到 AP、收发 IP 包”设为分阶段验收点。

## 2. 硬件身份与边界

### 2.1 已查证的产品信息

Intel 产品规格给出的主要信息：

- 产品：Intel Wireless-AC 9560，Wi-Fi 5 / 802.11ac。
- 双频，2x2 收发流，标称最高 1.73 Gbps，支持 160 MHz 信道。
- 模块接口规格为 **M.2: CNVio**。
- 产品页列出 Bluetooth 5.1；本项目第一阶段只做 Wi-Fi，Bluetooth 不在范围内。

Linux 上游 `iwlwifi` 的 PCI 设备表将 `8086:9DF0` 和 `8086:A370` 映射到 9560 的 MAC 配置（`iwl9560_mac_cfg`）。目标机实测又发现 `8086:34F0` / subsystem `8086:0234`（Ice Lake CNVi，Qu B0 JF B0）：Windows INF 与 Linux 5.7 都把它归为 9560 AC 2x2 no-160。当前 Linux `drv.c` 把所有 `34F0` 归到 QuZ/22000，不能据此匹配；必须按 subsystem `0234` 加固件 TLV 复核。实际机器一律以 PCI 枚举结果为准。

### 2.2 CNVio 对实现的含义

- **适配器身份不等于独立 PCIe WLAN 芯片。** 9560 的功能依赖主板/处理器平台内的 CNVio 主机侧功能；同样外形的 M.2 插槽不保证兼容。
- **设备协议不是 e1000 寄存器协议。** 即便平台通过 PCI 暴露出可探测的设备功能，也不能据此推导出 e1000 式的通用 DMA ring 和寄存器初始化序列。
- **固件是启动链路的一部分。** Linux `iwlwifi` 的 9000 系列配置定义 `iwlwifi-9000-pu-b0-jf-b0-<API>.ucode` 命名族。应根据所实现的固件 API/上游配置选择兼容版本，不要硬编码未经验证的单一 API 号。
- **需要区分 9560 与其他 Intel 无线系列。** 仅在本阶段支持 9560 对应的 9000 配置；不要将 9260、AX201/AX211 或其他 CNVio/CNVio2 设备当作兼容设备。
- **硬件验证需真机。** 普通 QEMU `-device e1000` 只能继续验证现有 Ethernet 驱动，不能验证 9560/CNVio 固件、RF、扫描或关联。若宿主机与虚拟机具备合适的设备直通条件，也不能把直通成功视为完整 CNVio 支持证明。

### 2.3 首轮硬件勘察清单

开始写寄存器代码前，在 Linux 或其他成熟系统上记录目标机器：

1. CPU/芯片组/主板型号、BIOS 版本，以及 9560 模块具体料号。
2. `lspci -nn` 中无线设备的完整 vendor/device、subsystem ID、class、BDF。
3. PCI 配置空间、BAR 类型/大小、Command（Memory Space / Bus Master）、中断模式及 capability 列表。
4. Linux `dmesg` 中 `iwlwifi` 匹配的设备配置、所请求固件文件名、固件 API、启动/校准错误。
5. 确认设备未被 BIOS 禁用、未被其他驱动/固件状态占有；记录冷启动与重启后的差异。

这些数据决定 Mini-OS 的 PCI 匹配、资源映射与中断设计；在得到这些信息前，不应猜测 9560 的 MMIO 寄存器布局。

## 3. Mini-OS 现状与实现影响

| 子系统 | 仓库现状 | 对 9560/CNVio 的影响 |
|---|---|---|
| PCI | `arch/pci.c` 使用 CF8/CFC；只扫 bus 0、每个 device 的 function 0；只取 BAR0/BAR1/IRQ。 | 需要扩展 function 发现、桥/多 bus 扫描、BAR 类型与大小解析、64 位 BAR、PCI capability 和资源冲突检查。 |
| MMIO | e1000 直接把 BAR 地址转指针；启动映射 identity map 到 4 GiB。 | 目前只适合当前映射假设；需明确 MMIO uncached 映射属性、BAR 范围边界和地址宽度，不能把某一 BAR 当固定常量。 |
| DMA | e1000 使用 `.netbuf` 中的静态缓冲区，虚拟地址即物理地址；无通用 DMA API/IOMMU 支持。 | 需要 DMA 分配/映射 API，约束对齐、连续性、物理地址宽度与设备 DMA mask，并规定内存屏障及回收生命周期。 |
| 中断 | 8259 PIC、IRQ handler 表；无 MSI/MSI-X、无 IOAPIC 路由框架。 | 必须先查真机使用的中断能力/路由。至少建立可测的 INTx 路径；若设备依赖 MSI，则需先实现 APIC/MSI 基础设施。 |
| 固件/文件系统 | 平面文件系统，inode 名称最多 15 字符；`fs_write` 最多 8 个直接块（4096 bytes），`fs_read` 整文件读入调用者缓冲区。 | 无法直接保存或加载典型 `.ucode` 文件：文件名太长、文件容量太小、也没有流式读接口。需设计只读固件来源/更大文件支持。 |
| 网络驱动 | `e1000.c` 是唯一 NIC 驱动，接口为 `e1000_send/recv`；`net/ip.c`、ARP 直接调用 e1000。 | 先抽象 `netdev`，Wi-Fi 成功后再接 IP；不能让 IP 层继续绑定到 e1000。 |
| 网络协议 | 已有 ARP、IPv4、ICMP、TCP；IP 地址固定为 `10.0.2.16`，网关为 QEMU slirp 地址。 | 当前测试配置是 QEMU Ethernet 环境，不代表真实 AP 网络。DHCP、DNS、MTU/分片策略需另行规划。 |
| 并发/调度 | 协作式调度和定时器；驱动 API 尚无统一锁/原子队列规范。 | IRQ handler 必须尽量短；固件命令等待不能在关中断状态下忙等；需定义轮询、超时和并发所有权。 |
| 构建/启动 | `build_64_boot.sh` 手工列举源文件；`kernel_main` 顺序初始化 e1000 后初始化 IP。 | 新框架需要显式加入构建；初始化顺序应为 PCI/平台 → CNVio transport → 固件 → Wi-Fi → netdev → IP。 |

参考项目文件：[pci.c](./long-mode/arch/pci.c)、[pci.h](./long-mode/arch/pci.h)、[e1000.c](./long-mode/drivers/e1000.c)、[irq.c](./long-mode/arch/irq.c)、[paging.c](./long-mode/kernel/paging.c)、[fs.c](./long-mode/kernel/fs.c)、[fs.h](./long-mode/kernel/fs.h)、[ip.c](./long-mode/net/ip.c)、[kernel.c](./long-mode/kernel/kernel.c)、[build_64_boot.sh](./long-mode/build_64_boot.sh)。

## 3.1 分支基线建议

建议将新驱动分支从 **`23ea4cb2027afc1ba88996968a5040868f31338b`** 开出，而不是直接在当前 QEMU/e1000 网络实现上叠加，前提是该分支的目标是“为真实 CNVio 硬件重新建立合适的设备/网络架构”。

核对该提交时的内容：

- 提交说明为 `small .c echo shell`，时间为 2026-04-21。
- `long-mode/kernel.c` 是轮询键盘并回显输入的单体程序；对应构建脚本只汇编 boot 阶段并链接该时代的内核。
- 当时尚无 `arch/pci.*`、e1000、文件系统或 IP/ARP 网络栈。
- 当前 `main` 比它多 28 个提交，后续引入了进程/系统调用、PCI/e1000、文件系统和 QEMU slirp 环境下的网络测试等。

因此，这个基线的优点是**不会默认继承当前 e1000/QEMU 网络路径的接口和假设**；缺点是它不是已有驱动框架的“精简版”，而是较早的裸机阶段。选它就需要按需重新实现 PCI、DMA、固件存储/加载、中断和 netdev 等基础能力。建议只从后续历史中择取经过审查且与设备无关的代码/经验，不要将 QEMU e1000 的 BAR、DMA 区、IRQ 或 Ethernet 收发策略直接移植到 CNVio 驱动。

如果项目目标改为尽快复用现有 IP/TCP 功能，可考虑从较新的稳定点开分支，但应先抽离 netdev 并验证 PCI/DMA/IRQ/内存假设，再接无线驱动。无论选哪个基线，都应在分支说明中记录该提交哈希，保持 main 与新硬件开发隔离。

> **项目决策（2026-11-21）**：实现基线改为 `uefi-boot` 渐进 + 选择性移植 `main` 中与硬件无关的协议/lib，不整体 merge `main`。原因是 `main` 深度绑定 BIOS/VGA/PIC/PIT/ATA PIO/e1000 等 QEMU 假设，而目标是 UEFI-only 的 Ice Lake 笔记本；本节 `23ea4cb` 的建议保留为历史备选。

### 3.2 当前最关键的前置工作

1. **通用 PCI 层**：扫描完整 PCI 拓扑；解析 BAR（I/O/MMIO、32/64 位）；安全读取 PCI capability；保存 subsystem ID、BDF、IRQ 信息。
2. **MMIO 与 DMA 层**：提供具有明确语义的 `ioread32/iowrite32` 和 `dma_alloc/dma_free`（或静态池的等价 API）；不再把“identity mapped”默认为 DMA 一定正确。
3. **中断层**：明确真实硬件的 INTx/MSI 路径；上游 iwlwifi 只申请 MSI-X/MSI、不走传统 INTx，因此阶段 1 必须实现 Local APIC + MSI/MSI-X（MSI-X table/vector 分配、屏蔽、EOI、重入），不能把 INTx 当默认验证路径。
4. **固件供应/加载**：增加容量足够、支持长名称和分块读取的固件路径；固件 blob 由使用者从可再分发的官方/发行版固件包提供，代码仓库不提交未经确认授权的二进制固件。
5. **网络接口**：把 Ethernet TX/RX 从 e1000 特有 API 提升到 `netdev`；Wi-Fi 只有在完成 802.11→802.3 数据面转换后才交给现有 ARP/IP。

## 4. CNVio 框架建议边界

CNVio 框架负责承载 Mini-OS 的设备生命周期与平台传输，不应把 9560 的固件命令、802.11 关联状态机塞进通用框架。

### 4.1 模块与目录建议

```text
long-mode/
├── arch/
│   ├── pci.c / pci.h                 # 扩展通用 PCI 枚举和配置空间能力
│   ├── apic.c / apic.h               # 若真机确认需要 MSI，再作为单独前置模块
│   └── dma.c / dma.h                 # DMA 地址/缓冲分配与同步契约
├── drivers/
│   ├── cnvio/
│   │   ├── core.c / core.h           # 匹配、资源、状态、探测/移除
│   │   ├── transport.c / transport.h # PCI/MMIO/中断/DMA 后端抽象
│   │   ├── firmware.c / firmware.h   # 固件定位、校验、下载与启动超时
│   │   └── iwl9000.c / iwl9000.h     # 9560/9000 代设备与固件接口
│   └── wifi/
│       ├── wifi.c / wifi.h           # Wi-Fi 设备及连接生命周期
│       ├── scan.c                    # 扫描/结果管理
│       ├── station.c                 # 认证与关联状态机
│       └── netdev.c / netdev.h       # Ethernet-like 上层接口
└── net/
    └── ...                           # 后续让 IP 栈消费通用 netdev，而非 e1000 API
```

初期可以少建文件，但接口边界应保持。`cnvio_transport` 先只提供确认过的 9560 必需操作；不要预先承诺支持所有 CNVio/CNVio2 产品。

### 4.2 核心对象与 API 草案

```c
struct cnvio_device {
    uint8_t bus, dev, func;
    uint16_t vendor_id, device_id;
    uint16_t subvendor_id, subdevice_id;
    uint64_t mmio_base;
    uint32_t mmio_size;
    uint8_t irq;
    uint32_t state;
};

struct cnvio_transport_ops {
    int (*read32)(struct cnvio_device *, uint32_t reg, uint32_t *value);
    int (*write32)(struct cnvio_device *, uint32_t reg, uint32_t value);
    int (*dma_alloc)(struct cnvio_device *, size_t size, size_t align,
                     void **cpu_addr, uint64_t *dma_addr);
    void (*dma_free)(struct cnvio_device *, void *cpu_addr,
                     uint64_t dma_addr, size_t size);
    int (*irq_enable)(struct cnvio_device *);
    void (*irq_disable)(struct cnvio_device *);
};

int cnvio_probe_all(void);
int cnvio_firmware_load(struct cnvio_device *, const char *name);
int iwl9000_start(struct cnvio_device *);
int wifi_scan_start(void);
int wifi_connect(const char *ssid, const void *credentials, size_t len);
int netdev_transmit(struct netdev *, const void *frame, size_t len);
```

此为接口草案，不代表现有 C 代码已经具备这些类型。真实寄存器访问必须以公开可用的硬件文档、Linux 上游实现与真机观测交叉确认；未证实的寄存器/位定义不得复制到代码中。

### 4.3 驱动状态机

```text
UNSEEN
  → PCI_MATCHED
  → RESOURCES_READY
  → TRANSPORT_READY
  → FW_LOADING
  → FW_ALIVE
  → HW_CONFIGURED
  → WIFI_IDLE
  → SCANNING
  → AUTHENTICATING
  → ASSOCIATED
  → NETDEV_UP
```

任一阶段失败都应保留明确错误码/日志并可停止后续初始化；超时不得伪装成成功。设备暂不可用时，内核其他功能仍应能够启动。

### 4.4 固件下载与命令通信：C 骨架示例

先区分三件事：**CNVio 是平台/设备集成方式，不是固件命令格式**；9560 使用 Intel 9000 系列的固件/设备协议；主机要先通过该设备对应的 CSR/DMA/中断机制把固件启动起来，之后才通过 command queue 与固件交换命令和通知。因此不能把下面的代码误认为可直接写 BAR 的完整 9560 驱动。

下面是可作为 Mini-OS 起点的**分层骨架**。协议解析、设备寄存器、DMA ring 布局和通知格式全部藏在 transport/parser 回调中；这些部分必须从匹配的 9000 系列固件 API 与上游实现逐项落实。示例有意不编造寄存器 offset、command ID 或 firmware wire format。

检查了用户提供的 `/mnt/c/Users/hemin/intel-9560-firmware` 目录（只查看文件名和大小，未读取或复制固件内容）：其中现有文件名是 `iwlwifi-9260-th-b0-jf-b0-{34,38,41,43,46}.ucode`，属于 **9260 命名族**，并不是 9560 的 `iwlwifi-9000-pu-b0-jf-b0-<API>.ucode` 命名族。不要因为两者同属 Intel 9000 系列或 API 数字相同，就假定固件可互换。应在支持该 9560 的 Linux 真机上查看 `dmesg` 的 `iwlwifi` 固件请求，取得它实际请求的 9000 固件文件，再用相同设备/固件组合验证。

```c
#include <stddef.h>
#include <stdint.h>

#define IWL_FW_CHUNK_SIZE 4096
#define IWL_TIMEOUT_FW_MS 5000
#define IWL_TIMEOUT_CMD_MS 1000

enum iwl_state {
    IWL_OFF,
    IWL_FW_LOADING,
    IWL_FW_STARTING,
    IWL_FW_ALIVE,
    IWL_FAILED,
};

struct iwl_fw_segment {
    const uint8_t *bytes;   /* 指向已验证固件映像中的只读数据 */
    size_t length;
    uint32_t device_addr;   /* 目标地址；语义由硬件/固件代际决定 */
    uint8_t target;         /* 例如指令/数据存储区，由真实解析器定义 */
};

struct iwl_fw_image {
    uint32_t api;
    const struct iwl_fw_segment *segments;
    size_t segment_count;
};

struct iwl_cmd_reply {
    uint8_t command;
    uint8_t sequence;
    const uint8_t *payload;
    size_t payload_len;
};

struct iwl_transport_ops {
    int (*boot_prepare)(void *ctx);

    /* 负责设备要求的传输方式：DMA、窗口寄存器或其他经验证的机制。 */
    int (*fw_write_chunk)(void *ctx, uint8_t target, uint32_t device_addr,
                          const void *data, size_t length);
    int (*fw_start)(void *ctx);
    int (*wait_alive)(void *ctx, uint32_t timeout_ms);

    /* reserve/submit/wait 必须管理 ring slot、序号及通知队列。 */
    int (*cmd_reserve)(void *ctx, uint8_t *sequence);
    int (*cmd_submit)(void *ctx, uint8_t sequence, uint8_t command,
                      const void *payload, size_t payload_len);
    int (*cmd_wait_reply)(void *ctx, uint8_t sequence, uint32_t timeout_ms,
                          struct iwl_cmd_reply *reply);
    void (*cmd_release)(void *ctx, uint8_t sequence);
};

struct iwl_device {
    void *transport_ctx;
    const struct iwl_transport_ops *ops;
    enum iwl_state state;
};

/*
 * iwl_fw_parse() 必须解析真实的 Intel TLV firmware 格式，
 * 验证文件边界、目标设备/代际和支持的 API，再生成 segment 视图。
 * 这里仅声明接口，不假设固件文件是一个可直接 memcpy 到设备的 blob。
 */
int iwl_fw_parse(const void *file, size_t file_len, struct iwl_fw_image *out);
void iwl_fw_release(struct iwl_fw_image *image);

static int iwl_load_firmware(struct iwl_device *dev,
                             const struct iwl_fw_image *fw)
{
    const struct iwl_transport_ops *ops;

    if (!dev || !fw || !fw->segments || fw->segment_count == 0)
        return -1;

    ops = dev->ops;
    if (!ops || !ops->boot_prepare || !ops->fw_write_chunk ||
        !ops->fw_start || !ops->wait_alive)
        return -1;

    dev->state = IWL_FW_LOADING;

    int ret = ops->boot_prepare(dev->transport_ctx);
    if (ret)
        goto failed;

    for (size_t s = 0; s < fw->segment_count; ++s) {
        const struct iwl_fw_segment *seg = &fw->segments[s];
        size_t offset = 0;

        if (!seg->bytes || seg->length == 0 ||
            (uint64_t)seg->device_addr + seg->length > (uint64_t)UINT32_MAX + 1) {
            ret = -2;
            goto failed;
        }

        while (offset < seg->length) {
            size_t n = seg->length - offset;
            if (n > IWL_FW_CHUNK_SIZE)
                n = IWL_FW_CHUNK_SIZE;

            ret = ops->fw_write_chunk(dev->transport_ctx, seg->target,
                                      seg->device_addr + (uint32_t)offset,
                                      seg->bytes + offset, n);
            if (ret)
                goto failed;

            offset += n;
        }
    }

    dev->state = IWL_FW_STARTING;
    ret = ops->fw_start(dev->transport_ctx);
    if (ret)
        goto failed;

    /* wait_alive 等待并验证设备发来的 ALIVE 通知；不能只看某寄存器非零。 */
    ret = ops->wait_alive(dev->transport_ctx, IWL_TIMEOUT_FW_MS);
    if (ret)
        goto failed;

    dev->state = IWL_FW_ALIVE;
    return 0;

failed:
    dev->state = IWL_FAILED;
    return ret;
}

static int iwl_send_cmd_sync(struct iwl_device *dev, uint8_t command,
                             const void *payload, size_t payload_len,
                             struct iwl_cmd_reply *reply)
{
    const struct iwl_transport_ops *ops;
    uint8_t seq;
    int ret;

    if (!dev || dev->state != IWL_FW_ALIVE ||
        (!payload && payload_len) || !reply)
        return -1;

    ops = dev->ops;
    if (!ops || !ops->cmd_reserve || !ops->cmd_submit ||
        !ops->cmd_wait_reply || !ops->cmd_release)
        return -1;

    ret = ops->cmd_reserve(dev->transport_ctx, &seq);
    if (ret)
        return ret;

    /*
     * cmd_submit 应先复制/映射 payload、填 TX descriptor，做 DMA 可见性
     * 屏障，再敲设备 doorbell。成功后 payload 必须保持有效直到设备消费。
     */
    ret = ops->cmd_submit(dev->transport_ctx, seq, command,
                          payload, payload_len);
    if (!ret) {
        /*
         * 等待时不能持有会阻止 IRQ/通知处理的锁，也不能关中断。
         * RX/IRQ 路径只把通知放入队列；这里按 seq 匹配 command response。
         */
        ret = ops->cmd_wait_reply(dev->transport_ctx, seq,
                                  IWL_TIMEOUT_CMD_MS, reply);
    }

    /*
     * 实际 transport 必须保证完成后才能回收 slot/DMA buffer。
     * 超时意味着设备状态未知，通常应触发 transport/fw recovery，
     * 不能盲目复用仍可能被设备访问的描述符或缓冲区。
     */
    ops->cmd_release(dev->transport_ctx, seq);
    return ret;
}
```

使用顺序示意：

```c
struct iwl_fw_image fw;

/* firmware_request 由 Mini-OS 固件存储层提供，需支持大文件和分块读取。 */
ret = firmware_request("iwlwifi-9000-pu-b0-jf-b0-<匹配的API>.ucode",
                       &file, &file_len);
if (ret)
    return report_error("firmware missing", ret);

ret = iwl_fw_parse(file, file_len, &fw);
if (ret)
    return report_error("unsupported/corrupt firmware", ret);

ret = iwl_load_firmware(dev, &fw);
iwl_fw_release(&fw);
firmware_release(file);
if (ret)
    return report_error("firmware failed to start", ret);

/* 到达 ALIVE 后才可以发送该固件 API 中定义的配置命令。 */
ret = iwl_send_cmd_sync(dev, CMD_ID_FROM_SUPPORTED_API,
                        cmd_data, cmd_data_len, &reply);
```

**落地时需替换/补齐的点：**

1. `4096` 只是主机侧分块示例；实际 chunk 上限、对齐、目标内存区及下载握手须按 9000 代传输实现决定。
2. `device_addr + length` 的检查应由真实 segment parser 和目标内存映射共同约束；不同 segment target 不能简单混为一个连续 SRAM。
3. 真正的 `iwl_fw_parse` 要支持所选 9000 系列 API 对应的 TLV 格式，并检查设备 family/API/长度；不能把固件头强转成自定义结构后就直接 DMA。
4. `wait_alive` 与 `cmd_wait_reply` 应消费中断接收队列中的正式通知，检查通知 ID、sequence、长度及状态码，并设置有限超时。
5. 不同固件命令的 response 语义不完全相同：有些是异步通知或无回复命令。最终 API 应根据命令元数据区分同步响应、异步事件和无响应命令。
6. DMA 写入与 ring 更新需要设备可见性屏障；超时/复位路径须停止 bus master、同步 IRQ，再回收 DMA 缓冲，避免设备继续访问已释放内存。

阅读 Linux 上游时，建议按职责顺序看 `iwlwifi/pcie/trans.c`（设备基础启动/传输准备）、`iwlwifi/pcie/trans-gen2.c`（固件 alive 等生命周期路径）、`iwlwifi/iwl-trans.h`（host command/transport 抽象）及 9000 配置文件；不要只复制单个寄存器宏。上游代码采用 GPL-2.0 OR BSD-3-Clause 双许可，若考虑移植实现，需逐文件遵守许可并保留版权/许可声明。

## 5. 实施分期、依赖与验收

| 阶段 | 交付内容 | 验收条件 |
|---|---|---|
| 0. 硬件确认 | 在目标真机采集 PCI ID/BDF、BAR、IRQ/capability、芯片组/BIOS、Linux 固件日志；确认是支持 9560 的 CNVio 平台。 | 能明确设备身份、固件 API/文件请求和中断模式；不具备兼容平台则暂停真机驱动实现。 |
| 1. 平台基础 | PCI 完整枚举、BAR 解析与映射、DMA API、必要的中断基础、超时/日志约定。 | 不触碰无线固件即可稳定枚举设备并证明资源范围/中断/DMA 地址契约。 |
| 2. CNVio core | 建立设备匹配、生命周期和 transport ops；仅匹配已核实的 ID/平台条件。 | 支持冷启动、重复初始化防护、资源失败回滚；未知设备不被误绑定。 |
| 3. 固件链路 | 固件介质/加载器扩容，按块读取、长度/版本检查、设备握手与启动超时。 | 真机上能报告请求的固件版本并进入 `FW_ALIVE`；缺失/不兼容固件给明确错误。 |
| 4. 基础 Wi-Fi 接口 | 初始化设备配置、命令队列/通知、接收事件与扫描结果输出。 | 至少完成扫描并列出 SSID/BSSID/频段/信号强度；错误与超时可诊断。 |
| 5. 客户端安全/关联 | Open 网络及 WPA2-Personal（根据固件协议与可用加密组件决定范围）；认证、关联、密钥安装和断连状态机。 | 能稳定连接目标 AP，重连/断开行为可测；不得把明文凭据写日志。 |
| 6. 数据面与 IP | Wi-Fi 802.11 数据帧经驱动解封装/转换为 Ethernet-like `netdev`；接入 ARP/IP，再增加 DHCP。 | 可 DHCP 获取配置并完成网关 ARP、ICMP ping、TCP 收发；有 MTU/丢包测试。 |
| 7. 稳定性 | 错误恢复、复位、队列满、丢中断、固件崩溃及设备不存在测试；性能/功耗后续评估。 | 反复连接/断开与冷启动无死锁、越界或内存破坏；失败可恢复或安全停机。 |

建议每一阶段独立提交并在功能分支上开发。阶段 1 的 PCI/DMA/APIC 基础可能比 9560 专用驱动本身更早成为主要工作量。

## 6. Wi-Fi 与现有网络栈的整合原则

### 6.1 不要把 802.11 当成 Ethernet 帧

无线数据链路还需要处理 802.11 帧头、QoS/地址字段、加密/解密、重放保护、AP 关联状态与固件队列。现有 `ip_poll()` 只按 Ethernet EtherType 分发 ARP/IPv4，因此建议：

1. `iwl9000` 负责设备/固件命令、DMA 队列和固件通知。
2. `wifi_core` 负责站点状态、扫描、认证、关联、密钥、802.11 数据路径。
3. `netdev` 向 IP 层提供统一 Ethernet-like 接口，由 Wi-Fi 侧完成必要的 802.11 数据面转换。
4. `e1000` 保留为另一种 `netdev` 后端，避免为 Wi-Fi 改写其硬件驱动。
5. 将 `arp.c`、`ip.c` 中直接调用 `e1000_get_mac/e1000_send/e1000_recv` 的代码收敛到 netdev；MAC 地址、链路状态和 MTU 从接口对象取得。

### 6.2 全功能客户端依赖

完成“扫描到 AP”不等于可上网。至少需要明确以下范围：

- 开放网络与 WPA2/WPA3 支持策略；密钥管理和加密功能可能主要由固件执行，但宿主仍需正确协商和安装密钥。
- 802.11 管理帧与关联状态、区域/信道限制、认证失败/超时与断线重连。
- DHCP、DNS、MTU 与 IP 配置。目前 IP 是静态 QEMU 测试地址，需避免硬编码沿用。
- 802.11 硬件射频/地区设置和平台 ACPI/BIOS 电源状态；首个版本可只针对一台已知机器。

## 7. 固件与存储设计

仓库当前平面文件系统不能存放标准 9000 系列固件文件：inode 名称长度限制 16 字节（含结尾零），直接块数限制 8 个、容量上限 4 KiB。固件名称长于该限制且通常远大于文件系统可保存的单文件容量。

优先选择一种有明确来源和边界的方案：

1. **推荐：启动介质上的只读固件区/包**，支持长文件名、足够容量和分块读取；在 `firmware.c` 校验长度/签名或格式，再按设备协议加载。
2. 早期 bring-up 可以由构建脚本打包到单独固件镜像，但要能替换、版本可追踪，且不得默认将固件 blob 提交到仓库。
3. 不建议把固件 C 数组静态编入内核：固件升级、许可和内核镜像体积都难以管理。

固件的确切 API 版本必须从实测机器的 Linux 日志/配置和上游兼容范围确认。若供给的文件不兼容、缺失或校验失败，驱动应在 `FW_LOADING` 阶段以明确诊断退出。

## 8. 测试矩阵与调试输出

| 测试环境 | 可验证内容 | 不可据此宣称的能力 |
|---|---|---|
| 普通 QEMU + `-device e1000` | 现有 e1000、ARP/IP、`netdev` 抽象、无设备时内核启动。 | 9560/CNVio 探测、固件、射频、扫描、关联。 |
| 真机兼容 CNVio 平台 | PCI 身份、资源、DMA/IRQ、固件启动、Wi-Fi 流程。 | 其他芯片组/BIOS/9560 子系统组合的普遍兼容性。 |
| 编译期/纯逻辑测试 | PCI ID 匹配、状态机、包长度校验、错误码、固件 header 校验。 | 硬件时序/中断可靠性及空中传输能力。 |

串口日志至少提供：BDF 与 PCI ID、BAR 类型/范围、IRQ 模式、DMA 地址宽度、固件文件名/API/校验结果、各状态转换、命令超时/固件错误码、扫描/关联结果。不要输出密码、密钥或敏感认证材料。

## 9. 主要风险和决策点

| 风险 | 后果 | 处理 |
|---|---|---|
| 目标主板不支持 CNVio 或 BIOS 禁用了设备 | 设备不可用，软件实现无法弥补平台缺失。 | 阶段 0 先确认硬件；不以外观相同的 M.2 插槽推断兼容。 |
| 误把 9560 当普通 PCIe WLAN 芯片 | 错误套用寄存器、BAR 和 DMA 假设，可能挂死或破坏内存。 | 以 9560/9000 上游实现和真机观测为基础；禁止照搬 e1000 初始化。 |
| 缺少 APIC/MSI 或 IRQ 路由 | 固件通知/收包无法可靠运行。 | 先勘察设备 capability 和平台路由；按需实现 Local APIC/MSI。 |
| DMA 区域/地址宽度不兼容 | 设备访问错误物理地址或静默丢包。 | 做显式 DMA mask、地址验证、屏障和生命周期管理。 |
| 固件不可供给或 API 不匹配 | 设备无法进入可用状态。 | 固件介质、版本、校验和授权策略作为单独交付。 |
| Wi-Fi 管理面与安全协议缺失 | 即使固件启动也无法连接 AP 或加密通信。 | 将扫描、认证、关联、密钥和数据面列为正式阶段，不能只验“扫描到 SSID”。 |
| 当前网络栈与驱动耦合 | 替换 NIC 时影响现有 e1000 和测试。 | 先建立 netdev 接口，逐步迁移调用点。 |
| QEMU 缺乏 9560 模型 | 开发者可能错误地把 e1000 测试当作 Wi-Fi 验收。 | QEMU 只测框架/回归；必须有明确的真机测试计划。 |

实施前需做出的项目决策：

1. **已确认**：Lenovo 81YJ（XiaoXinAir-14IIL 2020，i5-1035G1，16 GB），
   9560 位于 `00:14.3`，PCI `8086:34F0/0234`；Secure Boot 已关闭。
2. **已确认**：固件从开发机 `/lib/firmware` 的 linux-firmware 解压，
   构建时放入 ESP 的 `EFI/MINIOS/WIFI9.UC`；仓库不提交二进制。
3. 第一版安全范围（开放网络 / WPA2-Personal）仍待定。
4. **已确认**：必须先投入 Local APIC + MSI/MSI-X + PCI/DMA 基础设施；
   上游 iwlwifi 不支持传统 INTx，不能以 INTx 做最小验证。

## 10. 参考资料

以下资料用于硬件身份和上游软件行为参考；上游驱动是复杂协议的参考实现，不应直接复制其代码或忽略许可/依赖。

- Intel 产品规格：[Wireless-AC 9560 Specifications](https://www.intel.com/content/www/us/en/products/sku/99446/intel-wirelessac-9560/specifications.html)（接口、Wi-Fi 代际、无线规格）。
- Linux 上游设备匹配表：[iwlwifi/pcie/drv.c](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/intel/iwlwifi/pcie/drv.c)（9560 对应 `0x9DF0`、`0xA370` 与 `iwl9560_mac_cfg`）。
- Linux 上游 9000 系列配置：[iwlwifi/cfg/9000.c](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/intel/iwlwifi/cfg/9000.c)（9000 固件命名/API 配置）。
- Linux Wireless 文档：[iwlwifi driver](https://wireless.docs.kernel.org/en/latest/en/users/drivers/iwlwifi.html)（支持设备与驱动概览）。
- Linux 固件仓库：[linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/)（具体固件文件及其许可/版本应在实施前核实）。

---

本设计的完成标准不是“PCI 扫描到一个 Intel ID”，而是：在确认兼容的真实 CNVio 主机上，Mini-OS 可安全初始化设备、加载兼容固件、扫描并连接 AP，并通过通用网络接口完成 IP 收发；所有失败阶段均有显式诊断且不会破坏其他设备/内核启动。
