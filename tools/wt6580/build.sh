#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
output_dir=${OUT_DIR:-"$source_root/../out/wt6580-android17"}
output_dir=$(realpath -m -- "$output_dir")
toolchain=${LLVM_BIN:-/home/fuquan/buildtools/linux-x86/clang-r547379/bin}
toolchain=$(realpath -- "$toolchain")
jobs=${JOBS:-$(nproc)}
display=${DISPLAY_DRIVER:-lk}
dtb=mediatek/mt6580-wt6580-we-n.dtb
if [[ "$display" == drm ]]; then
	dtb=mediatek/mt6580-wt6580-we-n-drm.dtb
elif [[ "$display" != lk ]]; then
	echo "DISPLAY_DRIVER must be lk or drm" >&2
	exit 1
fi

for tool in clang ld.lld llvm-ar llvm-nm llvm-objcopy; do
	if [[ ! -x "$toolchain/$tool" ]]; then
		echo "Missing $toolchain/$tool; set LLVM_BIN to an Android Clang toolchain" >&2
		exit 1
	fi
done

userspace=${USERSPACE:-1}
userspace_out=${USERSPACE_OUT:-"$source_root/../out/wt6580-userspace"}
userspace_out=$(realpath -m -- "$userspace_out")
if [[ "$userspace" == 1 ]]; then
	USERSPACE_OUT="$userspace_out" LLVM_BIN="$toolchain" JOBS="$jobs" \
		"$source_root/tools/wt6580/build-userspace.sh"
elif [[ "$userspace" != 0 ]]; then
	echo "USERSPACE must be 0 or 1" >&2
	exit 1
fi

mkdir -p -- "$output_dir/diagnostic"
"$toolchain/clang" --target=armv7a-linux-gnueabi -mcpu=cortex-a7 -marm \
	-Os -Wall -Wextra -Werror -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-nostdlib -static -fuse-ld=lld -Wl,--build-id=none,-e,_start \
	"$source_root/tools/wt6580/init.c" -o "$output_dir/diagnostic/init"

cat > "$output_dir/diagnostic/initramfs.list" <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
dir /proc 0555 0 0
dir /sys 0555 0 0
file /init $output_dir/diagnostic/init 0755 0 0
EOF
if [[ "$userspace" == 1 ]]; then
	python3 - "$source_root" "$output_dir" "$userspace_out" <<'PYROOTFS'
from pathlib import Path
import sys
source, output, userspace = map(Path, sys.argv[1:])
links = userspace.joinpath("busybox.links").read_text().splitlines()
files = {
    "/bin/busybox": (userspace / "busybox", "0755"),
    "/etc/wt6580-storage.sh": (source / "tools/wt6580/storage.sh", "0755"),
    "/usr/share/licenses/busybox/LICENSE": (userspace / "busybox-1.37.0/LICENSE", "0644"),
    "/usr/share/licenses/musl/COPYRIGHT": (userspace / "musl-1.2.5/COPYRIGHT", "0644"),
}
dirs = {"/run", "/tmp", "/mnt", "/mnt/system", "/mnt/vendor", "/mnt/data", "/mnt/cache"}
for path in list(files) + links:
    parent = Path(path).parent
    while str(parent) != "/":
        dirs.add(str(parent))
        parent = parent.parent
with (output / "diagnostic/initramfs.list").open("a") as f:
    for path in sorted(dirs):
        f.write(f"dir {path} {'1777' if path == '/tmp' else '0755'} 0 0\n")
    for path, (local, mode) in files.items():
        f.write(f"file {path} {local} {mode} 0 0\n")
    for path in links:
        f.write(f"slink {path} /bin/busybox 0777 0 0\n")
PYROOTFS
fi


kernel_make=(make -C "$source_root" O="$output_dir" ARCH=arm "LLVM=$toolchain/")
"${kernel_make[@]}" wt6580_we_n_defconfig
if [[ "${EARLY_BOOT_TRACE:-0}" == 1 ]]; then
	python3 - "$source_root/scripts/config" "$output_dir/.config" <<'PY'
import json
from pathlib import Path
import subprocess
import sys

script, path = sys.argv[1:]
line = next(line for line in Path(path).read_text().splitlines()
            if line.startswith("CONFIG_CMDLINE="))
cmdline = json.loads(line.split("=", 1)[1])
# ARM tears down the earlycon fixmap before allocating its replacement.
# memblock=debug would print through the temporarily unmapped UART there.
cmdline += " arm_early_trace initcall_debug keep_bootcon"
subprocess.run([script, "--file", path, "--set-str", "CMDLINE", cmdline], check=True)
PY
fi
"$source_root/scripts/config" --file "$output_dir/.config" \
	--set-str INITRAMFS_SOURCE "$output_dir/diagnostic/initramfs.list" \
	--enable INITRAMFS_COMPRESSION_GZIP
if [[ "$display" == drm ]]; then
	"$source_root/scripts/config" --file "$output_dir/.config" \
		--disable FB_MT6580_LK --enable DRM_MEDIATEK \
		--enable PHY_MTK_MIPI_DSI --enable REGULATOR_FIXED_VOLTAGE \
		--enable CMA --enable DMA_CMA --enable DMADEVICES \
		--disable DRM_FBDEV_EMULATION
fi
"${kernel_make[@]}" olddefconfig
python3 - "$output_dir/.config" "$display" <<'PY'
from pathlib import Path
import sys

config = Path(sys.argv[1]).read_text().splitlines()
required = ["CONFIG_ARCH_MEDIATEK=y", "CONFIG_ARM_APPENDED_DTB=y",
            "CONFIG_MEDIATEK_WATCHDOG=y", "CONFIG_PSTORE_RAM=y",
            "CONFIG_DRM_PANEL_WINGTECH_WT6580=y", "CONFIG_ANDROID_BINDER_IPC=y",

            "CONFIG_POWER_RESET_MT6580_BOOTMODE=y",
            "CONFIG_MMC_MTK=y", "CONFIG_MMC_BLOCK=y",
            "CONFIG_MMC_BLOCK_MINORS=32", "CONFIG_REGULATOR_FIXED_VOLTAGE=y",
            "CONFIG_USB_MUSB_MEDIATEK=y", "CONFIG_MUSB_PIO_ONLY=y",
            "CONFIG_PHY_MTK_MT6580_USB=y", "CONFIG_COMMON_CLK_MT6580_USB=y",
            "CONFIG_USB_G_SERIAL=y", "CONFIG_U_SERIAL_CONSOLE=y",
            "# CONFIG_SMP is not set"]
if sys.argv[2] == "drm":
    required += ["CONFIG_DRM_MEDIATEK=y", "CONFIG_PHY_MTK_MIPI_DSI=y", "CONFIG_DMA_CMA=y"]
else:
    required += ["CONFIG_FB_MT6580_LK=y"]
missing = [line for line in required if line not in config]
if missing:
    raise SystemExit("Required bring-up configuration missing: " + ", ".join(missing))
PY
"${kernel_make[@]}" -j"$jobs" zImage "$dtb"

cat "$output_dir/arch/arm/boot/zImage" \
	"$output_dir/arch/arm/boot/dts/$dtb" \
	> "$output_dir/zImage-dtb"

python3 - "$output_dir" <<'PY'
import gzip
from pathlib import Path
import sys

output = Path(sys.argv[1])
(output / "diagnostic/initramfs.cpio.gz").write_bytes(
    gzip.compress((output / "usr/initramfs_data.cpio").read_bytes(), mtime=0))
PY

if [[ -n "${STOCK_BOOT:-}" ]]; then
	python3 "$source_root/tools/wt6580/pack_boot.py" \
		--stock "$STOCK_BOOT" --kernel "$output_dir/zImage-dtb" \
		--ramdisk "$output_dir/diagnostic/initramfs.cpio.gz" \
		--output "$output_dir/boot-wt6580-diagnostic.img"
fi

python3 - "$source_root" "$output_dir" "$toolchain" "$dtb" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

source, output, llvm = map(Path, sys.argv[1:4])
artifacts = ["zImage-dtb", "arch/arm/boot/zImage",
             "arch/arm/boot/dts/" + sys.argv[4],
             "diagnostic/initramfs.cpio.gz", "diagnostic/init"]
if os.environ.get("STOCK_BOOT"):
    artifacts.append("boot-wt6580-diagnostic.img")
manifest = {
    "base": subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
    "kernel_release": (output / "include/config/kernel.release").read_text().strip(),
    "compiler": subprocess.check_output([str(llvm / "clang"), "--version"],
                                        text=True).splitlines()[0],
    "hardware_boot_verified": False,
    "debug_transport": "usb-cdc-acm",
    "usb_enumeration_verified": False,
    "early_boot_trace": os.environ.get("EARLY_BOOT_TRACE", "0") == "1",
    "static_busybox": os.environ.get("USERSPACE", "1") == "1",
    "config_sha256": hashlib.sha256((output / ".config").read_bytes()).hexdigest(),
    "artifacts": {name: {"bytes": (output / name).stat().st_size,
                         "sha256": hashlib.sha256((output / name).read_bytes()).hexdigest()}
                  for name in artifacts},
}
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest, indent=2))
PY
