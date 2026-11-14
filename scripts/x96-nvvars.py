#!/usr/bin/env python3
"""x96-nvvars.py -- inspect and edit the X96 UEFI variable-store file.

The X96 Max+ EDK2 firmware (no SPI NOR) checkpoints its emulated
non-volatile variable store into a flat file named `NvVars` on the boot
card's FAT partition; from Linux the card is mounted at /boot.  This tool
lists, adds, replaces and deletes records in that file, so variables can be
authored from Linux -- including boot entries (Boot#### / BootOrder), which
the firmware cannot checkpoint after ExitBootServices.

File format (see Platform/Amlogic/X96MaxPlus/Drivers/X96NvVarsPersistDxe/
X96NvVarsPersist.h for the authoritative definition):

  header:  "X96NVRAM" | version:u32 | count:u32 | length:u32 | crc32:u32 |
           reserved:u64        (all little-endian; length is a multiple of 4)
  record:  guid:16 | name_len:u32 | name:utf-16le | attrs:u32 |
           data_len:u32 | data

Usage:
  x96-nvvars.py list                                  [--file PATH]
  x96-nvvars.py add    GUID NAME ATTRS [--data DATA | --data-hex HEX |
                        --file-bin FILE]                 [--file PATH]
  x96-nvvars.py del    GUID NAME                        [--file PATH]

ATTRS is a comma list of nv, bs, rt, append (e.g. nv,bs,rt for Boot####).
`add` replaces an existing record with the same GUID+name.

Examples:
  # show everything the firmware will restore on next boot
  x96-nvvars.py list

  # a custom variable
  x96-nvvars.py add 12345678-1234-5678-1234-567812345678 MyVar nv,bs,rt \
      --data-hex 01020304

Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent
"""

import argparse
import binascii
import os
import struct
import sys
import uuid
import zlib

HEADER_FMT = "<8sIIIIQ"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
MAGIC = b"X96NVRAM"

# Where the firmware keeps the store on the boot card: raw LBA 4, the gap
# between the MBR and the first partition.  The tool reads and writes the
# same bytes the firmware does.
STORE_OFFSET = 4 * 512
STORE_BYTES = 128 * 512

ATTR_NV = 0x00000001
ATTR_BS = 0x00000002
ATTR_RT = 0x00000004
ATTR_APPEND = 0x00010000  # EFI_VARIABLE_APPEND_WRITE is not stored; see below
ATTR_MAP = {
    "nv": ATTR_NV,
    "bs": ATTR_BS,
    "rt": ATTR_RT,
    "append": ATTR_APPEND,
}
ATTR_NAMES = {v: k for k, v in ATTR_MAP.items()}


def parse_attrs(text):
    value = 0
    for part in text.split(","):
        part = part.strip().lower()
        if part not in ATTR_MAP:
            raise SystemExit(f"unknown attribute {part!r} (use nv, bs, rt, append)")
        value |= ATTR_MAP[part]
    return value


def fmt_attrs(value):
    return ",".join(name for bit, name in sorted(ATTR_NAMES.items()) if value & bit)


def read_store(path):
    if args_device_mode():
        with open(path, "rb") as fh:
            blob = os.pread(fh.fileno(), STORE_BYTES, STORE_OFFSET)
    else:
        with open(path, "rb") as fh:
            blob = fh.read()
    if len(blob) < HEADER_SIZE:
        raise SystemExit(f"{path}: too small ({len(blob)} bytes)")
    magic, version, count, length, crc, _reserved = struct.unpack(HEADER_FMT, blob[:HEADER_SIZE])
    if magic != MAGIC:
        raise SystemExit(f"{path}: no store image (bad magic {magic!r})")
    data = blob[HEADER_SIZE:HEADER_SIZE + length]
    if len(data) != length:
        raise SystemExit(f"{path}: length field {length} exceeds store size {len(data)}")
    if zlib.crc32(data) & 0xFFFFFFFF != crc:
        raise SystemExit(f"{path}: CRC mismatch (file {crc:#010x})")
    if version != 1:
        raise SystemExit(f"{path}: unsupported version {version}")
    records = []
    pos = 0
    for _ in range(count):
        if pos + 20 > len(data):
            raise SystemExit(f"{path}: record {len(records)} truncated")
        guid = uuid.UUID(bytes_le=data[pos : pos + 16])
        pos += 16
        (name_len,) = struct.unpack_from("<I", data, pos)
        pos += 4
        name = data[pos : pos + name_len].decode("utf-16-le").rstrip("\x00")
        pos += name_len
        attrs, data_len = struct.unpack_from("<II", data, pos)
        pos += 8
        value = data[pos : pos + data_len]
        pos += data_len
        records.append((guid, name, attrs, value))
    return version, records


def args_device_mode():
    return not ARGS.file


def write_store(records):
    chunks = []
    for guid, name, attrs, value in records:
        name_bytes = (name + "\x00").encode("utf-16-le")
        chunks.append(
            guid.bytes_le
            + struct.pack("<I", len(name_bytes))
            + name_bytes
            + struct.pack("<II", attrs, len(value))
            + value
        )
    data = b"".join(chunks)
    while len(data) % 4:
        data += b"\x00"
    crc = zlib.crc32(data) & 0xFFFFFFFF
    header = struct.pack(HEADER_FMT, MAGIC, 1, len(records), len(data), crc, 0)
    blob = header + data
    if len(blob) > STORE_BYTES:
        raise SystemExit(f"store image too large: {len(blob)} bytes")
    if args_device_mode():
        blob += b"\x00" * (STORE_BYTES - len(blob))
        with open(ARGS.device, "r+b") as fh:
            os.pwrite(fh.fileno(), blob, STORE_OFFSET)
            os.fsync(fh.fileno())
    else:
        with open(ARGS.file, "wb") as fh:
            fh.write(blob)
    return blob


def cmd_list(args):
    _, records = read_store(store_path())
    for guid, name, attrs, value in records:
        preview = value[:40]
        shown = preview.hex() if args.hex else preview.decode("utf-8", "replace")
        suffix = "..." if len(value) > 40 else ""
        print(f"{name:24s} {guid}  {fmt_attrs(attrs):10s} {len(value):5d}  {shown}{suffix}")


def cmd_add(args):
    _, records = read_store(store_path())
    guid = uuid.UUID(args.guid)
    if args.data is not None:
        value = args.data.encode()
    elif args.data_hex is not None:
        value = binascii.unhexlify(args.data_hex.replace(" ", ""))
    elif args.file_bin is not None:
        with open(args.file_bin, "rb") as fh:
            value = fh.read()
    else:
        raise SystemExit("one of --data, --data-hex, --file-bin is required")
    new = (guid, args.name, parse_attrs(args.attrs), value)
    records = [r for r in records if not (r[0] == guid and r[1] == args.name)] + [new]
    write_store(records)
    print(f"stored {args.name} ({len(value)} bytes); total {len(records)} records")


def cmd_del(args):
    _, records = read_store(store_path())
    guid = uuid.UUID(args.guid)
    kept = [r for r in records if not (r[0] == guid and r[1] == args.name)]
    if len(kept) == len(records):
        raise SystemExit("no such record")
    write_store(kept)
    print(f"deleted {args.name}; total {len(kept)} records")


ARGS = None


def store_path():
    return ARGS.device if ARGS.device else ARGS.file


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default="/dev/mmcblk1",
                        help="raw boot card device (default /dev/mmcblk1, store at LBA 4); "
                             "pass an empty string to use --file instead")
    parser.add_argument("--file", default=None, help="store as a plain file instead of a raw device")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_list = sub.add_parser("list", help="list records")
    p_list.add_argument("--hex", action="store_true", help="show data as hex")
    p_list.set_defaults(func=cmd_list)
    p_add = sub.add_parser("add", help="add or replace a record")
    p_add.add_argument("guid")
    p_add.add_argument("name")
    p_add.add_argument("attrs", help="comma list of nv, bs, rt, append")
    p_add.add_argument("--data", help="data as text")
    p_add.add_argument("--data-hex", help="data as hex")
    p_add.add_argument("--file-bin", help="data from file")
    p_add.set_defaults(func=cmd_add)
    p_del = sub.add_parser("del", help="delete a record")
    p_del.add_argument("guid")
    p_del.add_argument("name")
    p_del.set_defaults(func=cmd_del)
    ARGS = parser.parse_args()
    ARGS.func(ARGS)


if __name__ == "__main__":
    main()
