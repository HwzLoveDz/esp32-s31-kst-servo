# ESP-IDF TWAI transport

`kst_twai_open(tx_gpio, rx_gpio, bitrate, &bus)` 创建控制器；`kst_twai_transport(bus)` 返回可交给 `kst_servo_init()` 的传输接口；`kst_twai_get_stats()` 读取IRQ计数与bus-off状态；`kst_twai_reopen()` 显式重建控制器；`kst_twai_close()` 停止、删除控制器后释放内存。

本适配器基于当前S31 SDK的 `esp_twai.h` / `esp_twai_onchip.h` 新驱动。没有使用旧 `driver/twai.h`。调用端单任务拥有bus及其client，禁止并发send/reopen/close。

- 外接3.3V兼容CAN收发器，GPIO默认由上层配置。
- 所有TX为经典29位扩展数据帧，禁用FD/BRS/RTR；RX先检查格式再交给协议解析器。
- 48帧有界接收队列，IRQ按值拷贝；满队列计数，不阻塞ISR。
- TX队列深度1，有限重发3次；描述符与8字节payload存于bus对象，不使用发出后失效的栈变量。
- 应用等待TX完成；TX超时删除控制器，避免恢复总线时重播过期的位置命令。显式重连重建节点并清空RX/信号量。
- `bus_off`、TX、RX、丢弃、总线错误计数为原子变量。其它对象不支持多任务并行访问。
- **保持 `CONFIG_TWAI_ISR_CACHE_SAFE=n`**，回调没有IRAM属性。当前应用也不启用动态电源管理。
- `close()`若底层节点无法删除会返回错误，并保留对象避免释放仍被ISR引用的内存；调用者应保留handle处理该错误。

典型接入见 `examples/headless_poll`。本适配器不解释舵机应用ACK，不做自动运动，不做自动SAVE或总线自动重连。总线硬件ACK不能证明舵机到位或Flash写入完成。
