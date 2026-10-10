# ESP32-S31 KST Servo: CAN Control and Telemetry Console

[中文](README.md) | **English**

Connect a KST CAN servo to the USB-A port of an ESP32-S31-Korvo-1 and use its built-in touchscreen for position control, trajectory demos, and telemetry. This repository includes portable C libraries, a candleLight USB Host transport, and a five-page LVGL interface styled as a mecha component inspection console.

[![Korvo touchscreen, isolated CANable, and KST servo running together on the bench](docs/images/servo-console.jpg)](docs/media/servo-showcase.mp4)

[Watch the 44-second hardware demo](docs/media/servo-showcase.mp4) · [Hardware](docs/HARDWARE.md) · [Operation](docs/OPERATIONS.md) · [Function coverage](docs/FUNCTIONS.md)

## Why I built it

I wanted a small, standalone servo console: one screen for target and measured position, current, temperature, communication status, and configuration. It should work without a PC during normal operation, while keeping the driver reusable without the UI.

The Korvo USB-A port connects directly to an isolated CANable, so the default setup needs no wires to the board's TWAI GPIOs. Protocol encoding, synchronous transactions, and transport adapters are separate. A controller without USB Host can keep the same servo library and supply its own CAN transport.

The hardware demo uses a **KST HS20-12-M-5518-3.2, firmware 3.66, node 5, at 1 Mbit/s**. Readback, manual positioning, and finite sweep demos have run on this unit. The implementation follows the KST V3.61B extended-frame protocol, with compatibility handling for replies observed on this 3.66 servo. It is not a generic CANopen driver and does not target the PWM, RS485, or standard-frame variants of the same product family.

## What is included

- **Standalone bench console:** angle, current, temperature, trends, and communication events on an 800 × 480 touchscreen, with manual positioning and finite trajectories.
- **Reusable C libraries:** the protocol layer needs no operating system or heap; the transaction layer takes three callbacks for send, receive, and monotonic time.
- **USB-CAN host example:** candleLight / gs_usb enumeration, bit timing, extended frames, disconnect handling, and explicit reconnection.
- **Configuration controls:** reporting, report interval, limits, center calibration, saving, node ID, and bitrate, with distinct sent and confirmed states.
- **Observable motion state:** startup is read-only; movement requires ARM and an explicit command. Stale feedback, communication faults, and device errors disarm motion.

## Hardware

```text
ESP32-S31-Korvo-1 USB-A
        │ USB data cable
Isolated CANable / candleLight (gs_usb)
        │ CANH / CANL / bus-side reference ground
KST HS20-12-M-5518-3.2 ← separate 12 V supply
```

| Part | Project configuration |
|---|---|
| Controller | ESP32-S31-Korvo-1, stock 800 × 480 touchscreen, 16 MB Flash |
| USB-CAN | Tested isolated CANable 1.0 with candleLight firmware matching its board |
| CAN | Classical CAN, 29-bit extended data frames; project default is 1 Mbit/s, node 5 |
| Servo | KST HS20-12-M-5518-3.2, firmware 3.66, separate 12 V power |
| Alternative transport | Native TWAI with an external CAN transceiver and the documented GPIO wiring |

The adapter must advertise one channel and `ONE_SHOT` support. PCAN, SLCAN, and DFU modes do not work with this USB driver. The CANable name alone is not enough to select a firmware image for a clone. See [HARDWARE](docs/HARDWARE.md) for board details, isolated-side grounding, termination, and wiring, and [USB_CANDLELIGHT](docs/USB_CANDLELIGHT.md) for adapter requirements.

## Build and first run

### 1. Prepare the SDK

Use **ESP-IDF 6.2 preview** with `esp32s31` support. The SDK revision used by this project is [`c712a0dde385d659a1470a136251980d31a70bc1`](https://github.com/espressif/esp-idf/commit/c712a0dde385d659a1470a136251980d31a70bc1). An ESP32-S3 toolchain is not a substitute.

On Linux, install the ESP-IDF system prerequisites, then prepare the pinned SDK:

```sh
git clone https://github.com/espressif/esp-idf.git esp-idf-s31
cd esp-idf-s31
git checkout c712a0dde385d659a1470a136251980d31a70bc1
git submodule update --init --recursive
./install.sh esp32s31
. ./export.sh
cd ..
```

On Windows, use the installation and environment activation scripts from that SDK revision. The following `idf.py` commands also work in an activated Windows environment. Component versions are pinned in `dependencies.lock`.

### 2. Clone, configure, and build

```sh
git clone https://github.com/HwzLoveDz/esp32-s31-kst-servo.git
cd esp32-s31-kst-servo
idf.py --preview set-target esp32s31
idf.py --preview menuconfig
idf.py --preview build
```

In `KST CAN servo / Korvo FUI`, check the transport, node, bitrate, and timeouts. Project `sdkconfig.defaults` selects **USB / node 5 / 1 Mbit/s**. Protocol library defaults and the standalone TWAI example retain the vendor's generic **node 37 / 500 kbit/s** settings. Check the actual configuration when connecting another servo.

Windows PowerShell users with an activated SDK can also run `./build.ps1`. The script builds only; it does not open a serial port or flash a device.

### 3. Check the partition layout before flashing

`partitions.csv` targets 16 MB Flash: an 8 MB application at `0x10000` and a 6 MB data partition at `0x810000`. Compare this with the target board's existing partition table. The repository layout does not establish what is currently installed on another Korvo.

**Only when the existing layout matches this project**, replace `PORT` and flash the application:

```sh
idf.py --preview -p PORT app-flash monitor
```

See [HARDWARE](docs/HARDWARE.md) for a different layout or blank Flash. `release/kst-servo-fui-s31-axis-view.bin` is an application image, not a complete package containing a bootloader and partition table. The application does not initialize or erase the reserved data partition, or start camera, Wi-Fi, or audio workloads.

### 4. Read first, then try a small move

1. Secure the servo and leave clearance for its arm. Connect the bus and separate supply as described in the wiring guide.
2. After startup, wait for valid angle, current, and temperature in **MONITOR**. Check the node and firmware version in **LINK**. `--` means no valid sample is available.
3. In **MOTION**, choose a small offset from the measured position, then press **ARM MOTION → SEND POSITION**. Observe both the feedback and the mechanism.
4. Check direction and limits before starting a finite sweep. The default is a 30° amplitude and 6-second total period: `current position → 0 → +A → −A → 0`, once.

Startup discovery is enabled by default and probes nodes only if the initial read-only queries receive no valid replies. It logs results to the serial console without changing the servo node, bitrate, or motion state. See [OPERATIONS](docs/OPERATIONS.md) for normal use and recovery.

## Five touchscreen pages

| Page | Purpose |
|---|---|
| MONITOR | Axis angle, target and actual feedback, current, temperature, trends, sample age, and connection state |
| MOTION | Target adjustment, presets, explicit arming and sending, finite smooth sweeps, and pausing future targets |
| SERVICE | Reporting and interval, status and feature reads, limits, current-position center calibration, and parameter saving |
| LINK | Node and bitrate reads/configuration, eight bitrate options, and host reconnection |
| LOG | Traffic statistics, timeouts, ABORT codes, and event history |

The home-page arm graphic shows positive angles counterclockwise from the viewing direction used in the demo. Numeric angles and motion commands keep the original coordinate system. A different viewing direction calls for a display adjustment, not treating feedback sign reversal as mechanical direction control.

## Important operating details

**Targets and feedback have different ranges.** Targets and limit writes retain the protocol's ±100° range and are further constrained by the limits read from the device. Position feedback accepts ±180° so an out-of-target-range position can still be displayed and recovered. This servo rejected a +180° limit write; the current version does not offer 360° travel or multi-turn positioning. For `OUTSIDE TARGET LIMITS`, select the nearest valid limit, adjust inward if needed, then explicitly ARM and SEND.

**Feature switches are read-only on this 3.66 unit.** Its FEATURES value is `0x5C90`. The unknown high byte is preserved, and the library/UI block reversal and undervoltage-alarm switch writes for this value. They do not guess a write format or clear unknown bits. See [FUNCTIONS](docs/FUNCTIONS.md) for implementation and hardware status of other settings.

**PAUSE does not release torque.** It cancels unsent host motion commands and trajectories. A command already sent cannot be recalled, so the servo may continue toward its last target and hold torque. There is no implemented torque-release or hard emergency-stop command.

**Sent does not mean reached.** USB TX completion, adapter echo, CAN ACK, an application reply, and actual motion are separate results. Position targets, SAVE, and stopping reports have no application ACK; the UI preserves their unconfirmed status. Saving writes all modified parameters. Node, bitrate, and limit changes require saving and the device's restart procedure, followed by readback.

**Servo firmware update is not implemented.** The available protocol references a separate upgrade guide without defining its frames or procedure. Torque mode, velocity mode, and voltage-value telemetry are also outside the current implementation. The 0.1° protocol unit and target increment are not a mechanical accuracy specification.

## Reuse the driver

```text
LVGL FUI → command queue → app_controller (one task owns the bus)
                             ↓
                         kst_servo
                             ↓
                       kst_transport_t
                       ├ kst_usb_gs → candleLight
                       └ kst_twai   → CAN transceiver

kst_protocol: KST codec       gs_usb_protocol: USB-CAN codec and bit timing
```

| Module | Reuse entry point |
|---|---|
| [kst_protocol](components/kst_protocol/README.md) | Platform-independent C99 frame construction, parsing, units, and compatibility handling |
| [kst_servo](components/kst_servo/README.md) | Synchronous API, reply matching, timeouts, and event callbacks |
| [gs_usb_protocol](components/gs_usb_protocol/README.md) | gs_usb byte format, descriptor validation, and exact bitrate calculation |
| [kst_usb_gs](components/kst_usb_gs/README.md) | ESP-IDF USB Host lifecycle, faults, and reconnection |
| [kst_twai](components/kst_twai/README.md) | Native ESP32-S31 TWAI transport |

For another platform, keep `kst_protocol` and `kst_servo`, then provide `send`, `receive`, and `now_ms`. One task must own each client and its receive queue. Callback signatures and a minimal integration example are in the component README files.

[examples/headless_poll](examples/headless_poll/README.md) is a standalone **read-only TWAI example** without LCD or LVGL dependencies. Configure its transceiver wiring, GPIOs, node, and bitrate as documented, then build it:

```sh
cd examples/headless_poll
idf.py --preview set-target esp32s31
idf.py --preview build
```

## Tests and compatibility

Host tests need CMake 3.16+ and a C compiler, but no servo, USB adapter, or ESP-IDF:

```sh
cmake -S tests -B tests/build-cmake
cmake --build tests/build-cmake --config Release
ctest --test-dir tests/build-cmake -C Release --output-on-failure
```

Tests cover protocol bytes, range checks, transaction matching, timeouts, controller behavior, and the USB state machine. They do not replace hardware tests for bus behavior, fault recovery, or mechanical motion. Build and hardware records are in [VALIDATION](docs/VALIDATION.md); per-function coverage is in [FUNCTIONS](docs/FUNCTIONS.md).

| Target | Current scope |
|---|---|
| Complete touchscreen application | ESP32-S31-Korvo-1; other boards need display, touch, and board-support porting |
| KST hardware | HS20-12-M-5518-3.2 / 3.66; other models or firmware need protocol and reply-format checks |
| USB adapter | The tested isolated CANable with matching candleLight; not a blanket claim for all clones |
| Bitrates | Hardware demo at 1 Mbit/s; code includes 20/50/100/125/250/500/800/1000 kbit/s |
| Portable C libraries | Portable interfaces; other MCUs, operating systems, and CAN adapters need their own integration tests |

Issues are welcome with the board model, full servo model, firmware version, node/bitrate, and a minimal reproduction. For protocol differences, raw CAN frames are more useful than a screenshot alone.

## License and references

Original application code, drivers, tests, and documentation use the [MIT License](LICENSE). ESP-IDF, LVGL, generated board support, and other third-party content retain their own licenses; see [THIRD_PARTY](THIRD_PARTY.md). The vendor's original document archive is not redistributed here.

The USB adapter protocol is based on the public [candleLight](https://github.com/candle-usb/candleLight_fw) and [Linux gs_usb](https://github.com/torvalds/linux/blob/master/drivers/net/can/usb/gs_usb.c) references; this repository's codecs and host adapter are independent implementations. Integration also used [KST-CAN-servo-driver](https://github.com/HwzLoveDz/KST-CAN-servo-driver), which had controlled the same servo, as a reference. Adapter firmware version and acquisition details are in [USB_CANDLELIGHT](docs/USB_CANDLELIGHT.md).
