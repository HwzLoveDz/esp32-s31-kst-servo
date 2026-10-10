# 预编译镜像 / Prebuilt images

这些是本项目使用的两个独立设备镜像。烧录前核对目标芯片、板型和已有分区；
不要把控制板应用镜像刷到 USB-CAN 适配器。

| 文件 | 目标 | 大小 | 状态 |
| --- | --- | ---: | --- |
| [kst-servo-fui-s31-axis-view.bin](kst-servo-fui-s31-axis-view.bin) | ESP32-S31-Korvo-1，16 MB Flash | 1,260,944 bytes | 演示实物所用应用镜像，保留 ±100° 目标范围 |
| [zt-canable-pro-f072c8-candlelight.bin](zt-canable-pro-f072c8-candlelight.bin) | ZT-CANable Pro V1.0，STM32F072C8T6 / ADM3053 | 63,492 bytes | 板型专用 candleLight，已完成实物烧录与回读校验 |

Korvo 文件仅含应用，不含 bootloader、分区表或数据分区；本工程应用起点
`0x10000` 只有在已有分区匹配时才适用。首次初始化和应用更新命令见
[HARDWARE](../docs/HARDWARE.md)。源码重新构建会包含本次构建的时间/版本信息，
因此不要求重新生成的应用文件与历史演示镜像逐字节相同。

适配器镜像链接地址为 `0x08000000`，仅适用于表中 F072C8 板型。
进入 STM32 DFU 后须核对具体设备，再按对应工具说明写入；退出 BOOT0 后连接
Korvo USB-A。它不适用于 F042 版 CANable、CANable 2.0 或任意其他 STM32。
配套 [完整源码与构建步骤](../firmware/zt-canable-candlelight/README.md) 及
[第三方许可](../firmware/zt-canable-candlelight/licenses/SOURCES.md) 同仓库提供。

These files target two different devices. The Korvo file is an **application-only**
image and requires the matching partition layout. The CANable image is only for
the listed STM32F072C8 board. Check the exact target before writing either device.
The corresponding sources and third-party license texts are included in this repository.

## SHA-256

```text
5e1c2f87b8543654e0cec94df64e603bd8b2aca6ab5b08491ed48fd75681e7a2  kst-servo-fui-s31-axis-view.bin
99f2a17c7bb5536343eee00ce6e907f394deac11b5c95f2e88804dba32a4a091  zt-canable-pro-f072c8-candlelight.bin
```
