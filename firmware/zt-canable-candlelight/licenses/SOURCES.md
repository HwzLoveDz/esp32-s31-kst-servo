# Third-party license sources

These notices accompany the STM32F072C8 adapter firmware. They do not change the licenses of the imported source files, and the repository's root MIT license does not replace them. Existing source headers are retained.

## candleLight

The original firmware's MIT license and Hubert Denkmair copyright are retained in [`../LICENSE.md`](../LICENSE.md) and the imported source files. The source archive identity and adapter-specific modifications are documented in the adapter README and `patches/vendor-diff.patch`.

## µOS++ startup and syscall templates

`UOS-PLUS-MIT.txt` is transcribed from the full license header in the official Eclipse Embedded CDT template repository at commit [`1c60f9ac589ff21402a4c69017a2323dc176b244`](https://github.com/eclipse-embed-cdt/eclipse-plugins/commit/1c60f9ac589ff21402a4c69017a2323dc176b244), authored by Liviu Ionescu on 2017-04-03, titled `templates: explicit MIT license`.

This upstream commit **replaces** the old `_syscalls.c` comment saying that parts came from newlib “issued under GPL” with an explicit MIT grant. Its patch makes no functional change to that file. The imported `_syscalls.c`, `_startup.c` and `_sbrk.c` in this repository were compared with that exact upstream commit; all code is identical after removing comments and whitespace. See `uos-license-provenance.json` for fixed URLs, SHA-256 hashes and the actual upstream license-header patch. This is the basis for using the upstream MIT grant; it is not an assumption that all newlib code is MIT. The historical headers remain in the imported files to preserve provenance.

The other µOS++ templates retain their 2014 Liviu Ionescu notices. The official template distribution's [general MIT notice](https://github.com/eclipse-embed-cdt/eclipse-plugins/blob/b8b061b0480a07c6151620925de72507065613d3/plugins/org.eclipse.embedcdt.templates.core/templates/LICENSE) covers files without a different explicit license. Source-specific third-party notices take precedence.

## STM32 USB device library

`ST-SLA0044-Rev5.txt` was obtained from STMicroelectronics' official [license endpoint](https://www.st.com/resource/en/license_agreement/SLA0044.txt) on 2026-10-10. The historic `https://www.st.com/software_license_agreement_liberty_v2` URL in these sources redirects there. The retrieved text identifies itself as SLA0044 Rev5 / February 2018.

This license permits redistribution of source and binary forms subject to its terms, including retention of notices and exclusive use on or in combination with STMicroelectronics microcontrollers or microprocessors. This firmware targets STM32F072C8. The ST USB library remains under this license, including clause 5; it is not relicensed under the root MIT license, GPL, or any other open-source terms. Include this directory with binary releases.

## STM32 HAL/device support and Arm CMSIS

`STM32-HAL-BSD-3-Clause.txt` copies the original 2016 STMicroelectronics notice from `libs/STM32_HAL/src/stm32f0xx/stm32f0xx_hal.c`. `ARM-CMSIS-BSD-3-Clause.txt` copies the 2009–2015 Arm notice from `libs/STM32_HAL/include/cmsis/core_cm0.h`. Imported source files retain their individual notices and dates; those specific notices remain authoritative. The Arm headers are CMSIS 4.x BSD-licensed material, not the newer CMSIS 5 Apache license.

## Linked newlib and GCC runtime

The build uses the pinned Ubuntu packages in `../prepare_toolchain.sh`; it does not distribute the toolchain executables. The link map of the existing image includes newlib-nano and libgcc routines.

`NEWLIB-4.4.0-DEBIAN-COPYRIGHT.txt` is copied verbatim from the installed `libnewlib-dev` package version `4.4.0.20231231-2` (`/usr/share/doc/libnewlib-dev/copyright`). The package SHA-256 is `f945b12c3abc844e76eebc28cf7d99e9aefd7caec39583116ef29556b3c52414`; the paired target-library package SHA-256 is `9af508599193d07c9460897d01a36af46a2707765bd6941e27c6d0747859b0eb`. It contains the collection of applicable newlib notices and explains that some entries are target-specific. It is not a claim that each listed license applies to this Cortex-M0 image. The [newlib project](https://sourceware.org/newlib/info.html) describes newlib as a collection of library parts with their respective free-software licenses.

`GCC-RUNTIME-EXCEPTION-3.1.txt` and `GPL-3.0.txt` are the unmodified `COPYING.RUNTIME` and `COPYING3` texts from the official [GCC source mirror, release 13.2.0](https://github.com/gcc-mirror/gcc/tree/releases/gcc-13.2.0). The runtime exception accompanies eligible GCC runtime use; including its text does not relicense the adapter or the ST libraries as GPL.

## Integrity and reproduction

`source-hashes.json` records the exact copied license texts and their sources. `uos-license-provenance.json` records the upstream license correction and comparisons. Original source files, the patch, the build instructions and these license notices should accompany firmware distributions. A successful software build does not replace hardware acceptance of a changed image.
