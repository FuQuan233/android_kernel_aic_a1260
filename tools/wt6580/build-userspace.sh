#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build a static ARMv7 BusyBox from pinned sources using the Android Clang tools.
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
userspace_out=${USERSPACE_OUT:-"$source_root/../out/wt6580-userspace"}
userspace_out=$(realpath -m -- "$userspace_out")
llvm=${LLVM_BIN:-/home/fuquan/buildtools/linux-x86/clang-r547379/bin}
llvm=$(realpath -- "$llvm")
resource_dir=$("$llvm/clang" -print-resource-dir)
runtime=$resource_dir/lib/linux/libclang_rt.builtins-arm-android.a
jobs=${JOBS:-8}
mkdir -p "$userspace_out/downloads"

fetch() {
	local url=$1 name=$2 digest=$3
	if ! [[ -f "$userspace_out/downloads/$name" ]]; then
		curl -fL --retry 2 --connect-timeout 15 --max-time 180 "$url" \
			-o "$userspace_out/downloads/$name.partial"
		mv "$userspace_out/downloads/$name.partial" "$userspace_out/downloads/$name"
	fi
	printf '%s  %s\n' "$digest" "$userspace_out/downloads/$name" | sha256sum -c -
}
fetch https://musl.libc.org/releases/musl-1.2.5.tar.gz musl-1.2.5.tar.gz \
	a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
fetch https://busybox.net/downloads/busybox-1.37.0.tar.bz2 busybox-1.37.0.tar.bz2 \
	3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4
if [[ ! -d "$userspace_out/musl-1.2.5" ]]; then
	tar -xzf "$userspace_out/downloads/musl-1.2.5.tar.gz" -C "$userspace_out"
fi
if [[ ! -d "$userspace_out/busybox-1.37.0" ]]; then
	tar -xjf "$userspace_out/downloads/busybox-1.37.0.tar.bz2" -C "$userspace_out"
fi
sysroot=$userspace_out/sysroot
(
	cd "$userspace_out/musl-1.2.5"
	./configure --target=arm-linux-musleabi --prefix="$sysroot" --disable-shared \
		CC="$llvm/clang --target=armv7a-linux-musleabi -mcpu=cortex-a7 -mfloat-abi=soft -fuse-ld=lld" \
		AR="$llvm/llvm-ar" RANLIB="$llvm/llvm-ranlib" LIBCC="$runtime"
	make -j"$jobs"
	make install
)
make -C "$source_root" ARCH=arm O="$userspace_out/headers-build" \
	INSTALL_HDR_PATH="$sysroot" headers_install

# musl's GCC wrapper expects GCC's startup objects. This C-only wrapper uses
# musl's own CRT and compiler-rt, and never adds CRT objects to relocatable links.
python3 - "$userspace_out/cc-musl" "$llvm" "$sysroot" "$runtime" <<'PY'
from pathlib import Path
import shlex
import sys
path, llvm, sysroot, runtime = sys.argv[1:]
variables = '\n'.join(f'{k}={shlex.quote(v)}' for k, v in
                      [('llvm', llvm), ('sysroot', sysroot), ('runtime', runtime)])
Path(path).write_text('''#!/bin/bash
set -e
''' + variables + '''
for arg; do
 case "$arg" in
 -c|-E|-S|-r|-nostdlib|--version|-dumpmachine|-dumpversion|-print*)
 exec "$llvm/clang" --target=armv7a-linux-musleabi -mcpu=cortex-a7 -mfloat-abi=soft --sysroot="$sysroot" -isystem "$sysroot/include" -fuse-ld=lld "$@";;
 esac
done
exec "$llvm/clang" --target=armv7a-linux-musleabi -mcpu=cortex-a7 -mfloat-abi=soft --sysroot="$sysroot" -isystem "$sysroot/include" -fuse-ld=lld -nostdlib -static "$sysroot/lib/crt1.o" "$sysroot/lib/crti.o" "$@" -Wl,--start-group -L"$sysroot/lib" -lc "$runtime" -Wl,--end-group "$sysroot/lib/crtn.o"
''')
Path(path).chmod(0o755)
PY
bb=$userspace_out/busybox-1.37.0
make -C "$bb" ARCH=arm allnoconfig
python3 - "$bb/.config" "$source_root/tools/wt6580/busybox.config" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
flags = Path(sys.argv[2]).read_text().splitlines()
keys = {line.split('=')[0] for line in flags if line.startswith('CONFIG_')}
lines = [line for line in p.read_text().splitlines()
         if line.split('=')[0] not in keys
         and not (line.startswith('# CONFIG_') and line[2:].split()[0] in keys)]
p.write_text('\n'.join(lines + flags) + '\n')
PY
make -C "$bb" ARCH=arm oldconfig < /dev/null
python3 - "$bb/.config" "$source_root/tools/wt6580/busybox.config" <<'PYCONFIG'
from pathlib import Path
import sys
actual = set(Path(sys.argv[1]).read_text().splitlines())
requested = Path(sys.argv[2]).read_text().splitlines()
missing = [line for line in requested if line.startswith("CONFIG_") and line not in actual]
if missing:
    raise SystemExit("BusyBox configuration was not selected: " + ", ".join(missing))
PYCONFIG
make -C "$bb" ARCH=arm CC="$userspace_out/cc-musl" AR="$llvm/llvm-ar" \
	STRIP="$llvm/llvm-strip" -j"$jobs"
cp "$bb/busybox" "$userspace_out/busybox"
# Keep the applet list for packaging symlinks without executing ARM code on x86.
(cd "$bb"; HOSTCC=cc sh applets/busybox.mkll) > "$userspace_out/busybox.links"
sha256sum "$userspace_out/busybox"
