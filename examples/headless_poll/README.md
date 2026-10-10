# 独立只读接入示例

这个ESP-IDF工程只依赖三个KST组件，不含LCD、触摸和LVGL。默认GPIO46/47、500kbit/s、节点0x25；仍需要遵守主工程 `docs/HARDWARE.md` 的摄像头断开和隔离接口接线。

上述是厂家文档的通用默认值。2026-10-10 本次实物通过 USB-CAN 读出节点 **5**、
通信速率 **1 Mbit/s**、版本 **3.66**；若用这个 TWAI 示例连接同一只舵机，应将
`main/main.c` 中 `kst_twai_open` 的速率改为 `1000000`，并将 `kst_servo_init`
的节点改为 `5`。主 FUI 工程已设置这组实物参数；本独立示例仍保留文档默认值。

在匹配的S31 IDF环境中：

```sh
cd examples/headless_poll
idf.py --preview set-target esp32s31
idf.py --preview build
```

`main/main.c` 展示创建bus、注入传输、读取功能/位置/状态、回调处理和失败时关闭。返回位置是协议原始符号；需要统一命令坐标时读取feature bit6并相应变号。

要移植到其他平台：保留 `kst_protocol`、`kst_servo`，把 `kst_twai` 替换为目标平台的send/receive/now_ms实现。接收接口必须返回扩展/远程/FD标志并尊重超时，`receive(0)`必须非阻塞；一个client及其RX队列只由一个任务持有。不要直接把两个client连接到同一个消费式队列，否则会互相吞掉响应；多节点应在一个总线管理任务串行调度或用路由队列。
