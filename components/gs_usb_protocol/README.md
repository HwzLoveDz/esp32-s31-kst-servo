# gs_usb_protocol

平台无关的 C99 gs_usb / candleLight 字节编解码与 CAN 位时序计算库。该实现按公开协议独立编写，MIT 许可；不依赖 ESP-IDF、USB 库、SocketCAN、结构体打包或主机字节序。

本组件只解释协议，不接触 USB 设备或 CAN 总线。USB Host 层负责允许的 VID/PID、USB 枚举、控制请求、传输超时与资源生命周期；舵机层负责节点、业务应答和运动状态。

## 支持范围

- 完整 USB configuration descriptor 的长度与 record 边界检查；选择唯一的 alt 0、vendor class、一个 Bulk IN + 一个 Bulk OUT 的接口。拒绝含糊的多个候选、重复端点、端点数量不符与结构损坏。接口解析不替代 VID/PID allowlist。
- HOST_FORMAT 的 `0x0000beef` 小端编码；DEVICE_CONFIG 和 BT_CONST 的严格长度解码。
- BITTIMING 和 MODE 的小端编码。MODE 只允许经典 CAN 支持的标志，并要求每个启用的功能均由设备 BT_CONST.features 宣告。
- 20 字节经典 CAN 帧；协商 HW_TIMESTAMP 后接收 24 字节帧。TX 固定为 20 字节。标准/扩展 ID、RTR、错误帧、overflow 和 echo 均独立表示。
- 拒绝 CAN FD、未知帧标志、DLC > 8、非零保留字节、错误通道、错误包长、标准帧超过 11 位的 ID、错误帧与 EFF/RTR 的矛盾组合。错误帧必须 DLC=8、来自接收方向。
- 支持非对齐 buffer；全部整数通过字节读写，失败时保留调用者输出。

不会把 USB echo 当成总线成功 ACK 或舵机执行确认。`frame.echo_id == GS_USB_ECHO_RX` 表示收到的 CAN 帧，其余 echo ID 是主机提交帧的回显。错误帧永远不能传给普通舵机响应解析器。

## 控制请求约定

USB setup 包由 Host 层生成。按 Linux 上游的 gs_usb 实现：

| 请求 | bmRequestType | wValue | wIndex | payload 长度 |
|---|---|---|---|---|
| HOST_FORMAT | 0x41 | 1 | bInterfaceNumber | 4 |
| DEVICE_CONFIG | 0xC1 | 1 | bInterfaceNumber | 12 |
| BT_CONST | 0xC1 | channel | 0 | 40 |
| BITTIMING | 0x41 | channel | 0 | 20 |
| MODE | 0x41 | channel | 0 | 8 |

DEVICE_CONFIG 的 `icount` 表示最后一个通道编号，组件转换成 `channel_count = icount + 1`，用 16 位避免 255 + 1 溢出。

`GS_USB_FEATURE_ONE_SHOT` 是 bit 3，同时用作 MODE flag。上游 bxCAN 固件宣告此能力后，在启动时设置 `CAN_MCR_NART`，禁用控制器自动重发。没有宣告的设备必须拒绝启动要求 ONE_SHOT 的驱动，不能悄悄丢弃此请求。ONE_SHOT 不会取消已经排进 USB/适配器队列的命令；Host 仍须串行发送和处理超时。

## 精确位时序

`gs_usb_find_bittiming()` 要求精确波特率，不接受近似速率。sample point 是目标值，按设备约束选择最近可实现值；误差相同时优先使用更多 time quanta。SJW 固定为 1。按查询到的设备时钟计算，不将 48 MHz 写死在算法中。

对于 48 MHz、TSEG1=1..16、TSEG2=1..8、BRP=1..1024 的 bxCAN：

| bitrate | BRP | TSEG1 | TSEG2 | sample point |
|---|---|---|---|---|
| 20 kbit/s | 120 | 15 | 4 | 80% |
| 50 kbit/s | 48 | 15 | 4 | 80% |
| 100 kbit/s | 24 | 15 | 4 | 80% |
| 125 kbit/s | 24 | 12 | 3 | 81.25% |
| 250 kbit/s | 12 | 12 | 3 | 81.25% |
| 500 kbit/s | 6 | 12 | 3 | 81.25% |
| 800 kbit/s | 3 | 15 | 4 | 80% |
| 1 Mbit/s | 3 | 12 | 3 | 81.25% |

`sample_point_permille` 为四舍五入的显示值，因此 81.25% 返回 813；CAN 外设实际配置仍是精确的整数分段。搜索限制 segment 最大值为 1024、BRP 最大值为 65536，防止恶意描述数据导致无界运算；这远覆盖目标 bxCAN 的范围。

## 验证与来源

`tests/test_gs_usb_protocol.c` 覆盖黄金字节向量、非对齐访问、截断与损坏的描述符、未知/矛盾帧标志、保留字节、通道、DLC、20/24 字节模式、echo/error/RTR 与完整 8 档速率。位时序结果用独立的穷举算法核对最佳采样点及 tie-break，另含 36 MHz 和非 1 的 BRP increment。

协议来源（核对日期 2026-10-10）：

- [candleLight 公共协议头](https://github.com/candle-usb/candleLight_fw/blob/master/include/gs_usb.h)
- [candleLight bxCAN 能力和 ONE_SHOT 实现](https://github.com/candle-usb/candleLight_fw/blob/master/src/can/bxcan.c)
- [Linux gs_usb 控制请求和设备 ID](https://github.com/torvalds/linux/blob/master/drivers/net/can/usb/gs_usb.c)

用户提供的 ZT 旧版本源码同样支持 20/24 字节帧和 ONE_SHOT→NART，但其 BT_CONST 未宣告 ONE_SHOT；不能据此将任意原装二进制视作已验证兼容。实际固件特性须以设备枚举/查询和实测为准。
