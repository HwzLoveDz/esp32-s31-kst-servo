#!/bin/sh
# Cache-only download/extraction. No sudo, package installation or device I/O.
set -eu
task_cache=${1:?Usage: sh prepare_toolchain.sh /absolute/tool-cache-directory}
mkdir -p "$task_cache/downloads" "$task_cache/root"
task_cache=$(CDPATH= cd -- "$task_cache" && pwd)
cd "$task_cache/downloads"
fetch() {
  if [ ! -f "$1" ]; then apt-get download "$2"; fi
  printf '%s  %s\n' "$3" "$1" | sha256sum -c -
}
fetch 'gcc-arm-none-eabi_15%3a13.2.rel1-2_amd64.deb' \
 'gcc-arm-none-eabi=15:13.2.rel1-2' \
 '519a5d0e5292a4b4861f69ae96e8918149bfd1646072ca53451ccfb8c5a91948'
fetch 'binutils-arm-none-eabi_2.42-1ubuntu1+23_amd64.deb' \
 'binutils-arm-none-eabi=2.42-1ubuntu1+23' \
 '2aab2d6a7e4c21ce33462f8b52b972e2a8e4597e79b5add5502e2d1f3e543472'
fetch 'libnewlib-dev_4.4.0.20231231-2_all.deb' \
 'libnewlib-dev=4.4.0.20231231-2' \
 'f945b12c3abc844e76eebc28cf7d99e9aefd7caec39583116ef29556b3c52414'
fetch 'libnewlib-arm-none-eabi_4.4.0.20231231-2_all.deb' \
 'libnewlib-arm-none-eabi=4.4.0.20231231-2' \
 '9af508599193d07c9460897d01a36af46a2707765bd6941e27c6d0747859b0eb'
for task_package in gcc-arm-none-eabi_*.deb binutils-arm-none-eabi_*.deb libnewlib-dev_*.deb; do
  dpkg-deb -x "$task_package" "$task_cache/root"
done
# Extract only Cortex-M0 soft-float libraries, not the other multilib variants.
dpkg-deb --fsys-tarfile libnewlib-arm-none-eabi_4.4.0.20231231-2_all.deb |
  tar -x -C "$task_cache/root" ./usr/lib/arm-none-eabi/newlib/thumb/v6-m/nofp
"$task_cache/root/usr/bin/arm-none-eabi-gcc" --version
printf 'Toolchain ready. Set GNU_ARM_ROOT=%s/root\n' "$task_cache"
