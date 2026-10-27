"""Create a fixed VHD containing a bounded read-only AOSFS volume and hello.elf."""
from pathlib import Path
import struct
import uuid

ROOT = Path(__file__).resolve().parent
ELF = ROOT / "user" / "hello.elf"
DISK = ROOT / "asterion-data.vhd"
SECTOR = 512
SECTORS = 32768  # 16 MiB
MAX_FILES = 7
ENTRY_SIZE = 64

def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value

def put16(buf, off, value): struct.pack_into("<H", buf, off, value)
def put32(buf, off, value): struct.pack_into("<I", buf, off, value)

payload = ELF.read_bytes()
if not payload or len(payload) > (SECTORS - 2) * SECTOR:
    raise SystemExit("hello.elf is empty or too large for AOSFS data area")
needed = (len(payload) + SECTOR - 1) // SECTOR
disk = bytearray(SECTORS * SECTOR)
header = memoryview(disk)[:SECTOR]
header[:8] = b"AOSFS1\0\0"
put16(header, 8, 1)
put16(header, 10, SECTOR)
put32(header, 12, 36)
put32(header, 16, SECTORS)
put32(header, 20, 1)
put16(header, 24, 1)
put16(header, 26, ENTRY_SIZE)
put32(header, 28, 2)
header[32:36] = b"\0\0\0\0"
put32(header, 32, fnv(header[:36]))
row = memoryview(disk)[SECTOR:SECTOR + ENTRY_SIZE]
row[:10] = b"hello.elf\0"
put32(row, 32, 1)  # regular file
put32(row, 36, 2)
put32(row, 40, len(payload))
put32(row, 44, needed)
put32(row, 48, fnv(payload))
disk[2 * SECTOR:2 * SECTOR + len(payload)] = payload

footer = bytearray(SECTOR)
footer[:8] = b"conectix"
struct.pack_into(">IIQ", footer, 8, 2, 0x00010000, 0xFFFFFFFFFFFFFFFF)
struct.pack_into(">4sI4sQQ", footer, 28, b"AOS ", 0x00010000, b"Wi2k", len(disk), len(disk))
cylinders = min(SECTORS // (16 * 63), 65535)
struct.pack_into(">HBB", footer, 56, cylinders, 16, 63)
struct.pack_into(">I", footer, 60, 2)
footer[68:84] = uuid.uuid5(uuid.NAMESPACE_URL, DISK.resolve().as_uri()).bytes
struct.pack_into(">I", footer, 64, (~sum(footer)) & 0xFFFFFFFF)
DISK.write_bytes(disk + footer)
print(f"Wrote {DISK} ({DISK.stat().st_size} bytes), AOSFS hello.elf={len(payload)} bytes, {needed} sectors")
