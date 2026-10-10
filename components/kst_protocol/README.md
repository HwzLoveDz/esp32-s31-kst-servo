# KST CAN V3.61B 协议模块

此模块仅负责报文编解码，无 ESP-IDF、FreeRTOS、硬件或动态内存依赖。依据用户提供的《KST 舵机 CAN 总线通信协议（扩展帧）V3.61B》，版本日期 2022-05-19。传输层负责实际发送、超时、请求与回复匹配以及运行安全。

## 总线格式与单位

- **经典 CAN 2.0B、29 位扩展数据帧**。虽然 ID 数值小于 `0x800`，也必须设置扩展帧标志。不是 CAN FD，不是远程帧。
- 默认节点 `0x25`，默认 `500000 bit/s`；所有构帧 API 接受节点 `1..127`，拒绝广播节点 `0`。
- 整数载荷采用小端序。角度单位 `0.1°`，使用 16 位补码。目标写入、设备限位读写保持文档范围 `-1000..1000`；位置读取和自动上报单独接受 `-1800..1800` 的单圈遥测。
- 电流单位 `10 mA`，无符号 16 位。温度单位 `1°C`，8 位补码，文档范围 `-40..120°C`。
- 数据的 `0.1°` 刻度不表示机构精度；电流遥测不等于力矩控制。

## API 覆盖与生效语义

| 函数 | 文档章节 | 回复/生效语义 |
|---|---|---|
| `kst_make_reporting` | 2.1、2.2 | 开启后首个位置上报是文档规定的成功迹象；停止没有应答帧 |
| `kst_make_report_interval` | 2.5 | 10..255 ms；写 ACK；立即生效，保存后持久化 |
| `kst_make_baudrate` | 2.6 | 写 ACK；必须保存并重启舵机后生效 |
| `kst_make_node_id` | 2.7 | 新节点 1..127；写 ACK；必须保存并重启舵机后生效 |
| `kst_make_position` | 2.8 | 成功无应用层 ACK；受当前运动范围限制，失败可能返回 ABORT |
| `kst_make_center` | 2.9 | 将当前位置设为中点；写 ACK；立即生效，保存后持久化 |
| `kst_make_save` | 2.10 | 保存所有已修改参数；无应用层 ACK，文档未给出写入完成时长 |
| `kst_make_read_node` | 2.11 | 节点号读取回复 |
| `kst_make_read_position` | 2.12 | 位置读取回复 |
| `kst_make_read_status` | 2.13 | 电流与温度读取回复 |
| `kst_make_read_features` / `kst_make_features` | 2.14 | 先读取再改指定功能位，保留其余位；写入立即生效，保存后持久化 |
| `kst_make_limit` / `kst_make_read_limit` | 2.15、2.16 | 正负限位读写；写 ACK；必须保存并重启舵机后生效 |
| `kst_make_read_version` | 2.17 | 返回四字节版本字符串 |
| `kst_parse` / `kst_abort_description` | 2.3、2.4、2.18、附录 1 | 解析自动上报、欠压、读回复、写 ACK、ABORT；错误码可读文本 |

`kst_baudrate_bps()` 将协议枚举转换为实际 bit/s。支持的枚举依次为 20k、50k、100k、125k、250k、500k、800k、1000k。

文档的“逻辑反向”说明针对**读取位置的符号**；此库不会把它解释为物理旋转方向设置。欠压报警默认关闭，其阈值文档说通常约为额定电压的 70%，不是精确电压遥测。

## 接入示例

```c
kst_can_frame_t tx;
if (kst_make_position(KST_DEFAULT_NODE_ID, -109, &tx)) {
    /* tx.id=0x625, tx.len=8
     * tx.data=22 03 60 00 93 FF 00 00
     * 在所用 CAN 驱动中显式设 extended=true、rtr=false、fd=false。 */
    transport_send_extended_classic_data(&tx);
}

kst_event_t event;
if (kst_parse(KST_DEFAULT_NODE_ID, &rx, is_extended, is_rtr, is_fd,
              &event) == KST_PARSE_OK) {
    if (event.type == KST_EVENT_STATUS) {
        float amperes = event.current_10ma * 0.01f;
        int temperature_c = event.temperature_c;
        /* 使用 amperes 和 temperature_c。 */
    }
}
```

上述 `transport_send_extended_classic_data` 仅代表平台发送接口，不是本模块定义的函数。

功能位修改示例：先完成 `kst_make_read_features()` 并收到 `KST_EVENT_FEATURES`，然后将完整的 `event.feature_raw` 传入 `kst_make_features(node, event.feature_raw, KST_FEATURE_UNDERVOLTAGE, KST_FEATURE_UNDERVOLTAGE, &tx)`。只有高字节为零时才构帧成功；这样只打开 bit 5，不改 bit 6 或文档保留位。关闭时将第四个参数改为 0。不要把原始值截断为 `feature_bits` 后尝试写入。

实物固件 3.66、节点 5 的 FEATURES 回复为扩展帧 `0x585 / DLC 8 / 4B 0F 30 00 90 5C 00 00`。V3.61B 第 6 页只定义低字节 `X1`，后续三字节为零。本模块兼容该实物的两字节读取：`feature_raw=0x5C90` 保留全部原值，`feature_bits=0x90` 继续提供文档低字节 bit 5/6；不解释高字节 `0x5C`，也不将其视为可以写入的格式。只接受 `0x4B` 回复，最后两字节仍必须为零。未知高字节非零时 `kst_make_features()` 返回 false，输出帧保持不变。

同一实物的负限位回复为 `0x585 / DLC 8 / 4B 0A 30 00 18 FC FF FF`，即低16位 `-1000`（-100°），高两字节为符号扩展。本模块只对 `LIMIT_NEGATIVE` 的合法负值 `-1000..-1` 接受恰好 `FF FF` 的尾字节，同时保留 V3.61B 的 `00 00` 格式。零/正值配 `FF FF`、混合 `FF 00` 或 `00 FF`、超范围值均拒绝；位置与正限位回复仍要求 `00 00` 尾字节。该兼容仅影响读取，负限位写帧继续使用文档的 `18 FC 00 00`，不推测其它对象格式。

同一实物在被转过 -100° 后仍返回合法位置帧 `0x585 / DLC 8 / 4B 02 60 00 13 FC 00 00`，即 `-1005`（-100.5°）。反馈不能沿用目标写入范围，否则有效遥测被丢弃、上层等待新位置而无法恢复。`KST_POSITION_FEEDBACK_MIN/MAX` 将位置读取与自动上报的接受范围独立限定为 ±180°，含边界；这只用于观察单圈角度，不表示固件支持 ±180°目标、限位写入或多圈定位。超出 ±180°、`INT16_MIN` 和不符合尾字节格式的数据仍拒绝；有效反馈取反后仍可存入 `int16_t`。`KST_POSITION_MIN/MAX`、目标构帧以及正负限位读写仍为 ±100°。运动许可与目标范围继续由调用方独立检查。

同一实物拒绝 +180°限位试写时返回 `0x585 / DLC 8 / 80 0B 30 00 06 09 00 30`。其错误码四字节采用大端序，对应文档 `0x06090030`（参数越界）。仅 ABORT 对象兼容此差异：先按小端序解释，若是附录已知码则优先保留；只有小端值未知且大端值恰好命中附录已知码时，才将 `abort_code` 规范化为大端值并设置 `abort_byte_swapped=true`。两种解释均未知时继续保留小端值，不根据前缀猜错误类型。`abort_wire_le` 始终保存原四字节的小端数值；本例为 `0x30000906`，依次取其位移 0、8、16、24 后的低字节即可重建原线序。对象索引、子索引及其它对象的字节序规则不变。

## 请求与回复匹配

- 普通命令 ID 为 `0x600 + node`，回复 ID 为 `0x580 + node`，欠压 ID 为 `0x380 + node`。启停上报命令 ID 直接等于 node。
- 同一个 `0x580 + node` 同时承载 4 字节自动位置上报和 8 字节读写回复，因此必须先看 DLC。
- 读写回复通过 `event.type`、`index`、`subindex` 匹配；`KST_INDEX_*` 提供所有对象索引。
- 协议没有请求序列号。单节点串行执行有回复的请求，可以减少迟到响应归属歧义。即使使用串行请求，超时后的同索引迟到回复仍无法仅凭协议区分。
- 发送成功、CAN 硬件 ACK、应用写 ACK、机构实际到位是不同层次。位置目标和保存命令没有应用 ACK；需要另外读位置或重启后读回可读配置验证。
- `KST_PARSE_IGNORED` 表示其他节点/其他 ID；`KST_PARSE_INVALID` 表示本节点报文不符合文档。失败时输出对象保持原值。
- 构帧失败时输出帧也保持原值，调用方必须检查返回值，不能继续发送先前帧。

## 文档勘误与边界

文档第 5 页将 `+50°` 示例写成 `High=0x01, Low=0xFC`。这等于 508，即 50.8°，与其声明的 0.1° 单位矛盾。模块按明确单位和补码编码实现：`+50° -> 500 -> 0x01F4`。原 PDF 渲染页已核对，此差异不是文本提取导致。

广播在文档中有节点 0 的定义，但默认库 API 不提供广播构帧。这样多舵机共总线时不会意外广播运动、校准、改节点或保存。

V3.61B 的固件升级附录只引用另一本《通讯端口升级软件说明》，没有升级帧定义。这里不提供或推测升级协议。

## 无硬件测试

在项目根目录运行：

```powershell
.\tests\run_host_tests.ps1
# 没有 Windows gcc 时使用现有 WSL Ubuntu-24.04；也可明确选择：
.\tests\run_host_tests.ps1 -Wsl -WslDistribution Ubuntu-24.04
```

Linux：

```sh
sh tests/run_host_tests.sh
```

或直接使用 CMake：

```sh
cmake -S tests -B tests/build-cmake
cmake --build tests/build-cmake
ctest --test-dir tests/build-cmake --output-on-failure
```

测试覆盖全部命令的预期字节、正负边界、功能保留位、全部回复类型、不同节点、错误帧格式/长度/索引、禁止伪造位置与保存 ACK，以及每个 `-1000..1000` 目标值的编解码。还覆盖实物 -100.5°反馈、±180°读取/上报、±180.1°及 `INT16_MIN` 拒收、反向后的有符号范围，以及反馈扩大不改变目标/限位的 ±100°边界。不会打开串口、CAN 设备或启动后台进程。
