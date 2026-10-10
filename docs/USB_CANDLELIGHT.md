# USB Host 与隔离 CANable

本项目在 **ESP32-S31-Korvo-1 + ZT-CANable Pro V1.0 隔离版**上完成 USB 枚举、gs_usb 配置和 KST 舵机通信。适配器使用与这块 STM32F072C8 板型匹配、明确声明 `ONE_SHOT` 能力的 candleLight 固件。PCAN、SLCAN 和 candleLight 是不同 USB 协议，本驱动使用 candleLight 的 gs_usb 接口。

## 接线

配套 [ZT-CANable Pro V1.0 / STM32F072C8 candleLight 源码](../firmware/zt-canable-candlelight/README.md)
包含本项目使用的 ONE_SHOT 能力声明与会话清理适配。
预编译镜像及对应板型见 [release](../release/README.md)。

```text
Korvo USB-A ── USB 数据线 ── ZT-CANable（candleLight）
                                CANH ── 舵机 CAN_H（黄）
                                CANL ── 舵机 CAN_L（白）
                               GND_O ── 舵机电源负极（棕，3脚）
                         独立 12 V 正极 ── 舵机电源正极（红）
```

Korvo 的 UART Type-C 用于烧录和日志，Power Type-C 用于电源；CANable 接 Type-A USB Host。USB 数据线仍传输 USB，CANable 完成 USB 与 CAN 的转换，并提供隔离 CAN 收发器。这条路径无需外加 GPIO CAN 模块，也不占用摄像头接口。

总线两端各接一个 120 Ω 终端。ZT-CANable 的 P2 跳帽接入板载终端；舵机端是否已有终端需按实际设备确认。KST 扩展帧协议要求两端终端，但没有说明舵机内部是否自带。关闭舵机 12 V、拔掉 CANable USB，保持 CAN 接线和终端跳帽，再测 CAN_H/CAN_L 间电阻：两只 120 Ω 并联约为 60 Ω；约 120 Ω 通常表示只测到一个终端，也需区分缺少终端与远端线路未连通。

12 V 供舵机，不接 USB 5 V。CANable 的隔离侧 GND_O 接舵机电源负极；USB 侧 GND 与 GND_O 不另加跨隔离连线。完整线序、电源条件和可选 TWAI 接线见 [HARDWARE.md](HARDWARE.md)。

## 实测板型

ZT-CANable Pro V1.0 原理图（2023-02-21）给出以下连接：

| 项目 | 配置 |
|---|---|
| 主控 | STM32F072C8T6 |
| CAN | PB8 RX，PB9 TX |
| USB | PA11 D−，PA12 D+ |
| 时钟 | 无外部晶振；配套固件使用内部 HSI48/USB CRS |
| 活动 LED | PA0、PA1，低有效 |
| 隔离 CAN | ADM3053，RS 接隔离侧地 |
| P3 总线端子 | 1 CAN_H，2 CAN_L，3 GND_O |
| 终端 | R7 120 Ω，经 P2 跳帽接入 |
| P1 | BOOT0 跳帽 |

这份连接表只对应上述板型。其它标为 CANable 1.0 的产品可能使用不同主控、晶振、引脚或 USB 身份，应选择与实际 PCB 匹配的固件构建目标。

## candleLight 的必要能力

Korvo 驱动要求适配器在能力查询中声明 `ONE_SHOT`，并在启动经典 CAN 时启用该模式。一次发送失败后由上层决定后续操作，避免旧运动目标因硬件无限自动重发而长期留在队列中。固件缺少该能力时，LINK 显示能力不足并拒绝进入 READY。

部分旧版厂家源码已经把 ONE_SHOT 模式映射到 bxCAN NART，却遗漏了能力查询 bit 3，因此“candleLight 固件”这个名称本身不足以判断兼容性。厂家资料中的 `candleLight` 构建目标面向 F072，而另一个名为 `canable` 的目标面向 F042；两者不能互刷。已交付二进制也不能仅凭源码包名称认定为同一版本。

复刻时需要取得与实际适配器匹配、明确支持 ONE_SHOT 的 gs_usb 固件。适配器刷写前核对 MCU、PCB 版本和目标 USB 设备；本仓库的 Korvo 应用不会自动替适配器选镜像或烧录。

## 驱动接口

[kst_usb_gs](../components/kst_usb_gs/README.md) 说明 USB 生命周期、故障恢复和复用 API；[app_controller.c](../main/app_controller.c) 是完整示例，由单个 CAN worker 调用 open/service、注入 `kst_transport_t`、处理事件并向 FUI 发布快照。

- 无适配器时界面继续运行并显示 USB 未连接；PCAN/SLCAN 不会被当成 gs_usb。
- 初始化成功后使用经典 CAN ONE_SHOT，仅启用通道 0。应用默认节点 **5 / 1 Mbit/s**；厂家协议通用默认 **37 / 500 kbit/s** 与这只实测舵机不同。
- USB 连接与有效舵机回复分别显示。LINK 提供 VID/PID、位速率和错误原因。
- USB TX/echo 不等于 CAN ACK；位置命令、SAVE 的结果仍显示发送未确认。
- 断连、故障和重配置使旧反馈失效，旧连接中排队的命令不会在新连接重放。
- 重连后只读，必须获得新反馈并重新 ARM 才能运动。主机暂停不释放舵机力矩。

## 首次连接

1. 确认 CANable 的实际板型及当前 USB 协议；需要更换适配器固件时，先解决板型匹配和 ONE_SHOT 能力。
2. Korvo 烧入本工程 USB 版，再将 CANable 接 USB-A。LINK 应显示匹配身份及 READY。
3. 接好 CAN 总线并给舵机供电，先读取版本、位置、电流、温度及双向限位。只有 USB READY、没有真实舵机回包时，不能解锁运动。
4. 手动 ARM 并发送小角度目标，确认机械方向和实际反馈，再按 [FUNCTIONS.md](FUNCTIONS.md) 逐项测试。
5. 在机构允许的条件下分别验证拔插、缺失 ACK、总线故障、旧命令失效与重连不恢复 ARM。

本项目已完成该硬件组合的只读通信、触屏运动和恢复操作；八档速率、所有配置持久化及故障注入仍须分别验证，详见 [VALIDATION.md](VALIDATION.md)。
