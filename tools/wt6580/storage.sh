#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Discover GPT labels through sysfs; never assume partition numbers.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

scan() {
	mkdir -p /dev/block/by-name /dev/block/platform/mtk-msdc.0/11120000.msdc0
	ln -sfn /dev/block/by-name /dev/block/platform/mtk-msdc.0/11120000.msdc0/by-name
	for event in /sys/class/block/mmcblk0p*/uevent; do
		[ -f "$event" ] || continue
		dev= label=
		while IFS= read -r line; do
			case "$line" in
			DEVNAME=*) dev=${line#DEVNAME=} ;;
			PARTNAME=*) label=${line#PARTNAME=} ;;
			esac
		done < "$event"
		# Labels become path components, including for later mount commands.
		case "$label" in ''|.|..|*[!a-zA-Z0-9_.-]*) continue ;; esac
		case "$dev" in mmcblk0p[0-9]*) ;; *) continue ;; esac
		ln -sf "/dev/$dev" "/dev/block/by-name/$label"
		printf '%s -> /dev/%s\n' "$label" "$dev"
	done
}

mounted() {
	grep -q " $1 " /proc/mounts
}

mount_android() {
	scan
	result=0
	for pair in system:system vendor:vendor userdata:data cache:cache; do
		label=${pair%:*}
		point=/mnt/${pair#*:}
		dev=/dev/block/by-name/$label
		[ -b "$dev" ] || continue
		mkdir -p "$point"
		if mounted "$point"; then
			printf 'Already mounted: %s\n' "$point"
			continue
		fi
		info=$(blkid "$dev")
		printf '%s\n' "$info"
		case "$info" in
		*'TYPE="ext4"'*) type=ext4 ;;
		*'TYPE="f2fs"'*) type=f2fs ;;
		*) printf 'No recognized filesystem: %s\n' "$dev"; result=1; continue ;;
		esac
		if mount -t "$type" -o rw,noatime "$dev" "$point"; then
			printf 'Mounted rw: %s -> %s\n' "$label" "$point"
		else
			printf 'Mount failed: %s\n' "$label"
			result=1
		fi
	done
	return "$result"
}

unmount_android() {
	sync
	result=0
	for point in /mnt/data /mnt/cache /mnt/vendor /mnt/system; do
		if mounted "$point"; then
			umount "$point" || result=1
		fi
	done
	return "$result"
}

rw_test() {
	result=0
	tested=0
	for point in /mnt/system /mnt/vendor /mnt/cache /mnt/data; do
		mounted "$point" || continue
		tested=$((tested + 1))
		dir=$point/.wt6580-rw-test.$$
		# mkdir refuses to reuse an existing object. Cleanup touches only this dir.
		if ! mkdir "$dir"; then result=1; continue; fi
		if (
			trap 'rm -f "$dir/probe"; rmdir "$dir"; sync' EXIT
			printf 'WT6580 Linux 6.18 filesystem write test\n' > "$dir/probe" || exit 1
			dd if=/dev/zero bs=65536 count=4 >> "$dir/probe" 2>/dev/null || exit 1
			printf 'WT6580 END\n' >> "$dir/probe" || exit 1
			before=$(sha256sum "$dir/probe") || exit 1
			before=${before%% *}
			sync || exit 1
			# Evict file cache so readback reaches the block driver.
			echo 3 > /proc/sys/vm/drop_caches || exit 1
			after=$(sha256sum "$dir/probe") || exit 1
			after=${after%% *}
			[ "$before" = "$after" ] || exit 1
			printf 'RW PASS %s SHA256=%s\n' "$point" "$after"
		); then :; else
			printf 'RW FAIL %s\n' "$point"
			result=1
		fi
	done
	[ "$tested" -gt 0 ] || { echo 'No filesystems mounted; run mountall'; return 1; }
	return "$result"
}

case "${1:-scan}" in
scan) scan ;;
mount) mount_android ;;
umount) unmount_android ;;
test) rw_test ;;
*) echo 'Usage: wt6580-storage.sh scan|mount|umount|test'; exit 2 ;;
esac
