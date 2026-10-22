#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pack a diagnostic legacy boot image using a captured board-specific header."""

import argparse
import hashlib
from pathlib import Path
import struct


def pack(stock, kernel, ramdisk):
    if len(stock) < 2048 or stock[:8] != b"ANDROID!":
        raise ValueError("Expected a complete legacy Android boot partition dump")
    fields = struct.unpack_from("<10I", stock, 8)
    page = fields[7]
    if fields[8] != 0 or page not in (2048, 4096):
        raise ValueError("Only header version 0 without a separate DT image is supported")
    if stock[-64:-60] == b"AVBf":
        raise ValueError("AVB footer detected; this tool does not sign or bypass verification")
    if kernel[0x24:0x28] != b"\x18\x28\x6f\x01":
        raise ValueError("Expected an ARM zImage with an appended board DTB")
    start, end = struct.unpack_from("<II", kernel, 0x28)
    zimage_size = end - start
    if kernel[zimage_size:zimage_size + 4] != b"\xd0\x0d\xfe\xed":
        raise ValueError("Board DTB not found at the end of the zImage")
    dtb_size = struct.unpack_from(">I", kernel, zimage_size + 4)[0]
    if zimage_size + dtb_size != len(kernel):
        raise ValueError("Appended DTB length mismatch")
    if ramdisk[:2] != b"\x1f\x8b":
        raise ValueError("Expected a gzip-compressed diagnostic initramfs")

    def aligned(data):
        return data + bytes(-len(data) % page)

    header = bytearray(stock[:page])
    struct.pack_into("<I", header, 8, len(kernel))
    struct.pack_into("<I", header, 16, len(ramdisk))
    struct.pack_into("<I", header, 24, 0)  # no second-stage payload
    # Diagnostic image: retain the board's addresses and bootopt command line.
    digest = hashlib.sha1()
    for blob in (kernel, ramdisk, b""):
        digest.update(blob)
        digest.update(struct.pack("<I", len(blob)))
    header[576:608] = digest.digest() + bytes(12)
    result = bytes(header) + aligned(kernel) + aligned(ramdisk)
    if len(result) > len(stock):
        raise ValueError("Diagnostic image exceeds the captured boot partition size")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--ramdisk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.resolve() == args.stock.resolve():
        parser.error("Output must not overwrite the stock boot backup")
    try:
        image = pack(args.stock.read_bytes(), args.kernel.read_bytes(),
                     args.ramdisk.read_bytes())
    except (ValueError, struct.error) as error:
        parser.error(str(error))
    args.output.write_bytes(image)
    print(f"Wrote {args.output}: {len(image)} bytes (not flashed)")


if __name__ == "__main__":
    main()
