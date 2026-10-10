# 可移植 KST 同步驱动

`kst_servo` 在 `kst_protocol` 之上提供完整的 V3.61B 读写事务。调用方提供发送、接收和单调时钟回调，即可用于 ESP-IDF、其他 MCU 或主机 CAN 接口。驱动不分配堆内存、不创建任务、不打开设备，也不会自动保存参数或重启舵机。

## 接入约定

```c
kst_servo_t servo;
kst_transport_t transport = {
    .ctx = my_can_handle,
    .send = my_can_send,
    .receive = my_can_receive,
    .now_ms = my_monotonic_ms,
};
kst_result_t result = kst_servo_init(&servo, 0x25, &transport, 300,
                                     on_telemetry, my_application);
if (result == KST_SERVO_OK) {
    kst_event_t position;
    result = kst_servo_read_position(&servo, &position);
    if (result == KST_SERVO_OK) {
        /* position.position_deci_deg / 10.0 is the reported angle. */
    }
}
```

`my_can_*`、`my_monotonic_ms`、`on_telemetry` 是调用方提供的函数，其准确签名见头文件。传输层必须满足以下契约：

| 回调 | 约定 |
|---|---|
| `send(ctx, frame, timeout_ms)` | 0 成功，负值为传输错误；发送经典 CAN 扩展数据帧 |
| `receive(ctx, frame, extended, rtr, fd, timeout_ms)` | 1 返回一帧，0 为空/超时，负值为传输错误；如实填写帧类型标志 |
| `receive(..., 0)` | 必须立即返回，用于排空旧帧及非阻塞轮询 |
| `now_ms(ctx)` | 单调毫秒时钟；不要使用会校时跳变的墙上时钟 |

两个 I/O 回调必须遵守给定超时。`timeout_ms` 同时涵盖事务的发送和等待响应时间。队列清理最多 64 帧，若仍未遇到空队列，返回 `KST_SERVO_ERR_RX_BUSY`，不发送新命令。

## 所有高级方法

| 功能 | 方法 |
|---|---|
| 查询节点、角度、电流温度、功能位、版本 | `kst_servo_read_node/position/status/features/version` |
| 查询正负运动范围 | `kst_servo_read_limit(client, positive, out)` |
| 启停主动位置上报 | `kst_servo_set_reporting(client, enabled)` |
| 设置上报间隔 | `kst_servo_set_report_interval(client, milliseconds)` |
| 设置目标位置 | `kst_servo_set_position(client, position_deci_deg)` |
| 将当前位置设为中点 | `kst_servo_set_center(client)` |
| 修改低压报警/逻辑反向位 | `kst_servo_set_features(client, mask, values)` |
| 暂存新节点、新波特率、正负运动范围 | `kst_servo_set_node_id`、`kst_servo_set_baudrate`、`kst_servo_set_limit` |
| 显式保存修改的参数 | `kst_servo_save(client)` |
| 收集一个有效事件 | `kst_servo_poll(client, timeout_ms, out)` |

所有读取方法的 `out` 可以为 `NULL`；非空时仅在成功后写入。每个有效收到的事件都会同步调用 `event_callback`，包括请求期间穿插的主动上报、与请求不匹配的合法回复，以及发送前清理到的旧帧。其他节点、错误格式或错误固定字节的帧不会进入回调。

`kst_servo_set_features()` 完成“读取完整原值 → 按 mask 更新并写入 → 等待正确 ACK → 读回验证指定 mask”，最多三个独立事务。只有 bit 5、bit 6 允许修改，其余低字节位保持读取值；不会隐式保存。任何步骤失败都立即返回，不重试写入。

固件 3.66 的实物 FEATURES 原值为 `0x5C90`。读取可以成功返回 `event.feature_raw=0x5C90` 及 `event.feature_bits=0x90`；高字节含义与写入格式没有文档。写 API 的前置读取若发现高字节非零，立即返回 `KST_SERVO_ERR_UNSUPPORTED_FEATURES`：只发出读取帧，不发 FEATURES 写帧、不保存、不尝试将高字节清零或推测16位写法。原高字节为零的 V3.61B 读改写流程保留；若写后验证突然出现非零高字节，返回验证失败且不重试。

## 返回值的实际含义

| 返回值 | 含义 |
|---|---|
| `KST_SERVO_OK` | 收到所期望的应用层响应；启动上报时表示收到位置上报 |
| `KST_SERVO_SENT_UNCONFIRMED` | 已交给 CAN 发送接口，但此命令没有协议应用层 ACK |
| `KST_SERVO_ERR_ARGUMENT` | 参数非法、客户端未初始化或传输接口不完整 |
| `KST_SERVO_ERR_TRANSPORT` | CAN 接口发送/接收失败 |
| `KST_SERVO_ERR_TIMEOUT` | 等待预算结束，仍无匹配响应；非阻塞 poll 没有事件也返回此值 |
| `KST_SERVO_ERR_ABORT` | 舵机返回同一索引/子索引的失败；查看 `last_abort` 等字段 |
| `KST_SERVO_ERR_VERIFY` | 功能位读回值与指定修改不符 |
| `KST_SERVO_ERR_RX_BUSY` | 旧帧过多、持续异常流量或发生重入调用 |
| `KST_SERVO_ERR_UNSUPPORTED_FEATURES` | FEATURES 回读含未定义的非零高字节；为保留原值禁止写入 |

位置目标、保存、停止上报成功发送后返回 **SENT_UNCONFIRMED**。位置不等待虚构 ACK，实际到位需要读位置判断；保存无 ACK，不能凭返回值认定闪存写入完成。稍后返回的 ABORT 会在 `poll()` 或其他接收期间进入回调。

节点、波特率和正负限位的写 ACK 仅表示配置命令被接受，必须显式保存并重启舵机才能生效。设置节点后此客户端的 `node` 仍是旧值；硬件重启后由调用方重新初始化客户端。设置波特率不会修改主控 CAN 波特率。

## 并发、旧帧与超时

一个客户端及其 RX 队列由一个任务独占。不要让另一个任务同时从同一队列取帧，也不要从事件回调中再次调用驱动。驱动会拒绝同步重入，但不提供跨线程锁；FreeRTOS 等平台应在外层串行化命令。

匹配回复必须同时满足事件类型、对象索引、子索引。因此主动位置上报不会误充位置读取应答，其他配置项的 ACK 也不会误充当前操作成功。每次发命令前会清理有界旧帧，减少将队列中的旧回复当成新回复的风险。

**协议没有事务序列号。** 新请求发出后才到达的同索引旧回复，单靠此协议无法区分。驱动不声称消除这种歧义；配置变更仍应通过可读项回读和重启后的实际状态验证。收到上报也只能证明一帧上报到达；如果先前已开启上报，无法据此严格证明新的启动命令被处理。

错误帧、其他节点或合法但不匹配的事件均不会延长绝对等待预算。单次等待额外设有 4096 帧上限，避免失常时钟/适配器无休止地产生帧。

`stats.timeouts` 包括非阻塞 poll 的空队列结果，不能直接用该计数作为设备掉线告警。应用应根据成功通信时间和显式读操作的结果判断。

## 测试边界

`tests/test_kst_servo.c` 使用独立 fake transport，覆盖回复与上报交织、错误索引、旧队列、绝对超时、失败响应、功能位保留与回读、无 ACK 命令、所有高级方法、格式过滤、重入和队列上限。项目的 `tests/run_host_tests.ps1` 与 `tests/run_host_tests.sh` 同时运行协议测试与驱动测试。

这些测试证明纯 C 编解码及事务行为，不等于已经验证 ESP32-S31 外设、隔离 CAN 模块接线或实际舵机动作。
