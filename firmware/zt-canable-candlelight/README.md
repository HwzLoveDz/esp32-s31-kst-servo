# ZT-CANable Pro V1.0 candleLight 固件

本目录针对用户提供原理图中的 **ZT-CANable Pro V1.0 / STM32F072C8T6 / ADM3053**，基于同一厂家资料包中的 candleLight 源码制作。已在该型号适配器上完成烧录、回读校验，并通过 Korvo USB Host 连接 KST 舵机完成通信与运动演示；详细结果见 [实物验证](../../docs/VALIDATION.md)。

目标文件：`build/zt-canable-pro-f072c8-candlelight.bin`。构建后的 SHA256、工具链版本、源文件哈希、镜像大小与内存检查在 `build/build-manifest.json`。

## 硬件匹配

| 项目 | 本固件配置 |
|---|---|
| MCU | STM32F072C8T6，Cortex-M0 |
| Flash / RAM | 64 KiB / 16 KiB |
| CAN | PB8 RX、PB9 TX、AF4 |
| USB | PA11 D−、PA12 D+，原生 Full Speed |
| 时钟 | 内部 HSI48；USB SOF 驱动 CRS 校准；不要求外部晶振 |
| LED | PA1 / PA0，低电平有效，开漏输出 |
| 隔离 CAN | ADM3053，RS 接地；固件不操作 PC13 收发器使能 |
| USB 身份 | VID:PID `1d50:606f`；产品字符串 `ZT-CANable Pro F072C8 candleLight` |
| DEVICE_CONFIG | software version 3、hardware version 1、1 个 CAN 通道 |

厂家源码的 `canable` 编译目标实际选择 STM32F042；本项目使用其 F072 `candleLight` 基础目标，并按以上原理图适配，不能根据文件名把 F042 固件刷入 F072。

## 相对于厂家源码的修改

1. 宣告原源码已实现但 BT_CONST 漏报的 ONE_SHOT bit 3；MODE flag 到 `CAN_MCR_NART` 的路径保留。ONE_SHOT 模式禁用 ABOM，bus-off 后保持停止，等待显式 MODE_START。
2. USB reset/deinit、suspend、MODE_RESET、重复 MODE_START、DFU detach 和 bus-off 都经过 StopSession：停止 CAN、请求 abort 三个 mailbox、清除两方向软件队列并归还 frame pool。USB resume 不自动重开 CAN。
3. STOP 状态拒收 CAN TX；mailbox 忙时丢弃请求并不给 echo，Host 应用其超时处理，不在适配器中无限延迟重发运动命令。
4. 主循环中的取帧、状态检查、发送和归还 frame pool 使用同一短 IRQ 临界区；IRQ 屏蔽汇编加 memory clobber，防止 RESET 与主循环交错后发送旧命令。
5. USB OUT 使用独立 32 字节持久缓冲，验证 20/24 字节经典帧后复制进 pool；USB IN 也使用独立持久副本，避免异步传输引用已归还的 pool 或局部栈。反复配置不消耗 pool 槽。
6. bus-off 恢复的零错误状态也更新错误去重状态；错误帧显式清 channel/flags/reserved。
7. BRP 保存真实除数，修复 1024 被 `& 0x3ff` 截成零的问题。
8. 修正链接内存布局：程序区 62 KiB，最后 2 KiB 擦除页 `0x0800F800` 专用于 USER_ID，避免 SET_USER_ID 擦除程序；RAM 16 KiB、初始 SP `0x20004000`。因此 `.bin` 包括到 USER_ID 位置的填充，不等于有效代码大小。
9. 去掉原构建工程的自动烧录 target，增加固定工具链、主机测试与静态产物检查。

`patches/vendor-diff.patch` 记录既有厂家文件的差异；新构建脚本和测试文件直接保存在本目录。原始资料包和原厂二进制保持原样。

## 构建和测试

已使用 Ubuntu 24.04 / WSL 中的 **GNU Arm Embedded GCC 13.2.1** 构建，`-Wall -Wextra -Werror` 通过。原生 Linux 工具链从 Ubuntu 官方仓库下载，仅解压到工作区工具缓存，未安装系统软件包。newlib 仅解包 Cortex-M0 对应的 v6-m/nofp 库。

在 Ubuntu 24.04 环境中运行；以下命令只准备/编译，不读写设备：

```sh
cd firmware/zt-canable-candlelight

# 尚未准备工具链时执行：
# sh prepare_toolchain.sh /path/to/arm-none-eabi-ubuntu-13.2

export GNU_ARM_ROOT=/path/to/arm-none-eabi-ubuntu-13.2/root
sh tests/run.sh
sh build.sh
```

可向 `build.sh` 传入单独构建目录。需要系统已有 `cmake`、`make`、`python3`；主机测试另需 `gcc` 与 ASAN/UBSAN。准备工具链脚本固定版本并核对四个 `.deb` 的 SHA256，不运行厂商的一键烧录脚本。

验证包括：

- `tests/session_test.c` 编译真实 USB class 和 ring queue 源码，外设函数由 fake 代替；**2573 个断言**，ASAN/UBSAN 通过。覆盖 100 次 session 周期、STOP 拒发、重复 START 清队列、64 槽耗尽/归还、重复 DeInit、异步 IN 数据寿命。
- `verify_artifact.py` 校验二进制不超过 64 KiB、栈顶对应 16 KiB、Thumb reset vector 落在程序区、USER_ID 单独擦除页、可用 heap 足够，并记录构建与源文件哈希。
- 这些测试未模拟真实 USB IRQ 延迟、CAN 外设、隔离器或电源。首次上板仍需验证 USB 枚举、控制请求、CAN 收发、断连/恢复和 bus-off。

Korvo 的 USB-A 供电不会因软件关闭 root port 必然断开；本固件依赖 USB suspend/reset 的会话清理，不依赖适配器掉电。USB 失联检测及 mailbox abort 有实际硬件时序，已经开始在 CAN 总线上发送的帧无法撤回。这不是硬件急停。

## 来源与哈希

源码包标识：

- `ZT-CANable资料/02-源码/candleLight/candleLight_fw.tar.bz2`（由适配器供应商提供，不随仓库分发）
- 原源码 archive SHA256：`4fa1fe37842590e72d1084d78febf612e36a464678fd1170ee7f9cbf2cb424a6`
- 原厂 `candleLight_fw.bin` SHA256：`33e9aa08294f590c31ef775162ec24ef7027eda233a469e6dee13d37c004f8b4`

本目录只复制构建需要的源码、头文件、链接脚本与许可证；未复制厂商 `.git`、旧 build、安装器或工具链。`LICENSE.md` 保留 candleLight、CMSIS 和 STM32 库的原许可说明，各源文件版权声明保留。

完整补充许可与固定上游来源见 [licenses/SOURCES.md](licenses/SOURCES.md)。预编译镜像位于 [release](../../release/README.md)，仅用于本节列出的 F072C8 板型。
