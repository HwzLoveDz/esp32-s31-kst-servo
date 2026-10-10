# Third-party software and reference material

The root MIT license applies to the original application, KST protocol and
transaction libraries, transport adapters, tests and original documentation.
Third-party code retains its own terms.

| Component | License / scope | Source |
| --- | --- | --- |
| `components/gen_bmgr_codes` | [Espressif Modified MIT](components/gen_bmgr_codes/LICENSE); use with Espressif products only | Generated for `esp32_s31_korvo_1` by Espressif Board Manager |
| ESP-IDF | Apache-2.0 and individual component notices | [espressif/esp-idf](https://github.com/espressif/esp-idf) |
| LVGL | MIT | [lvgl/lvgl](https://github.com/lvgl/lvgl) |
| ESP-IDF managed components | Each component's own license; fetched by the component manager | Exact versions and hashes in `dependencies.lock` |
| ZT-CANable candleLight firmware | MIT for candleLight, plus the individual STM32/CMSIS/startup terms | [Firmware license](firmware/zt-canable-candlelight/LICENSE.md), [license sources](firmware/zt-canable-candlelight/licenses/SOURCES.md) |

The vendor's KST protocol/specification and ZT-CANable documentation packages
are not redistributed. The original driver implements the documented KST
extended-frame protocol; see [protocol comparison](docs/REFERENCE_DRIVER_COMPARISON.md)
for the earlier working STM32 reference implementation used to compare frames.

The repository includes an original bench demonstration and its cover image.
The video uses a score composed and synthesized for this project, without
external music recordings or samples.
