# ESP32 USB Host → candleLight → KST

本组件把 `kst_transport_t` 接到 **candleLight / gs_usb** USB CAN 适配器。
它不支持 PCAN、SLCAN、DFU，也不会更新适配器固件。USB VID/PID 识别只决定
是否尝试 gs_usb 协议，不能据此选择适配器的烧录镜像或推断 PCB 型号。

## 连接及运行

```text
ESP32-S31-Korvo-1 USB-A (USB Host)
             │ USB 数据线
       candleLight CANable
             │ CANH / CANL / 总线侧参考地
           KST 舵机 ← 独立 12 V 电源
```

遵循主项目硬件文档的隔离地、终端电阻和供电说明。这个路径不占用 TWAI
GPIO，也不使用 USB 转串口类驱动。只启用经典 CAN、通道 0、扩展数据帧；
固件需报告恰好一个 CAN 通道和 `ONE_SHOT` 特征。

依赖 `espressif/usb == 1.5.0`、`gs_usb_protocol`、`kst_servo`、`esp_timer`。
组件独占 USB Host 库和根端口，不能与已安装的 CDC/其他 USB Host 驱动共享。
不能在同一进程打开多个实例。

```c
kst_usb_gs_bus_t *bus = NULL;
ESP_ERROR_CHECK(kst_usb_gs_open(500000, &bus));
kst_transport_t transport = kst_usb_gs_transport(bus);
/* 使用 transport 创建 kst_servo_t，参考主项目的完整控制器。 */
for (;;) {
    kst_usb_gs_service(bus, 10);
    kst_usb_gs_stats_t state;
    kst_usb_gs_get_stats(bus, &state);
    /* generation 变化：取消 ARM、旧命令和旧反馈。
     * ready 后先执行只读查询，绝不自动恢复动作。 */
    if (state.ready) {
        /* kst_servo_poll / 经过用户明确 ARM 后的控制请求 */
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}
/* 有界关闭；若返回错误，保留 bus 并再次调用 close，不能 free(bus)。 */
```

`open()` 在没有适配器时也可成功，界面仍能运行。调用者必须持续调用
`service()`，包括 `ready=false` 时。一次 `service` 以传入预算推进状态机，
不阻塞等待某一整套枚举或配置完成。配置顺序为 HOST_FORMAT、DEVICE_CONFIG、
BT_CONST、MODE RESET、BITTIMING、MODE START。位时序由设备时钟和约束计算，
仅接受精确目标波特率；目标采样点 80%，无法精确命中时取可实现的最近值。
该预算约束事件等待循环；底层 SDK 的设备/端口管理锁不提供硬实时上界。

所有公开 API、transport 回调及统计读取应由**同一个任务**调用。组件只创建
USB 库事件任务；USB client 事件由上述调用者处理，不会从另一个任务进入
舵机协议栈。给其他 UI 任务看的快照，应由调用者自行复制并同步。

## 状态和发送语义

| 字段 | 含义 |
| --- | --- |
| `connected` | 已认领匹配的 USB 接口；并不保证 CAN 已配置 |
| `ready` | 已完成配置并启动 ONE_SHOT 经典 CAN |
| `generation` | 连接、断连、故障、显式重连或 READY 转换使其改变 |
| `tx_ok` | USB OUT 完成的帧数，**不是 CAN ACK** |
| `tx_echoes` | 匹配当前发送标识的回显数，**不是舵机执行确认** |
| `rx_frames` | 接收的合法扩展 CAN 数据帧数 |
| `rx_standard` | KST 过滤前观察到的标准帧数；不包含自身 TX echo |
| `last_can_*` | 最近一帧真实接收的 CAN ID、EFF/RTR 标志、DLC、原始 8 字节和收到时的 generation；使用前检查 `last_can_valid` |
| `last_error_*` | 最近 CAN 错误帧的错误分类 ID、原始 8 字节和收到时的 generation；使用前检查 `last_error_valid` |
| `rx_dropped` | 无关帧、无效回显、溢出等丢弃数量 |
| `bus_errors` | 适配器报告的 CAN 错误及 RX 溢出数 |
| `usb_errors` | USB API、控制或数据传输错误 |
| `status` | 可直接显示的英文状态/失败原因 |
| `requires_close` | 部分初始化/关闭失败或库故障，须成功 close 后再 open |

最近帧在故障后保留供诊断，必须结合其 generation，不能当成当前连接的新反馈。
标准帧/RTR 仍不会交给 KST 扩展帧协议；这些诊断字段仅用于区分“没有任何 CAN
回包”和“有回包但帧格式/节点不匹配”。主应用默认每 2 秒输出 `kst_diag` 串口
快照，可用 `CONFIG_KST_DIAGNOSTIC_LOG=n` 关闭；不会增加 CAN 查询或改变运动控制。

`send()` 返回 0 只表示 USB OUT 完成；KST 有回包的命令仍需要等待协议回包。
位置及 SAVE 等本来没有应用 ACK 的命令，仍需显示 `SENT_UNCONFIRMED`。
candleLight 的 echo 表示写入 CAN 外设，不等于总线传输完成。组件最多提交
一个 USB OUT；下一帧须等到上一帧的匹配 echo，避免在适配器中堆积命令。
应用应注册 `kst_usb_gs_set_tx_guard()`：组件在等待上一帧 echo 后、真正提交
USB OUT 前调用此检查，避免等待期间的 PAUSE、失效 ARM 或新反馈被忽略。
回调得到待发帧及 `rx_pending`（有新 CAN 数据尚未交给应用），应有界、
非阻塞地检查当前命令是否仍有效，不能递归进入收发或 service。返回 false
只拒绝该帧，不破坏健康 USB 连接。对于运动命令且存在未处理反馈，应用应
拒绝提交、先正常处理反馈再由控制逻辑决定后续动作。

开始前要求适配器声明 ONE_SHOT，拒绝静默降级到自动重发模式。ONE_SHOT
只禁止 CAN 自动重发，不能撤销已经进入外设的任意指令，也不保证总线 ACK。
本组件不会重试、跨连接重发任何旧帧。主项目还须在 generation 改变时取消
已排队动作，且重新连接只恢复只读检测。

CAN bus-off、USB 超时、无效报文、RX 队列或适配器溢出都会锁存故障并撤销
ready。调用 `reopen()` 才能解除故障；它异步断开 USB 会话、等待所有 pending
transfer 回调归还、重新启用根端口并重新配置。物理拔插也会清空所有已接收
帧和 echo 状态。若已锁存故障，拔插不会自动清除故障。
`send()` 超时后立即锁存故障并返回，端口取消由下一次 `service()` 推进；
不会为了清理在发送回调中追加 SDK 的阻塞端口操作。因此超时不证明帧尚未
发送，也不表示 USB 已立即断开。调用者必须持续 service，不能报错后停泵。

ESP USB Host 1.5.0 的 transfer `timeout_ms` 尚未实现，EP0 也没有公开的单独
取消 API，因此控制请求以 500 ms 绝对期限检测，超时关闭**整个根端口**，
由断连流程取消传输。USB 根端口关闭不是舵机急停。Korvo 原理图的 USB-A
TPS2051C 使能由电阻拉高，并无 GPIO 控制，因此此 API **不能关闭该插座
5 V**。适配器必须正确处理 USB reset/suspend、清空软件发送队列并取消 CAN
mailbox；这些行为仍需实物验证。

`receive(timeout=0)` 只做一次非阻塞事件 pump，不枚举、不等待初始化、不
进行阻塞清理。回调只交还传输和保存状态；RX 不在回调中重提交，因此连续
CAN 流量不会让一次事件处理无限循环。RX 队列有 48 帧上限。

`close()` 先停止发送及关闭端口，等待传输回调，释放接口，注销 client，
再等待库事件任务退出并卸载 USB Host。绝不释放 in-flight transfer。如果
异常导致清理超时，它返回错误并保留有效对象，应重试关闭；不能自行释放。
极少数情况下 `open()` 失败后清理也未完成，它返回错误同时保留非空 `*out`；
此对象会报告 `requires_close=true`。必须成功 `close()` 后清空调用者指针，
才能重新 `open()`；不能在部分关闭对象上调用 `reopen()`。

## 兼容范围与验收

- 当前仅尝试 `1d50:606f` 和 `1209:2323`；USB 配置须为明确的单一厂商接口、
  alt 0、两个 Bulk 端点。拒绝歧义配置；对未知 VID/PID 不发送厂商请求。
- 不启用 CAN FD、硬件时间戳、padding、loopback、listen-only。这些不是 KST
  V3.61B 所需的功能。支持这些特征的固件仍可使用经典模式。
- 非扩展或 RTR 帧只计丢弃，不送入 KST 库。CAN 错误帧在本层计数和处理。
- USB VID/PID、控制请求响应和成功编译都不是实际 CAN 通信验收。实物需要
  验证枚举、500k 扩展帧读回、全部 KST 波特率、拔插、无 ACK、bus-off、
  连续流量、显式重连及 ARM 不恢复。未经实测不要报告已支持某个克隆板。

协议与硬件行为参考：

- [candleLight 上游及发送回显限制](https://github.com/candle-usb/candleLight_fw)
- [Linux gs_usb 协议实现](https://github.com/torvalds/linux/blob/master/drivers/net/can/usb/gs_usb.c)
- [candleLight bxCAN 配置](https://github.com/candle-usb/candleLight_fw/blob/master/src/can/bxcan.c)
- [candleLight USB ID 配置](https://github.com/candle-usb/candleLight_fw/blob/master/include/config.h)
- [candleLight 单帧 USB 发送](https://github.com/candle-usb/candleLight_fw/blob/master/src/usbd_gs_can.c)
- [ESP32-S31 USB Host 文档](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s31/usb_host.html)

组件为独立编写的 MIT 实现，通过纯 C 的 `gs_usb_protocol` 处理 USB/CAN
字节格式；没有复制 Linux GPL 驱动实现。
