# 使用、架构与协议语义

ESP32-S31-Korvo-1 原装 800×480 触摸屏上的舵机检测台。包含可移植纯 C 协议库、同步事务驱动、默认 candleLight / gs_usb USB Host 适配器、可选 TWAI 适配器，以及五页 LVGL 机甲部件检测界面。上电只读，运动由用户解锁后明确发起。

**目标舵机：KST HS20-12-M-5518-3.2，CAN 扩展帧版本。** `HS20-12-M-5518-x` 是系列资料，不能把 PWM、RS485、CAN 标准帧、RS422 版本混用。本工程依据《KST 舵机 CAN 总线通信协议（扩展帧）V3.61B》，不是 CANopen 通用驱动。

## 从哪里开始

1. 按 [USB-CAN 说明](USB_CANDLELIGHT.md) 连接：Korvo USB-A → USB 数据线 → candleLight 隔离 CANable → 舵机 CANH/CANL；舵机使用独立 12 V。此方案无需引出 GPIO，也不占用摄像头接口。
2. 先在 MONITOR 检查位置、电流和温度，在 LINK 读取节点与版本。实测设备为节点 **5**、固件 **3.66**、1 Mbit/s，七项只读查询已通过，机械动作与越界恢复已确认。当前应用镜像为 `kst-servo-fui-s31-axis-view.bin`，首页正角向逆时针显示，数字与运动命令保持原坐标。详细结果及尚未完成的项目见 [VALIDATION.md](VALIDATION.md)。
3. 此项目默认连接实测节点 **5（0x05）/1 Mbit/s**。厂家协议的通用默认值为37（0x25）/500 kbit/s，库中的默认常量保留厂家定义；换舵机应核对实际配置。先在 MONITOR 观察真实反馈、LINK 读节点/版本；未知数据用 `--` 表示。
4. MOTION 调整目标后按 ARM，再按 SEND。反馈越界会显示 `OUTSIDE TARGET LIMITS`，可按 `SELECT NEAREST LIMIT` 选中合法边界，或进一步向范围内微调，再明确发送。轨迹示例提供有限的一轮 `当前位置→0→+A→−A→0`，默认幅度30°、总周期6秒；当前位置越界时禁止启动轨迹。
5. SERVICE 和 LINK 提供全部已定义参数的演示入口。按 [逐功能验收表](FUNCTIONS.md) 验证读写、上报、欠压、校准、限位、节点和波特率。

当前3.66实物功能原值为 `0x5C90`，高字节含义和写入格式未见资料说明。读取已支持；为保留未知位，库及界面暂时禁止反向、低压报警开关写入，并显示 `FLAGS READ ONLY`。这不影响其它只读查询，不能视为3.66所有配置功能已验证。

位置反馈允许±180°用于显示和恢复；普通目标、限位写入仍按协议和本次实测保留±100°。
两者刻意分离，能显示某个角度不代表舵机接受该目标。实测固件拒绝 +180° 限位写入并返回参数越界 ABORT；原 +100° 已恢复并读回确认。总行程 360° 需要厂家提供支持该范围的协议/固件。

## 五页触屏界面

| 页面 | 功能 |
|---|---|
| MONITOR | 轴角度、实际/目标、电流、温度、数据年龄、趋势、连接与故障状态 |
| MOTION | 主机解锁、0.1°目标、预置角度、有限平滑往返、暂停后续目标 |
| SERVICE | 周期上报、10～255 ms间隔、读取状态/功能、欠压上报开关、反馈符号反向、双向限位、当前点设零、保存所有参数 |
| LINK | 读取/修改节点与8档波特率、主机重连 |
| LOG | CAN收发/丢帧/错误、事务超时、ABORT码、事件记录、升级资料缺失说明 |

图表来自收到的数据。目标是命令值，不是实测到位；主机预览使用明确标记的测试快照。实际反馈超过800 ms会标陈旧并撤销运动解锁。预览见 [preview](../preview/)。

## 可复用的三层库

```text
LVGL FUI → 消息队列 → app_controller（唯一CAN调用任务）
                         ↓
                  kst_servo（同步事务/匹配/超时）
                         ↓
                  kst_protocol（纯C报文编解码）
                         ↓
                  kst_transport_t
                    ├ kst_usb_gs → gs_usb_protocol → USB-A → candleLight → 舵机
                    └ kst_twai → 外置隔离 CAN 收发器 → 舵机（可选）
```

| 目录 | 复用方法 |
|---|---|
| [components/kst_protocol](../components/kst_protocol/README.md) | 不依赖操作系统和堆；可直接编译到STM32、Linux或其他MCU，使用构帧与解析API |
| [components/kst_servo](../components/kst_servo/README.md) | 注入发送、接收、单调时钟3个函数，取得完整同步读写接口；实例和接收队列由单一任务持有 |
| [components/kst_twai](../components/kst_twai/README.md) | ESP32-S31板端传输适配器；经典29位扩展帧；有界队列，TX存储跨异步回调保持有效 |
| [components/gs_usb_protocol](../components/gs_usb_protocol/README.md) | 纯 C 的 gs_usb 字节编解码、USB 描述符检查、精确 CAN 位时序计算；不依赖 ESP-IDF |
| [components/kst_usb_gs](../components/kst_usb_gs/README.md) | 单任务所有权的 USB Host 适配器；异步枚举/配置、拔插、超时、故障锁存、晚期发送门控与安全释放 |
| [main/app_controller.c](../main/app_controller.c) | 展示如何把命令、主动反馈、告警、轨迹、取消与UI线程分离 |
| [examples/headless_poll](../examples/headless_poll/README.md) | 独立ESP-IDF只读示例，不需要LCD或LVGL；用于移植时从小程序开始 |

完整API、每条命令单位/副作用/返回值、线程约束、迟到响应限制在各模块README。`tests` 中包含协议字节级和虚拟传输事务测试；`preview` 用同一份 `main/fui.c` 渲染触屏页面并测试按钮实参。

## 编译

构建需要支持 `esp32s31` 目标的 ESP-IDF **6.2 preview** 和对应工具链。实测使用 GCC 16.1.0，SDK 提交为 `c712a0dde385d659a1470a136251980d31a70bc1`。一般ESP32-S3工具链不能替代ESP32-S31。

```powershell
# 在仓库根目录、已激活匹配 ESP-IDF 的 PowerShell 中执行
.\build.ps1
.\tests\run_host_tests.ps1 -Wsl -WslDistribution Ubuntu-24.04
```

构建脚本使用已激活且支持 S31 的 `IDF_PATH` 环境；父工作区存在 `tools/activate-s31.ps1` 时会优先调用。依赖版本由 `dependencies.lock` 固定。`build.ps1` 只编译，不打开串口或烧写设备。主机测试命令中的 WSL 发行版名称按本机安装情况填写，也可以在 Linux 仓库根目录运行 `sh tests/run_host_tests.sh`。

```sh
# 已激活匹配的ESP-IDF环境时
idf.py --preview set-target esp32s31
idf.py --preview build
```

参数位于 `main/Kconfig.projbuild`，可在 `idf.py --preview menuconfig` 的 `KST CAN servo / Korvo FUI` 中选择 USB 或 TWAI、初始节点、波特率和超时；GPIO 仅在 TWAI 模式显示。默认 `CONFIG_KST_TRANSPORT_USB_GS=y`。8档总线速率为20/50/100/125/250/500/800/1000 kbit/s。USB 根据适配器返回的时钟/约束计算精确位速率；目标采样点80%，取硬件可实现的最近值。实际编译结果和测试记录见 [VALIDATION.md](VALIDATION.md)。

`CONFIG_KST_DIAGNOSTIC_LOG` 每 2 秒打印读取有效位与过滤前 CAN 观测。可选
`CONFIG_KST_DISCOVER_ON_START` 的 Kconfig 默认关闭，本项目 `sdkconfig.defaults` 开启：USB 首次就绪且初始查询无有效回复时，
先用当前主机波特率广播读取节点号并收集原始回复 500 ms，再向 1～127 号节点
读取版本，每节点最多等待 200 ms。结果打印在
串口 `kst_discover` 日志；不自动切换节点、不改舵机参数。扫描期间运动撤销解锁，
PAUSE/配置操作或断连可取消扫描，扫描前和期间排队的操作作废。完成后恢复原节点轮询。

分区表面向 16 MB Flash，应用从 `0x10000` 起占 8 MiB，`0x810000` 起保留 6 MiB 数据区。本工程不使用该数据区，也不初始化相机、Wi-Fi 或音频业务；UART 用于诊断。使用 app-only 烧录前，先核对目标设备已有分区表与应用偏移、分区容量一致，不能仅根据本仓库的分区表覆盖现有设备。

## 命令结果如何理解

- **读取回复/写ACK**：只证明对应协议事务得到回复；位置目标、SAVE、停止上报没有应用ACK，返回 `KST_SERVO_SENT_UNCONFIRMED`。
- **USB TX / echo**：只证明适配器 USB 传输完成或接收进入 CAN 外设，均不等于 CAN ACK 或舵机执行。LOG 区分 `USB TX` 与 TWAI 的 `CAN TX`。
- **USB 重连**：全部旧读数失效，已排队的旧连接操作（包括保存、改参数、清告警）作废；重新就绪只读取反馈，不恢复 ARM 或轨迹。USB/CAN 故障锁存需 LINK 显式重连。
- **运动完成**：有限轨迹发送结束，不代表舵机实测达到终点。看实际角度；CAN硬件ACK也不能证明机构转动。
- **保存**：写入全部已更改配置。文档没有完成时长或成功应答，界面保留未验证提示；不能按固定延时宣称写入完成。
- **改节点、波特率、限位**：先写参数、手动SAVE，再按供应商要求重启舵机。随后在LINK设正确本地主机参数并读取验证。没有协议重启命令。
- **配置超时**：可能是指令未到达，也可能是设备已执行但ACK丢失；不能当作“没有修改”。工程保留dirty/pending提示、立即失效校准前的缓存位置，并保守收紧请求过的限位。恢复链路后先重新读回，再决定保存或恢复配置。
- **反馈反向**：库保留原始反馈符号；FUI依据功能位将显式读取回复转成命令坐标用于比较。文档没有明确反向对自动上报的作用，因此反向打开时自动报告只证明链路活动，运动位置始终来自显式读取。功能位变化后必须收到新位置样本，旧值失效。
- **主机PAUSE/DISARM**：撤销未发送的运动队列和轨迹，不发送虚构的停止/放力命令。已经进入CAN发送器或已经被舵机执行的命令不能撤回；舵机可能继续走向最后目标并保持力矩。
- **总线故障/欠压/ABORT/遥测陈旧**：撤销解锁。总线重连需手动；欠压和ABORT需人工确认。ACKNOWLEDGE只清主机锁存，不表示供电已恢复。
- **限位**：当前运行周期只收紧主机允许范围。写入更宽范围不会立即放宽主机约束，防止把尚未重启生效的值当成有效值；需要重启控制板并重新读回后重新建立范围。

## 支持范围

已实现 V3.61B 协议 2.1～2.18 定义的操作与事件；具体设备的配置写入仍受版本兼容性限制。固件升级附录仅引用另一本《通讯端口升级软件说明》，没有升级帧、文件格式或流程；**升级未实现，需要那份资料才能完成**。界面对此明确标注不可用。

没有文档依据的力矩模式、速度模式、释放力矩、硬急停、电压数值、自动复位等不会被虚构。节点0广播在文档中有定义，但这个单舵机库的公共接口限定节点1～127；可选启动诊断仅有固定的广播读节点报文，不提供广播写入。

协议第5页`+50°`示例低字节存在笔误：按0.1°编码应为`0x01F4`，不是文档示例的`0x01FC`。代码和测试按单位定义处理。

本项目原创应用/驱动使用MIT许可；第三方板级生成代码、LVGL、ESP-IDF及资料保留各自许可。供应商资料是协议与板型依据，未随本仓库重新分发。
